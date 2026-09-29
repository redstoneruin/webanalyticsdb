#include "database_fixture.h"
#include "../src/query/query.h"
#include <float.h>
#include <math.h>
#include <time.h>

static int64_t clock_value;
static int64_t controlled_clock(void *unused) { (void)unused; return clock_value; }
static wadb_table *new_table(uint64_t size) {
    wadb_options o;
    wadb_options_default(&o); o.batch_delay_ms = 0; o.segment_target_bytes = size;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, &o, &db, NULL));
    db->realtime = controlled_clock;
    wadb_field_def fields[] = {{"site", WADB_U32, 0, false}, {"path", WADB_SYMBOL32, 0, false},
        {"value", WADB_I64, 0, true}, {"amount", WADB_F64, 0, false}};
    wadb_table *t;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_create_table(db, "samples", fields, 4, 0, &t, NULL));
    return t;
}
static void append(wadb_table *t, int64_t time, uint64_t site, const char *path, int64_t value, bool null) {
    clock_value = time;
    wadb_value values[] = {{.as.u64 = site}, {.as.bytes = {path, strlen(path)}},
        {.is_null = null, .as.i64 = value}, {.as.f64 = (double)value / 2}};
    TEST_ASSERT_EQUAL(WADB_OK, wadb_append_batch(t, values, 1, NULL, NULL));
}
static wadb_result *query(wadb_table *t, const wadb_query_options *q) {
    wadb_result *r;
    wadb_error e;
    TEST_ASSERT_EQUAL_MESSAGE(WADB_OK, wadb_query(t, q, &r, &e), e.message);
    return r;
}
static wadb_aggregate reductions[] = {{WADB_COUNT_ALL, NULL, "rows"}, {WADB_COUNT, "value", "values"},
    {WADB_SUM, "value", "sum"}, {WADB_MIN, "value", "min"}, {WADB_MAX, "value", "max"}, {WADB_AVG, "value", "average"}};
static void aggregates(wadb_query_options *q) {
    wadb_query_options_default(q); q->aggregates = reductions; q->aggregate_count = 6;
}
static void test_projection_filters_and_owned_results(void) {
    wadb_table *t = new_table(256);
    append(t, 1, 1, "/a", 2, false); append(t, 2, 2, "/b", 4, true); append(t, 3, 2, "/a", 6, false);
    wadb_query_options q;
    wadb_query_options_default(&q);
    const char *projection[] = {"path", "value"}; q.projection = projection; q.projection_count = 2;
    wadb_filter filters[] = {{"site", WADB_GE, {.as.u64 = 2}}, {"path", WADB_EQ, {.as.bytes = {"/a", 2}}}};
    q.filters = filters; q.filter_count = 2;
    wadb_result *r = query(t, &q);
    TEST_ASSERT_EQUAL_UINT64(1, r->row_count);
    TEST_ASSERT_EQUAL_UINT64(3, r->sequences[0]);
    TEST_ASSERT_EQUAL_INT64(6, r->values[1].as.i64);
    wadb_close(db); db = NULL;
    TEST_ASSERT_EQUAL_MEMORY("/a", r->values[0].as.bytes.data, 2);
    wadb_result_free(r);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, NULL, &db, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_find_table(db, "samples", &t, NULL));
    filters[0].value.as.u64 = UINT64_MAX;
    TEST_ASSERT_EQUAL(WADB_INVALID, wadb_query(t, &q, &r, NULL)); TEST_ASSERT_NULL(r);
    filters[0] = (wadb_filter){"value", WADB_IS_NULL, {0}}; q.filter_count = 1;
    r = query(t, &q); TEST_ASSERT_EQUAL_UINT64(1, r->row_count); TEST_ASSERT_TRUE(r->values[1].is_null);
    wadb_result_free(r);
}
static void test_grouping_merges_values_across_segment_dictionaries(void) {
    wadb_table *t = new_table(256);
    append(t, 1, 1, "/a", 2, false); append(t, 2, 1, "/b", 100, true);
    append(t, 3, 1, "/b", 8, false); append(t, 4, 1, "/a", 4, false); append(t, 5, 1, "/a", 9, false);
    wadb_query_options q; aggregates(&q);
    const char *groups[] = {"path"}; q.group_by = groups; q.group_count = 1;
    q.order_by = "rows"; q.descending = true;
    wadb_result *r = query(t, &q);
    TEST_ASSERT_EQUAL_UINT64(2, r->row_count); TEST_ASSERT_EQUAL_UINT64(5, r->matched_rows);
    TEST_ASSERT_EQUAL_MEMORY("/a", r->values[0].as.bytes.data, 2);
    TEST_ASSERT_EQUAL_UINT64(3, r->values[1].as.u64);
    TEST_ASSERT_EQUAL_UINT64(3, r->values[2].as.u64);
    TEST_ASSERT_EQUAL_INT64(15, r->values[3].as.i64);
    TEST_ASSERT_EQUAL_INT64(2, r->values[4].as.i64); TEST_ASSERT_EQUAL_INT64(9, r->values[5].as.i64);
    TEST_ASSERT_TRUE(fabs(r->values[6].as.f64 - 5) < 1e-12);
    TEST_ASSERT_EQUAL_UINT64(2, r->values[8].as.u64); TEST_ASSERT_EQUAL_UINT64(1, r->values[9].as.u64);
    TEST_ASSERT_EQUAL_INT64(8, r->values[10].as.i64);
    wadb_result_free(r);
    q.scan.limit = 1; r = query(t, &q);
    TEST_ASSERT_EQUAL_UINT64(1, r->row_count); TEST_ASSERT_EQUAL_UINT64(2, r->total_groups); wadb_result_free(r);
}
static void test_buckets_origin_empty_intervals_and_null_aggregates(void) {
    wadb_table *t = new_table(4096);
    append(t, 0, 1, "a", 2, false); append(t, 5, 1, "a", 4, true);
    append(t, 15, 1, "a", 6, false); append(t, 35, 1, "a", 8, false);
    wadb_query_options q; aggregates(&q);
    q.scan.start_time = 0; q.scan.end_time = 46; q.bucket_width_us = 10; q.bucket_origin_us = 5;
    wadb_result *r = query(t, &q);
    TEST_ASSERT_EQUAL_UINT64(6, r->row_count);
    for (size_t i = 0; i < 6; ++i) TEST_ASSERT_EQUAL_INT64((int64_t)i * 10 - 5, r->values[i * 7].as.i64);
    TEST_ASSERT_EQUAL_UINT64(1, r->values[8].as.u64); TEST_ASSERT_EQUAL_UINT64(0, r->values[9].as.u64);
    for (size_t i = 10; i <= 13; ++i) TEST_ASSERT_TRUE(r->values[i].is_null);
    TEST_ASSERT_EQUAL_UINT64(0, r->values[22].as.u64); TEST_ASSERT_TRUE(r->values[24].is_null);
    wadb_result_free(r);
    q.scan.end_time = q.scan.start_time;
    r = query(t, &q); TEST_ASSERT_EQUAL_UINT64(0, r->row_count); wadb_result_free(r);
    q.bucket_width_us = 0;
    r = query(t, &q); TEST_ASSERT_EQUAL_UINT64(1, r->row_count); TEST_ASSERT_EQUAL_UINT64(0, r->values[0].as.u64);
    TEST_ASSERT_TRUE(r->values[2].is_null); wadb_result_free(r);
    q.scan.start_time = INT64_MIN; q.scan.end_time = 0; q.bucket_width_us = INT64_MAX; q.bucket_origin_us = 2;
    TEST_ASSERT_EQUAL(WADB_LIMIT, wadb_query(t, &q, &r, NULL)); TEST_ASSERT_NULL(r);
}
static void test_resource_limits_return_no_partial_aggregate(void) {
    wadb_table *t = new_table(4096);
    append(t, 1, 1, "a", 1, false); append(t, 2, 2, "b", 2, false);
    wadb_query_options q; aggregates(&q);
    const char *groups[] = {"site"}; q.group_by = groups; q.group_count = 1; q.max_groups = 1;
    wadb_result *r;
    TEST_ASSERT_EQUAL(WADB_LIMIT, wadb_query(t, &q, &r, NULL)); TEST_ASSERT_NULL(r);
    q.max_groups = 100; q.memory_limit_bytes = sizeof(wadb_query_plan) + 100;
    TEST_ASSERT_EQUAL(WADB_LIMIT, wadb_query(t, &q, &r, NULL)); TEST_ASSERT_NULL(r);
    aggregates(&q); q.scan.max_scan_bytes = 1;
    TEST_ASSERT_EQUAL(WADB_LIMIT, wadb_query(t, &q, &r, NULL)); TEST_ASSERT_NULL(r);
    aggregates(&q); q.bucket_width_us = 1; q.scan.end_time = 11; q.max_buckets = 10;
    TEST_ASSERT_EQUAL(WADB_LIMIT, wadb_query(t, &q, &r, NULL)); TEST_ASSERT_NULL(r);
    TEST_ASSERT_EQUAL_UINT64(0, db->reader_count); TEST_ASSERT_EQUAL_UINT64(0, db->snapshot_bytes);
}
static void test_integer_and_float_overflow_are_explicit(void) {
    wadb_table *t = new_table(4096);
    append(t, 1, 1, "a", INT64_MAX, false); append(t, 2, 1, "a", 1, false);
    wadb_query_options q; aggregates(&q);
    wadb_result *r;
    TEST_ASSERT_EQUAL(WADB_LIMIT, wadb_query(t, &q, &r, NULL)); TEST_ASSERT_NULL(r);
    wadb_field_def fields[] = {{"value", WADB_U64, 0, false}, {"amount", WADB_F64, 0, false}};
    TEST_ASSERT_EQUAL(WADB_OK, wadb_create_table(db, "large", fields, 2, 0, &t, NULL));
    wadb_value values[] = {{.as.u64 = UINT64_MAX}, {.as.f64 = DBL_MAX}, {.as.u64 = 1}, {.as.f64 = DBL_MAX}};
    TEST_ASSERT_EQUAL(WADB_OK, wadb_append_batch(t, values, 2, NULL, NULL));
    wadb_aggregate a = {WADB_SUM, "value", "sum"}; q.aggregates = &a; q.aggregate_count = 1;
    TEST_ASSERT_EQUAL(WADB_LIMIT, wadb_query(t, &q, &r, NULL)); TEST_ASSERT_NULL(r);
    a.field = "amount";
    TEST_ASSERT_EQUAL(WADB_LIMIT, wadb_query(t, &q, &r, NULL)); TEST_ASSERT_NULL(r);
    a.op = WADB_AVG;
    r = query(t, &q); TEST_ASSERT_TRUE(isfinite(r->values[0].as.f64)); TEST_ASSERT_TRUE(r->values[0].as.f64 == DBL_MAX);
    wadb_result_free(r);
}
typedef struct { uint64_t count, nonnull; int64_t sum, min, max; } reference;
static void test_seeded_aggregates_match_reference_across_rotations_and_ties(void) {
    wadb_table *t = new_table(1024);
    reference expected[11][8] = {{{0}}};
    uint32_t seed = 123456789;
    for (unsigned i = 0; i < 200; ++i) {
        seed = seed * 1664525u + 1013904223u;
        unsigned site = seed % 8, time = i / 2, bucket = (time + 7) / 10;
        int64_t value = (int64_t)((seed >> 8) % 101) - 50;
        bool null = !(i % 7);
        append(t, time, site, i % 3 ? "b" : "a", value, null);
        if (site < 2) continue;
        reference *ref = &expected[bucket][site]; ++ref->count;
        if (!null) {
            if (!ref->nonnull || value < ref->min) ref->min = value;
            if (!ref->nonnull || value > ref->max) ref->max = value;
            ref->sum += value; ++ref->nonnull;
        }
    }
    wadb_query_options q; aggregates(&q);
    q.scan.end_time = 100; q.bucket_width_us = 10; q.bucket_origin_us = 3;
    const char *groups[] = {"site"}; q.group_by = groups; q.group_count = 1;
    wadb_filter filter = {"site", WADB_GE, {.as.u64 = 2}}; q.filters = &filter; q.filter_count = 1;
    q.order_by = "_bucket";
    wadb_result *r = query(t, &q);
    uint64_t total = 0;
    for (size_t i = 0; i < r->row_count; ++i) {
        wadb_value *row = r->values + i * 8;
        unsigned bucket = (unsigned)((row[0].as.i64 + 7) / 10), site = (unsigned)row[1].as.u64;
        TEST_ASSERT_TRUE(bucket < 11 && site < 8);
        reference *ref = &expected[bucket][site];
        TEST_ASSERT_EQUAL_UINT64(ref->count, row[2].as.u64); TEST_ASSERT_EQUAL_UINT64(ref->nonnull, row[3].as.u64);
        if (ref->nonnull) {
            TEST_ASSERT_EQUAL_INT64(ref->sum, row[4].as.i64); TEST_ASSERT_EQUAL_INT64(ref->min, row[5].as.i64);
            TEST_ASSERT_EQUAL_INT64(ref->max, row[6].as.i64);
            TEST_ASSERT_TRUE(fabs((double)ref->sum / (double)ref->nonnull - row[7].as.f64) < 1e-10);
        } else for (size_t j = 4; j < 8; ++j) TEST_ASSERT_TRUE(row[j].is_null);
        total += ref->count; ref->count = 0;
    }
    TEST_ASSERT_EQUAL_UINT64(total, r->matched_rows);
    for (size_t b = 0; b < 11; ++b) for (size_t s = 0; s < 8; ++s) TEST_ASSERT_EQUAL_UINT64(0, expected[b][s].count);
    wadb_result_free(r);
}
static void test_cursor_preserves_snapshot_and_copies_filter_strings(void) {
    wadb_table *t = new_table(256);
    for (unsigned i = 1; i <= 6; ++i) append(t, 1, 1, i % 2 ? "a" : "b", i, false);
    wadb_query_options q; wadb_query_options_default(&q); q.scan.limit = 2;
    char path[] = "a";
    wadb_filter filter = {"path", WADB_EQ, {.as.bytes = {path, 1}}}; q.filters = &filter; q.filter_count = 1;
    uint64_t id;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_cursor_open(t, &q, 10000, &id, NULL));
    path[0] = 'b';
    wadb_result *r;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_cursor_next(db, id, &r, NULL));
    TEST_ASSERT_EQUAL_UINT64(2, r->row_count); TEST_ASSERT_EQUAL_UINT64(1, r->sequences[0]);
    TEST_ASSERT_EQUAL_UINT64(3, r->sequences[1]); TEST_ASSERT_TRUE(r->has_more); wadb_result_free(r);
    append(t, WADB_DAY_US + 1, 1, "a", 7, false);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_cursor_next(db, id, &r, NULL));
    TEST_ASSERT_EQUAL_UINT64(1, r->row_count); TEST_ASSERT_EQUAL_UINT64(5, r->sequences[0]);
    TEST_ASSERT_EQUAL_UINT64(6, r->stats.snapshot_sequence); TEST_ASSERT_FALSE(r->has_more); wadb_result_free(r);
    TEST_ASSERT_EQUAL(WADB_EXPIRED, wadb_cursor_next(db, id, &r, NULL)); TEST_ASSERT_NULL(r);
    TEST_ASSERT_EQUAL_UINT64(0, t->readers); TEST_ASSERT_EQUAL_UINT64(0, db->snapshot_bytes);
}
static void test_cursor_expiry_and_reader_backpressure_release_pins(void) {
    wadb_table *t = new_table(4096);
    append(t, 1, 1, "a", 1, false);
    db->options.max_readers = 1;
    uint64_t id, second;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_cursor_open(t, NULL, 10000, &id, NULL));
    TEST_ASSERT_EQUAL(WADB_BACKPRESSURE, wadb_cursor_open(t, NULL, 1000, &second, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_cursor_close(db, id, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_cursor_open(t, NULL, 1, &id, NULL));
    struct timespec delay = {.tv_nsec = 10000000}; nanosleep(&delay, NULL);
    wadb_result *r;
    TEST_ASSERT_EQUAL(WADB_EXPIRED, wadb_cursor_next(db, id, &r, NULL));
    TEST_ASSERT_EQUAL_UINT64(0, t->readers); TEST_ASSERT_EQUAL_UINT64(0, db->snapshot_bytes);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_cursor_open(t, NULL, 1000, &second, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_cursor_close(db, second, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_cursor_close(db, id, NULL));
    TEST_ASSERT_EQUAL_UINT64(0, db->reader_count);
}
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_projection_filters_and_owned_results);
    RUN_TEST(test_grouping_merges_values_across_segment_dictionaries);
    RUN_TEST(test_buckets_origin_empty_intervals_and_null_aggregates);
    RUN_TEST(test_resource_limits_return_no_partial_aggregate);
    RUN_TEST(test_integer_and_float_overflow_are_explicit);
    RUN_TEST(test_seeded_aggregates_match_reference_across_rotations_and_ties);
    RUN_TEST(test_cursor_preserves_snapshot_and_copies_filter_strings);
    RUN_TEST(test_cursor_expiry_and_reader_backpressure_release_pins);
    return UNITY_END();
}
