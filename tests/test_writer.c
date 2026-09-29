#include "database_fixture.h"
#include <time.h>

typedef struct {
    wadb_table *table;
    wadb_value *values;
    size_t count;
    wadb_status status;
    wadb_append_receipt receipt;
} producer;
static void *produce(void *context) {
    producer *p = context;
    p->status = wadb_append_batch(p->table, p->values, p->count, &p->receipt, NULL);
    return NULL;
}
static void wait_for_reserved(size_t minimum) {
    uint64_t deadline = wadb_monotonic_ns() + UINT64_C(5000000000);
    for (;;) {
        pthread_mutex_lock(&db->mutex);
        size_t bytes = db->queue_bytes;
        pthread_mutex_unlock(&db->mutex);
        if (bytes >= minimum) return;
        TEST_ASSERT_TRUE(wadb_monotonic_ns() < deadline);
        struct timespec brief = {.tv_nsec = 1000000};
        nanosleep(&brief, NULL);
    }
}
static wadb_table *writer_table(size_t queue_limit) {
    wadb_options o;
    wadb_options_default(&o); o.batch_delay_ms = 10; o.queue_limit_bytes = queue_limit;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, &o, &db, NULL));
    wadb_field_def field = {"value", WADB_U32, 0, false};
    wadb_table *t;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_create_table(db, "values", &field, 1, 0, &t, NULL));
    return t;
}

static void test_concurrent_producers_share_frames_and_keep_receipts(void) {
    wadb_table *t = writer_table(4 * 1024 * 1024);
    enum { N = 16 };
    producer producers[N]; pthread_t threads[N]; wadb_value values[N];
    pthread_mutex_lock(&db->writer_mutex);
    for (size_t i = 0; i < N; ++i) {
        values[i] = (wadb_value){.as.u64 = i};
        producers[i] = (producer){.table = t, .values = &values[i], .count = 1};
        TEST_ASSERT_EQUAL_INT(0, pthread_create(&threads[i], NULL, produce, &producers[i]));
    }
    wait_for_reserved(N * (sizeof(wadb_append_request) + t->schema.row_width));
    pthread_mutex_unlock(&db->writer_mutex);
    bool seen[N + 1] = {0};
    for (size_t i = 0; i < N; ++i) {
        TEST_ASSERT_EQUAL_INT(0, pthread_join(threads[i], NULL));
        TEST_ASSERT_EQUAL(WADB_OK, producers[i].status);
        uint64_t seq = producers[i].receipt.first_sequence;
        TEST_ASSERT_TRUE(seq >= 1 && seq <= N);
        TEST_ASSERT_FALSE(seen[seq]); seen[seq] = true;
        TEST_ASSERT_EQUAL_UINT64(seq, producers[i].receipt.last_sequence);
    }
    TEST_ASSERT_EQUAL_UINT64(N, t->last_sequence);
    TEST_ASSERT_EQUAL_UINT64(N, t->append_requests);
    TEST_ASSERT_TRUE(t->committed_frames < N);
    TEST_ASSERT_EQUAL_UINT32(0, db->queue_bytes);
    wadb_close(db); db = NULL;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, NULL, &db, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_find_table(db, "values", &t, NULL));
    TEST_ASSERT_EQUAL_UINT64(N, t->last_sequence);
}

static void test_queue_rejects_overload_before_allocating_staging(void) {
    wadb_table *t = writer_table(WADB_MAX_FRAME_BYTES);
    const size_t count = 40000;
    wadb_value *values = calloc(count, sizeof(*values));
    TEST_ASSERT_NOT_NULL(values);
    producer p = {.table = t, .values = values, .count = count};
    pthread_t thread;
    pthread_mutex_lock(&db->writer_mutex);
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, NULL, produce, &p));
    wait_for_reserved(count * t->schema.row_width);
    TEST_ASSERT_EQUAL(WADB_BACKPRESSURE, wadb_append_batch(t, values, count, NULL, NULL));
    pthread_mutex_unlock(&db->writer_mutex);
    TEST_ASSERT_EQUAL_INT(0, pthread_join(thread, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, p.status);
    TEST_ASSERT_EQUAL_UINT64(count, t->last_sequence);
    TEST_ASSERT_EQUAL_UINT64(1, db->rejected_appends);
    TEST_ASSERT_EQUAL_UINT32(0, db->queue_bytes);
    free(values);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_concurrent_producers_share_frames_and_keep_receipts);
    RUN_TEST(test_queue_rejects_overload_before_allocating_staging);
    return UNITY_END();
}
