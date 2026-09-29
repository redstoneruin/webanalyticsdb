#include "../core/engine.h"
#include "../platform/fault.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Caller holds writer_mutex. Pins are table-wide; new snapshots omit retired
 * segments. No current snapshot can start referencing them during deletion. */
static wadb_status cleanup(wadb_table *t, uint64_t *deleted, wadb_error *e) {
    if (deleted) *deleted = 0;
    pthread_mutex_lock(&t->db->mutex);
    bool allowed = t->pending_retention && !t->readers;
    pthread_mutex_unlock(&t->db->mutex);
    if (!allowed) return WADB_OK;
    wadb_status status = WADB_OK;
    for (size_t i = 0; i < t->segment_count && !status; ++i) {
        if (t->segments[i].state != WADB_SEG_RETIRED) continue;
        bool removed = false;
        for (unsigned ext = 0; ext < 2 && !status; ++ext) {
            char *path = wadb_segment_path(t, &t->segments[i], ext ? "idx" : "seg");
            if (!path) { status = wadb_fail(e, WADB_NOMEM, 0, "retention path"); break; }
            if (!WADB_IO_CALL("retention.unlink", path, unlink(path))) { if (!ext) removed = true; }
            else if (errno != ENOENT) status = wadb_fail(e, WADB_IO, errno, "remove retired segment file");
            if (!status && ext) {
                char *slash = strrchr(path, '/'); *slash = 0;
                status = wadb_sync_directory(path, e);
            }
            free(path);
        }
        if (removed && deleted) ++*deleted;
    }
    pthread_mutex_lock(&t->db->mutex);
    t->pending_retention = status != WADB_OK; t->maintenance_status = status;
    pthread_mutex_unlock(&t->db->mutex);
    return status;
}
wadb_status wadb_retention_cleanup(wadb_table *t, uint64_t *deleted, wadb_error *e) {
    pthread_mutex_lock(&t->db->writer_mutex);
    wadb_status status = cleanup(t, deleted, e);
    pthread_mutex_unlock(&t->db->writer_mutex);
    return status;
}
static void protect_uncertain(wadb_table *t, wadb_status status) {
    if (status != WADB_INDETERMINATE) return;
    pthread_mutex_lock(&t->db->mutex); t->read_only = true; pthread_mutex_unlock(&t->db->mutex);
}
wadb_status wadb_set_retention(wadb_table *t, uint64_t duration, wadb_error *e) {
    wadb_error_clear(e);
    if (!t || duration > INT64_MAX) return wadb_fail(e, WADB_INVALID, 0, "invalid retention duration");
    pthread_mutex_lock(&t->db->writer_mutex);
    pthread_mutex_lock(&t->db->mutex);
    wadb_table staged = *t;
    bool read_only = t->read_only || t->db->read_only;
    pthread_mutex_unlock(&t->db->mutex);
    wadb_status status = WADB_OK;
    if (read_only) status = wadb_fail(e, WADB_READ_ONLY, 0, "metadata requires recovery");
    else if (t->manifest_generation == UINT64_MAX) status = wadb_fail(e, WADB_LIMIT, 0, "manifest generations exhausted");
    else {
        staged.retention_us = duration; ++staged.manifest_generation;
        status = wadb_save_manifest(&staged, e);
        if (!status) {
            pthread_mutex_lock(&t->db->mutex);
            t->retention_us = duration; t->manifest_generation = staged.manifest_generation;
            pthread_mutex_unlock(&t->db->mutex);
        }
    }
    protect_uncertain(t, status);
    pthread_mutex_unlock(&t->db->writer_mutex);
    return status;
}
static void preview(wadb_table *t, int64_t now, wadb_retention_stats *out) {
    memset(out, 0, sizeof(*out));
    out->cutoff = t->retention_us && (uint64_t)now >= t->retention_us ? now - (int64_t)t->retention_us : 0;
    for (size_t i = 0; i < t->segment_count; ++i) {
        const wadb_segment_meta *s = &t->segments[i];
        if (s->state == WADB_SEG_RETIRED) { if (t->pending_retention) ++out->pending_segments; continue; }
        if (!s->last_sequence || s->min_time >= out->cutoff) continue;
        if (s->max_time >= out->cutoff) ++out->partially_expired_segments;
        else {
            ++out->eligible_segments; out->eligible_rows += s->last_sequence - s->first_sequence + 1;
            out->eligible_bytes += s->committed_bytes;
        }
    }
}
wadb_status wadb_retention_preview(wadb_table *t, int64_t now, wadb_retention_stats *out, wadb_error *e) {
    wadb_error_clear(e);
    if (!t || !out || now < 0) return wadb_fail(e, WADB_INVALID, 0, "table, nonnegative time and preview output required");
    pthread_mutex_lock(&t->db->mutex); preview(t, now, out); pthread_mutex_unlock(&t->db->mutex);
    return WADB_OK;
}
wadb_status wadb_retention_apply(wadb_table *t, int64_t now, wadb_retention_stats *out, wadb_error *e) {
    wadb_error_clear(e);
    if (!t || now < 0) return wadb_fail(e, WADB_INVALID, 0, "table and nonnegative retention time required");
    wadb_cursor_expire(t->db);
    pthread_mutex_lock(&t->db->writer_mutex);
    pthread_mutex_lock(&t->db->mutex);
    wadb_retention_stats stats;
    preview(t, now, &stats);
    wadb_table staged = *t;
    bool read_only = t->read_only || t->db->read_only;
    pthread_mutex_unlock(&t->db->mutex);
    wadb_status status = WADB_OK;
    wadb_segment_meta *segments = NULL;
    if (read_only) status = wadb_fail(e, WADB_READ_ONLY, 0, "retention requires recovered metadata");
    else if (stats.eligible_segments) {
        if (t->manifest_generation == UINT64_MAX) status = wadb_fail(e, WADB_LIMIT, 0, "manifest generations exhausted");
        else if (!(segments = malloc(t->segment_count * sizeof(*segments)))) status = wadb_fail(e, WADB_NOMEM, 0, "retention manifest");
        if (!status) {
            memcpy(segments, t->segments, t->segment_count * sizeof(*segments));
            staged.segments = segments; ++staged.manifest_generation;
            for (size_t i = 0; i < t->segment_count; ++i) {
                wadb_segment_meta *s = &segments[i];
                if (s->state == WADB_SEG_RETIRED || !s->last_sequence || s->max_time >= stats.cutoff) continue;
                s->state = WADB_SEG_RETIRED;
                staged.preserved_sequence = s->last_sequence; staged.preserved_time = s->max_time;
            }
            status = wadb_save_manifest(&staged, e);
            if (!status) {
                pthread_mutex_lock(&t->db->mutex);
                free(t->segments); t->segments = segments; segments = NULL;
                t->manifest_generation = staged.manifest_generation;
                t->preserved_sequence = staged.preserved_sequence; t->preserved_time = staged.preserved_time;
                t->pending_retention = true;
                t->retained_rows -= stats.eligible_rows; t->retained_bytes -= stats.eligible_bytes;
                t->retained_segments -= stats.eligible_segments; t->retired_segments += stats.eligible_segments;
                size_t retained = 0;
                for (size_t i = 0; i < t->readable_count; ++i) {
                    size_t pos = t->readable_segments[i];
                    if (t->segments[pos].state != WADB_SEG_RETIRED) t->readable_segments[retained++] = pos;
                }
                t->readable_count = retained;
                if (t->segments[t->segment_count - 1].state == WADB_SEG_RETIRED) {
                    if (t->active_fd >= 0) close(t->active_fd);
                    t->active_fd = -1; wadb_index_free(&t->index);
                    t->published_dictionary_bytes = 0;
                    wadb_dictionary_free(t->dictionary); t->dictionary = NULL;
                }
                pthread_mutex_unlock(&t->db->mutex);
            }
        }
    }
    protect_uncertain(t, status);
    free(segments);
    if (!status) status = cleanup(t, &stats.deleted_segments, e);
    pthread_mutex_lock(&t->db->mutex);
    stats.pending_segments = 0;
    if (t->pending_retention) for (size_t i = 0; i < t->segment_count; ++i)
        if (t->segments[i].state == WADB_SEG_RETIRED) ++stats.pending_segments;
    pthread_mutex_unlock(&t->db->mutex);
    pthread_mutex_unlock(&t->db->writer_mutex);
    if (out) *out = stats;
    return status;
}
