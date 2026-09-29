#include "../core/engine.h"
#include <stdlib.h>
#include <string.h>

wadb_status wadb_snapshot_acquire(wadb_table *t, const wadb_scan_options *o,
    wadb_snapshot *snapshot, wadb_error *e) {
    memset(snapshot, 0, sizeof(*snapshot));
    pthread_mutex_lock(&t->db->mutex);
    size_t lo = 0, hi = t->readable_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const wadb_segment_meta *s = &t->segments[t->readable_segments[mid]];
        if (o && (s->max_time < o->start_time || s->last_sequence <= o->after_sequence)) lo = mid + 1;
        else hi = mid;
    }
    size_t first = lo;
    hi = t->readable_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (!o || t->segments[t->readable_segments[mid]].min_time < o->end_time) lo = mid + 1;
        else hi = mid;
    }
    size_t count = o && o->start_time == o->end_time ? 0 : lo - first;
    size_t active_count = count && t->segments[t->readable_segments[lo - 1]].state == WADB_SEG_ACTIVE ? t->index.count : 0;
    size_t charge = sizeof(*snapshot) + count * sizeof(wadb_segment_meta) + active_count * sizeof(wadb_index_entry);
    if (t->db->reader_count >= t->db->options.max_readers || charge > t->db->options.snapshot_limit_bytes - t->db->snapshot_bytes) {
        pthread_mutex_unlock(&t->db->mutex);
        return wadb_fail(e, WADB_BACKPRESSURE, 0, "reader snapshot budget exhausted");
    }
    wadb_segment_meta *segments = count ? malloc(count * sizeof(*segments)) : NULL;
    wadb_frame_index active = {0};
    if (segments) for (size_t i = 0; i < count; ++i) segments[i] = t->segments[t->readable_segments[first + i]];
    if (count && segments && segments[count - 1].state == WADB_SEG_ACTIVE) {
        active.count = t->index.count;
        active.entries = active.count ? malloc(active.count * sizeof(*active.entries)) : NULL;
        if (active.entries) memcpy(active.entries, t->index.entries, active.count * sizeof(*active.entries));
    }
    if ((count && !segments) || (active.count && !active.entries)) {
        pthread_mutex_unlock(&t->db->mutex); free(segments); wadb_index_free(&active);
        return wadb_fail(e, WADB_NOMEM, 0, "query snapshot metadata");
    }
    *snapshot = (wadb_snapshot){.table = t, .segments = segments, .count = count,
        .charge = charge, .active = active, .sequence = t->last_sequence};
    t->db->snapshot_bytes += charge;
    ++t->db->reader_count;
    ++t->readers;
    pthread_mutex_unlock(&t->db->mutex);
    return WADB_OK;
}
void wadb_snapshot_release(wadb_snapshot *snapshot) {
    if (!snapshot->table) return;
    wadb_table *t = snapshot->table;
    pthread_mutex_lock(&t->db->mutex);
    --t->readers; --t->db->reader_count;
    t->db->snapshot_bytes -= snapshot->charge;
    bool cleanup = !t->readers && t->pending_retention;
    pthread_mutex_unlock(&t->db->mutex);
    free(snapshot->segments); wadb_index_free(&snapshot->active);
    memset(snapshot, 0, sizeof(*snapshot));
    if (cleanup) (void)wadb_retention_cleanup(t, NULL, NULL);
}
