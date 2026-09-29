#include "query.h"
#include <stdlib.h>

typedef struct wadb_cursor_state {
    struct wadb_cursor_state *next;
    wadb_query_plan *plan;
    wadb_snapshot snapshot;
    uint64_t id, deadline, after, remaining_bytes;
    bool busy;
} cursor;
static void destroy(cursor *p) {
    wadb_snapshot_release(&p->snapshot); wadb_query_plan_free(p->plan); free(p);
}
void wadb_cursor_clear(wadb_db *db) {
    while (db->cursors) { cursor *p = db->cursors; db->cursors = p->next; destroy(p); }
}
void wadb_cursor_expire(wadb_db *db) {
    if (!db) return;
    uint64_t now = wadb_monotonic_ns();
    cursor *expired = NULL;
    pthread_mutex_lock(&db->mutex);
    for (cursor **link = &db->cursors; *link;) {
        cursor *p = *link;
        if (!p->busy && p->deadline <= now) { *link = p->next; p->next = expired; expired = p; }
        else link = &p->next;
    }
    pthread_mutex_unlock(&db->mutex);
    while (expired) { cursor *p = expired; expired = p->next; destroy(p); }
}
wadb_status wadb_cursor_open(wadb_table *t, const wadb_query_options *options,
    uint32_t ttl_ms, uint64_t *id, wadb_error *e) {
    wadb_error_clear(e);
    if (!t || !id || !ttl_ms || ttl_ms > 300000 || (options && options->aggregate_count))
        return wadb_fail(e, WADB_INVALID, 0, "scan cursor requires a table, output and TTL in 1..300000 ms");
    *id = 0;
    wadb_cursor_expire(t->db);
    cursor *p = calloc(1, sizeof(*p));
    if (!p) return wadb_fail(e, WADB_NOMEM, 0, "cursor state");
    wadb_status status = wadb_query_compile(t, options, &p->plan, e);
    if (!status) status = wadb_snapshot_acquire(t, &p->plan->options.scan, &p->snapshot, e);
    if (status) { destroy(p); return status; }
    p->deadline = wadb_monotonic_ns() + (uint64_t)ttl_ms * 1000000;
    p->after = p->plan->options.scan.after_sequence;
    p->remaining_bytes = p->plan->options.scan.max_scan_bytes;
    wadb_db *db = t->db;
    size_t charge = p->plan->allocated_bytes + sizeof(*p);
    pthread_mutex_lock(&db->mutex);
    if (db->next_cursor_id == UINT64_MAX || charge > db->options.snapshot_limit_bytes - db->snapshot_bytes)
        status = wadb_fail(e, WADB_BACKPRESSURE, 0, "cursor memory or IDs exhausted");
    else {
        p->snapshot.charge += charge; db->snapshot_bytes += charge;
        p->id = ++db->next_cursor_id; p->next = db->cursors; db->cursors = p; *id = p->id;
    }
    pthread_mutex_unlock(&db->mutex);
    if (status) destroy(p);
    return status;
}
static cursor **find(wadb_db *db, uint64_t id) {
    cursor **p = &db->cursors;
    while (*p && (*p)->id != id) p = &(*p)->next;
    return p;
}
wadb_status wadb_cursor_next(wadb_db *db, uint64_t id, wadb_result **out, wadb_error *e) {
    wadb_error_clear(e);
    if (!db || !out || !id) return wadb_fail(e, WADB_INVALID, 0, "database, cursor ID and result required");
    *out = NULL;
    wadb_cursor_expire(db);
    pthread_mutex_lock(&db->mutex);
    cursor *p = *find(db, id);
    if (!p || p->busy) {
        pthread_mutex_unlock(&db->mutex);
        return wadb_fail(e, p ? WADB_BACKPRESSURE : WADB_EXPIRED, 0, p ? "cursor is busy" : "cursor expired or closed");
    }
    p->busy = true;
    pthread_mutex_unlock(&db->mutex);
    uint64_t now = wadb_monotonic_ns();
    wadb_status status = WADB_OK;
    if (now >= p->deadline) status = wadb_fail(e, WADB_EXPIRED, 0, "cursor expired");
    else {
        uint64_t remaining_ms = (p->deadline - now + 999999) / 1000000;
        uint32_t timeout = p->plan->options.scan.timeout_ms;
        if (remaining_ms < timeout) timeout = (uint32_t)remaining_ms;
        status = wadb_query_execute(p->plan, &p->snapshot, p->after, p->remaining_bytes, timeout, out, e);
        if (wadb_monotonic_ns() >= p->deadline) {
            wadb_result_free(*out); *out = NULL;
            status = wadb_fail(e, WADB_EXPIRED, 0, "cursor expired during page read");
        }
    }
    bool finished = status || !(*out)->has_more;
    pthread_mutex_lock(&db->mutex);
    if (finished) *find(db, id) = p->next;
    else {
        p->after = (*out)->stats.last_sequence;
        p->remaining_bytes -= (*out)->stats.bytes_scanned;
        p->busy = false;
    }
    pthread_mutex_unlock(&db->mutex);
    if (finished) destroy(p);
    return status;
}
wadb_status wadb_cursor_close(wadb_db *db, uint64_t id, wadb_error *e) {
    wadb_error_clear(e);
    if (!db || !id) return wadb_fail(e, WADB_INVALID, 0, "database and cursor ID required");
    pthread_mutex_lock(&db->mutex);
    cursor **link = find(db, id), *p = *link;
    if (p && p->busy) { pthread_mutex_unlock(&db->mutex); return wadb_fail(e, WADB_BACKPRESSURE, 0, "cursor is busy"); }
    if (p) *link = p->next;
    pthread_mutex_unlock(&db->mutex);
    if (p) destroy(p);
    return WADB_OK;
}
