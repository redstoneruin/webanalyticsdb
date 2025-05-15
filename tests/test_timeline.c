#include "../src/timeline.h"
#include "../test/unity/unity.h"

#include <stdio.h>
#include <unistd.h>

#define TEST_ID 0xFFFFFFFF

void setUp(void) {}
void tearDown(void) {}

void delete_data_file() {
    char data_path[128];
    sprintf(data_path, "data/timeline_%u.dat", TEST_ID);
    remove(data_path);
}

void test_timeline_init() {
    Timeline t;
    TEST_ASSERT_FALSE(init_timeline(&t, TEST_ID));
    TEST_ASSERT(t.length == 0);
    TEST_ASSERT(t.first_unwritten == 0);
    TEST_ASSERT(t.events);
    destroy_timeline(&t);
}

void test_timeline_append_1() {
    Timeline t;
    TEST_ASSERT_FALSE(init_timeline(&t, TEST_ID));

    TEST_ASSERT_FALSE(
        append_event_to_timeline(&t, 0, 0)
    );

    TEST_ASSERT(t.length == 1);
    TEST_ASSERT(t.events[0].path == 0 && t.events[0].type == 0);

    destroy_timeline(&t);
}

void test_timeline_append_2() {
    Timeline t;
    TEST_ASSERT_FALSE(init_timeline(&t, TEST_ID));

    for (int i = 0; i != 5001; ++i) {
        TEST_ASSERT_FALSE(
            append_event_to_timeline(&t, 0, 0)
        );
        TEST_ASSERT(
            t.events[i].path == 0 && t.events[i].type == 0
        );
    }

    TEST_ASSERT(t.allocated > 5000);
    TEST_ASSERT(t.length == 5001);

    destroy_timeline(&t);
}

void test_timeline_write_to_disk_1() {
    Timeline t;
    TEST_ASSERT_FALSE(init_timeline(&t, TEST_ID));

    for (int i = 0; i != 5001; ++i) {
        append_event_to_timeline(&t, 0, 0);
    }

    TEST_ASSERT_FALSE(write_timeline_to_disk(&t));
    TEST_ASSERT(t.first_unwritten == 5001);

    destroy_timeline(&t);
    delete_data_file();
}

void test_timeline_read_from_disk_1() {
    Timeline t;
    TEST_ASSERT_FALSE(init_timeline(&t, TEST_ID));

    for (int i = 0; i != 5001; ++i) {
        append_event_to_timeline(&t, 0, 0);
    }
    write_timeline_to_disk(&t);

    TEST_ASSERT_FALSE(read_timeline_from_disk(&t));
    TEST_ASSERT(t.length == 5001 && t.allocated >= 5001);
    TEST_ASSERT(t.events[5000].path == 0 && t.events[5000].type == 0);

    destroy_timeline(&t);
    delete_data_file();
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_timeline_init);
    RUN_TEST(test_timeline_append_1);
    RUN_TEST(test_timeline_append_2);
    RUN_TEST(test_timeline_write_to_disk_1);
    RUN_TEST(test_timeline_read_from_disk_1);
    return UNITY_END();
}
