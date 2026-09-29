#include "database_fixture.h"
#include <errno.h>

static int64_t clock_value;
static int64_t controlled_clock(void *unused) { (void)unused; return clock_value; }
static wadb_table *new_table(uint64_t segment_bytes) {
    wadb_options o;
    wadb_options_default(&o); o.batch_delay_ms = 0; o.segment_target_bytes = segment_bytes;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, &o, &db, NULL));
    db->realtime = controlled_clock;
    wadb_field_def field = {"value", WADB_U32, 0, false};
    wadb_table *t;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_create_table(db, "samples", &field, 1, 10, &t, NULL));
    return t;
}
static void append(wadb_table *t, uint64_t number, int64_t time) {
    clock_value = time;
    wadb_value v = {.as.u64 = number};
    TEST_ASSERT_EQUAL(WADB_OK, wadb_append_batch(t, &v, 1, NULL, NULL));
}
static void test_statistics_follow_commits_queries_retirement_and_restart(void) {
    wadb_table *t = new_table(256);
    append(t, 1, 100); append(t, 2, 50);
    wadb_result *r;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_query(t, NULL, &r, NULL)); wadb_result_free(r);
    wadb_query_options q;
    wadb_query_options_default(&q); q.scan.max_scan_bytes = 1;
    TEST_ASSERT_EQUAL(WADB_LIMIT, wadb_query(t, &q, &r, NULL));
    wadb_stats stats;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_get_stats(db, &stats, NULL));
    TEST_ASSERT_EQUAL_UINT64(2, stats.committed_rows); TEST_ASSERT_EQUAL_UINT64(2, stats.committed_frames);
    TEST_ASSERT_EQUAL_UINT64(224, stats.committed_bytes); TEST_ASSERT_EQUAL_UINT64(2, stats.append_requests);
    TEST_ASSERT_EQUAL_UINT64(2, stats.append_latency.samples); TEST_ASSERT_EQUAL_UINT64(2, stats.sync_latency.samples);
    TEST_ASSERT_EQUAL_UINT64(1, stats.clock_adjustments); TEST_ASSERT_EQUAL_UINT64(2, stats.queries);
    TEST_ASSERT_EQUAL_UINT64(1, stats.failed_queries); TEST_ASSERT_EQUAL_UINT64(2, stats.query_latency.samples);
    TEST_ASSERT_EQUAL_UINT64(0, stats.queue_bytes); TEST_ASSERT_EQUAL_UINT64(0, stats.readers);
    TEST_ASSERT_TRUE(stats.append_latency.p50_us <= stats.append_latency.p99_us);
    wadb_table_stats table;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_get_table_stats(t, &table, NULL));
    TEST_ASSERT_EQUAL_UINT64(2, table.rows); TEST_ASSERT_EQUAL_UINT64(480, table.bytes);
    TEST_ASSERT_EQUAL_UINT64(2, table.segments); TEST_ASSERT_EQUAL_INT64(100, table.min_time);
    wadb_segment_info segment; size_t segment_count; bool has_more;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_list_segments(t, 0, &segment, 1, &segment_count, &has_more, NULL));
    TEST_ASSERT_EQUAL_UINT64(1, segment_count); TEST_ASSERT_TRUE(has_more); TEST_ASSERT_EQUAL(WADB_SEG_SEALED, segment.state);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_list_segments(t, segment.id, &segment, 1, &segment_count, &has_more, NULL));
    TEST_ASSERT_EQUAL_UINT64(1, segment_count); TEST_ASSERT_FALSE(has_more); TEST_ASSERT_EQUAL(WADB_SEG_ACTIVE, segment.state);
    TEST_ASSERT_EQUAL_UINT64(2, segment.first_sequence);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_retention_apply(t, 200, NULL, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_get_table_stats(t, &table, NULL));
    TEST_ASSERT_EQUAL_UINT64(0, table.rows); TEST_ASSERT_EQUAL_UINT64(0, table.bytes);
    TEST_ASSERT_EQUAL_UINT64(0, table.segments); TEST_ASSERT_EQUAL_UINT64(2, table.retired_segments);
    TEST_ASSERT_EQUAL_UINT64(0, table.dictionary_bytes); TEST_ASSERT_EQUAL_UINT64(2, table.last_sequence);
    wadb_close(db); db = NULL;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, NULL, &db, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_find_table(db, "samples", &t, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_get_stats(db, &stats, NULL));
    TEST_ASSERT_EQUAL_UINT64(0, stats.committed_rows); TEST_ASSERT_EQUAL_UINT64(0, stats.latest_notification);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_get_table_stats(t, &table, NULL));
    TEST_ASSERT_EQUAL_UINT64(2, table.last_sequence); TEST_ASSERT_EQUAL_UINT64(2, table.retired_segments);
    db->realtime = controlled_clock; append(t, 3, 1);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_get_table_stats(t, &table, NULL));
    TEST_ASSERT_EQUAL_UINT64(1, table.rows); TEST_ASSERT_EQUAL_UINT64(240, table.bytes);
    TEST_ASSERT_EQUAL_UINT64(1, table.segments); TEST_ASSERT_EQUAL_INT64(100, table.last_assigned_time);
}
static int uncertain_sync(void *unused, int fd) { (void)unused; (void)fd; errno = EIO; return -1; }
static void test_only_durable_published_frames_produce_notifications(void) {
    wadb_table *t = new_table(4096);
    append(t, 1, 1);
    db->io.sync = uncertain_sync;
    wadb_value v = {.as.u64 = 2};
    TEST_ASSERT_EQUAL(WADB_INDETERMINATE, wadb_append_batch(t, &v, 1, NULL, NULL));
    wadb_commit_event events[4]; size_t count; uint64_t latest; bool gap;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_read_notifications(db, 0, events, 4, &count, &latest, &gap, NULL));
    TEST_ASSERT_EQUAL_UINT64(1, count); TEST_ASSERT_EQUAL_UINT64(1, latest); TEST_ASSERT_FALSE(gap);
    TEST_ASSERT_EQUAL_UINT64(t->id, events[0].table_id); TEST_ASSERT_EQUAL_UINT64(1, events[0].last_sequence);
    wadb_stats stats; TEST_ASSERT_EQUAL(WADB_OK, wadb_get_stats(db, &stats, NULL));
    TEST_ASSERT_EQUAL_UINT64(1, stats.committed_rows); TEST_ASSERT_EQUAL_UINT64(1, stats.failed_appends);
    TEST_ASSERT_EQUAL_UINT64(1, stats.append_latency.samples); TEST_ASSERT_EQUAL_UINT64(2, stats.sync_latency.samples);
    TEST_ASSERT_TRUE(stats.read_only);
}
static int instant_sync(void *unused, int fd) { (void)unused; (void)fd; return 0; }
static void test_monitor_ring_overwrites_metadata_without_blocking_appends(void) {
    wadb_table *t = new_table(1024 * 1024);
    /* This scheduling/observability unit test models an instantly durable device.
     * The storage failure tests exercise actual file synchronization separately. */
    db->io.sync = instant_sync;
    for (uint64_t i = 1; i <= WADB_NOTIFICATION_CAPACITY + 5; ++i) append(t, i, 1);
    wadb_commit_event events[4]; size_t count; uint64_t latest; bool gap;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_read_notifications(db, 0, events, 4, &count, &latest, &gap, NULL));
    TEST_ASSERT_TRUE(gap); TEST_ASSERT_EQUAL_UINT64(WADB_NOTIFICATION_CAPACITY + 5, latest);
    TEST_ASSERT_EQUAL_UINT64(4, count);
    for (size_t i = 0; i < count; ++i) {
        TEST_ASSERT_EQUAL_UINT64(i + 6, events[i].id); TEST_ASSERT_EQUAL_UINT64(i + 6, events[i].first_sequence);
    }
    TEST_ASSERT_EQUAL(WADB_OK, wadb_read_notifications(db, latest, events, 4, &count, &latest, &gap, NULL));
    TEST_ASSERT_FALSE(gap); TEST_ASSERT_EQUAL_UINT64(0, count);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_read_notifications(db, latest + 1, events, 4, &count, &latest, &gap, NULL));
    TEST_ASSERT_TRUE(gap); TEST_ASSERT_EQUAL_UINT64(0, count);
    wadb_table_stats table; TEST_ASSERT_EQUAL(WADB_OK, wadb_get_table_stats(t, &table, NULL));
    TEST_ASSERT_EQUAL_UINT64(WADB_NOTIFICATION_CAPACITY + 5, table.rows);
}
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_statistics_follow_commits_queries_retirement_and_restart);
    RUN_TEST(test_only_durable_published_frames_produce_notifications);
    RUN_TEST(test_monitor_ring_overwrites_metadata_without_blocking_appends);
    return UNITY_END();
}
