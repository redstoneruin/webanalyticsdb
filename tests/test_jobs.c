#define tearDown database_tear_down
#include "database_fixture.h"
#undef tearDown
#include "../src/server/jobs.h"
static wadb_jobs *manager;
void tearDown(void) { wadb_jobs_close(manager); manager = NULL; database_tear_down(); }
static wadb_jobs *open_jobs(void) {
    wadb_options o; wadb_options_default(&o); o.batch_delay_ms = 0;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, &o, &db, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_jobs_open(db, &manager, NULL)); return manager;
}
static wadb_job_info wait_job(wadb_jobs *jobs, uint64_t id) {
    wadb_job_info info; uint64_t deadline = wadb_monotonic_ns() + UINT64_C(5000000000);
    do {
        TEST_ASSERT_EQUAL(WADB_OK, wadb_job_get(jobs, id, &info, NULL));
        if (info.state >= WADB_JOB_COMPLETE) return info;
        struct timespec pause = {.tv_nsec = 1000000}; nanosleep(&pause, NULL);
    } while (wadb_monotonic_ns() < deadline);
    TEST_FAIL_MESSAGE("job timed out"); return info;
}
static void test_generator_uses_durable_fixed_schema_and_integrity_job(void) {
    wadb_jobs *jobs = open_jobs();
    wadb_generator_options o = {.rows_per_second = 1000, .duration_ms = 200, .batch_rows = 32, .cardinality = 3, .seed = 123};
    uint64_t id; TEST_ASSERT_EQUAL(WADB_OK, wadb_job_generate(jobs, &o, &id, NULL));
    wadb_job_info info = wait_job(jobs, id);
    TEST_ASSERT_EQUAL(WADB_JOB_COMPLETE, info.state); TEST_ASSERT_TRUE(info.committed_rows > 0 && info.committed_rows <= 200);
    uint64_t committed = info.committed_rows;
    TEST_ASSERT_EQUAL_UINT64(committed, info.attempted_rows); TEST_ASSERT_TRUE(info.cpu_ns > 0);
    wadb_table *table; TEST_ASSERT_EQUAL(WADB_OK, wadb_get_table(db, info.table_id, &table, NULL));
    TEST_ASSERT_EQUAL_UINT32(48, wadb_table_schema(table)->row_width);
    wadb_result *result; TEST_ASSERT_EQUAL(WADB_OK, wadb_query(table, NULL, &result, NULL));
    TEST_ASSERT_EQUAL_UINT64(committed, result->row_count); wadb_result_free(result);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_job_integrity(jobs, table, 1024 * 1024, 5000, &id, NULL));
    info = wait_job(jobs, id); TEST_ASSERT_EQUAL(WADB_JOB_COMPLETE, info.state); TEST_ASSERT_EQUAL_UINT64(committed, info.integrity.rows);
    wadb_jobs_close(jobs); manager = NULL;
}
static void test_job_limits_stop_and_existing_table_protection(void) {
    wadb_jobs *jobs = open_jobs();
    wadb_generator_options o = {.table_name = "load_a", .rows_per_second = 1000, .duration_ms = 60000, .batch_rows = 64, .cardinality = 10, .seed = 1};
    uint64_t a, b, extra;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_job_generate(jobs, &o, &a, NULL));
    TEST_ASSERT_EQUAL(WADB_EXISTS, wadb_job_generate(jobs, &o, &extra, NULL));
    strcpy(o.table_name, "load_b"); TEST_ASSERT_EQUAL(WADB_OK, wadb_job_generate(jobs, &o, &b, NULL));
    strcpy(o.table_name, "load_c"); TEST_ASSERT_EQUAL(WADB_BACKPRESSURE, wadb_job_generate(jobs, &o, &extra, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_job_stop(jobs, a, NULL)); TEST_ASSERT_EQUAL(WADB_OK, wadb_job_stop(jobs, b, NULL));
    TEST_ASSERT_EQUAL(WADB_JOB_STOPPED, wait_job(jobs, a).state); TEST_ASSERT_EQUAL(WADB_JOB_STOPPED, wait_job(jobs, b).state);
    o.duration_ms = 300001; TEST_ASSERT_EQUAL(WADB_LIMIT, wadb_job_generate(jobs, &o, &extra, NULL));
    TEST_ASSERT_EQUAL(WADB_NOT_FOUND, wadb_job_stop(jobs, 99999, NULL));
    wadb_jobs_close(jobs); manager = NULL;
}
static void test_job_history_is_bounded_and_integrity_failures_visible(void) {
    wadb_jobs *jobs = open_jobs(); wadb_table *t;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_create_table(db, "empty", NULL, 0, 0, &t, NULL));
    uint64_t first = 0, id = 0;
    for (unsigned i = 0; i < WADB_JOB_HISTORY + 2; ++i) {
        TEST_ASSERT_EQUAL(WADB_OK, wadb_job_integrity(jobs, t, 1024, 1000, &id, NULL));
        if (!i) first = id;
        TEST_ASSERT_EQUAL(WADB_JOB_COMPLETE, wait_job(jobs, id).state);
    }
    wadb_job_info infos[WADB_JOB_HISTORY]; size_t count;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_job_list(jobs, infos, WADB_JOB_HISTORY, &count, NULL));
    TEST_ASSERT_EQUAL_UINT64(WADB_JOB_HISTORY, count); TEST_ASSERT_EQUAL_UINT64(id, infos[count - 1].id);
    TEST_ASSERT_EQUAL(WADB_NOT_FOUND, wadb_job_get(jobs, first, &infos[0], NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_append_batch(t, NULL, 1, NULL, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_job_integrity(jobs, t, 1, 1000, &id, NULL));
    wadb_job_info info = wait_job(jobs, id);
    TEST_ASSERT_EQUAL(WADB_JOB_FAILED, info.state); TEST_ASSERT_EQUAL(WADB_LIMIT, info.error.code);
    wadb_jobs_close(jobs); manager = NULL;
}
int main(void) {
    UNITY_BEGIN(); RUN_TEST(test_generator_uses_durable_fixed_schema_and_integrity_job);
    RUN_TEST(test_job_limits_stop_and_existing_table_protection);
    RUN_TEST(test_job_history_is_bounded_and_integrity_failures_visible); return UNITY_END();
}
