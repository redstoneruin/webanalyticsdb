#ifndef WADB_INTERNAL_H
#define WADB_INTERNAL_H

#include "wadb.h"
#include <stdarg.h>
#include <sys/types.h>

#define WADB_SEGMENT_HEADER_BYTES 128u
#define WADB_FRAME_HEADER_BYTES 80u
#define WADB_FRAME_TRAILER_BYTES 16u
#define WADB_FORMAT_VERSION 1u
#define WADB_DAY_US INT64_C(86400000000)

wadb_status wadb_fail(wadb_error *error, wadb_status code, int system_errno,
                      const char *format, ...);
void wadb_error_clear(wadb_error *error);
bool wadb_add_size(size_t a, size_t b, size_t *out);
bool wadb_mul_size(size_t a, size_t b, size_t *out);
bool wadb_identifier(const char *name);
bool wadb_utf8(const void *data, size_t length);
uint16_t wadb_get_u16(const void *p);
uint32_t wadb_get_u32(const void *p);
uint64_t wadb_get_u64(const void *p);
int64_t wadb_get_i64(const void *p);
void wadb_put_u16(void *p, uint16_t value);
void wadb_put_u32(void *p, uint32_t value);
void wadb_put_u64(void *p, uint64_t value);
uint32_t wadb_crc32c(const void *data, size_t length);
uint64_t wadb_hash(const void *data, size_t length, uint64_t hash);

typedef wadb_status (*wadb_symbol_encode_fn)(void *context, uint32_t field,
    const void *data, size_t length, uint32_t *id, wadb_error *error);
typedef wadb_status (*wadb_symbol_decode_fn)(void *context, uint32_t field,
    uint32_t id, const void **data, size_t *length, wadb_error *error);
wadb_status wadb_encode_field(const wadb_field *field, const wadb_value *value,
    unsigned char *bytes, uint32_t field_index, wadb_symbol_encode_fn resolve, void *context, wadb_error *error);

/* Values are schema fields 1..N. The resolver is only needed for symbols.
 * Encoding clears the entire output row; callers publish only on success.
 * Decoded byte values borrow row/dictionary memory for the duration of use. */
wadb_status wadb_encode_row(const wadb_schema *schema, const wadb_value *values,
    int64_t time, wadb_symbol_encode_fn resolve, void *context,
    void *row, size_t row_capacity, wadb_error *error);
wadb_status wadb_decode_value(const wadb_schema *schema, uint32_t field,
    const void *row, wadb_symbol_decode_fn resolve, void *context,
    wadb_value *out, wadb_error *error);
wadb_status wadb_validate_row(const wadb_schema *schema, const void *row,
    wadb_symbol_decode_fn resolve, void *context, wadb_error *error);

typedef struct {
    uint64_t table_id, segment_id, schema_hash;
    uint32_t row_width;
    int64_t day_start;
    uint64_t first_sequence;
} wadb_segment_header;

wadb_status wadb_segment_encode(const wadb_segment_header *header,
    unsigned char out[WADB_SEGMENT_HEADER_BYTES], wadb_error *error);
wadb_status wadb_segment_decode(const void *data, size_t length,
    wadb_segment_header *out, wadb_error *error);

typedef struct {
    uint64_t schema_hash, first_sequence;
    int64_t min_time, max_time;
    uint32_t row_count, row_width, dictionary_count, dictionary_bytes;
    const unsigned char *dictionary, *rows;
    uint32_t frame_bytes, rows_offset;
} wadb_frame;

wadb_status wadb_frame_encode(const wadb_frame *frame, unsigned char **out,
    size_t *length, wadb_error *error);
/* Header inspection can be used before allocating a frame body. */
wadb_status wadb_frame_inspect(const void *header, size_t length,
    wadb_frame *out, wadb_error *error);
wadb_status wadb_frame_decode(const void *data, size_t length,
    wadb_frame *out, wadb_error *error);

int64_t wadb_realtime_us(void);
uint64_t wadb_monotonic_ns(void);
wadb_status wadb_mkdir(const char *path, wadb_error *error);
wadb_status wadb_sync_directory(const char *path, wadb_error *error);
wadb_status wadb_atomic_file(const char *path, const void *data, size_t length,
    wadb_error *error);
wadb_status wadb_read_file(const char *path, size_t limit, unsigned char **out,
    size_t *length, wadb_error *error);

/* Optional injected calls let tests exercise short writes and sync failure
 * without process-global hooks or weakening production durability. */
typedef struct {
    void *context;
    ssize_t (*write)(void *context, int fd, const void *data, size_t length);
    int (*sync)(void *context, int fd);
} wadb_io;
wadb_status wadb_write_all(const wadb_io *io, int fd, const void *data,
    size_t length, wadb_error *error);
wadb_status wadb_sync_file(const wadb_io *io, int fd, wadb_error *error);
wadb_status wadb_pread_all(int fd, void *data, size_t length, uint64_t offset,
    wadb_error *error);

#endif
