#include "engine.h"
#include <string.h>
#include <sys/statvfs.h>

void wadb_histogram_record(wadb_histogram *h, uint64_t ns) {
    uint64_t us = ns / 1000 + (ns % 1000 != 0), bound = us;
    unsigned bin = 0;
    while (bound > 1) { bound = bound / 2 + (bound % 2 != 0); ++bin; }
    if (h->count == UINT64_MAX || h->bins[bin] == UINT64_MAX) return;
    ++h->count; ++h->bins[bin];
    if (us > h->max_us) h->max_us = us;
}
static uint64_t percentile(const wadb_histogram *h, unsigned percent) {
    if (!h->count) return 0;
    uint64_t target = (h->count / 100) * percent + ((h->count % 100) * percent + 99) / 100, seen = 0;
    for (unsigned i = 0; i < 64; ++i) { seen += h->bins[i]; if (seen >= target) return UINT64_C(1) << i; }
    return UINT64_MAX;
}
static wadb_latency_stats summarize(const wadb_histogram *h) {
    return (wadb_latency_stats){h->count, percentile(h, 50), percentile(h, 95), percentile(h, 99), h->max_us};
}
void wadb_record_query(wadb_db *db, uint64_t start, wadb_status status, const wadb_scan_stats *stats) {
    pthread_mutex_lock(&db->mutex);
    ++db->queries;
    if (status) ++db->failed_queries;
    if (stats) { db->rows_scanned += stats->rows_scanned; db->bytes_scanned += stats->bytes_scanned; }
    wadb_histogram_record(&db->query_latency, wadb_monotonic_ns() - start);
    pthread_mutex_unlock(&db->mutex);
}
wadb_status wadb_get_stats(wadb_db *db, wadb_stats *out, wadb_error *e) {
    wadb_error_clear(e);
    if (!db || !out) return wadb_fail(e, WADB_INVALID, 0, "database and statistics output required");
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&db->mutex);
    out->uptime_us = (wadb_monotonic_ns() - db->boot_ns) / 1000;
    out->table_count = db->table_count; out->queue_bytes = db->queue_bytes; out->queue_limit_bytes = db->options.queue_limit_bytes;
    out->cache_bytes = db->cache_bytes; out->cache_hits = db->cache_hits; out->cache_misses = db->cache_misses;
    out->readers = db->reader_count; out->snapshot_bytes = db->snapshot_bytes;
    out->committed_rows = db->committed_rows; out->committed_bytes = db->committed_bytes;
    out->committed_frames = db->committed_frames; out->append_requests = db->append_requests;
    out->rejected_appends = db->rejected_appends; out->failed_appends = db->failed_appends;
    out->clock_adjustments = db->clock_adjustments;
    out->queries = db->queries; out->failed_queries = db->failed_queries;
    out->rows_scanned = db->rows_scanned; out->bytes_scanned = db->bytes_scanned;
    out->latest_notification = db->latest_notification;
    out->read_only = db->read_only;
    for (size_t i = 0; i < db->table_count; ++i) out->read_only |= db->tables[i]->read_only;
    out->append_latency = summarize(&db->append_latency); out->sync_latency = summarize(&db->sync_latency);
    out->query_latency = summarize(&db->query_latency);
    pthread_mutex_unlock(&db->mutex);
    struct statvfs disk;
    if (!statvfs(db->directory, &disk) && (!disk.f_frsize || (uint64_t)disk.f_bavail <= UINT64_MAX / disk.f_frsize)) {
        out->disk_free_available = true; out->disk_free_bytes = (uint64_t)disk.f_bavail * disk.f_frsize;
    }
    return WADB_OK;
}
wadb_status wadb_get_table_stats(wadb_table *t, wadb_table_stats *out, wadb_error *e) {
    wadb_error_clear(e);
    if (!t || !out) return wadb_fail(e, WADB_INVALID, 0, "table and statistics output required");
    pthread_mutex_lock(&t->db->mutex);
    *out = (wadb_table_stats){.id = t->id, .retention_us = t->retention_us,
        .rows = t->retained_rows, .bytes = t->retained_bytes, .segments = t->retained_segments,
        .retired_segments = t->retired_segments, .last_sequence = t->last_sequence,
        .dictionary_bytes = t->published_dictionary_bytes, .active_index_bytes = t->index.capacity * sizeof(wadb_index_entry),
        .last_assigned_time = t->last_time, .row_width = t->schema.row_width,
        .read_only = t->read_only || t->db->read_only, .pending_retention = t->pending_retention,
        .maintenance_status = t->maintenance_status};
    if (t->readable_count) {
        out->min_time = t->segments[t->readable_segments[0]].min_time;
        out->max_time = t->segments[t->readable_segments[t->readable_count - 1]].max_time;
    }
    pthread_mutex_unlock(&t->db->mutex);
    return WADB_OK;
}
wadb_status wadb_read_notifications(wadb_db *db, uint64_t after, wadb_commit_event *events,
    size_t capacity, size_t *count, uint64_t *latest, bool *gap, wadb_error *e) {
    wadb_error_clear(e);
    if (!db || !count || !latest || !gap || capacity > WADB_NOTIFICATION_CAPACITY || (capacity && !events))
        return wadb_fail(e, WADB_INVALID, 0, "invalid notification output or capacity");
    pthread_mutex_lock(&db->mutex);
    *count = 0; *latest = db->latest_notification; *gap = after > *latest;
    uint64_t oldest = *latest >= WADB_NOTIFICATION_CAPACITY ? *latest - WADB_NOTIFICATION_CAPACITY + 1 : 1;
    uint64_t first = after < *latest ? after + 1 : 0;
    if (first && first < oldest) { *gap = true; first = oldest; }
    if (first) {
        uint64_t available = *latest - first + 1;
        size_t n = available < capacity ? (size_t)available : capacity;
        for (size_t i = 0; i < n; ++i) events[i] = db->notifications[(first + i - 1) % WADB_NOTIFICATION_CAPACITY];
        *count = n;
    }
    pthread_mutex_unlock(&db->mutex);
    return WADB_OK;
}

wadb_status wadb_list_segments(wadb_table *t, uint64_t after, wadb_segment_info *segments,
    size_t capacity, size_t *count, bool *more, wadb_error *e) {
    wadb_error_clear(e);
    if (!t || !segments || !capacity || capacity > 1024 || !count || !more)
        return wadb_fail(e, WADB_INVALID, 0, "invalid segment page output or capacity");
    pthread_mutex_lock(&t->db->mutex);
    size_t lo = 0, hi = t->segment_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (t->segments[mid].id <= after) lo = mid + 1;
        else hi = mid;
    }
    size_t n = t->segment_count - lo;
    *more = n > capacity;
    if (n > capacity) n = capacity;
    if (n) memcpy(segments, t->segments + lo, n * sizeof(*segments));
    *count = n;
    pthread_mutex_unlock(&t->db->mutex);
    return WADB_OK;
}
