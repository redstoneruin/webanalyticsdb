#include "../core/internal.h"
#include <stdlib.h>
#include <string.h>

static bool zeroes(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; ++i) if (p[i]) return false;
    return true;
}

wadb_status wadb_segment_encode(const wadb_segment_header *h,
    unsigned char out[WADB_SEGMENT_HEADER_BYTES], wadb_error *e) {
    if (!h || !out || !h->table_id || !h->segment_id || !h->first_sequence ||
        !h->row_width || h->row_width > WADB_MAX_ROW_BYTES || h->row_width % 8 ||
        h->day_start < 0 || h->day_start % WADB_DAY_US)
        return wadb_fail(e, WADB_INVALID, 0, "invalid segment header");
    memset(out, 0, WADB_SEGMENT_HEADER_BYTES);
    memcpy(out, "WADBSEG1", 8);
    wadb_put_u32(out + 8, WADB_FORMAT_VERSION);
    wadb_put_u32(out + 12, WADB_SEGMENT_HEADER_BYTES);
    wadb_put_u64(out + 16, h->table_id);
    wadb_put_u64(out + 24, h->segment_id);
    wadb_put_u64(out + 32, h->schema_hash);
    wadb_put_u32(out + 40, h->row_width);
    wadb_put_u64(out + 48, (uint64_t)h->day_start);
    wadb_put_u64(out + 56, h->first_sequence);
    wadb_put_u32(out + 124, wadb_crc32c(out, 124));
    return WADB_OK;
}

wadb_status wadb_segment_decode(const void *data, size_t n,
    wadb_segment_header *out, wadb_error *e) {
    const unsigned char *p = data;
    if (!p || !out || n != WADB_SEGMENT_HEADER_BYTES || memcmp(p, "WADBSEG1", 8))
        return wadb_fail(e, WADB_CORRUPT, 0, "missing or invalid segment header");
    if (wadb_crc32c(p, 124) != wadb_get_u32(p + 124))
        return wadb_fail(e, WADB_CORRUPT, 0, "segment header checksum mismatch");
    if (wadb_get_u32(p + 8) != WADB_FORMAT_VERSION)
        return wadb_fail(e, WADB_VERSION, 0, "unsupported segment version");
    if (wadb_get_u32(p + 12) != WADB_SEGMENT_HEADER_BYTES || !zeroes(p + 44, 4) || !zeroes(p + 64, 60))
        return wadb_fail(e, WADB_CORRUPT, 0, "invalid segment header size or reserved bytes");
    wadb_segment_header h = {
        .table_id = wadb_get_u64(p + 16), .segment_id = wadb_get_u64(p + 24),
        .schema_hash = wadb_get_u64(p + 32), .row_width = wadb_get_u32(p + 40),
        .day_start = wadb_get_i64(p + 48), .first_sequence = wadb_get_u64(p + 56)
    };
    unsigned char check[WADB_SEGMENT_HEADER_BYTES];
    if (wadb_segment_encode(&h, check, NULL))
        return wadb_fail(e, WADB_CORRUPT, 0, "invalid segment header values");
    *out = h;
    return WADB_OK;
}

static bool frame_sizes(const wadb_frame *f, size_t *row_bytes, size_t *total) {
    if (!f->row_count || !f->row_width || f->row_width > WADB_MAX_ROW_BYTES || f->row_width % 8 ||
        !f->first_sequence || f->row_count - 1 > UINT64_MAX - f->first_sequence ||
        f->min_time < 0 || f->max_time < f->min_time ||
        f->min_time / WADB_DAY_US != f->max_time / WADB_DAY_US ||
        f->dictionary_bytes % 8 || f->dictionary_bytes > WADB_MAX_FRAME_BYTES ||
        f->dictionary_count > f->dictionary_bytes / 12 ||
        (!f->dictionary_count && f->dictionary_bytes)) return false;
    return wadb_mul_size(f->row_count, f->row_width, row_bytes) &&
           wadb_add_size(*row_bytes, f->dictionary_bytes, total) &&
           wadb_add_size(*total, WADB_FRAME_HEADER_BYTES + WADB_FRAME_TRAILER_BYTES, total) &&
           *total <= WADB_MAX_FRAME_BYTES;
}

static bool dictionary_valid(const wadb_frame *f) {
    size_t offset = 0;
    for (uint32_t i = 0; i < f->dictionary_count; ++i) {
        if (f->dictionary_bytes - offset < 12) return false;
        const unsigned char *p = f->dictionary + offset;
        uint32_t length = wadb_get_u32(p + 8);
        if (!wadb_get_u16(p) || wadb_get_u16(p) >= WADB_MAX_FIELDS || wadb_get_u16(p + 2) ||
            !wadb_get_u32(p + 4) || length > WADB_MAX_STRING_BYTES ||
            length > f->dictionary_bytes - offset - 12 || !wadb_utf8(p + 12, length)) return false;
        offset += 12 + length;
    }
    return f->dictionary_bytes - offset < 8 &&
        (!f->dictionary_bytes || zeroes(f->dictionary + offset, f->dictionary_bytes - offset));
}

static bool times_valid(const wadb_frame *f) {
    int64_t previous = f->min_time;
    for (uint32_t i = 0; i < f->row_count; ++i) {
        int64_t time = wadb_get_i64(f->rows + (size_t)i * f->row_width);
        if (time < previous || time > f->max_time || (!i && time != f->min_time)) return false;
        previous = time;
    }
    return previous == f->max_time;
}

wadb_status wadb_frame_encode(const wadb_frame *f, unsigned char **out,
    size_t *length, wadb_error *e) {
    size_t rows, total;
    if (!f || !out || !length || !frame_sizes(f, &rows, &total) || !f->rows ||
        (f->dictionary_bytes && !f->dictionary) || !dictionary_valid(f) || !times_valid(f))
        return wadb_fail(e, WADB_INVALID, 0, "invalid frame values or size");
    *out = NULL; *length = 0;
    unsigned char *p = calloc(1, total);
    if (!p) return wadb_fail(e, WADB_NOMEM, 0, "allocating frame");
    memcpy(p, "WADBBAT1", 8);
    wadb_put_u32(p + 8, (uint32_t)total);
    wadb_put_u32(p + 12, WADB_FRAME_HEADER_BYTES);
    wadb_put_u32(p + 16, f->row_count);
    wadb_put_u32(p + 20, f->row_width);
    wadb_put_u64(p + 24, f->first_sequence);
    wadb_put_u64(p + 32, (uint64_t)f->min_time);
    wadb_put_u64(p + 40, (uint64_t)f->max_time);
    wadb_put_u32(p + 48, f->dictionary_bytes);
    wadb_put_u32(p + 52, f->dictionary_count);
    wadb_put_u32(p + 56, WADB_FRAME_HEADER_BYTES + f->dictionary_bytes);
    wadb_put_u64(p + 64, f->schema_hash);
    wadb_put_u32(p + 72, wadb_crc32c(p, 72));
    if (f->dictionary_bytes) memcpy(p + WADB_FRAME_HEADER_BYTES, f->dictionary, f->dictionary_bytes);
    memcpy(p + WADB_FRAME_HEADER_BYTES + f->dictionary_bytes, f->rows, rows);
    memcpy(p + total - WADB_FRAME_TRAILER_BYTES, "WADBEND1", 8);
    wadb_put_u32(p + total - 8, (uint32_t)total);
    wadb_put_u32(p + total - 4, wadb_crc32c(p, total - 4));
    *out = p; *length = total;
    return WADB_OK;
}

wadb_status wadb_frame_inspect(const void *data, size_t n, wadb_frame *out, wadb_error *e) {
    const unsigned char *p = data;
    if (!p || !out || n < WADB_FRAME_HEADER_BYTES || memcmp(p, "WADBBAT1", 8) ||
        wadb_get_u32(p + 12) != WADB_FRAME_HEADER_BYTES ||
        wadb_crc32c(p, 72) != wadb_get_u32(p + 72) ||
        !zeroes(p + 60, 4) || !zeroes(p + 76, 4))
        return wadb_fail(e, WADB_CORRUPT, 0, "invalid frame header or checksum");
    wadb_frame f = {
        .row_count = wadb_get_u32(p + 16), .row_width = wadb_get_u32(p + 20),
        .first_sequence = wadb_get_u64(p + 24), .min_time = wadb_get_i64(p + 32),
        .max_time = wadb_get_i64(p + 40), .dictionary_bytes = wadb_get_u32(p + 48),
        .dictionary_count = wadb_get_u32(p + 52), .rows_offset = wadb_get_u32(p + 56),
        .schema_hash = wadb_get_u64(p + 64), .frame_bytes = wadb_get_u32(p + 8)
    };
    size_t rows, total;
    if (!frame_sizes(&f, &rows, &total) || total != f.frame_bytes ||
        f.rows_offset != WADB_FRAME_HEADER_BYTES + f.dictionary_bytes)
        return wadb_fail(e, WADB_CORRUPT, 0, "invalid frame lengths or ranges");
    *out = f;
    return WADB_OK;
}

wadb_status wadb_frame_decode(const void *data, size_t n, wadb_frame *out, wadb_error *e) {
    wadb_frame f;
    wadb_status status = wadb_frame_inspect(data, n, &f, e);
    if (status) return status;
    const unsigned char *p = data;
    if (n != f.frame_bytes || memcmp(p + n - WADB_FRAME_TRAILER_BYTES, "WADBEND1", 8) ||
        wadb_get_u32(p + n - 8) != n || wadb_crc32c(p, n - 4) != wadb_get_u32(p + n - 4))
        return wadb_fail(e, WADB_CORRUPT, 0, "frame is incomplete or checksum mismatches");
    f.dictionary = p + WADB_FRAME_HEADER_BYTES;
    f.rows = p + f.rows_offset;
    if (!dictionary_valid(&f) || !times_valid(&f))
        return wadb_fail(e, WADB_CORRUPT, 0, "invalid dictionary or unordered frame rows");
    *out = f;
    return WADB_OK;
}
