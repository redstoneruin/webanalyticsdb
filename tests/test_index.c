#include "database_fixture.h"

static int64_t clock_value;
static int64_t controlled_clock(void *context) { (void)context; return clock_value; }
static wadb_table *new_table(bool symbols) {
    wadb_options o;
    wadb_options_default(&o); o.batch_delay_ms = 0;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, &o, &db, NULL));
    db->realtime = controlled_clock;
    wadb_field_def field = {"value", symbols ? WADB_SYMBOL32 : WADB_U32, 0, false};
    wadb_table *t;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_create_table(db, "samples", &field, 1, 0, &t, NULL));
    return t;
}
static void append_number(wadb_table *t, uint64_t n, int64_t time) {
    clock_value = time;
    wadb_value value = {.as.u64 = n};
    TEST_ASSERT_EQUAL(WADB_OK, wadb_append_batch(t, &value, 1, NULL, NULL));
}
typedef struct { uint64_t count, expected; bool symbols; wadb_table *rotate; } collector;
static wadb_status collect(void *context, uint64_t seq, const wadb_value *values, size_t count) {
    collector *c = context;
    if (count != 2 || seq != ++c->expected) return WADB_CORRUPT;
    if (c->symbols) {
        if (values[1].as.bytes.length != 3 || memcmp(values[1].as.bytes.data, "abc", 3)) return WADB_CORRUPT;
    } else if (values[1].as.u64 != seq) return WADB_CORRUPT;
    if (++c->count == 1 && c->rotate) {
        clock_value = WADB_DAY_US + 1;
        wadb_value value = {.as.u64 = 4};
        return wadb_append_batch(c->rotate, &value, 1, NULL, NULL);
    }
    return WADB_OK;
}
static void flip(const char *path, uint64_t offset) {
    int fd = open(path, O_RDWR);
    TEST_ASSERT_TRUE(fd >= 0);
    unsigned char byte;
    TEST_ASSERT_EQUAL_INT(1, pread(fd, &byte, 1, (off_t)offset)); byte ^= 0x40;
    TEST_ASSERT_EQUAL_INT(1, pwrite(fd, &byte, 1, (off_t)offset));
    close(fd);
}
static void test_narrow_queries_read_one_frame_before_and_after_sealing(void) {
    wadb_table *t = new_table(false);
    for (uint64_t i = 1; i <= 130; ++i) append_number(t, i, (int64_t)i);
    wadb_scan_options o;
    wadb_scan_options_default(&o); o.start_time = 66; o.end_time = 67;
    for (unsigned mode = 0; mode < 3; ++mode) {
        if (mode == 1) append_number(t, 131, WADB_DAY_US + 1);
        if (mode == 2) {
            wadb_close(db); db = NULL;
            TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, NULL, &db, NULL));
            TEST_ASSERT_EQUAL(WADB_OK, wadb_find_table(db, "samples", &t, NULL));
        }
        collector c = {.expected = 65};
        wadb_scan_stats stats;
        TEST_ASSERT_EQUAL(WADB_OK, wadb_scan(t, &o, collect, &c, &stats, NULL));
        TEST_ASSERT_EQUAL_UINT64(1, c.count);
        TEST_ASSERT_EQUAL_UINT64(1, stats.frames_scanned);
        TEST_ASSERT_EQUAL_UINT64(1, stats.rows_scanned);
        TEST_ASSERT_EQUAL_UINT64(112, stats.bytes_scanned);
        TEST_ASSERT_EQUAL_UINT64(1, stats.segments_scanned);
        TEST_ASSERT_TRUE(stats.index_bytes <= 5 * 4096);
    }
}
static void test_missing_and_damaged_index_rebuild_without_duplicate_rows(void) {
    wadb_table *t = new_table(false);
    for (uint64_t i = 1; i <= 130; ++i) append_number(t, i, (int64_t)i);
    append_number(t, 131, WADB_DAY_US + 1);
    char *path = wadb_segment_path(t, &t->segments[0], "idx");
    for (unsigned fault = 0; fault < 3; ++fault) {
        if (!fault) TEST_ASSERT_EQUAL_INT(0, unlink(path));
        else flip(path, fault == 1 ? 24 : WADB_INDEX_HEADER_BYTES + 65 * WADB_INDEX_ENTRY_BYTES + 40);
        collector c = {0};
        wadb_error error;
        TEST_ASSERT_EQUAL_MESSAGE(WADB_OK, wadb_scan(t, NULL, collect, &c, NULL, &error), error.message);
        TEST_ASSERT_EQUAL_UINT64(131, c.count);
    }
    free(path);
}
static void test_dictionary_checkpoint_rebuild_and_padding_validation(void) {
    wadb_table *t = new_table(true);
    wadb_value value = {.as.bytes = {"abc", 3}};
    clock_value = 1;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_append_batch(t, &value, 1, NULL, NULL));
    clock_value = WADB_DAY_US + 1;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_append_batch(t, &value, 1, NULL, NULL));
    char *path = wadb_segment_path(t, &t->segments[0], "idx");
    flip(path, WADB_INDEX_HEADER_BYTES + WADB_INDEX_ENTRY_BYTES + 12);
    collector c = {.symbols = true};
    TEST_ASSERT_EQUAL(WADB_OK, wadb_scan(t, NULL, collect, &c, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT64(2, c.count);
    /* Even a valid checksum cannot make noncanonical dictionary padding legal. */
    unsigned char definitions[16] = {0};
    wadb_put_u16(definitions, 1); wadb_put_u32(definitions + 4, 1);
    wadb_put_u32(definitions + 8, 3); memcpy(definitions + 12, "abc", 3); definitions[15] = 1;
    wadb_dictionary *dictionary;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_dictionary_new(&t->schema, 4096, &dictionary, NULL));
    wadb_frame frame = {.dictionary = definitions, .dictionary_bytes = sizeof(definitions), .dictionary_count = 1};
    TEST_ASSERT_EQUAL(WADB_CORRUPT, wadb_dictionary_apply(dictionary, &frame, NULL));
    TEST_ASSERT_EQUAL_UINT64(0, dictionary->count);
    wadb_dictionary_free(dictionary); free(path);
}
static void test_sealed_data_corruption_is_detected_on_read_and_not_repaired_as_index(void) {
    wadb_table *t = new_table(false);
    append_number(t, 1, 1); append_number(t, 2, WADB_DAY_US + 1);
    char *path = wadb_segment_path(t, &t->segments[0], "seg");
    uint64_t length = t->segments[0].committed_bytes;
    wadb_close(db); db = NULL;
    flip(path, WADB_SEGMENT_HEADER_BYTES + WADB_FRAME_HEADER_BYTES + 8);
    /* Startup validates sealed metadata; full historical payload checking is a separate operation. */
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, NULL, &db, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_find_table(db, "samples", &t, NULL));
    collector c = {0};
    TEST_ASSERT_EQUAL(WADB_CORRUPT, wadb_scan(t, NULL, collect, &c, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT64(0, c.count);
    struct stat st;
    TEST_ASSERT_EQUAL_INT(0, stat(path, &st));
    TEST_ASSERT_EQUAL_UINT64(length, st.st_size);
    free(path);
}
static void test_active_snapshot_survives_rotation_in_callback(void) {
    wadb_table *t = new_table(false);
    for (uint64_t i = 1; i <= 3; ++i) append_number(t, i, 1);
    collector c = {.rotate = t};
    wadb_scan_stats stats;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_scan(t, NULL, collect, &c, &stats, NULL));
    TEST_ASSERT_EQUAL_UINT64(3, c.count);
    TEST_ASSERT_EQUAL_UINT64(3, stats.snapshot_sequence);
    TEST_ASSERT_EQUAL_UINT64(2, t->segment_count);
    memset(&c, 0, sizeof(c));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_scan(t, NULL, collect, &c, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT64(4, c.count);
}
static void test_transient_index_keeps_reads_available_when_index_cannot_be_written(void) {
    wadb_table *t = new_table(true);
    wadb_value v = {.as.bytes = {"abc", 3}};
    clock_value = 1; TEST_ASSERT_EQUAL(WADB_OK, wadb_append_batch(t, &v, 1, NULL, NULL));
    clock_value = WADB_DAY_US + 1; TEST_ASSERT_EQUAL(WADB_OK, wadb_append_batch(t, &v, 1, NULL, NULL));
    char *path = wadb_segment_path(t, &t->segments[0], "idx");
    TEST_ASSERT_EQUAL_INT(0, unlink(path));
    /* A directory at the index path deterministically prevents atomic install,
     * without relying on machine-wide disk exhaustion or user privileges. */
    TEST_ASSERT_EQUAL_INT(0, mkdir(path, 0700));
    wadb_close(db); db = NULL;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, NULL, &db, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_find_table(db, "samples", &t, NULL));
    collector c = {.symbols = true};
    wadb_scan_stats stats;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_scan(t, NULL, collect, &c, &stats, NULL));
    TEST_ASSERT_EQUAL_UINT64(2, c.count);
    TEST_ASSERT_EQUAL_UINT64(t->segments[0].committed_bytes, stats.rebuild_bytes);
    TEST_ASSERT_FALSE(t->read_only);
    free(path);
}
static void test_index_rebuild_obeys_query_byte_budget(void) {
    wadb_table *t = new_table(false);
    for (unsigned i = 1; i <= 10; ++i) append_number(t, i, i);
    append_number(t, 11, WADB_DAY_US + 1);
    char *path = wadb_segment_path(t, &t->segments[0], "idx");
    TEST_ASSERT_EQUAL_INT(0, unlink(path));
    wadb_scan_options o;
    wadb_scan_options_default(&o); o.start_time = 1; o.end_time = 2; o.max_scan_bytes = 112;
    collector c = {0};
    TEST_ASSERT_EQUAL(WADB_LIMIT, wadb_scan(t, &o, collect, &c, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT64(0, c.count); TEST_ASSERT_EQUAL_INT(-1, access(path, F_OK));
    o.max_scan_bytes = 4096;
    wadb_scan_stats stats;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_scan(t, &o, collect, &c, &stats, NULL));
    TEST_ASSERT_EQUAL_UINT64(1, c.count); TEST_ASSERT_EQUAL_UINT64(1248, stats.rebuild_bytes);
    free(path);
}
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_narrow_queries_read_one_frame_before_and_after_sealing);
    RUN_TEST(test_missing_and_damaged_index_rebuild_without_duplicate_rows);
    RUN_TEST(test_dictionary_checkpoint_rebuild_and_padding_validation);
    RUN_TEST(test_sealed_data_corruption_is_detected_on_read_and_not_repaired_as_index);
    RUN_TEST(test_active_snapshot_survives_rotation_in_callback);
    RUN_TEST(test_transient_index_keeps_reads_available_when_index_cannot_be_written);
    RUN_TEST(test_index_rebuild_obeys_query_byte_budget);
    return UNITY_END();
}
