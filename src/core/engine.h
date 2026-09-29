#ifndef WADB_ENGINE_H
#define WADB_ENGINE_H
#include "internal.h"
#include "../storage/dictionary.h"
#include <pthread.h>

#define WADB_MAX_TABLES 1024u
#define WADB_MAX_SEGMENTS 1000000u
#define WADB_MAX_INDEX_ENTRIES 65536u
#define WADB_INDEX_HEADER_BYTES 128u
#define WADB_INDEX_ENTRY_BYTES 64u
typedef struct { uint64_t bins[64], count, max_us; } wadb_histogram;

typedef wadb_segment_info wadb_segment_meta;

typedef struct {
    int64_t min_time, max_time;
    uint64_t first_sequence, frame_offset;
    uint32_t row_count, dictionary_count, frame_bytes, rows_offset;
    uint32_t frame_crc, dictionary_crc;
} wadb_index_entry;
typedef struct { wadb_index_entry *entries; size_t count, capacity; } wadb_frame_index;
typedef struct {
    wadb_table *table;
    wadb_segment_meta *segments;
    size_t count, charge;
    wadb_frame_index active;
    uint64_t sequence;
} wadb_snapshot;
typedef struct wadb_cache_entry {
    struct wadb_cache_entry *next;
    uint64_t table_id, segment_id, covered_bytes, used;
    uint32_t dictionary_crc;
    size_t references, charge;
    wadb_dictionary *dictionary;
} wadb_cache_entry;
typedef struct {
    wadb_table *table;
    wadb_segment_meta segment;
    int fd;
    const wadb_index_entry *memory;
    size_t count;
    uint64_t dictionary_offset, dictionary_bytes;
    uint32_t dictionary_count, dictionary_crc;
    uint64_t page_number, io_bytes, dictionary_io_bytes;
    size_t page_bytes;
    unsigned char page[4096];
    bool repaired;
    wadb_frame_index rebuilt_index;
    wadb_dictionary *rebuilt_dictionary;
    uint64_t rebuild_budget, rebuilt_bytes, deadline;
} wadb_index_reader;

typedef struct wadb_append_request {
    wadb_table *table;
    const wadb_value *values;
    unsigned char *rows;
    size_t row_count, row_bytes, charge;
    uint64_t submitted_ns, started_ns;
    int64_t ingestion_time;
    wadb_append_receipt receipt;
    wadb_error error;
    wadb_status status;
    bool done;
    pthread_cond_t ready;
    struct wadb_append_request *next;
} wadb_append_request;

struct wadb_table {
    wadb_db *db;
    uint64_t id;
    char name[WADB_NAME_CAP];
    char *directory;
    wadb_schema schema;
    uint64_t next_segment_id;
    uint64_t preserved_sequence;
    int64_t preserved_time;
    uint64_t retention_us;
    uint64_t manifest_generation;
    wadb_segment_meta *segments;
    size_t segment_count;
    int active_fd;
    uint64_t last_sequence;
    int64_t last_time;
    size_t readers;
    bool read_only;
    bool pending_retention;
    wadb_status maintenance_status;
    wadb_dictionary *dictionary;
    wadb_frame_index index;
    size_t *readable_segments;
    size_t readable_count, readable_capacity;
    uint64_t committed_frames, append_requests;
    uint64_t retained_rows, retained_bytes, retained_segments, retired_segments, published_dictionary_bytes;
};

struct wadb_db {
    char *directory;
    int lock_fd;
    pthread_mutex_t mutex;
    bool mutex_initialized;
    pthread_mutex_t writer_mutex;
    bool writer_mutex_initialized;
    bool read_only;
    pthread_t writer;
    pthread_cond_t work;
    bool work_initialized, writer_started, stopping;
    wadb_append_request *queue_head, *queue_tail;
    size_t queue_bytes, ready_bytes;
    uint64_t rejected_appends, failed_appends;
    size_t snapshot_bytes, reader_count;
    wadb_cache_entry *cache;
    size_t cache_bytes;
    uint64_t cache_clock, cache_hits, cache_misses;
    struct wadb_cursor_state *cursors;
    uint64_t next_cursor_id;
    uint64_t boot_ns, committed_rows, committed_bytes, committed_frames, append_requests, clock_adjustments;
    uint64_t queries, failed_queries, rows_scanned, bytes_scanned;
    wadb_histogram append_latency, sync_latency, query_latency;
    wadb_commit_event notifications[WADB_NOTIFICATION_CAPACITY];
    uint64_t latest_notification;
    wadb_options options;
    uint64_t next_table_id;
    wadb_table **tables;
    size_t table_count;
    wadb_io io;
    int64_t (*realtime)(void *context);
    void *clock_context;
};

char *wadb_path(const char *base, const char *name);
char *wadb_id_path(const char *base, uint64_t id);
wadb_status wadb_save_catalog(wadb_db *db, wadb_table *addition, wadb_error *error);
wadb_status wadb_load_catalog(wadb_db *db, wadb_error *error);
wadb_status wadb_save_schema(wadb_table *table, wadb_error *error);
wadb_status wadb_load_schema(wadb_table *table, wadb_error *error);
wadb_status wadb_save_manifest(wadb_table *table, wadb_error *error);
wadb_status wadb_load_manifest(wadb_table *table, wadb_error *error);
void wadb_table_free(wadb_table *table);
char *wadb_segment_path(wadb_table *table, const wadb_segment_meta *segment, const char *extension);
wadb_status wadb_storage_recover(wadb_table *table, wadb_error *error);
wadb_status wadb_commit_requests(wadb_table *table, wadb_append_request **requests,
    size_t count, wadb_error *error);
wadb_status wadb_writer_start(wadb_db *db, wadb_error *error);
void wadb_writer_stop(wadb_db *db);
int wadb_condition_init(pthread_cond_t *condition);
int wadb_condition_wait_until(pthread_cond_t *condition, pthread_mutex_t *mutex, uint64_t deadline_ns);
void wadb_index_free(wadb_frame_index *index);
wadb_status wadb_index_reserve(wadb_frame_index *index, wadb_error *error);
wadb_index_entry wadb_index_for_frame(const wadb_frame *frame, const unsigned char *bytes, uint64_t offset);
wadb_status wadb_index_save(wadb_table *table, const wadb_segment_meta *segment,
    const wadb_frame_index *index, const wadb_dictionary *dictionary, wadb_error *error);
wadb_status wadb_index_open(wadb_table *table, const wadb_segment_meta *segment,
    wadb_index_reader *reader, bool repair, wadb_error *error);
wadb_status wadb_index_open_bounded(wadb_table *table, const wadb_segment_meta *segment,
    wadb_index_reader *reader, uint64_t rebuild_budget, uint64_t deadline, wadb_error *error);
void wadb_index_close(wadb_index_reader *reader);
wadb_status wadb_index_get(wadb_index_reader *reader, size_t position, wadb_index_entry *entry, wadb_error *error);
wadb_status wadb_index_dictionary(wadb_index_reader *reader, wadb_dictionary **dictionary, wadb_error *error);
wadb_status wadb_index_lower_bound(wadb_index_reader *reader, int64_t time, uint64_t after_sequence,
    size_t *position, wadb_error *error);
wadb_status wadb_rebuild_index(wadb_table *table, const wadb_segment_meta *segment, wadb_error *error);
wadb_status wadb_rebuild_index_data(wadb_table *table, const wadb_segment_meta *segment,
    wadb_frame_index *index, wadb_dictionary **dictionary, uint64_t deadline, wadb_error *error);
wadb_status wadb_readable_reserve(wadb_table *table, wadb_error *error);
wadb_status wadb_snapshot_acquire(wadb_table *table, const wadb_scan_options *options,
    wadb_snapshot *snapshot, wadb_error *error);
void wadb_snapshot_release(wadb_snapshot *snapshot);
wadb_status wadb_scan_snapshot(const wadb_snapshot *snapshot, const wadb_scan_options *options,
    wadb_row_fn consume, void *context, wadb_scan_stats *stats, wadb_error *error);
wadb_status wadb_cache_acquire(wadb_index_reader *reader, wadb_cache_entry **entry, wadb_error *error);
void wadb_cache_release(wadb_db *db, wadb_cache_entry *entry);
void wadb_cache_clear(wadb_db *db);
void wadb_cursor_clear(wadb_db *db);
wadb_status wadb_retention_cleanup(wadb_table *table, uint64_t *deleted, wadb_error *error);
void wadb_histogram_record(wadb_histogram *histogram, uint64_t elapsed_ns);
void wadb_record_query(wadb_db *db, uint64_t started_ns, wadb_status status, const wadb_scan_stats *stats);
#endif
