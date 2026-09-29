#ifndef WADB_H
#define WADB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WADB_MAX_FIELDS 256u
#define WADB_NAME_CAP 64u
#define WADB_MAX_ROW_BYTES 16384u
#define WADB_MAX_FRAME_BYTES (1024u * 1024u)
#define WADB_MAX_STRING_BYTES 65536u

typedef enum {
    WADB_OK = 0,
    WADB_INVALID,
    WADB_NOMEM,
    WADB_IO,
    WADB_CORRUPT,
    WADB_VERSION,
    WADB_EXISTS,
    WADB_NOT_FOUND,
    WADB_LOCKED,
    WADB_SCHEMA_MISMATCH,
    WADB_BACKPRESSURE,
    WADB_LIMIT,
    WADB_INDETERMINATE,
    WADB_EXPIRED,
    WADB_READ_ONLY,
    WADB_CANCELLED
} wadb_status;

typedef struct {
    wadb_status code;
    int system_errno;
    char message[256];
} wadb_error;

const char *wadb_status_name(wadb_status status);

typedef enum {
    WADB_I8 = 1, WADB_U8, WADB_I16, WADB_U16, WADB_I32, WADB_U32,
    WADB_I64, WADB_U64, WADB_F32, WADB_F64, WADB_BOOL,
    WADB_TIMESTAMP_US, WADB_UUID, WADB_BYTES, WADB_TEXT, WADB_SYMBOL32
} wadb_type;

const char *wadb_type_name(wadb_type type);
wadb_type wadb_type_parse(const char *name);

/* Names are ASCII identifiers, at most 63 bytes. size is the payload capacity
 * for BYTES/TEXT and must be zero for all other types. _time is reserved. */
typedef struct {
    const char *name;
    wadb_type type;
    uint32_t size;
    bool nullable;
} wadb_field_def;

typedef struct {
    char name[WADB_NAME_CAP];
    wadb_type type;
    uint32_t size;
    bool nullable;
    uint32_t offset;
    uint32_t width;
    uint32_t null_bit; /* UINT32_MAX if not nullable */
} wadb_field;

/* Created by wadb_schema_build; treat as immutable. fields[0] is _time.
 * Row layouts are packed explicit bytes, not native C structs. */
typedef struct {
    uint32_t field_count;
    uint32_t row_width;
    uint32_t null_bytes;
    uint64_t fingerprint;
    wadb_field fields[WADB_MAX_FIELDS];
} wadb_schema;

wadb_status wadb_schema_build(wadb_schema *out, const wadb_field_def *fields,
                              size_t count, wadb_error *error);
int wadb_schema_find(const wadb_schema *schema, const char *name);

/* A value's union member is determined by its schema field:
 * i64: signed integers/timestamp; u64: unsigned integers/bool;
 * f64: either float width; bytes: UUID/BYTES/TEXT/SYMBOL32.
 * Append values exclude the automatic _time field. Byte inputs are borrowed
 * until the synchronous API returns. Scan callbacks borrow decoded bytes;
 * query results own them until wadb_result_free. */
typedef struct {
    bool is_null;
    union {
        int64_t i64;
        uint64_t u64;
        double f64;
        struct { const void *data; size_t length; } bytes;
    } as;
} wadb_value;

typedef struct wadb_db wadb_db;
typedef struct wadb_table wadb_table;

typedef struct {
    uint64_t segment_target_bytes;
    size_t queue_limit_bytes;
    size_t dictionary_limit_bytes;
    size_t read_cache_bytes;
    size_t snapshot_limit_bytes;
    uint32_t max_readers;
    uint32_t batch_target_bytes;
    uint32_t batch_delay_ms;
    uint32_t max_tables;
} wadb_options;

void wadb_options_default(wadb_options *options);
/* Opens a local directory with exclusive process ownership. Parent directory
 * must already exist. Callers must finish all concurrent operations before
 * close; every table/schema handle becomes invalid when its database closes. */
wadb_status wadb_open(const char *directory, const wadb_options *options,
                      wadb_db **out, wadb_error *error);
void wadb_close(wadb_db *db);
wadb_status wadb_create_table(wadb_db *db, const char *name,
    const wadb_field_def *fields, size_t field_count, uint64_t retention_us,
    wadb_table **out, wadb_error *error);
wadb_status wadb_get_table(wadb_db *db, uint64_t id, wadb_table **out, wadb_error *error);
wadb_status wadb_find_table(wadb_db *db, const char *name, wadb_table **out, wadb_error *error);
/* Set tables=NULL and capacity=0 to obtain count. A too-small array returns
 * WADB_LIMIT and the required count; all returned handles are database-owned. */
wadb_status wadb_list_tables(wadb_db *db, wadb_table **tables, size_t capacity,
                            size_t *count, wadb_error *error);
uint64_t wadb_table_id(const wadb_table *table);
const char *wadb_table_name(const wadb_table *table);
const wadb_schema *wadb_table_schema(const wadb_table *table);

typedef struct {
    uint64_t first_sequence, last_sequence;
    int64_t ingestion_time;
    uint32_t row_count;
} wadb_append_receipt;

/* Row-major values, excluding _time. One request commits atomically. Caller
 * memory remains valid until return. Only WADB_OK guarantees durable success;
 * WADB_INDETERMINATE requires recovery and retries may duplicate records. */
wadb_status wadb_append_batch(wadb_table *table, const wadb_value *values,
    size_t row_count, wadb_append_receipt *receipt, wadb_error *error);

typedef struct {
    int64_t start_time, end_time; /* half-open ingestion-time interval */
    uint64_t after_sequence;
    uint64_t max_scan_bytes;
    uint32_t limit;
    uint32_t timeout_ms;
} wadb_scan_options;
typedef struct {
    uint64_t rows_scanned, rows_returned, bytes_scanned, snapshot_sequence;
    uint64_t index_bytes, dictionary_bytes, frames_scanned, segments_scanned;
    uint64_t rebuild_bytes;
    uint64_t last_sequence;
    bool limit_reached;
} wadb_scan_stats;

void wadb_scan_options_default(wadb_scan_options *options);
/* Callback values include _time at index zero. Byte values are borrowed until
 * callback return. Returning nonzero stops the scan and propagates that status.
 * A scan observes a stable committed prefix, even during concurrent appends. */
typedef wadb_status (*wadb_row_fn)(void *context, uint64_t sequence,
    const wadb_value *values, size_t count);
wadb_status wadb_scan(wadb_table *table, const wadb_scan_options *options,
    wadb_row_fn consume, void *context, wadb_scan_stats *stats, wadb_error *error);

#define WADB_MAX_FILTERS 32u
#define WADB_MAX_GROUP_FIELDS 8u
#define WADB_MAX_AGGREGATES 16u
typedef enum { WADB_EQ, WADB_NE, WADB_LT, WADB_LE, WADB_GT, WADB_GE,
    WADB_IS_NULL, WADB_IS_NOT_NULL } wadb_filter_op;
typedef struct { const char *field; wadb_filter_op op; wadb_value value; } wadb_filter;
typedef enum { WADB_COUNT_ALL, WADB_COUNT, WADB_SUM, WADB_MIN, WADB_MAX, WADB_AVG } wadb_aggregate_op;
/* COUNT_ALL has no field. Other aggregates require a field; SUM/MIN/MAX/AVG
 * currently accept numeric fields. Every aggregate has a unique result name. */
typedef struct { wadb_aggregate_op op; const char *field; const char *name; } wadb_aggregate;
typedef struct {
    wadb_scan_options scan; /* limit is output rows, after filtering/aggregation */
    const char *const *projection; size_t projection_count; /* empty means all */
    const wadb_filter *filters; size_t filter_count; /* conjunction */
    const char *const *group_by; size_t group_count;
    const wadb_aggregate *aggregates; size_t aggregate_count;
    int64_t bucket_width_us, bucket_origin_us; /* zero width disables buckets */
    uint32_t max_groups, max_buckets;
    size_t memory_limit_bytes; /* includes groups, result cells and copied bytes */
    const char *order_by; /* aggregate output column; NULL means first occurrence */
    bool descending;
} wadb_query_options;
typedef struct { char name[WADB_NAME_CAP]; wadb_type type; } wadb_result_column;
typedef struct {
    wadb_result_column *columns;
    size_t column_count, row_count;
    wadb_value *values; /* row-major, owned until result_free */
    uint64_t *sequences; /* scan results only; NULL for aggregates */
    uint64_t matched_rows, total_groups;
    wadb_scan_stats stats;
    bool has_more; /* scan limit reached; next cursor page may be empty */
} wadb_result;
void wadb_query_options_default(wadb_query_options *options);
wadb_status wadb_query(wadb_table *table, const wadb_query_options *options,
    wadb_result **result, wadb_error *error);
void wadb_result_free(wadb_result *result);
/* Scan cursors preserve the schema/range/filters/projection and committed
 * snapshot. TTL is absolute, 1..300000 ms. IDs are local to this database open.
 * Pages share the total byte budget. Aggregate queries use wadb_query instead.
 * Concurrent next/close of a busy cursor returns BACKPRESSURE. Cursors expire
 * explicitly; they never switch snapshots. Closing an expired cursor is safe. */
wadb_status wadb_cursor_open(wadb_table *table, const wadb_query_options *options,
    uint32_t ttl_ms, uint64_t *cursor_id, wadb_error *error);
wadb_status wadb_cursor_next(wadb_db *db, uint64_t cursor_id, wadb_result **result, wadb_error *error);
wadb_status wadb_cursor_close(wadb_db *db, uint64_t cursor_id, wadb_error *error);
void wadb_cursor_expire(wadb_db *db);

typedef struct {
    int64_t cutoff;
    uint64_t eligible_segments, eligible_rows, eligible_bytes;
    uint64_t partially_expired_segments, pending_segments, deleted_segments;
} wadb_retention_stats;
/* Retention uses ingestion time and removes whole segments with max_time below
 * now-retention_us. Zero retention disables retirement. Explicit now supports
 * previewing a policy; normal callers pass current Unix time in microseconds.
 * Existing readers/cursors pin files, even after their durable retirement. */
wadb_status wadb_set_retention(wadb_table *table, uint64_t retention_us, wadb_error *error);
wadb_status wadb_retention_preview(wadb_table *table, int64_t now,
    wadb_retention_stats *stats, wadb_error *error);
wadb_status wadb_retention_apply(wadb_table *table, int64_t now,
    wadb_retention_stats *stats, wadb_error *error);
typedef struct {
    uint64_t max_scan_bytes;
    uint32_t timeout_ms;
    bool (*cancelled)(void *context);
    void *context;
} wadb_integrity_options;
typedef struct { uint64_t segments, frames, rows, bytes, snapshot_sequence; } wadb_integrity_stats;
void wadb_integrity_options_default(wadb_integrity_options *options);
wadb_status wadb_integrity_check(wadb_table *table, const wadb_integrity_options *options,
    wadb_integrity_stats *stats, wadb_error *error);

typedef struct { uint64_t samples, p50_us, p95_us, p99_us, max_us; } wadb_latency_stats;
typedef struct {
    uint64_t uptime_us, table_count, queue_bytes, queue_limit_bytes;
    uint64_t cache_bytes, cache_hits, cache_misses, readers, snapshot_bytes;
    uint64_t committed_rows, committed_bytes, committed_frames, append_requests;
    uint64_t rejected_appends, failed_appends, clock_adjustments;
    uint64_t queries, failed_queries, rows_scanned, bytes_scanned;
    uint64_t disk_free_bytes, latest_notification;
    bool disk_free_available, read_only;
    wadb_latency_stats append_latency, sync_latency, query_latency;
} wadb_stats;
typedef struct {
    uint64_t id, retention_us, rows, bytes, segments, retired_segments;
    uint64_t last_sequence, dictionary_bytes, active_index_bytes;
    int64_t min_time, max_time, last_assigned_time;
    uint32_t row_width;
    bool read_only, pending_retention;
    wadb_status maintenance_status;
} wadb_table_stats;
/* Counters/latencies are since this database open; table row/byte totals describe
 * current retained data. Percentiles are upper bounds from log2-us histograms. */
wadb_status wadb_get_stats(wadb_db *db, wadb_stats *stats, wadb_error *error);
wadb_status wadb_get_table_stats(wadb_table *table, wadb_table_stats *stats, wadb_error *error);
typedef enum { WADB_SEG_ACTIVE = 1, WADB_SEG_SEALED = 2, WADB_SEG_RETIRED = 3 } wadb_segment_state;
typedef struct {
    uint64_t id;
    int64_t day_start;
    uint64_t first_sequence, last_sequence;
    int64_t min_time, max_time;
    uint64_t committed_bytes;
    wadb_segment_state state;
} wadb_segment_info;
/* Returns a bounded page in increasing segment-ID order, including tombstones.
 * Pass after_id=0 for the first page; capacity must be in 1..1024. */
wadb_status wadb_list_segments(wadb_table *table, uint64_t after_id,
    wadb_segment_info *segments, size_t capacity, size_t *count, bool *has_more, wadb_error *error);
typedef struct {
    uint64_t id, table_id, first_sequence, last_sequence, frame_bytes;
    int64_t max_time;
    uint32_t rows;
} wadb_commit_event;
#define WADB_NOTIFICATION_CAPACITY 1024u
/* Lightweight monitoring feed; events contain metadata, never borrowed rows.
 * The bounded ring may overwrite old entries and then reports gap=true.
 * IDs reset on database open; a network server must add its own boot identity. */
wadb_status wadb_read_notifications(wadb_db *db, uint64_t after, wadb_commit_event *events,
    size_t capacity, size_t *count, uint64_t *latest, bool *gap, wadb_error *error);

#ifdef __cplusplus
}
#endif
#endif
