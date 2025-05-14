#include "../src/timeline.h"
#include "../test/unity/unity.h"

void setUp(void) {}
void tearDown(void) {}

void test_timeline_init() {
    Timeline t;
    TEST_ASSERT_FALSE(init_timeline(&t));
    destroy_timeline(&t);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_timeline_init);
    return UNITY_END();
}
