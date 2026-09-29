#include "jobs.h"
#include "../core/engine.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    struct wadb_jobs *owner;
    wadb_job_info info;
    wadb_table *table;
    wadb_integrity_options check;
    pthread_t thread;
    bool joinable, cancelled;
} job;
struct wadb_jobs {
    wadb_db *db;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool stopping;
    uint64_t next_id;
    job slots[WADB_JOB_HISTORY];
};
static bool active(const job *j) { return j->info.id && j->info.state <= WADB_JOB_RUNNING; }
static uint64_t cpu_now(void) {
    struct timespec t;
    return clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) ? 0 : (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
}
static bool cancelled(void *context) {
    job *j = context; wadb_jobs *m = j->owner;
    pthread_mutex_lock(&m->mutex); bool stop = m->stopping || j->cancelled; pthread_mutex_unlock(&m->mutex);
    return stop;
}
static bool wait_until(job *j, uint64_t deadline) {
    wadb_jobs *m = j->owner;
    pthread_mutex_lock(&m->mutex);
    while (!m->stopping && !j->cancelled && wadb_monotonic_ns() < deadline)
        (void)wadb_condition_wait_until(&m->changed, &m->mutex, deadline);
    bool stop = m->stopping || j->cancelled;
    pthread_mutex_unlock(&m->mutex); return !stop;
}
static uint64_t random_next(uint64_t *state) {
    uint64_t z = (*state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}
typedef struct { wadb_value values[9]; unsigned char visitor[16]; char path[32], referrer[32]; } generated_row;
static void fill_batch(generated_row *rows, wadb_value *values, size_t n, uint64_t *seed, uint32_t cardinality) {
    static const char countries[][3] = {"US", "DE", "JP", "GB", "CA", "FR"};
    for (size_t i = 0; i < n; ++i) {
        generated_row *r = &rows[i]; uint64_t a = random_next(seed), b = random_next(seed);
        for (unsigned k = 0; k < 8; ++k) { r->visitor[k] = (unsigned char)(a >> (k * 8)); r->visitor[k + 8] = (unsigned char)(b >> (k * 8)); }
        int p = snprintf(r->path, sizeof(r->path), "/page/%" PRIu64, a % cardinality);
        int ref = snprintf(r->referrer, sizeof(r->referrer), "https://ref/%" PRIu64, b % cardinality);
        r->values[0] = (wadb_value){.as.u64 = a % 10 + 1};
        r->values[1] = (wadb_value){.as.bytes = {r->visitor, sizeof(r->visitor)}};
        r->values[2] = (wadb_value){.as.bytes = {r->path, (size_t)p}};
        r->values[3] = (wadb_value){.as.bytes = {r->referrer, (size_t)ref}};
        r->values[4] = (wadb_value){.as.u64 = b % 10000};
        r->values[5] = (wadb_value){.as.u64 = a % 20 ? 200 : 404};
        r->values[6] = (wadb_value){.as.bytes = {countries[b % 6], 2}};
        r->values[7] = (wadb_value){.as.u64 = a % 3};
        r->values[8] = (wadb_value){.as.u64 = b % 4};
        memcpy(values + i * 9, r->values, sizeof(r->values));
    }
}
static wadb_status generate(job *j, wadb_error *error, uint64_t cpu_start) {
    wadb_generator_options o = j->info.generator;
    /* Keep low-rate jobs responsive even when their requested batch is large. */
    uint32_t batch = o.rows_per_second / 10;
    if (!batch) batch = 1;
    if (batch > o.batch_rows) batch = o.batch_rows;
    generated_row *rows = calloc(batch, sizeof(*rows));
    wadb_value *values = calloc((size_t)batch * 9, sizeof(*values));
    if (!rows || !values) { free(rows); free(values); return wadb_fail(error, WADB_NOMEM, 0, "generator batch allocation"); }
    uint64_t target = (uint64_t)o.rows_per_second * o.duration_ms / 1000, committed = 0, seed = o.seed;
    uint64_t start = j->info.started_ns, end = start + (uint64_t)o.duration_ms * 1000000;
    wadb_status status = WADB_OK;
    while (committed < target && !cancelled(j)) {
        size_t count = target - committed < batch ? (size_t)(target - committed) : batch;
        uint64_t due = start + (committed + count) * 1000000000 / o.rows_per_second;
        if (!wait_until(j, due)) break;
        /* A saturated server may miss its offered rate. Do not extend the job
         * indefinitely to make reported throughput look like the target rate. */
        if (wadb_monotonic_ns() > end + UINT64_C(10000000)) break;
        fill_batch(rows, values, count, &seed, o.cardinality);
        wadb_append_receipt receipt;
        status = wadb_append_batch(j->table, values, count, &receipt, error);
        pthread_mutex_lock(&j->owner->mutex);
        j->info.attempted_rows += count;
        if (!status) {
            committed += count; j->info.committed_rows = committed; j->info.last_sequence = receipt.last_sequence;
        } else if (status == WADB_BACKPRESSURE) ++j->info.rejected_batches;
        j->info.cpu_ns = cpu_now() - cpu_start;
        pthread_mutex_unlock(&j->owner->mutex);
        if (status && status != WADB_BACKPRESSURE) break;
        if (status == WADB_BACKPRESSURE) {
            status = WADB_OK; memset(error, 0, sizeof(*error));
            if (!wait_until(j, wadb_monotonic_ns() + UINT64_C(10000000))) break;
        }
    }
    free(values); free(rows); return status;
}
static void *run(void *context) {
    job *j = context; wadb_jobs *m = j->owner; uint64_t cpu_start = cpu_now();
    pthread_mutex_lock(&m->mutex);
    j->info.state = WADB_JOB_RUNNING; j->info.started_ns = wadb_monotonic_ns();
    pthread_mutex_unlock(&m->mutex);
    wadb_error error = {0}; wadb_integrity_stats result = {0};
    wadb_status status = j->info.kind == WADB_JOB_GENERATOR ? generate(j, &error, cpu_start) :
        wadb_integrity_check(j->table, &j->check, &result, &error);
    pthread_mutex_lock(&m->mutex);
    j->info.finished_ns = wadb_monotonic_ns();
    j->info.elapsed_ms = (j->info.finished_ns - j->info.started_ns) / 1000000;
    j->info.cpu_ns = cpu_now() - cpu_start; j->info.integrity = result;
    bool stop = j->cancelled || m->stopping || status == WADB_CANCELLED;
    j->info.state = status && status != WADB_CANCELLED ? WADB_JOB_FAILED : stop ? WADB_JOB_STOPPED : WADB_JOB_COMPLETE;
    j->info.error = error;
    pthread_cond_broadcast(&m->changed); pthread_mutex_unlock(&m->mutex);
    return NULL;
}
wadb_status wadb_jobs_open(wadb_db *db, wadb_jobs **out, wadb_error *error) {
    if (!db || !out) return wadb_fail(error, WADB_INVALID, 0, "jobs require an open database");
    *out = NULL; wadb_jobs *m = calloc(1, sizeof(*m));
    if (!m) return wadb_fail(error, WADB_NOMEM, 0, "job manager");
    int rc = pthread_mutex_init(&m->mutex, NULL);
    if (rc) { free(m); return wadb_fail(error, WADB_IO, rc, "job mutex"); }
    rc = wadb_condition_init(&m->changed);
    if (rc) { pthread_mutex_destroy(&m->mutex); free(m); return wadb_fail(error, WADB_IO, rc, "job condition"); }
    m->db = db; m->next_id = 1; *out = m; return WADB_OK;
}
void wadb_jobs_close(wadb_jobs *m) {
    if (!m) return;
    pthread_mutex_lock(&m->mutex); m->stopping = true; pthread_cond_broadcast(&m->changed); pthread_mutex_unlock(&m->mutex);
    for (size_t i = 0; i < WADB_JOB_HISTORY; ++i) if (m->slots[i].joinable) pthread_join(m->slots[i].thread, NULL);
    pthread_cond_destroy(&m->changed); pthread_mutex_destroy(&m->mutex); free(m);
}
/* The mutex stays held from slot reservation through thread creation, keeping
 * close/list/start from observing a partially initialized job. Callers finish
 * concurrent API calls before closing the manager. */
static job *reserve(wadb_jobs *m, wadb_error *error) {
    unsigned running = 0; job *slot = NULL;
    for (size_t i = 0; i < WADB_JOB_HISTORY; ++i) {
        job *j = &m->slots[i];
        if (active(j)) ++running;
        else if (!slot || j->info.id < slot->info.id) slot = j;
    }
    if (m->stopping || running >= WADB_JOB_CONCURRENCY || !slot || !m->next_id) {
        wadb_fail(error, WADB_BACKPRESSURE, 0, "at most two jobs may run concurrently"); return NULL;
    }
    if (slot->joinable) pthread_join(slot->thread, NULL);
    memset(slot, 0, sizeof(*slot)); slot->owner = m; slot->info.id = m->next_id++; slot->info.state = WADB_JOB_STARTING;
    return slot;
}
static wadb_status start(job *j, uint64_t *id, wadb_error *error) {
    int rc = pthread_create(&j->thread, NULL, run, j);
    if (rc) {
        j->info.state = WADB_JOB_FAILED;
        wadb_fail(&j->info.error, WADB_IO, rc, "create job thread");
        if (error) *error = j->info.error;
        return WADB_IO;
    }
    j->joinable = true; *id = j->info.id; return WADB_OK;
}
wadb_status wadb_job_generate(wadb_jobs *m, const wadb_generator_options *o, uint64_t *id, wadb_error *error) {
    if (!m || !o || !id || !memchr(o->table_name, 0, sizeof(o->table_name)) ||
        (o->table_name[0] && !wadb_identifier(o->table_name))) return wadb_fail(error, WADB_INVALID, 0, "invalid generator arguments");
    uint64_t total = (uint64_t)o->rows_per_second * o->duration_ms / 1000;
    if (!o->rows_per_second || o->rows_per_second > 1000000 || !o->duration_ms || o->duration_ms > 300000 ||
        !o->batch_rows || o->batch_rows > 4096 || !o->cardinality || o->cardinality > 100000 || !total || total > 10000000)
        return wadb_fail(error, WADB_LIMIT, 0, "generator limits: 1M rows/s, 5 minutes, 4096 rows/batch, 100K symbols, 10M total rows");
    pthread_mutex_lock(&m->mutex); job *j = reserve(m, error);
    if (!j) { pthread_mutex_unlock(&m->mutex); return WADB_BACKPRESSURE; }
    j->info.kind = WADB_JOB_GENERATOR; j->info.generator = *o;
    if (!o->table_name[0]) snprintf(j->info.generator.table_name, WADB_NAME_CAP, "_test_%" PRIu64 "_%" PRId64, j->info.id, wadb_realtime_us());
    wadb_field_def fields[] = {
        {"site_id", WADB_U32, 0, false}, {"visitor_id", WADB_BYTES, 16, false}, {"path", WADB_SYMBOL32, 0, false},
        {"referrer", WADB_SYMBOL32, 0, false}, {"duration_ms", WADB_U32, 0, false}, {"status", WADB_U16, 0, false},
        {"country", WADB_BYTES, 2, false}, {"device", WADB_U8, 0, false}, {"flags", WADB_U8, 0, false}
    };
    wadb_status status = wadb_create_table(m->db, j->info.generator.table_name, fields, 9, 0, &j->table, error);
    if (!status) { j->info.table_id = wadb_table_id(j->table); status = start(j, id, error); }
    else { j->info.state = WADB_JOB_FAILED; if (error) j->info.error = *error; }
    pthread_mutex_unlock(&m->mutex); return status;
}
wadb_status wadb_job_integrity(wadb_jobs *m, wadb_table *table, uint64_t bytes, uint32_t timeout, uint64_t *id, wadb_error *error) {
    if (!m || !table || !id) return wadb_fail(error, WADB_INVALID, 0, "invalid integrity job arguments");
    if (!bytes || bytes > UINT64_C(64) * 1024 * 1024 * 1024 || !timeout || timeout > 300000)
        return wadb_fail(error, WADB_LIMIT, 0, "integrity job limits: 64 GiB and 5 minutes");
    pthread_mutex_lock(&m->mutex); job *j = reserve(m, error);
    if (!j) { pthread_mutex_unlock(&m->mutex); return WADB_BACKPRESSURE; }
    j->table = table; j->info.kind = WADB_JOB_INTEGRITY; j->info.table_id = wadb_table_id(table);
    j->check = (wadb_integrity_options){.max_scan_bytes = bytes, .timeout_ms = timeout, .cancelled = cancelled, .context = j};
    wadb_status status = start(j, id, error); pthread_mutex_unlock(&m->mutex); return status;
}
static void copy_info(const job *j, wadb_job_info *out) {
    *out = j->info;
    if (active(j) && out->started_ns) out->elapsed_ms = (wadb_monotonic_ns() - out->started_ns) / 1000000;
}
wadb_status wadb_job_get(wadb_jobs *m, uint64_t id, wadb_job_info *out, wadb_error *error) {
    if (!m || !out || !id) return wadb_fail(error, WADB_INVALID, 0, "invalid job lookup");
    pthread_mutex_lock(&m->mutex);
    for (size_t i = 0; i < WADB_JOB_HISTORY; ++i) if (m->slots[i].info.id == id) {
        copy_info(&m->slots[i], out); pthread_mutex_unlock(&m->mutex); return WADB_OK;
    }
    pthread_mutex_unlock(&m->mutex); return wadb_fail(error, WADB_NOT_FOUND, 0, "job is unknown or no longer retained");
}
wadb_status wadb_job_list(wadb_jobs *m, wadb_job_info *out, size_t capacity, size_t *count, wadb_error *error) {
    if (!m || !count || (!out && capacity)) return wadb_fail(error, WADB_INVALID, 0, "invalid job list");
    pthread_mutex_lock(&m->mutex); *count = 0;
    for (size_t i = 0; i < WADB_JOB_HISTORY; ++i) if (m->slots[i].info.id) {
        if (*count < capacity) copy_info(&m->slots[i], &out[*count]);
        ++*count;
    }
    pthread_mutex_unlock(&m->mutex);
    if (*count > capacity) return wadb_fail(error, WADB_LIMIT, 0, "job output capacity");
    /* Stable ID order despite ring-slot reuse. */
    for (size_t i = 1; i < *count; ++i) {
        wadb_job_info v = out[i]; size_t k = i;
        while (k && out[k - 1].id > v.id) { out[k] = out[k - 1]; --k; }
        out[k] = v;
    }
    return WADB_OK;
}
wadb_status wadb_job_stop(wadb_jobs *m, uint64_t id, wadb_error *error) {
    if (!m || !id) return wadb_fail(error, WADB_INVALID, 0, "invalid job stop");
    pthread_mutex_lock(&m->mutex);
    for (size_t i = 0; i < WADB_JOB_HISTORY; ++i) if (m->slots[i].info.id == id) {
        m->slots[i].cancelled = true; pthread_cond_broadcast(&m->changed); pthread_mutex_unlock(&m->mutex); return WADB_OK;
    }
    pthread_mutex_unlock(&m->mutex); return wadb_fail(error, WADB_NOT_FOUND, 0, "job is unknown or no longer retained");
}
