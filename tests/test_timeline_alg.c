#include "../src/timeline.h"
#include "../src/timeline_alg.h"
#include "../test/unity/unity.h"

#include <stdio.h>
#include <unistd.h>

#define TEST_ID 0xFFFFFFFF

// Time constants for readability
#define MS_PER_SECOND  1000LL
#define MS_PER_MINUTE  (60LL * MS_PER_SECOND)
#define MS_PER_HOUR    (60LL * MS_PER_MINUTE)
#define MS_PER_DAY     (24LL * MS_PER_HOUR)
#define MS_PER_WEEK    (7LL * MS_PER_DAY)

void setUp(void) {}
void tearDown(void) {}

void delete_data_file() {
    char data_path[128];
    sprintf(data_path, "data/timeline_%u.dat", TEST_ID);
    remove(data_path);
}


void test_timeline_search() {
    Timeline t;
    TEST_ASSERT_FALSE(init_timeline(&t, TEST_ID));

    for (int i = 0; i != 5001; ++i) {
        TEST_ASSERT_FALSE(
            append_event_to_timeline(&t, 0, 0)
        );
        t.events[i].time = i;
    }

    TEST_ASSERT(timeline_search(t, 0) == 0);
    TEST_ASSERT(timeline_search(t, 1) == 1);
    TEST_ASSERT(timeline_search(t, 100) == 100);
    TEST_ASSERT(timeline_search(t, 2500) == 2500);
    TEST_ASSERT(timeline_search(t, 5000) == 5000);
    TEST_ASSERT(timeline_search(t, 5001) == MAX_POS);

    destroy_timeline(&t);
}

// Test basic bucketed query with events spread across buckets
void test_bucketed_query_basic() {
    Timeline t;
    TEST_ASSERT_FALSE(init_timeline(&t, TEST_ID));

    // Create 10 events, one every 100ms starting at time 0
    for (int i = 0; i < 10; i++) {
        append_event_to_timeline(&t, i, i);
        t.events[i].time = i * 100;  // 0, 100, 200, ..., 900
    }

    BucketedQuery result;
    // Query from 0 to 1000ms, with 200ms buckets -> 5 buckets
    TEST_ASSERT_FALSE(timeline_bucketed_query(&t, 0, 1000, 200, &result));

    TEST_ASSERT_EQUAL_UINT32(5, result.num_buckets);
    TEST_ASSERT_EQUAL_UINT32(10, result.total_events);

    // Each bucket should have 2 events
    for (uint32_t i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_UINT32(2, result.buckets[i].count);
        TEST_ASSERT_EQUAL_INT64(i * 200, result.buckets[i].start_time);
        TEST_ASSERT_EQUAL_INT64((i + 1) * 200, result.buckets[i].end_time);
    }

    destroy_bucketed_query(&result);
    destroy_timeline(&t);
}

// Test bucketed query on empty timeline
void test_bucketed_query_empty_timeline() {
    Timeline t;
    TEST_ASSERT_FALSE(init_timeline(&t, TEST_ID));

    BucketedQuery result;
    TEST_ASSERT_FALSE(timeline_bucketed_query(&t, 0, 1000, 100, &result));

    TEST_ASSERT_EQUAL_UINT32(10, result.num_buckets);
    TEST_ASSERT_EQUAL_UINT32(0, result.total_events);

    // All buckets should be empty
    for (uint32_t i = 0; i < result.num_buckets; i++) {
        TEST_ASSERT_EQUAL_UINT32(0, result.buckets[i].count);
        TEST_ASSERT_NULL(result.buckets[i].events);
    }

    destroy_bucketed_query(&result);
    destroy_timeline(&t);
}

// Test query range with no events (events exist but not in range)
void test_bucketed_query_no_events_in_range() {
    Timeline t;
    TEST_ASSERT_FALSE(init_timeline(&t, TEST_ID));

    // Events at time 5000-5100
    for (int i = 0; i < 10; i++) {
        append_event_to_timeline(&t, i, i);
        t.events[i].time = 5000 + i * 10;
    }

    BucketedQuery result;
    // Query from 0 to 1000ms - no events in this range
    TEST_ASSERT_FALSE(timeline_bucketed_query(&t, 0, 1000, 100, &result));

    TEST_ASSERT_EQUAL_UINT32(10, result.num_buckets);
    TEST_ASSERT_EQUAL_UINT32(0, result.total_events);

    destroy_bucketed_query(&result);
    destroy_timeline(&t);
}

// Test events in some buckets but not others
void test_bucketed_query_sparse_events() {
    Timeline t;
    TEST_ASSERT_FALSE(init_timeline(&t, TEST_ID));

    // Events only in buckets 0, 2, and 4 (5 buckets total)
    append_event_to_timeline(&t, 0, 0);
    t.events[0].time = 50;   // Bucket 0
    append_event_to_timeline(&t, 1, 1);
    t.events[1].time = 250;  // Bucket 2
    append_event_to_timeline(&t, 2, 2);
    t.events[2].time = 450;  // Bucket 4

    BucketedQuery result;
    TEST_ASSERT_FALSE(timeline_bucketed_query(&t, 0, 500, 100, &result));

    TEST_ASSERT_EQUAL_UINT32(5, result.num_buckets);
    TEST_ASSERT_EQUAL_UINT32(3, result.total_events);

    // Check bucket 0
    TEST_ASSERT_EQUAL_UINT32(1, result.buckets[0].count);
    TEST_ASSERT_EQUAL_UINT32(0, result.buckets[0].events->type);

    // Check bucket 1 (empty)
    TEST_ASSERT_EQUAL_UINT32(0, result.buckets[1].count);

    // Check bucket 2
    TEST_ASSERT_EQUAL_UINT32(1, result.buckets[2].count);
    TEST_ASSERT_EQUAL_UINT32(1, result.buckets[2].events->type);

    // Check bucket 3 (empty)
    TEST_ASSERT_EQUAL_UINT32(0, result.buckets[3].count);

    // Check bucket 4
    TEST_ASSERT_EQUAL_UINT32(1, result.buckets[4].count);
    TEST_ASSERT_EQUAL_UINT32(2, result.buckets[4].events->type);

    destroy_bucketed_query(&result);
    destroy_timeline(&t);
}

// Test events exactly on bucket boundaries
void test_bucketed_query_boundary_events() {
    Timeline t;
    TEST_ASSERT_FALSE(init_timeline(&t, TEST_ID));

    // Events exactly at bucket boundaries: 0, 100, 200
    append_event_to_timeline(&t, 0, 0);
    t.events[0].time = 0;
    append_event_to_timeline(&t, 1, 1);
    t.events[1].time = 100;
    append_event_to_timeline(&t, 2, 2);
    t.events[2].time = 200;

    BucketedQuery result;
    TEST_ASSERT_FALSE(timeline_bucketed_query(&t, 0, 300, 100, &result));

    TEST_ASSERT_EQUAL_UINT32(3, result.num_buckets);
    TEST_ASSERT_EQUAL_UINT32(3, result.total_events);

    // Each bucket should have exactly 1 event at its start
    TEST_ASSERT_EQUAL_UINT32(1, result.buckets[0].count);
    TEST_ASSERT_EQUAL_INT64(0, result.buckets[0].events->time);

    TEST_ASSERT_EQUAL_UINT32(1, result.buckets[1].count);
    TEST_ASSERT_EQUAL_INT64(100, result.buckets[1].events->time);

    TEST_ASSERT_EQUAL_UINT32(1, result.buckets[2].count);
    TEST_ASSERT_EQUAL_INT64(200, result.buckets[2].events->time);

    destroy_bucketed_query(&result);
    destroy_timeline(&t);
}

// Test week-long query with daily buckets (realistic use case)
void test_bucketed_query_week_with_daily_buckets() {
    Timeline t;
    TEST_ASSERT_FALSE(init_timeline(&t, TEST_ID));

    timestamp_ms week_start = 1000000000000LL;  // Some epoch time

    // Add 2 events per day for a week
    for (int day = 0; day < 7; day++) {
        for (int event = 0; event < 2; event++) {
            int idx = day * 2 + event;
            append_event_to_timeline(&t, day, event);
            // Spread events throughout the day
            t.events[idx].time = week_start + day * MS_PER_DAY + event * MS_PER_HOUR;
        }
    }

    BucketedQuery result;
    TEST_ASSERT_FALSE(timeline_bucketed_query(&t, week_start, MS_PER_WEEK, MS_PER_DAY, &result));

    TEST_ASSERT_EQUAL_UINT32(7, result.num_buckets);
    TEST_ASSERT_EQUAL_UINT32(14, result.total_events);

    // Each day bucket should have 2 events
    for (uint32_t i = 0; i < 7; i++) {
        TEST_ASSERT_EQUAL_UINT32(2, result.buckets[i].count);
        TEST_ASSERT_EQUAL_INT64(week_start + i * MS_PER_DAY, result.buckets[i].start_time);
        TEST_ASSERT_EQUAL_INT64(week_start + (i + 1) * MS_PER_DAY, result.buckets[i].end_time);
    }

    destroy_bucketed_query(&result);
    destroy_timeline(&t);
}

// Test hourly buckets for a day
void test_bucketed_query_day_with_hourly_buckets() {
    Timeline t;
    TEST_ASSERT_FALSE(init_timeline(&t, TEST_ID));

    timestamp_ms day_start = 1000000000000LL;

    // Add events at minutes 15 and 45 of each hour
    for (int hour = 0; hour < 24; hour++) {
        int idx = hour * 2;
        append_event_to_timeline(&t, hour, 15);
        t.events[idx].time = day_start + hour * MS_PER_HOUR + 15 * MS_PER_MINUTE;
        
        append_event_to_timeline(&t, hour, 45);
        t.events[idx + 1].time = day_start + hour * MS_PER_HOUR + 45 * MS_PER_MINUTE;
    }

    BucketedQuery result;
    TEST_ASSERT_FALSE(timeline_bucketed_query(&t, day_start, MS_PER_DAY, MS_PER_HOUR, &result));

    TEST_ASSERT_EQUAL_UINT32(24, result.num_buckets);
    TEST_ASSERT_EQUAL_UINT32(48, result.total_events);

    // Each hour bucket should have 2 events
    for (uint32_t i = 0; i < 24; i++) {
        TEST_ASSERT_EQUAL_UINT32(2, result.buckets[i].count);
    }

    destroy_bucketed_query(&result);
    destroy_timeline(&t);
}

// Test events outside query range are excluded
void test_bucketed_query_excludes_out_of_range() {
    Timeline t;
    TEST_ASSERT_FALSE(init_timeline(&t, TEST_ID));

    // Events before, during, and after query range
    append_event_to_timeline(&t, 0, 0);
    t.events[0].time = 50;    // Before range
    append_event_to_timeline(&t, 1, 1);
    t.events[1].time = 150;   // In range
    append_event_to_timeline(&t, 2, 2);
    t.events[2].time = 250;   // In range
    append_event_to_timeline(&t, 3, 3);
    t.events[3].time = 350;   // After range

    BucketedQuery result;
    // Query from 100 to 300 (span of 200)
    TEST_ASSERT_FALSE(timeline_bucketed_query(&t, 100, 200, 100, &result));

    TEST_ASSERT_EQUAL_UINT32(2, result.num_buckets);
    TEST_ASSERT_EQUAL_UINT32(2, result.total_events);

    // Only events 1 and 2 should be included
    TEST_ASSERT_EQUAL_UINT32(1, result.buckets[0].count);
    TEST_ASSERT_EQUAL_UINT32(1, result.buckets[0].events->type);
    
    TEST_ASSERT_EQUAL_UINT32(1, result.buckets[1].count);
    TEST_ASSERT_EQUAL_UINT32(2, result.buckets[1].events->type);

    destroy_bucketed_query(&result);
    destroy_timeline(&t);
}

// Test uneven bucket division (span not divisible by bucket size)
void test_bucketed_query_uneven_buckets() {
    Timeline t;
    TEST_ASSERT_FALSE(init_timeline(&t, TEST_ID));

    // Events spread across the range
    for (int i = 0; i < 10; i++) {
        append_event_to_timeline(&t, i, i);
        t.events[i].time = i * 100;  // 0, 100, ..., 900
    }

    BucketedQuery result;
    // Query 950ms with 300ms buckets -> ceil(950/300) = 4 buckets
    // Last bucket should be shorter
    TEST_ASSERT_FALSE(timeline_bucketed_query(&t, 0, 950, 300, &result));

    TEST_ASSERT_EQUAL_UINT32(4, result.num_buckets);

    // Check time ranges
    TEST_ASSERT_EQUAL_INT64(0, result.buckets[0].start_time);
    TEST_ASSERT_EQUAL_INT64(300, result.buckets[0].end_time);
    
    TEST_ASSERT_EQUAL_INT64(300, result.buckets[1].start_time);
    TEST_ASSERT_EQUAL_INT64(600, result.buckets[1].end_time);
    
    TEST_ASSERT_EQUAL_INT64(600, result.buckets[2].start_time);
    TEST_ASSERT_EQUAL_INT64(900, result.buckets[2].end_time);
    
    // Last bucket is clamped to span end
    TEST_ASSERT_EQUAL_INT64(900, result.buckets[3].start_time);
    TEST_ASSERT_EQUAL_INT64(950, result.buckets[3].end_time);

    destroy_bucketed_query(&result);
    destroy_timeline(&t);
}

// Test invalid parameters
void test_bucketed_query_invalid_params() {
    Timeline t;
    TEST_ASSERT_FALSE(init_timeline(&t, TEST_ID));

    BucketedQuery result;

    // NULL timeline
    TEST_ASSERT_TRUE(timeline_bucketed_query(NULL, 0, 1000, 100, &result));

    // NULL result
    TEST_ASSERT_TRUE(timeline_bucketed_query(&t, 0, 1000, 100, NULL));

    // Zero span
    TEST_ASSERT_TRUE(timeline_bucketed_query(&t, 0, 0, 100, &result));

    // Zero bucket size
    TEST_ASSERT_TRUE(timeline_bucketed_query(&t, 0, 1000, 0, &result));

    // Negative span
    TEST_ASSERT_TRUE(timeline_bucketed_query(&t, 0, -1000, 100, &result));

    // Negative bucket size
    TEST_ASSERT_TRUE(timeline_bucketed_query(&t, 0, 1000, -100, &result));

    destroy_timeline(&t);
}

// Test many events in single bucket
void test_bucketed_query_many_events_one_bucket() {
    Timeline t;
    TEST_ASSERT_FALSE(init_timeline(&t, TEST_ID));

    // Add 100 events all in the same time range
    for (int i = 0; i < 100; i++) {
        append_event_to_timeline(&t, i, i);
        t.events[i].time = 500 + i;  // 500 to 599
    }

    BucketedQuery result;
    // Single bucket covering all events
    TEST_ASSERT_FALSE(timeline_bucketed_query(&t, 0, 1000, 1000, &result));

    TEST_ASSERT_EQUAL_UINT32(1, result.num_buckets);
    TEST_ASSERT_EQUAL_UINT32(100, result.total_events);
    TEST_ASSERT_EQUAL_UINT32(100, result.buckets[0].count);

    // Verify all events are accessible
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_EQUAL_UINT32(i, result.buckets[0].events[i].type);
    }

    destroy_bucketed_query(&result);
    destroy_timeline(&t);
}

// Test large timeline with many buckets
void test_bucketed_query_large_timeline() {
    Timeline t;
    TEST_ASSERT_FALSE(init_timeline(&t, TEST_ID));

    // Add 10000 events
    for (int i = 0; i < 10000; i++) {
        append_event_to_timeline(&t, i % 256, i % 256);
        t.events[i].time = i * 10;  // 0, 10, 20, ... 99990
    }

    BucketedQuery result;
    // Query all with 100ms buckets -> 1000 buckets, 10 events each
    TEST_ASSERT_FALSE(timeline_bucketed_query(&t, 0, 100000, 100, &result));

    TEST_ASSERT_EQUAL_UINT32(1000, result.num_buckets);
    TEST_ASSERT_EQUAL_UINT32(10000, result.total_events);

    // Each bucket should have 10 events
    for (uint32_t i = 0; i < result.num_buckets; i++) {
        TEST_ASSERT_EQUAL_UINT32(10, result.buckets[i].count);
    }

    destroy_bucketed_query(&result);
    destroy_timeline(&t);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_timeline_search);
    RUN_TEST(test_bucketed_query_basic);
    RUN_TEST(test_bucketed_query_empty_timeline);
    RUN_TEST(test_bucketed_query_no_events_in_range);
    RUN_TEST(test_bucketed_query_sparse_events);
    RUN_TEST(test_bucketed_query_boundary_events);
    RUN_TEST(test_bucketed_query_week_with_daily_buckets);
    RUN_TEST(test_bucketed_query_day_with_hourly_buckets);
    RUN_TEST(test_bucketed_query_excludes_out_of_range);
    RUN_TEST(test_bucketed_query_uneven_buckets);
    RUN_TEST(test_bucketed_query_invalid_params);
    RUN_TEST(test_bucketed_query_many_events_one_bucket);
    RUN_TEST(test_bucketed_query_large_timeline);
    return UNITY_END();
}
