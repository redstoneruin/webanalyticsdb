#include "../src/timeline.h"
#include "../test/unity/unity.h"

void setUp(void) {}
void tearDown(void) {}

void test_timeline_init() {
    Timeline t;
    TEST_ASSERT_FALSE(init_timeline(&t));
    destroy_timeline(&t);
}

void test_timeline_append_1() {
    Timeline t;
    TEST_ASSERT_FALSE(init_timeline(&t));

    TEST_ASSERT_FALSE(
        append_event_to_timeline(&t, 0, 0)
    );

    TEST_ASSERT(t.events[0].path == 0 && t.events[0].type == 0);

    destroy_timeline(&t);
}

void test_timeline_append_2() {
    Timeline t;
    TEST_ASSERT_FALSE(init_timeline(&t));

    for (int i = 0; i != 5001; ++i) {
        TEST_ASSERT_FALSE(
            append_event_to_timeline(&t, 0, 0)
        );
        TEST_ASSERT(
            t.events[i].path == 0 && t.events[i].type == 0
        );
    }


    destroy_timeline(&t);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_timeline_init);
    RUN_TEST(test_timeline_append_1);
    RUN_TEST(test_timeline_append_2);
    return UNITY_END();
}
