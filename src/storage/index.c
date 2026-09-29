#include "../core/engine.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void wadb_index_free(wadb_frame_index *index) {
    free(index->entries);
    memset(index, 0, sizeof(*index));
}
wadb_status wadb_index_reserve(wadb_frame_index *index, wadb_error *e) {
    if (index->count >= WADB_MAX_INDEX_ENTRIES) return wadb_fail(e, WADB_LIMIT, 0, "active index is full");
    if (index->count < index->capacity) return WADB_OK;
    size_t cap = index->capacity ? index->capacity * 2 : 64;
    if (cap > WADB_MAX_INDEX_ENTRIES) cap = WADB_MAX_INDEX_ENTRIES;
    wadb_index_entry *entries = realloc(index->entries, cap * sizeof(*entries));
    if (!entries) return wadb_fail(e, WADB_NOMEM, 0, "active frame index");
    index->entries = entries; index->capacity = cap;
    return WADB_OK;
}
wadb_index_entry wadb_index_for_frame(const wadb_frame *f, const unsigned char *p, uint64_t offset) {
    uint32_t n = wadb_get_u32(p + 8);
    return (wadb_index_entry){.min_time = f->min_time, .max_time = f->max_time,
        .first_sequence = f->first_sequence, .frame_offset = offset, .row_count = f->row_count,
        .dictionary_count = f->dictionary_count, .frame_bytes = n,
        .rows_offset = WADB_FRAME_HEADER_BYTES + f->dictionary_bytes,
        .frame_crc = wadb_get_u32(p + n - 4),
        .dictionary_crc = wadb_crc32c(p + WADB_FRAME_HEADER_BYTES, f->dictionary_bytes)};
}
static void entry_encode(unsigned char *p, const wadb_index_entry *v) {
    memset(p, 0, WADB_INDEX_ENTRY_BYTES);
    wadb_put_u64(p, (uint64_t)v->min_time); wadb_put_u64(p + 8, (uint64_t)v->max_time);
    wadb_put_u64(p + 16, v->first_sequence); wadb_put_u32(p + 24, v->row_count);
    wadb_put_u32(p + 28, v->dictionary_count); wadb_put_u64(p + 32, v->frame_offset);
    wadb_put_u32(p + 40, v->frame_bytes); wadb_put_u32(p + 44, v->rows_offset);
    wadb_put_u32(p + 48, v->frame_crc); wadb_put_u32(p + 52, v->dictionary_crc);
    wadb_put_u32(p + 60, wadb_crc32c(p, 60));
}
static wadb_status entry_decode(wadb_index_reader *r, const unsigned char *p,
    wadb_index_entry *out, wadb_error *e) {
    if (wadb_get_u32(p + 56) || wadb_get_u32(p + 60) != wadb_crc32c(p, 60))
        return wadb_fail(e, WADB_CORRUPT, 0, "index record checksum mismatch");
    wadb_index_entry v = {.min_time = wadb_get_i64(p), .max_time = wadb_get_i64(p + 8),
        .first_sequence = wadb_get_u64(p + 16), .row_count = wadb_get_u32(p + 24),
        .dictionary_count = wadb_get_u32(p + 28), .frame_offset = wadb_get_u64(p + 32),
        .frame_bytes = wadb_get_u32(p + 40), .rows_offset = wadb_get_u32(p + 44),
        .frame_crc = wadb_get_u32(p + 48), .dictionary_crc = wadb_get_u32(p + 52)};
    if (!v.row_count || !v.first_sequence || v.first_sequence < r->segment.first_sequence ||
        v.row_count - 1 > UINT64_MAX - v.first_sequence ||
        v.first_sequence + v.row_count - 1 > r->segment.last_sequence ||
        v.min_time < r->segment.min_time || v.max_time < v.min_time || v.max_time > r->segment.max_time ||
        v.frame_offset < WADB_SEGMENT_HEADER_BYTES || v.frame_offset > r->segment.committed_bytes ||
        v.frame_bytes > WADB_MAX_FRAME_BYTES || v.frame_bytes > r->segment.committed_bytes - v.frame_offset ||
        v.rows_offset < WADB_FRAME_HEADER_BYTES || v.rows_offset % 8 ||
        v.frame_bytes != v.rows_offset + (uint64_t)v.row_count * r->table->schema.row_width + WADB_FRAME_TRAILER_BYTES ||
        v.dictionary_count > (v.rows_offset - WADB_FRAME_HEADER_BYTES) / 12)
        return wadb_fail(e, WADB_CORRUPT, 0, "invalid index record bounds");
    *out = v;
    return WADB_OK;
}

wadb_status wadb_index_save(wadb_table *t, const wadb_segment_meta *s,
    const wadb_frame_index *index, const wadb_dictionary *dictionary, wadb_error *e) {
    unsigned char *definitions = NULL;
    uint32_t dictionary_bytes = 0, dictionary_count = 0;
    wadb_status status = wadb_dictionary_export_limit(dictionary, 0, 64u * 1024 * 1024,
        &definitions, &dictionary_bytes, &dictionary_count, e);
    if (status) return status;
    size_t dictionary_offset = WADB_INDEX_HEADER_BYTES + index->count * WADB_INDEX_ENTRY_BYTES;
    size_t n = dictionary_offset + dictionary_bytes;
    unsigned char *p = calloc(1, n);
    if (!p) { free(definitions); return wadb_fail(e, WADB_NOMEM, 0, "sealed index serialization"); }
    memcpy(p, "WADBIDX1", 8);
    wadb_put_u32(p + 8, WADB_FORMAT_VERSION); wadb_put_u32(p + 12, WADB_INDEX_HEADER_BYTES);
    wadb_put_u64(p + 16, t->id); wadb_put_u64(p + 24, s->id);
    wadb_put_u64(p + 32, t->schema.fingerprint); wadb_put_u64(p + 40, s->committed_bytes);
    wadb_put_u64(p + 48, index->count); wadb_put_u64(p + 56, dictionary_offset);
    wadb_put_u64(p + 64, dictionary_bytes); wadb_put_u32(p + 72, dictionary_count);
    wadb_put_u64(p + 80, (uint64_t)s->min_time); wadb_put_u64(p + 88, (uint64_t)s->max_time);
    wadb_put_u64(p + 96, s->first_sequence); wadb_put_u64(p + 104, s->last_sequence);
    wadb_put_u32(p + 112, t->schema.row_width);
    wadb_put_u32(p + 116, wadb_crc32c(definitions, dictionary_bytes));
    wadb_put_u32(p + 124, wadb_crc32c(p, 124));
    for (size_t i = 0; i < index->count; ++i)
        entry_encode(p + WADB_INDEX_HEADER_BYTES + i * WADB_INDEX_ENTRY_BYTES, &index->entries[i]);
    if (dictionary_bytes) memcpy(p + dictionary_offset, definitions, dictionary_bytes);
    free(definitions);
    char *path = wadb_segment_path(t, s, "idx");
    status = path ? wadb_atomic_file(path, p, n, e) : wadb_fail(e, WADB_NOMEM, 0, "index path");
    free(path); free(p);
    return status;
}

static wadb_status get_raw(wadb_index_reader *r, size_t position, wadb_index_entry *entry, wadb_error *e) {
    if (position >= r->count) return wadb_fail(e, WADB_INVALID, 0, "index position out of bounds");
    if (r->memory) { *entry = r->memory[position]; return WADB_OK; }
    uint64_t page = position / 64;
    if (r->page_number != page) {
        size_t first = (size_t)page * 64, records = r->count - first;
        if (records > 64) records = 64;
        r->page_bytes = records * WADB_INDEX_ENTRY_BYTES;
        wadb_status status = wadb_pread_all(r->fd, r->page, r->page_bytes,
            WADB_INDEX_HEADER_BYTES + (uint64_t)first * WADB_INDEX_ENTRY_BYTES, e);
        if (status) return status;
        r->io_bytes += r->page_bytes;
        r->page_number = page;
    }
    return entry_decode(r, r->page + (position % 64) * WADB_INDEX_ENTRY_BYTES, entry, e);
}

static wadb_status open_raw(wadb_table *t, const wadb_segment_meta *s, wadb_index_reader *r, wadb_error *e) {
    memset(r, 0, sizeof(*r)); r->fd = -1; r->page_number = UINT64_MAX;
    r->table = t; r->segment = *s;
    char *path = wadb_segment_path(t, s, "idx");
    if (!path) return wadb_fail(e, WADB_NOMEM, 0, "index path");
    r->fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    free(path);
    if (r->fd < 0) return wadb_fail(e, errno == ENOENT ? WADB_NOT_FOUND : WADB_IO, errno, "open derived index");
    unsigned char p[WADB_INDEX_HEADER_BYTES];
    struct stat st;
    wadb_status status = wadb_pread_all(r->fd, p, sizeof(p), 0, e);
    if (status) goto fail;
    r->io_bytes += sizeof(p);
    if (fstat(r->fd, &st) || !S_ISREG(st.st_mode)) { status = wadb_fail(e, WADB_CORRUPT, errno, "invalid index file"); goto fail; }
    uint64_t count = wadb_get_u64(p + 48), offset = wadb_get_u64(p + 56), bytes = wadb_get_u64(p + 64);
    if (memcmp(p, "WADBIDX1", 8) || wadb_get_u32(p + 8) != WADB_FORMAT_VERSION ||
        wadb_get_u32(p + 12) != sizeof(p) || wadb_crc32c(p, 124) != wadb_get_u32(p + 124) ||
        wadb_get_u64(p + 16) != t->id || wadb_get_u64(p + 24) != s->id ||
        wadb_get_u64(p + 32) != t->schema.fingerprint || wadb_get_u64(p + 40) != s->committed_bytes ||
        count > WADB_MAX_INDEX_ENTRIES || offset != sizeof(p) + count * WADB_INDEX_ENTRY_BYTES ||
        bytes > 64u * 1024 * 1024 || bytes % 8 || st.st_size < 0 || (uint64_t)st.st_size != offset + bytes ||
        wadb_get_u32(p + 72) > bytes / 12 || wadb_get_u32(p + 76) || wadb_get_u32(p + 120) ||
        wadb_get_i64(p + 80) != s->min_time || wadb_get_i64(p + 88) != s->max_time ||
        wadb_get_u64(p + 96) != s->first_sequence || wadb_get_u64(p + 104) != s->last_sequence ||
        wadb_get_u32(p + 112) != t->schema.row_width || (!count != !s->last_sequence)) {
        status = wadb_fail(e, WADB_CORRUPT, 0, "stale or invalid derived index header"); goto fail;
    }
    r->count = (size_t)count; r->dictionary_offset = offset; r->dictionary_bytes = bytes;
    r->dictionary_count = wadb_get_u32(p + 72); r->dictionary_crc = wadb_get_u32(p + 116);
    if (count) {
        wadb_index_entry first, last;
        status = get_raw(r, 0, &first, e);
        if (!status) status = get_raw(r, r->count - 1, &last, e);
        if (status) goto fail;
        if (first.first_sequence != s->first_sequence || first.frame_offset != WADB_SEGMENT_HEADER_BYTES ||
            first.min_time != s->min_time || last.max_time != s->max_time ||
            last.first_sequence + last.row_count - 1 != s->last_sequence ||
            last.frame_offset + last.frame_bytes != s->committed_bytes) {
            status = wadb_fail(e, WADB_CORRUPT, 0, "index endpoints contradict segment"); goto fail;
        }
    }
    return WADB_OK;
fail:
    close(r->fd); r->fd = -1;
    return status;
}

void wadb_index_close(wadb_index_reader *r) {
    if (r->fd >= 0) close(r->fd);
    r->fd = -1;
    wadb_index_free(&r->rebuilt_index); wadb_dictionary_free(r->rebuilt_dictionary);
    r->rebuilt_dictionary = NULL;
}
static wadb_status repair_reader(wadb_index_reader *r, wadb_error *e) {
    if (r->segment.committed_bytes > r->rebuild_budget)
        return wadb_fail(e, WADB_LIMIT, 0, "index rebuild exceeds scan byte budget");
    wadb_frame_index index = {0};
    wadb_dictionary *dictionary = NULL;
    wadb_status status = wadb_rebuild_index_data(r->table, &r->segment, &index, &dictionary, r->deadline, e);
    if (status) return status;
    r->rebuilt_bytes += r->segment.committed_bytes;
    r->rebuild_budget -= r->segment.committed_bytes;
    /* Reuse validated memory immediately, even if installing the derived file
     * fails. Neither a disk-full condition nor a racing repair hides rows. */
    wadb_status saved = wadb_index_save(r->table, &r->segment, &index, dictionary, e);
    if (saved != WADB_OK && saved != WADB_IO && saved != WADB_INDETERMINATE && saved != WADB_NOMEM) {
        wadb_index_free(&index); wadb_dictionary_free(dictionary); return saved;
    }
    wadb_index_close(r);
    r->rebuilt_index = index; r->memory = index.entries; r->count = index.count;
    r->rebuilt_dictionary = dictionary; r->repaired = true;
    uint64_t bytes = 0;
    for (size_t i = 0; i < dictionary->count; ++i) bytes += 12 + (uint64_t)dictionary->symbols[i].length;
    r->dictionary_bytes = (bytes + 7) & ~UINT64_C(7);
    r->dictionary_count = (uint32_t)dictionary->count;
    /* CRC identifies cache contents when repaired files cannot be installed.
     * Segment IDs and committed boundaries are never reused. */
    r->dictionary_crc = 0;
    wadb_error_clear(e);
    return WADB_OK;
}
wadb_status wadb_index_open(wadb_table *t, const wadb_segment_meta *s,
    wadb_index_reader *r, bool repair, wadb_error *e) {
    if (repair) return wadb_index_open_bounded(t, s, r, UINT64_MAX, UINT64_MAX, e);
    return open_raw(t, s, r, e);
}
wadb_status wadb_index_open_bounded(wadb_table *t, const wadb_segment_meta *s,
    wadb_index_reader *r, uint64_t budget, uint64_t deadline, wadb_error *e) {
    wadb_status status = open_raw(t, s, r, e);
    r->rebuild_budget = budget; r->deadline = deadline;
    if (status && status != WADB_NOMEM && status != WADB_LIMIT) status = repair_reader(r, e);
    if (!status) wadb_error_clear(e);
    return status;
}
wadb_status wadb_index_get(wadb_index_reader *r, size_t pos, wadb_index_entry *v, wadb_error *e) {
    wadb_status status = get_raw(r, pos, v, e);
    if ((status == WADB_CORRUPT || status == WADB_IO) && !r->memory && !r->repaired) {
        status = repair_reader(r, e);
        if (!status) status = get_raw(r, pos, v, e);
    }
    return status;
}
wadb_status wadb_index_lower_bound(wadb_index_reader *r, int64_t time, uint64_t sequence,
    size_t *position, wadb_error *e) {
    size_t lo = 0, hi = r->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        wadb_index_entry v;
        wadb_status status = wadb_index_get(r, mid, &v, e);
        if (status) return status;
        if (v.max_time < time || v.first_sequence + v.row_count - 1 <= sequence) lo = mid + 1;
        else hi = mid;
    }
    *position = lo;
    return WADB_OK;
}
wadb_status wadb_index_dictionary(wadb_index_reader *r, wadb_dictionary **out, wadb_error *e) {
    *out = NULL;
    if (r->memory) {
        if (!r->rebuilt_dictionary) return wadb_fail(e, WADB_INVALID, 0, "transient dictionary already transferred");
        *out = r->rebuilt_dictionary; r->rebuilt_dictionary = NULL;
        return WADB_OK;
    }
    unsigned char *bytes = malloc(r->dictionary_bytes ? (size_t)r->dictionary_bytes : 1);
    if (!bytes) return wadb_fail(e, WADB_NOMEM, 0, "dictionary checkpoint buffer");
    wadb_status status = wadb_pread_all(r->fd, bytes, (size_t)r->dictionary_bytes, r->dictionary_offset, e);
    r->io_bytes += r->dictionary_bytes;
    r->dictionary_io_bytes += r->dictionary_bytes;
    if (!status && wadb_crc32c(bytes, (size_t)r->dictionary_bytes) != r->dictionary_crc)
        status = wadb_fail(e, WADB_CORRUPT, 0, "dictionary checkpoint checksum mismatch");
    if (!status) status = wadb_dictionary_new(&r->table->schema, r->table->db->options.dictionary_limit_bytes, out, e);
    if (!status) {
        wadb_frame f = {.dictionary = bytes, .dictionary_bytes = (uint32_t)r->dictionary_bytes,
            .dictionary_count = r->dictionary_count};
        status = wadb_dictionary_apply(*out, &f, e);
    }
    free(bytes);
    if (status) { wadb_dictionary_free(*out); *out = NULL; }
    if ((status == WADB_CORRUPT || status == WADB_IO) && !r->repaired) {
        status = repair_reader(r, e);
        if (!status) status = wadb_index_dictionary(r, out, e);
    }
    return status;
}
