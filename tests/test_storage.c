#include "database_fixture.h"
#include <errno.h>

static int64_t clock_value;
static int64_t controlled_clock(void *context) { (void)context; return clock_value; }
static wadb_table *new_table(uint64_t segment_size) {
    wadb_options o;
    wadb_options_default(&o); o.segment_target_bytes = segment_size;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, &o, &db, NULL));
    db->realtime = controlled_clock;
    clock_value = 100;
    wadb_field_def field = {"value", WADB_U32, 0, false};
    wadb_table *t;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_create_table(db, "samples", &field, 1, 0, &t, NULL));
    return t;
}
typedef struct {
    uint64_t sequences[64];
    uint64_t values[64];
    int64_t times[64];
    size_t count;
    wadb_table *append_on_first;
} collected;
static wadb_status collect(void *context, uint64_t seq, const wadb_value *values, size_t count) {
    collected *c = context;
    if (count != 2 || c->count >= 64) return WADB_INVALID;
    c->sequences[c->count] = seq; c->values[c->count] = values[1].as.u64;
    c->times[c->count++] = values[0].as.i64;
    if (c->count == 1 && c->append_on_first) {
        wadb_value v = {.as.u64 = 99};
        return wadb_append_batch(c->append_on_first, &v, 1, NULL, NULL);
    }
    return WADB_OK;
}

static void test_durable_batches_rotation_clock_rollback_and_ranges(void) {
    wadb_table *t = new_table(256);
    wadb_value values[2] = {{.as.u64 = 11}, {.as.u64 = 12}};
    wadb_append_receipt receipt;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_append_batch(t, values, 2, &receipt, NULL));
    TEST_ASSERT_EQUAL_UINT64(1, receipt.first_sequence);
    values[0].as.u64 = 21; values[1].as.u64 = 22;
    clock_value = 50;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_append_batch(t, values, 2, &receipt, NULL));
    TEST_ASSERT_EQUAL_INT64(100, receipt.ingestion_time);
    TEST_ASSERT_EQUAL_UINT32(2, t->segment_count);
    wadb_close(db); db = NULL;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, NULL, &db, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_find_table(db, "samples", &t, NULL));
    db->realtime = controlled_clock;
    values[0].as.u64 = 31; values[1].as.u64 = 32;
    clock_value = WADB_DAY_US + 10;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_append_batch(t, values, 2, &receipt, NULL));
    TEST_ASSERT_EQUAL_UINT64(5, receipt.first_sequence);
    TEST_ASSERT_EQUAL_UINT32(3, t->segment_count);
    collected c = {0};
    wadb_scan_options scan;
    wadb_scan_options_default(&scan); scan.start_time = 100; scan.end_time = 101;
    wadb_scan_stats stats;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_scan(t, &scan, collect, &c, &stats, NULL));
    TEST_ASSERT_EQUAL_UINT32(4, c.count);
    const uint64_t expected[] = {11,12,21,22};
    for (size_t i = 0; i < c.count; ++i) {
        TEST_ASSERT_EQUAL_UINT64(i + 1, c.sequences[i]);
        TEST_ASSERT_EQUAL_UINT64(expected[i], c.values[i]);
        TEST_ASSERT_EQUAL_INT64(100, c.times[i]);
    }
    memset(&c, 0, sizeof(c)); scan.end_time = 100;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_scan(t, &scan, collect, &c, &stats, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, c.count);
    wadb_scan_options_default(&scan); scan.after_sequence = 4;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_scan(t, &scan, collect, &c, &stats, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, c.count);
    TEST_ASSERT_EQUAL_UINT64(31, c.values[0]);
}

static wadb_status reject_row(void *ctx, uint64_t seq, const wadb_value *values, size_t count) {
    (void)ctx; (void)seq; (void)values; (void)count;
    return WADB_CORRUPT; /* Application callback failure is not storage corruption. */
}
static void test_scan_snapshot_excludes_reentrant_append(void) {
    wadb_table *t = new_table(1024);
    wadb_value values[2] = {{.as.u64 = 1}, {.as.u64 = 2}};
    TEST_ASSERT_EQUAL(WADB_OK, wadb_append_batch(t, values, 2, NULL, NULL));
    collected c = {.append_on_first = t};
    wadb_scan_stats stats;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_scan(t, NULL, collect, &c, &stats, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, c.count);
    TEST_ASSERT_EQUAL_UINT64(2, stats.snapshot_sequence);
    memset(&c, 0, sizeof(c));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_scan(t, NULL, collect, &c, &stats, NULL));
    TEST_ASSERT_EQUAL_UINT32(3, c.count);
    TEST_ASSERT_EQUAL_UINT64(99, c.values[2]);
    TEST_ASSERT_EQUAL(WADB_CORRUPT, wadb_scan(t, NULL, reject_row, NULL, NULL, NULL));
    TEST_ASSERT_FALSE(t->read_only);
}

static void test_whole_request_validation_and_scan_limits(void) {
    wadb_table *t = new_table(1024);
    wadb_value values[2] = {{.as.u64 = 1}, {.as.u64 = UINT64_MAX}};
    TEST_ASSERT_EQUAL(WADB_INVALID, wadb_append_batch(t, values, 2, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, t->segment_count);
    TEST_ASSERT_EQUAL_UINT64(0, t->last_sequence);
    values[1].as.u64 = 2;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_append_batch(t, values, 2, NULL, NULL));
    wadb_scan_options scan;
    wadb_scan_options_default(&scan); scan.limit = 1;
    collected c = {0};
    wadb_scan_stats stats;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_scan(t, &scan, collect, &c, &stats, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, c.count);
    TEST_ASSERT_TRUE(stats.limit_reached);
    scan.limit = 10; scan.max_scan_bytes = 1;
    TEST_ASSERT_EQUAL(WADB_LIMIT, wadb_scan(t, &scan, collect, &c, &stats, NULL));
}

static ssize_t torn_write(void *context, int fd, const void *data, size_t n) {
    size_t *remaining = context;
    if (!*remaining) { errno = ENOSPC; return -1; }
    if (n > *remaining) n = *remaining;
    ssize_t result = write(fd, data, n);
    if (result > 0) *remaining -= (size_t)result;
    return result;
}
static int reject_sync(void *context, int fd) { (void)context; (void)fd; errno = EIO; return -1; }

static void test_partial_append_recovers_only_complete_prefix(void) {
    wadb_table *t = new_table(4096);
    wadb_value values[2] = {{.as.u64 = 1}, {.as.u64 = 2}};
    TEST_ASSERT_EQUAL(WADB_OK, wadb_append_batch(t, values, 2, NULL, NULL));
    size_t remaining = 110;
    db->io = (wadb_io){.context = &remaining, .write = torn_write};
    values[0].as.u64 = 3; values[1].as.u64 = 4;
    TEST_ASSERT_EQUAL(WADB_INDETERMINATE, wadb_append_batch(t, values, 2, NULL, NULL));
    TEST_ASSERT_EQUAL(WADB_READ_ONLY, wadb_append_batch(t, values, 2, NULL, NULL));
    collected c = {0};
    TEST_ASSERT_EQUAL(WADB_OK, wadb_scan(t, NULL, collect, &c, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, c.count);
    wadb_close(db); db = NULL;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, NULL, &db, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_find_table(db, "samples", &t, NULL));
    TEST_ASSERT_EQUAL_UINT64(2, t->last_sequence);
    TEST_ASSERT_EQUAL_UINT64(256, t->segments[0].committed_bytes);
    wadb_append_receipt receipt;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_append_batch(t, values, 2, &receipt, NULL));
    TEST_ASSERT_EQUAL_UINT64(3, receipt.first_sequence);
}

static void test_complete_uncertain_append_can_survive_recovery(void) {
    wadb_table *t = new_table(4096);
    wadb_value value = {.as.u64 = 42};
    TEST_ASSERT_EQUAL(WADB_OK, wadb_append_batch(t, &value, 1, NULL, NULL));
    db->io.sync = reject_sync;
    value.as.u64 = 43;
    TEST_ASSERT_EQUAL(WADB_INDETERMINATE, wadb_append_batch(t, &value, 1, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT64(1, t->last_sequence);
    wadb_close(db); db = NULL;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, NULL, &db, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_find_table(db, "samples", &t, NULL));
    TEST_ASSERT_EQUAL_UINT64(2, t->last_sequence);
    collected c = {0};
    TEST_ASSERT_EQUAL(WADB_OK, wadb_scan(t, NULL, collect, &c, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT64(43, c.values[1]);
}

static void test_complete_corruption_preserves_file_and_fails_open(void) {
    wadb_table *t = new_table(4096);
    wadb_value value = {.as.u64 = 7};
    TEST_ASSERT_EQUAL(WADB_OK, wadb_append_batch(t, &value, 1, NULL, NULL));
    char *path = wadb_segment_path(t, &t->segments[0], "seg");
    uint64_t length = t->segments[0].committed_bytes;
    wadb_close(db); db = NULL;
    int fd = open(path, O_RDWR);
    TEST_ASSERT_TRUE(fd >= 0);
    unsigned char byte = 8;
    TEST_ASSERT_EQUAL_INT(1, pwrite(fd, &byte, 1, 216));
    close(fd);
    TEST_ASSERT_EQUAL(WADB_CORRUPT, wadb_open(directory, NULL, &db, NULL));
    struct stat st;
    TEST_ASSERT_EQUAL_INT(0, stat(path, &st));
    TEST_ASSERT_EQUAL_UINT64(length, st.st_size);
    free(path);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_durable_batches_rotation_clock_rollback_and_ranges);
    RUN_TEST(test_scan_snapshot_excludes_reentrant_append);
    RUN_TEST(test_whole_request_validation_and_scan_limits);
    RUN_TEST(test_partial_append_recovers_only_complete_prefix);
    RUN_TEST(test_complete_uncertain_append_can_survive_recovery);
    RUN_TEST(test_complete_corruption_preserves_file_and_fails_open);
    return UNITY_END();
}
