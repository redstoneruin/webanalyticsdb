#include "../core/engine.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void wadb_scan_options_default(wadb_scan_options *o) {
    if (!o) return;
    *o = (wadb_scan_options){.start_time = 0, .end_time = INT64_MAX,
        .max_scan_bytes = UINT64_C(64) * 1024 * 1024, .limit = 10000, .timeout_ms = 30000};
}

static wadb_status check_header(wadb_table *t, const wadb_segment_meta *s, int fd, wadb_error *e) {
    unsigned char bytes[WADB_SEGMENT_HEADER_BYTES];
    wadb_segment_header h;
    wadb_status status = wadb_pread_all(fd, bytes, sizeof(bytes), 0, e);
    if (!status) status = wadb_segment_decode(bytes, sizeof(bytes), &h, e);
    if (!status && (h.table_id != t->id || h.segment_id != s->id || h.schema_hash != t->schema.fingerprint ||
        h.row_width != t->schema.row_width || h.first_sequence != s->first_sequence || h.day_start != s->day_start))
        status = wadb_fail(e, WADB_CORRUPT, 0, "snapshot segment identity mismatch");
    return status;
}

static wadb_status active_dictionary(wadb_table *t, int fd, const wadb_frame_index *index,
    uint64_t deadline, wadb_dictionary **out, wadb_error *e) {
    wadb_status status = wadb_dictionary_new(&t->schema, t->db->options.dictionary_limit_bytes, out, e);
    for (size_t i = 0; !status && i < index->count; ++i) {
        const wadb_index_entry *v = &index->entries[i];
        if (!v->dictionary_count) continue;
        if (wadb_monotonic_ns() >= deadline) { status = wadb_fail(e, WADB_LIMIT, 0, "dictionary scan deadline exceeded"); break; }
        uint32_t n = v->rows_offset - WADB_FRAME_HEADER_BYTES;
        unsigned char *p = malloc(n);
        if (!p) { status = wadb_fail(e, WADB_NOMEM, 0, "active dictionary buffer"); break; }
        status = wadb_pread_all(fd, p, n, v->frame_offset + WADB_FRAME_HEADER_BYTES, e);
        if (!status && wadb_crc32c(p, n) != v->dictionary_crc)
            status = wadb_fail(e, WADB_CORRUPT, 0, "committed dictionary checksum mismatch");
        if (!status) {
            wadb_frame frame = {.dictionary = p, .dictionary_bytes = n, .dictionary_count = v->dictionary_count};
            status = wadb_dictionary_apply(*out, &frame, e);
        }
        free(p);
    }
    if (status) { wadb_dictionary_free(*out); *out = NULL; }
    return status;
}

static wadb_status account_rebuild(wadb_index_reader *r, wadb_scan_stats *stats,
    uint64_t *accounted, uint64_t budget, wadb_error *e) {
    uint64_t extra = r->rebuilt_bytes - *accounted;
    if (extra > budget - stats->bytes_scanned) return wadb_fail(e, WADB_LIMIT, 0, "index rebuild exceeds scan budget");
    stats->bytes_scanned += extra; stats->rebuild_bytes += extra;
    *accounted = r->rebuilt_bytes;
    r->rebuild_budget = budget - stats->bytes_scanned;
    return WADB_OK;
}

wadb_status wadb_scan(wadb_table *t, const wadb_scan_options *options,
    wadb_row_fn consume, void *context, wadb_scan_stats *stats, wadb_error *e) {
    wadb_error_clear(e);
    if (!t || !consume) return wadb_fail(e, WADB_INVALID, 0, "table and row callback required");
    wadb_scan_options o;
    wadb_scan_options_default(&o);
    if (options) o = *options;
    if (o.end_time < o.start_time || !o.limit || !o.max_scan_bytes || !o.timeout_ms || o.timeout_ms > 3600000)
        return wadb_fail(e, WADB_INVALID, 0, "invalid scan interval or resource budget");
    wadb_scan_stats result = {0};
    if (stats) *stats = result;
    wadb_cursor_expire(t->db);
    uint64_t started = wadb_monotonic_ns();
    wadb_snapshot snapshot;
    wadb_status status = wadb_snapshot_acquire(t, &o, &snapshot, e);
    if (!status) status = wadb_scan_snapshot(&snapshot, &o, consume, context, &result, e);
    wadb_snapshot_release(&snapshot);
    wadb_record_query(t->db, started, status, &result);
    if (stats) *stats = result;
    return status;
}

wadb_status wadb_scan_snapshot(const wadb_snapshot *snapshot, const wadb_scan_options *options,
    wadb_row_fn consume, void *context, wadb_scan_stats *stats, wadb_error *e) {
    wadb_table *t = snapshot->table;
    wadb_scan_options o = *options;
    wadb_scan_stats result = {.snapshot_sequence = snapshot->sequence};
    uint64_t deadline = wadb_monotonic_ns() + (uint64_t)o.timeout_ms * 1000000;
    wadb_status status = WADB_OK;
    bool stop = false, callback_failed = false;
    size_t first = 0, upper = snapshot->count;
    while (first < upper) {
        size_t mid = first + (upper - first) / 2;
        const wadb_segment_meta *s = &snapshot->segments[mid];
        if (s->last_sequence <= o.after_sequence || s->max_time < o.start_time) first = mid + 1;
        else upper = mid;
    }
    for (size_t i = first; i < snapshot->count && !stop; ++i) {
        if (wadb_monotonic_ns() >= deadline) { status = wadb_fail(e, WADB_LIMIT, 0, "scan deadline exceeded"); break; }
        const wadb_segment_meta *s = &snapshot->segments[i];
        if (s->last_sequence <= o.after_sequence || s->max_time < o.start_time || s->min_time >= o.end_time) continue;
        char *path = wadb_segment_path(t, s, "seg");
        if (!path) { status = wadb_fail(e, WADB_NOMEM, 0, "scan path"); break; }
        int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        free(path);
        if (fd < 0) { status = wadb_fail(e, WADB_IO, errno, "open snapshot segment"); break; }
        status = check_header(t, s, fd, e);
        wadb_index_reader reader = {.table = t, .segment = *s, .fd = -1, .page_number = UINT64_MAX};
        wadb_dictionary *dictionary = NULL;
        wadb_cache_entry *cached = NULL;
        uint64_t rebuilt = 0;
        if (!status && s->state == WADB_SEG_ACTIVE) {
            reader.memory = snapshot->active.entries; reader.count = snapshot->active.count;
            for (size_t j = 0; j < snapshot->active.count; ++j)
                reader.dictionary_bytes += snapshot->active.entries[j].rows_offset - WADB_FRAME_HEADER_BYTES;
        } else if (!status) status = wadb_index_open_bounded(t, s, &reader,
            o.max_scan_bytes - result.bytes_scanned, deadline, e);
        if (!status) status = account_rebuild(&reader, &result, &rebuilt, o.max_scan_bytes, e);
        if (!status && reader.dictionary_bytes > o.max_scan_bytes - result.bytes_scanned)
            status = wadb_fail(e, WADB_LIMIT, 0, "dictionary exceeds scan byte budget");
        if (!status) {
            result.dictionary_bytes += reader.dictionary_bytes;
            result.bytes_scanned += reader.dictionary_bytes;
            reader.rebuild_budget = o.max_scan_bytes - result.bytes_scanned;
            if (s->state == WADB_SEG_ACTIVE) status = active_dictionary(t, fd, &snapshot->active, deadline, &dictionary, e);
            else {
                status = wadb_cache_acquire(&reader, &cached, e);
                if (!status) dictionary = cached->dictionary;
            }
        }
        if (!status) status = account_rebuild(&reader, &result, &rebuilt, o.max_scan_bytes, e);
        size_t position = 0;
        if (!status) status = wadb_index_lower_bound(&reader, o.start_time, o.after_sequence, &position, e);
        if (!status) status = account_rebuild(&reader, &result, &rebuilt, o.max_scan_bytes, e);
        ++result.segments_scanned;
        for (; !status && position < reader.count && !stop; ++position) {
            if (wadb_monotonic_ns() >= deadline) { status = wadb_fail(e, WADB_LIMIT, 0, "scan deadline exceeded"); break; }
            wadb_index_entry entry;
            reader.rebuild_budget = o.max_scan_bytes - result.bytes_scanned;
            status = wadb_index_get(&reader, position, &entry, e);
            if (!status) status = account_rebuild(&reader, &result, &rebuilt, o.max_scan_bytes, e);
            if (status || entry.min_time >= o.end_time) break;
            if (entry.frame_bytes > o.max_scan_bytes - result.bytes_scanned) {
                status = wadb_fail(e, WADB_LIMIT, 0, "scan byte budget exceeded"); break;
            }
            result.bytes_scanned += entry.frame_bytes;
            unsigned char *bytes = malloc(entry.frame_bytes);
            if (!bytes) { status = wadb_fail(e, WADB_NOMEM, 0, "scan frame"); break; }
            wadb_frame frame;
            status = wadb_pread_all(fd, bytes, entry.frame_bytes, entry.frame_offset, e);
            if (!status) status = wadb_frame_decode(bytes, entry.frame_bytes, &frame, e);
            if (!status && (frame.schema_hash != t->schema.fingerprint || frame.row_width != t->schema.row_width ||
                frame.first_sequence != entry.first_sequence || frame.row_count != entry.row_count ||
                frame.min_time != entry.min_time || frame.max_time != entry.max_time ||
                wadb_get_u32(bytes + entry.frame_bytes - 4) != entry.frame_crc))
                status = wadb_fail(e, WADB_CORRUPT, 0, "frame contradicts index");
            if (!status) ++result.frames_scanned;
            for (uint32_t row = 0; !status && row < frame.row_count; ++row) {
                if (!(row % 256) && wadb_monotonic_ns() >= deadline) { status = wadb_fail(e, WADB_LIMIT, 0, "scan deadline exceeded"); break; }
                uint64_t sequence = frame.first_sequence + row;
                const unsigned char *p = frame.rows + (size_t)row * frame.row_width;
                int64_t time = wadb_get_i64(p);
                ++result.rows_scanned;
                if (time < o.start_time || time >= o.end_time || sequence <= o.after_sequence || sequence > result.snapshot_sequence) continue;
                wadb_value values[WADB_MAX_FIELDS];
                status = wadb_validate_row(&t->schema, p, wadb_dictionary_resolve, dictionary, e);
                if (status) break;
                for (uint32_t field = 0; field < t->schema.field_count; ++field) {
                    status = wadb_decode_value(&t->schema, field, p, wadb_dictionary_resolve, dictionary, &values[field], e);
                    if (status) break;
                }
                if (status) break;
                status = consume(context, sequence, values, t->schema.field_count);
                if (status) { callback_failed = true; wadb_fail(e, status, 0, "scan stopped by callback"); break; }
                ++result.rows_returned; result.last_sequence = sequence;
                if (result.rows_returned == o.limit) { result.limit_reached = true; stop = true; break; }
            }
            free(bytes);
        }
        result.index_bytes += reader.io_bytes - reader.dictionary_io_bytes;
        if (cached) wadb_cache_release(t->db, cached); else wadb_dictionary_free(dictionary);
        wadb_index_close(&reader); close(fd);
        if (status) break;
    }
    if (stats) *stats = result;
    if (status == WADB_CORRUPT && !callback_failed) {
        pthread_mutex_lock(&t->db->mutex); t->read_only = true; pthread_mutex_unlock(&t->db->mutex);
    }
    return status;
}
