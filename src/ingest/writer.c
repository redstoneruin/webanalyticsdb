#include "../core/engine.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void commit_group(wadb_append_request **requests, size_t count) {
    wadb_error error = {0};
    wadb_status status = wadb_commit_requests(requests[0]->table, requests, count, &error);
    /* Dictionary additions can make a combined frame too large even when each
     * request fits alone. Split only between requests, preserving atomicity. */
    if (status == WADB_LIMIT && count > 1) {
        size_t half = count / 2;
        commit_group(requests, half);
        commit_group(requests + half, count - half);
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        requests[i]->status = status;
        requests[i]->error = error;
    }
}

static void *writer_main(void *context) {
    wadb_db *db = context;
    for (;;) {
        pthread_mutex_lock(&db->mutex);
        while (!db->queue_head && !db->stopping) pthread_cond_wait(&db->work, &db->mutex);
        if (!db->queue_head && db->stopping) { pthread_mutex_unlock(&db->mutex); break; }
        uint64_t deadline = db->queue_head->submitted_ns + (uint64_t)db->options.batch_delay_ms * 1000000;
        while (!db->stopping && db->ready_bytes < db->options.batch_target_bytes && wadb_monotonic_ns() < deadline) {
            int rc = wadb_condition_wait_until(&db->work, &db->mutex, deadline);
            if (rc) break;
        }
        wadb_append_request *requests[64];
        size_t count = 0, bytes = 0;
        wadb_table *table = db->queue_head->table;
        int64_t previous = table->last_time, day = -1;
        while (db->queue_head && db->queue_head->table == table && count < 64) {
            wadb_append_request *r = db->queue_head;
            if (count && (bytes >= db->options.batch_target_bytes || r->row_bytes > WADB_MAX_FRAME_BYTES - 96 - bytes)) break;
            int64_t now = db->realtime ? db->realtime(db->clock_context) : wadb_realtime_us();
            if (now >= 0 && now < previous) { now = previous; ++db->clock_adjustments; }
            if (count && (now < 0 || now / WADB_DAY_US != day)) break;
            r->ingestion_time = now;
            previous = now; day = now < 0 ? -1 : now / WADB_DAY_US;
            db->queue_head = r->next;
            if (!db->queue_head) db->queue_tail = NULL;
            r->next = NULL;
            db->ready_bytes -= r->row_bytes;
            requests[count++] = r; bytes += r->row_bytes;
            if (now < 0) break;
        }
        pthread_mutex_unlock(&db->mutex);
        commit_group(requests, count);
        pthread_mutex_lock(&db->mutex);
        for (size_t i = 0; i < count; ++i) {
            wadb_append_request *r = requests[i];
            free(r->rows); r->rows = NULL;
            db->queue_bytes -= r->charge;
            if (r->status) ++db->failed_appends;
            else wadb_histogram_record(&db->append_latency, wadb_monotonic_ns() - r->started_ns);
            r->done = true;
            pthread_cond_signal(&r->ready);
        }
        pthread_mutex_unlock(&db->mutex);
    }
    return NULL;
}

wadb_status wadb_writer_start(wadb_db *db, wadb_error *e) {
    int rc = wadb_condition_init(&db->work);
    if (rc) return wadb_fail(e, WADB_IO, rc, "initialize writer condition");
    db->work_initialized = true;
    rc = pthread_create(&db->writer, NULL, writer_main, db);
    if (rc) return wadb_fail(e, WADB_IO, rc, "start writer thread");
    db->writer_started = true;
    return WADB_OK;
}

void wadb_writer_stop(wadb_db *db) {
    if (!db->writer_started) return;
    pthread_mutex_lock(&db->mutex);
    db->stopping = true;
    pthread_cond_signal(&db->work);
    pthread_mutex_unlock(&db->mutex);
    pthread_join(db->writer, NULL);
    db->writer_started = false;
}

static wadb_status validate_symbol(void *ctx, uint32_t field, const void *data,
    size_t length, uint32_t *id, wadb_error *e) {
    (void)ctx; (void)field; (void)data; (void)length; (void)e;
    *id = 1; /* The row codec validates the string; the writer resolves its ID. */
    return WADB_OK;
}

wadb_status wadb_append_batch(wadb_table *t, const wadb_value *values, size_t count,
    wadb_append_receipt *receipt, wadb_error *e) {
    uint64_t started = wadb_monotonic_ns();
    wadb_error_clear(e);
    if (receipt) memset(receipt, 0, sizeof(*receipt));
    if (!t || !count || count > UINT32_MAX || (!values && t->schema.field_count > 1))
        return wadb_fail(e, WADB_INVALID, 0, "table and nonempty row batch required");
    size_t row_bytes;
    if (!wadb_mul_size(count, t->schema.row_width, &row_bytes) || row_bytes > WADB_MAX_FRAME_BYTES - 96)
        return wadb_fail(e, WADB_LIMIT, 0, "append exceeds frame limit");
    wadb_append_request request = {.table = t, .values = values, .row_count = count,
        .row_bytes = row_bytes, .charge = row_bytes + sizeof(wadb_append_request), .started_ns = started};
    wadb_db *db = t->db;
    /* Reserve owned staging memory before allocation/validation, including
     * producers which are preparing and requests currently being committed. */
    pthread_mutex_lock(&db->mutex);
    if (db->stopping || !db->writer_started) {
        pthread_mutex_unlock(&db->mutex);
        return wadb_fail(e, WADB_CANCELLED, 0, "writer is stopping");
    }
    if (request.charge > db->options.queue_limit_bytes - db->queue_bytes) {
        ++db->rejected_appends;
        pthread_mutex_unlock(&db->mutex);
        return wadb_fail(e, WADB_BACKPRESSURE, 0, "ingestion queue byte limit reached");
    }
    db->queue_bytes += request.charge;
    pthread_mutex_unlock(&db->mutex);
    request.rows = malloc(row_bytes);
    wadb_status status = request.rows ? WADB_OK : wadb_fail(e, WADB_NOMEM, 0, "append staging buffer");
    for (size_t i = 0; !status && i < count; ++i) {
        const wadb_value *v = values ? values + i * (t->schema.field_count - 1) : NULL;
        status = wadb_encode_row(&t->schema, v, 0, validate_symbol, NULL,
            request.rows + i * t->schema.row_width, t->schema.row_width, e);
    }
    bool condition_initialized = false;
    if (!status) {
        int rc = pthread_cond_init(&request.ready, NULL);
        if (rc) status = wadb_fail(e, WADB_IO, rc, "append completion condition");
        else condition_initialized = true;
    }
    pthread_mutex_lock(&db->mutex);
    if (!status) {
        request.submitted_ns = wadb_monotonic_ns();
        if (db->queue_tail) db->queue_tail->next = &request;
        else db->queue_head = &request;
        db->queue_tail = &request;
        db->ready_bytes += row_bytes;
        pthread_cond_signal(&db->work);
        while (!request.done) pthread_cond_wait(&request.ready, &db->mutex);
        status = request.status;
        if (e) *e = request.error;
        if (!status && receipt) *receipt = request.receipt;
    } else {
        free(request.rows);
        db->queue_bytes -= request.charge;
        ++db->rejected_appends;
    }
    pthread_mutex_unlock(&db->mutex);
    if (condition_initialized) pthread_cond_destroy(&request.ready);
    return status;
}
