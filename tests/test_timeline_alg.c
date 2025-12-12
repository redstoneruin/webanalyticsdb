#include "../src/timeline.h"
#include "../src/timeline_alg.h"
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

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_timeline_search);
    return UNITY_END();
}
