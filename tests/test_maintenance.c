#include "database_fixture.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>

static int64_t clock_value;
static int64_t controlled_clock(void *unused) { (void)unused; return clock_value; }
static wadb_table *new_table(uint64_t size) {
    wadb_options o;
    wadb_options_default(&o); o.batch_delay_ms = 0; o.segment_target_bytes = size;
    o.dictionary_limit_bytes = 2048; o.read_cache_bytes = 2048 + sizeof(wadb_cache_entry);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, &o, &db, NULL));
    db->realtime = controlled_clock;
    wadb_field_def field = {"value", WADB_U32, 0, false};
    wadb_table *t;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_create_table(db, "samples", &field, 1, 10, &t, NULL));
    return t;
}
static void append(wadb_table *t, int64_t time, uint64_t value) {
    clock_value = time;
    wadb_value v = {.as.u64 = value};
    TEST_ASSERT_EQUAL(WADB_OK, wadb_append_batch(t, &v, 1, NULL, NULL));
}
static void test_retirement_pins_existing_cursor_and_excludes_new_readers(void) {
    wadb_table *t = new_table(256);
    append(t, 1, 1); append(t, 2, 2); append(t, 20, 3);
    char *paths[2] = {wadb_segment_path(t, &t->segments[0], "seg"), wadb_segment_path(t, &t->segments[1], "seg")};
    wadb_query_options q;
    wadb_query_options_default(&q); q.scan.limit = 1;
    uint64_t cursor;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_cursor_open(t, &q, 10000, &cursor, NULL));
    wadb_result *r;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_cursor_next(db, cursor, &r, NULL));
    TEST_ASSERT_EQUAL_UINT64(1, r->sequences[0]); wadb_result_free(r);
    append(t, 21, 4);
    wadb_retention_stats stats;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_retention_preview(t, 25, &stats, NULL));
    TEST_ASSERT_EQUAL_UINT64(2, stats.eligible_segments); TEST_ASSERT_EQUAL_UINT64(2, stats.eligible_rows);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_retention_apply(t, 25, &stats, NULL));
    TEST_ASSERT_EQUAL_UINT64(0, stats.deleted_segments); TEST_ASSERT_EQUAL_UINT64(2, stats.pending_segments);
    for (unsigned i = 0; i < 2; ++i) TEST_ASSERT_EQUAL_INT(0, access(paths[i], F_OK));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_query(t, NULL, &r, NULL));
    TEST_ASSERT_EQUAL_UINT64(2, r->row_count); TEST_ASSERT_EQUAL_UINT64(3, r->sequences[0]); wadb_result_free(r);
    for (uint64_t seq = 2; seq <= 3; ++seq) {
        TEST_ASSERT_EQUAL(WADB_OK, wadb_cursor_next(db, cursor, &r, NULL));
        TEST_ASSERT_EQUAL_UINT64(seq, r->sequences[0]); wadb_result_free(r);
    }
    TEST_ASSERT_EQUAL_UINT64(0, t->readers); TEST_ASSERT_FALSE(t->pending_retention);
    for (unsigned i = 0; i < 2; ++i) { TEST_ASSERT_EQUAL_INT(-1, access(paths[i], F_OK)); TEST_ASSERT_EQUAL_INT(ENOENT, errno); free(paths[i]); }
    wadb_close(db); db = NULL;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, NULL, &db, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_find_table(db, "samples", &t, NULL));
    TEST_ASSERT_EQUAL_UINT64(4, t->last_sequence); TEST_ASSERT_EQUAL_UINT64(2, t->preserved_sequence);
}
static void test_partial_expiry_keeps_segment_and_full_expiry_preserves_watermarks(void) {
    wadb_table *t = new_table(4096);
    append(t, 1, 1); append(t, 20, 2);
    wadb_retention_stats stats;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_retention_apply(t, 25, &stats, NULL));
    TEST_ASSERT_EQUAL_UINT64(0, stats.eligible_segments); TEST_ASSERT_EQUAL_UINT64(1, stats.partially_expired_segments);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_retention_apply(t, 50, &stats, NULL));
    TEST_ASSERT_EQUAL_UINT64(1, stats.deleted_segments); TEST_ASSERT_EQUAL_INT(-1, t->active_fd);
    wadb_close(db); db = NULL;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, NULL, &db, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_find_table(db, "samples", &t, NULL));
    TEST_ASSERT_EQUAL_UINT64(2, t->last_sequence); TEST_ASSERT_EQUAL_INT64(20, t->last_time);
    wadb_result *r;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_query(t, NULL, &r, NULL)); TEST_ASSERT_EQUAL_UINT64(0, r->row_count); wadb_result_free(r);
    db->realtime = controlled_clock;
    append(t, 2, 3);
    TEST_ASSERT_EQUAL_UINT64(3, t->last_sequence); TEST_ASSERT_EQUAL_INT64(20, t->last_time);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_set_retention(t, 0, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_retention_apply(t, 10000, &stats, NULL));
    TEST_ASSERT_EQUAL_UINT64(0, stats.eligible_segments);
    wadb_close(db); db = NULL;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, NULL, &db, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_find_table(db, "samples", &t, NULL));
    TEST_ASSERT_EQUAL_UINT64(0, t->retention_us); TEST_ASSERT_EQUAL_UINT64(3, t->last_sequence);
}
static void test_restart_finishes_deletion_after_durable_tombstone(void) {
    wadb_table *t = new_table(4096);
    append(t, 1, 1);
    char *path = wadb_segment_path(t, &t->segments[0], "seg");
    wadb_close(db); db = NULL;
    fflush(NULL);
    pid_t pid = fork(); TEST_ASSERT_TRUE(pid >= 0);
    if (!pid) {
        wadb_db *child = NULL; wadb_table *table = NULL; uint64_t cursor;
        if (wadb_open(directory, NULL, &child, NULL) || wadb_find_table(child, "samples", &table, NULL) ||
            wadb_cursor_open(table, NULL, 300000, &cursor, NULL) || wadb_retention_apply(table, 100, NULL, NULL)) _exit(2);
        /* Deliberately terminate without close/releasing the cursor. */
        _exit(0);
    }
    int result; TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &result, 0));
    TEST_ASSERT_TRUE(WIFEXITED(result)); TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(result));
    TEST_ASSERT_EQUAL_INT(0, access(path, F_OK));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, NULL, &db, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_find_table(db, "samples", &t, NULL));
    TEST_ASSERT_EQUAL_INT(-1, access(path, F_OK)); TEST_ASSERT_EQUAL_INT(ENOENT, errno);
    TEST_ASSERT_EQUAL_UINT64(1, t->preserved_sequence); TEST_ASSERT_EQUAL_UINT64(0, t->readable_count);
    free(path);
}
static void test_dictionary_cache_budget_evicts_only_unpinned_entries(void) {
    wadb_table *t = new_table(256);
    append(t, 1, 1); append(t, 2, 2); append(t, 3, 3);
    wadb_index_reader first, second;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_index_open(t, &t->segments[0], &first, true, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_index_open(t, &t->segments[1], &second, true, NULL));
    wadb_cache_entry *a, *b, *same;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_cache_acquire(&first, &a, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_cache_acquire(&first, &same, NULL));
    TEST_ASSERT_EQUAL_PTR(a, same); TEST_ASSERT_EQUAL_UINT64(1, db->cache_hits);
    TEST_ASSERT_EQUAL(WADB_BACKPRESSURE, wadb_cache_acquire(&second, &b, NULL));
    wadb_cache_release(db, same); wadb_cache_release(db, a);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_cache_acquire(&second, &b, NULL));
    TEST_ASSERT_TRUE(db->cache_bytes <= db->options.read_cache_bytes);
    TEST_ASSERT_EQUAL_UINT64(t->segments[1].id, b->segment_id);
    wadb_cache_release(db, b); wadb_index_close(&first); wadb_index_close(&second);
}
static bool cancel(void *context) { unsigned *count = context; return ++*count >= 2; }
static void test_integrity_checks_authoritative_data_and_has_budgets(void) {
    wadb_table *t = new_table(256);
    append(t, 1, 1); append(t, 2, 2); append(t, 3, 3);
    wadb_integrity_stats stats;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_integrity_check(t, NULL, &stats, NULL));
    TEST_ASSERT_EQUAL_UINT64(3, stats.rows); TEST_ASSERT_EQUAL_UINT64(3, stats.segments); TEST_ASSERT_EQUAL_UINT64(3, stats.frames);
    wadb_integrity_options o;
    wadb_integrity_options_default(&o); o.max_scan_bytes = 1;
    TEST_ASSERT_EQUAL(WADB_LIMIT, wadb_integrity_check(t, &o, &stats, NULL));
    unsigned calls = 0; wadb_integrity_options_default(&o); o.cancelled = cancel; o.context = &calls;
    TEST_ASSERT_EQUAL(WADB_CANCELLED, wadb_integrity_check(t, &o, &stats, NULL));
    TEST_ASSERT_EQUAL_UINT64(0, t->readers); TEST_ASSERT_FALSE(t->read_only);
    wadb_result *r; TEST_ASSERT_EQUAL(WADB_OK, wadb_query(t, NULL, &r, NULL)); wadb_result_free(r);
    char *path = wadb_segment_path(t, &t->segments[0], "seg");
    int fd = open(path, O_RDWR); TEST_ASSERT_TRUE(fd >= 0);
    unsigned char byte = 9; TEST_ASSERT_EQUAL_INT(1, pwrite(fd, &byte, 1, 216)); close(fd);
    TEST_ASSERT_EQUAL(WADB_CORRUPT, wadb_integrity_check(t, NULL, &stats, NULL));
    wadb_value value = {.as.u64 = 4};
    TEST_ASSERT_EQUAL(WADB_READ_ONLY, wadb_append_batch(t, &value, 1, NULL, NULL));
    struct stat st; TEST_ASSERT_EQUAL_INT(0, stat(path, &st)); TEST_ASSERT_EQUAL_UINT64(240, st.st_size);
    free(path);
}
typedef struct { wadb_table *table; atomic_int status; } concurrent_writer;
static void *write_during_retention(void *context) {
    concurrent_writer *writer = context;
    for (unsigned i = 2; i <= 40; ++i) {
        clock_value = i;
        wadb_value value = {.as.u64 = i};
        wadb_status status = wadb_append_batch(writer->table, &value, 1, NULL, NULL);
        if (status) { atomic_store(&writer->status, status); break; }
    }
    return NULL;
}
static void test_concurrent_retention_appends_and_snapshot_reads(void) {
    wadb_table *t = new_table(256);
    append(t, 1, 1);
    uint64_t cursor;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_cursor_open(t, NULL, 30000, &cursor, NULL));
    concurrent_writer writer = {.table = t}; atomic_init(&writer.status, 0);
    pthread_t thread; TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, NULL, write_during_retention, &writer));
    for (unsigned i = 0; i < 20; ++i) {
        wadb_result *r;
        TEST_ASSERT_EQUAL(WADB_OK, wadb_query(t, NULL, &r, NULL));
        for (size_t row = 0; row < r->row_count; ++row) {
            TEST_ASSERT_EQUAL_UINT64(r->sequences[row], r->values[row * 2 + 1].as.u64);
            if (row) TEST_ASSERT_EQUAL_UINT64(r->sequences[row - 1] + 1, r->sequences[row]);
        }
        wadb_result_free(r);
        TEST_ASSERT_EQUAL(WADB_OK, wadb_retention_apply(t, 1000, NULL, NULL));
    }
    TEST_ASSERT_EQUAL_INT(0, pthread_join(thread, NULL)); TEST_ASSERT_EQUAL_INT(0, atomic_load(&writer.status));
    wadb_result *r;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_cursor_next(db, cursor, &r, NULL));
    TEST_ASSERT_EQUAL_UINT64(1, r->row_count); TEST_ASSERT_EQUAL_UINT64(1, r->sequences[0]); wadb_result_free(r);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_retention_apply(t, 1000, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT64(40, t->last_sequence); TEST_ASSERT_EQUAL_UINT64(40, t->preserved_sequence);
    TEST_ASSERT_EQUAL_UINT64(0, t->readers); TEST_ASSERT_FALSE(t->pending_retention);
    wadb_close(db); db = NULL;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, NULL, &db, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_find_table(db, "samples", &t, NULL));
    TEST_ASSERT_EQUAL_UINT64(40, t->last_sequence);
}
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_retirement_pins_existing_cursor_and_excludes_new_readers);
    RUN_TEST(test_partial_expiry_keeps_segment_and_full_expiry_preserves_watermarks);
    RUN_TEST(test_restart_finishes_deletion_after_durable_tombstone);
    RUN_TEST(test_dictionary_cache_budget_evicts_only_unpinned_entries);
    RUN_TEST(test_integrity_checks_authoritative_data_and_has_budgets);
    RUN_TEST(test_concurrent_retention_appends_and_snapshot_reads);
    return UNITY_END();
}
