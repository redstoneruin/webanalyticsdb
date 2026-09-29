#include "../core/engine.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

void wadb_integrity_options_default(wadb_integrity_options *o) {
    if (o) *o = (wadb_integrity_options){.max_scan_bytes = UINT64_C(1024) * 1024 * 1024, .timeout_ms = 60000};
}
static wadb_status check_limits(const wadb_integrity_options *o, uint64_t deadline, wadb_error *e) {
    if (o->cancelled && o->cancelled(o->context)) return wadb_fail(e, WADB_CANCELLED, 0, "integrity check cancelled");
    if (wadb_monotonic_ns() >= deadline) return wadb_fail(e, WADB_LIMIT, 0, "integrity deadline exceeded");
    return WADB_OK;
}
static wadb_status check_segment(wadb_table *t, const wadb_segment_meta *s,
    const wadb_integrity_options *o, uint64_t deadline, wadb_integrity_stats *stats, wadb_error *e) {
    if (s->committed_bytes > o->max_scan_bytes - stats->bytes)
        return wadb_fail(e, WADB_LIMIT, 0, "integrity byte budget exceeded");
    char *path = wadb_segment_path(t, s, "seg");
    if (!path) return wadb_fail(e, WADB_NOMEM, 0, "integrity path");
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    free(path);
    if (fd < 0) return wadb_fail(e, WADB_IO, errno, "open segment for integrity check");
    unsigned char header[WADB_SEGMENT_HEADER_BYTES];
    wadb_segment_header h;
    wadb_dictionary *dictionary = NULL;
    wadb_status status = wadb_pread_all(fd, header, sizeof(header), 0, e);
    if (!status) { stats->bytes += sizeof(header); status = wadb_segment_decode(header, sizeof(header), &h, e); }
    if (!status && (h.table_id != t->id || h.segment_id != s->id || h.schema_hash != t->schema.fingerprint ||
        h.row_width != t->schema.row_width || h.first_sequence != s->first_sequence || h.day_start != s->day_start))
        status = wadb_fail(e, WADB_CORRUPT, 0, "integrity segment identity mismatch");
    struct stat st;
    if (!status && (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size < s->committed_bytes || (s->state == WADB_SEG_SEALED && (uint64_t)st.st_size != s->committed_bytes)))
        status = wadb_fail(e, WADB_CORRUPT, errno, "integrity file size contradicts snapshot");
    if (!status) status = wadb_dictionary_new(&t->schema, t->db->options.dictionary_limit_bytes, &dictionary, e);
    uint64_t offset = WADB_SEGMENT_HEADER_BYTES, last_sequence = s->first_sequence - 1;
    int64_t last_time = s->min_time;
    while (!status && offset < s->committed_bytes) {
        status = check_limits(o, deadline, e);
        if (status) break;
        if (s->committed_bytes - offset < WADB_FRAME_HEADER_BYTES) { status = wadb_fail(e, WADB_CORRUPT, 0, "partial committed frame"); break; }
        wadb_frame f;
        status = wadb_pread_all(fd, header, WADB_FRAME_HEADER_BYTES, offset, e);
        if (!status) status = wadb_frame_inspect(header, WADB_FRAME_HEADER_BYTES, &f, e);
        if (!status && f.frame_bytes > s->committed_bytes - offset) status = wadb_fail(e, WADB_CORRUPT, 0, "frame exceeds snapshot boundary");
        if (status) break;
        unsigned char *bytes = malloc(f.frame_bytes);
        if (!bytes) { status = wadb_fail(e, WADB_NOMEM, 0, "integrity frame"); break; }
        status = wadb_pread_all(fd, bytes, f.frame_bytes, offset, e);
        if (!status) { stats->bytes += f.frame_bytes; status = wadb_frame_decode(bytes, f.frame_bytes, &f, e); }
        if (!status && (f.schema_hash != t->schema.fingerprint || f.row_width != t->schema.row_width ||
            last_sequence == UINT64_MAX || f.first_sequence != last_sequence + 1 ||
            f.min_time < last_time || f.max_time > s->max_time ||
            f.min_time / WADB_DAY_US != s->day_start / WADB_DAY_US ||
            (offset == WADB_SEGMENT_HEADER_BYTES && f.min_time != s->min_time)))
            status = wadb_fail(e, WADB_CORRUPT, 0, "integrity frame contradicts segment sequence or time range");
        if (!status) status = wadb_dictionary_apply(dictionary, &f, e);
        for (uint32_t row = 0; !status && row < f.row_count; ++row) {
            if (!(row % 256)) status = check_limits(o, deadline, e);
            if (!status) status = wadb_validate_row(&t->schema, f.rows + (size_t)row * f.row_width,
                wadb_dictionary_resolve, dictionary, e);
        }
        if (!status) {
            ++stats->frames; stats->rows += f.row_count;
            last_sequence = f.first_sequence + f.row_count - 1; last_time = f.max_time; offset += f.frame_bytes;
        }
        free(bytes);
    }
    if (!status && (last_sequence != s->last_sequence || last_time != s->max_time))
        status = wadb_fail(e, WADB_CORRUPT, 0, "integrity segment endpoint mismatch");
    if (!status) ++stats->segments;
    wadb_dictionary_free(dictionary); close(fd);
    return status;
}
wadb_status wadb_integrity_check(wadb_table *t, const wadb_integrity_options *options,
    wadb_integrity_stats *stats, wadb_error *e) {
    wadb_error_clear(e);
    wadb_integrity_options o;
    wadb_integrity_options_default(&o); if (options) o = *options;
    wadb_integrity_stats result = {0};
    if (stats) *stats = result;
    if (!t || !o.max_scan_bytes || !o.timeout_ms || o.timeout_ms > 3600000)
        return wadb_fail(e, WADB_INVALID, 0, "table and valid integrity budgets required");
    wadb_cursor_expire(t->db);
    uint64_t deadline = wadb_monotonic_ns() + (uint64_t)o.timeout_ms * 1000000;
    wadb_snapshot snapshot;
    wadb_status status = wadb_snapshot_acquire(t, NULL, &snapshot, e);
    result.snapshot_sequence = snapshot.sequence;
    for (size_t i = 0; !status && i < snapshot.count; ++i) {
        if (i && (snapshot.segments[i].first_sequence != snapshot.segments[i - 1].last_sequence + 1 ||
            snapshot.segments[i].min_time < snapshot.segments[i - 1].max_time))
            status = wadb_fail(e, WADB_CORRUPT, 0, "integrity segment directory is unordered");
        if (!status) status = check_limits(&o, deadline, e);
        if (!status) status = check_segment(t, &snapshot.segments[i], &o, deadline, &result, e);
    }
    wadb_snapshot_release(&snapshot);
    if (status == WADB_CORRUPT) {
        pthread_mutex_lock(&t->db->mutex); t->read_only = true; pthread_mutex_unlock(&t->db->mutex);
    }
    if (stats) *stats = result;
    return status;
}
