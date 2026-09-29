#include "database_fixture.h"
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>

/* Each child opens its own database after the parent has joined every thread.
 * The test archive instruments real syscalls; _exit intentionally bypasses all
 * database cleanup. This models process termination, not lost device caches. */
enum { CRASH_EXIT = 86, CHILD_ERROR = 87, MAX_POINTS = 256 };
typedef struct { char operation[32], leaf[48]; int before, status; } point;
typedef struct { point points[MAX_POINTS]; size_t count; bool returned; wadb_status status; } trace;
static atomic_bool armed;
static size_t point_number, target;
static int report_fd, injected_errno;
static int64_t clock_value;

static void report(const point *p) {
    size_t written = 0;
    while (written < sizeof(*p)) {
        ssize_t n = write(report_fd, (const char *)p + written, sizeof(*p) - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) _exit(CHILD_ERROR);
        written += (size_t)n;
    }
}
static bool boundary(const char *operation, const char *path, bool before) {
    if (!atomic_load_explicit(&armed, memory_order_acquire)) return false;
    int saved = errno;
    const char *leaf = path ? strrchr(path, '/') : NULL;
    point p = {.before = before};
    snprintf(p.operation, sizeof(p.operation), "%s", operation);
    snprintf(p.leaf, sizeof(p.leaf), "%s", leaf ? leaf + 1 : path ? path : "");
    report(&p);
    if (++point_number == target) {
        if (!injected_errno) _exit(CRASH_EXIT);
        if (!before) _exit(CHILD_ERROR);
        atomic_store_explicit(&armed, false, memory_order_release);
        errno = injected_errno;
        return true;
    }
    errno = saved;
    return false;
}
int wadb_test_io_before(const char *operation, const char *path) {
    return boundary(operation, path, true);
}
int64_t wadb_test_io_after(const char *operation, const char *path, int64_t result) {
    if (result >= 0) (void)boundary(operation, path, false);
    return result;
}

typedef enum { INITIALIZE, CREATE, FIRST_APPEND, ROTATE_SIZE, ROTATE_DAY,
    RETIRE_SOME, RETIRE_ALL, CONFIGURE, REPAIR_TAIL } scenario;
static const char *scenario_names[] = {"initialize", "create", "first append", "size rotation",
    "day rotation", "partial retention", "all retention", "retention config", "tail repair"};
static const wadb_field_def fields[] = {{"value", WADB_U64, 0, false}, {"path", WADB_SYMBOL32, 0, false}};
static int64_t controlled_clock(void *unused) { (void)unused; return clock_value; }
static wadb_options options_for(scenario s) {
    wadb_options options;
    wadb_options_default(&options); options.batch_delay_ms = 0;
    options.segment_target_bytes = s == ROTATE_DAY || s == REPAIR_TAIL ? 4096 : 256;
    return options;
}
static wadb_status append_values(wadb_table *t, uint64_t first, size_t count, wadb_append_receipt *receipt) {
    wadb_value values[6];
    if (count > 3) return WADB_INVALID;
    for (size_t i = 0; i < count; ++i) {
        uint64_t seq = first + i;
        const char *symbol = seq % 2 ? "/alpha" : "/beta";
        values[2 * i] = (wadb_value){.as.u64 = seq * 17};
        values[2 * i + 1] = (wadb_value){.as.bytes = {symbol, strlen(symbol)}};
    }
    return wadb_append_batch(t, values, count, receipt, NULL);
}
static void prepare(scenario s) {
    TEST_ASSERT_NULL(db);
    if (s == INITIALIZE) return;
    wadb_options options = options_for(s);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, &options, &db, NULL));
    if (s != CREATE) {
        wadb_table *t;
        TEST_ASSERT_EQUAL(WADB_OK, wadb_create_table(db, "samples", fields, 2, 10, &t, NULL));
        db->realtime = controlled_clock;
        unsigned batches = s == FIRST_APPEND ? 0 : s == RETIRE_SOME || s == RETIRE_ALL ? 3 : 1;
        for (unsigned i = 0; i < batches; ++i) {
            clock_value = (i + 1) * 100;
            TEST_ASSERT_EQUAL(WADB_OK, append_values(t, 2 * i + 1, 2, NULL));
        }
        if (s == REPAIR_TAIL) {
            /* A nonempty incomplete header must be truncated on each restart. */
            const unsigned char tail[13] = {0};
            TEST_ASSERT_EQUAL_INT(sizeof(tail), write(t->active_fd, tail, sizeof(tail)));
        }
    }
    wadb_close(db); db = NULL;
}
static void child_action(scenario s, size_t stop_at, int fault_errno, int fd) {
    alarm(20);
    report_fd = fd; target = stop_at; injected_errno = fault_errno; point_number = 0;
    wadb_options options = options_for(s);
    wadb_db *child = NULL;
    wadb_table *t = NULL;
    bool opening = s == INITIALIZE || s == REPAIR_TAIL;
    atomic_store_explicit(&armed, opening, memory_order_release);
    wadb_status status = wadb_open(directory, &options, &child, NULL);
    if (!opening) {
        if (status) _exit(CHILD_ERROR);
        if (s != CREATE && wadb_find_table(child, "samples", &t, NULL)) _exit(CHILD_ERROR);
        child->realtime = controlled_clock;
        clock_value = s == ROTATE_DAY ? WADB_DAY_US + 200 : 200;
        atomic_store_explicit(&armed, true, memory_order_release);
        switch (s) {
        case CREATE: status = wadb_create_table(child, "samples", fields, 2, 10, &t, NULL); break;
        case FIRST_APPEND: status = append_values(t, 1, 3, NULL); break;
        case ROTATE_SIZE: case ROTATE_DAY: status = append_values(t, 3, 3, NULL); break;
        case RETIRE_SOME: status = wadb_retention_apply(t, 250, NULL, NULL); break;
        case RETIRE_ALL: status = wadb_retention_apply(t, 1000, NULL, NULL); break;
        case CONFIGURE: status = wadb_set_retention(t, 500, NULL); break;
        default: _exit(CHILD_ERROR);
        }
    }
    atomic_store_explicit(&armed, false, memory_order_release);
    /* A failed authoritative metadata replacement must stop further mutation
     * in this process; recovery decides which version became installed. */
    if (status == WADB_INDETERMINATE && (s == CREATE || s == CONFIGURE || s == RETIRE_SOME || s == RETIRE_ALL)) {
        if (!child || !(child->read_only || (t && t->read_only))) _exit(CHILD_ERROR);
    }
    point result = {.before = -1, .status = status};
    report(&result);
    _exit(0);
}
static trace run_child(scenario s, size_t stop_at, int fault_errno) {
    int pipe_fds[2];
    TEST_ASSERT_EQUAL_INT(0, pipe(pipe_fds));
    fflush(NULL);
    pid_t pid = fork(); TEST_ASSERT_TRUE(pid >= 0);
    if (!pid) { close(pipe_fds[0]); child_action(s, stop_at, fault_errno, pipe_fds[1]); }
    close(pipe_fds[1]);
    trace output = {0};
    point p;
    size_t filled = 0;
    for (;;) {
        ssize_t n = read(pipe_fds[0], (char *)&p + filled, sizeof(p) - filled);
        if (n < 0 && errno == EINTR) continue;
        TEST_ASSERT_TRUE(n >= 0);
        if (!n) break;
        filled += (size_t)n;
        if (filled != sizeof(p)) continue;
        if (p.before < 0) { output.returned = true; output.status = (wadb_status)p.status; }
        else { TEST_ASSERT_TRUE(output.count < MAX_POINTS); output.points[output.count++] = p; }
        filled = 0;
    }
    close(pipe_fds[0]);
    int result;
    TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &result, 0));
    TEST_ASSERT_EQUAL_UINT64(0, filled);
    TEST_ASSERT_TRUE(WIFEXITED(result));
    TEST_ASSERT_EQUAL_INT(stop_at && !fault_errno ? CRASH_EXIT : 0, WEXITSTATUS(result));
    if (stop_at) TEST_ASSERT_EQUAL_UINT64(stop_at, output.count);
    if (!stop_at) { TEST_ASSERT_TRUE(output.returned); TEST_ASSERT_EQUAL(WADB_OK, output.status); }
    else if (fault_errno) { TEST_ASSERT_TRUE(output.returned); TEST_ASSERT_NOT_EQUAL(WADB_OK, output.status); }
    else TEST_ASSERT_FALSE(output.returned);
    return output;
}
static void check_rows(wadb_table *t, uint64_t first, size_t count) {
    wadb_result *result;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_query(t, NULL, &result, NULL));
    TEST_ASSERT_EQUAL_UINT64(count, result->row_count);
    TEST_ASSERT_EQUAL_UINT64(3, result->column_count);
    for (size_t i = 0; i < count; ++i) {
        uint64_t sequence = first + i;
        const char *symbol = sequence % 2 ? "/alpha" : "/beta";
        TEST_ASSERT_EQUAL_UINT64(sequence, result->sequences[i]);
        TEST_ASSERT_EQUAL_UINT64(sequence * 17, result->values[3 * i + 1].as.u64);
        TEST_ASSERT_EQUAL_UINT64(strlen(symbol), result->values[3 * i + 2].as.bytes.length);
        TEST_ASSERT_EQUAL_MEMORY(symbol, result->values[3 * i + 2].as.bytes.data, strlen(symbol));
        if (i) TEST_ASSERT_TRUE(result->values[3 * i].as.i64 >= result->values[3 * (i - 1)].as.i64);
    }
    wadb_result_free(result);
}
static bool installed(const trace *operation, const char *leaf) {
    for (size_t i = 0; i < operation->count; ++i)
        if (!operation->points[i].before && !strcmp(operation->points[i].operation, "atomic.rename") &&
            !strcmp(operation->points[i].leaf, leaf)) return true;
    return false;
}
static void verify_recovery(scenario s, bool acknowledged, const trace *operation) {
    wadb_options options = options_for(s);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, &options, &db, NULL));
    wadb_table *t;
    if (s == INITIALIZE) {
        TEST_ASSERT_EQUAL_UINT64(0, db->table_count);
        TEST_ASSERT_EQUAL(WADB_OK, wadb_create_table(db, "samples", fields, 2, 10, &t, NULL));
    } else if (s == CREATE) {
        wadb_status found = wadb_find_table(db, "samples", &t, NULL);
        /* The OS stays alive in this failure model. Before catalog rename,
         * even a complete orphan schema/manifest must remain undiscoverable. */
        TEST_ASSERT_EQUAL(installed(operation, "catalog") ? WADB_OK : WADB_NOT_FOUND, found);
        if (acknowledged) TEST_ASSERT_EQUAL(WADB_OK, found);
        if (found == WADB_NOT_FOUND)
            TEST_ASSERT_EQUAL(WADB_OK, wadb_create_table(db, "samples", fields, 2, 10, &t, NULL));
        else TEST_ASSERT_EQUAL(WADB_OK, found);
    } else TEST_ASSERT_EQUAL(WADB_OK, wadb_find_table(db, "samples", &t, NULL));
    TEST_ASSERT_EQUAL_UINT64(1, db->table_count);
    wadb_schema expected;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_schema_build(&expected, fields, 2, NULL));
    TEST_ASSERT_EQUAL_UINT64(expected.fingerprint, t->schema.fingerprint);
    uint64_t first = 1, last = t->last_sequence;
    if (s == INITIALIZE || s == CREATE) TEST_ASSERT_EQUAL_UINT64(0, last);
    else if (s == FIRST_APPEND) {
        TEST_ASSERT_EQUAL_UINT64(installed(operation, "manifest") ? 1 : 0, t->segment_count);
        TEST_ASSERT_TRUE(last == 0 || last == 3);
        if (acknowledged) TEST_ASSERT_EQUAL_UINT64(3, last);
    } else if (s == ROTATE_SIZE || s == ROTATE_DAY) {
        TEST_ASSERT_EQUAL_UINT64(installed(operation, "manifest") ? 2 : 1, t->segment_count);
        TEST_ASSERT_TRUE(last == 2 || last == 5);
        if (acknowledged) TEST_ASSERT_EQUAL_UINT64(5, last);
    } else if (s == RETIRE_SOME || s == RETIRE_ALL) {
        TEST_ASSERT_EQUAL_UINT64(6, last);
        uint64_t retired = s == RETIRE_ALL ? 6 : 4;
        TEST_ASSERT_EQUAL_UINT64(installed(operation, "manifest") ? retired : 0, t->preserved_sequence);
        if (acknowledged) TEST_ASSERT_EQUAL_UINT64(retired, t->preserved_sequence);
        first = t->preserved_sequence + 1;
        TEST_ASSERT_FALSE(t->pending_retention);
    } else TEST_ASSERT_EQUAL_UINT64(2, last);
    if (s == CONFIGURE) {
        TEST_ASSERT_EQUAL_UINT64(installed(operation, "manifest") ? 500 : 10, t->retention_us);
        if (acknowledged) TEST_ASSERT_EQUAL_UINT64(500, t->retention_us);
    } else TEST_ASSERT_EQUAL_UINT64(10, t->retention_us);
    check_rows(t, first, (size_t)(last + 1 - first));
    unsigned active = 0;
    for (size_t i = 0; i < t->segment_count; ++i) {
        if (t->segments[i].state == WADB_SEG_ACTIVE) { ++active; TEST_ASSERT_EQUAL_UINT64(t->segment_count - 1, i); }
        if (t->segments[i].state == WADB_SEG_RETIRED) {
            char *path = wadb_segment_path(t, &t->segments[i], "seg");
            TEST_ASSERT_EQUAL_INT(-1, access(path, F_OK)); TEST_ASSERT_EQUAL_INT(ENOENT, errno); free(path);
        }
    }
    TEST_ASSERT_TRUE(active <= 1);
    wadb_integrity_stats stats;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_integrity_check(t, NULL, &stats, NULL));
    TEST_ASSERT_EQUAL_UINT64(last + 1 - first, stats.rows);
    int64_t watermark = t->last_time;
    db->realtime = controlled_clock; clock_value = 0;
    wadb_append_receipt receipt;
    TEST_ASSERT_EQUAL(WADB_OK, append_values(t, last + 1, 1, &receipt));
    TEST_ASSERT_EQUAL_UINT64(last + 1, receipt.first_sequence);
    TEST_ASSERT_EQUAL_INT64(watermark, receipt.ingestion_time);
    check_rows(t, first, (size_t)(last + 2 - first));
    wadb_close(db); db = NULL;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, &options, &db, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_find_table(db, "samples", &t, NULL));
    check_rows(t, first, (size_t)(last + 2 - first));
    wadb_close(db); db = NULL;
}
static void reset_fixture(void) { remove_fixture(directory); TEST_ASSERT_EQUAL_INT(0, mkdir(directory, 0700)); }
static void matrix(scenario s) {
    prepare(s);
    trace expected = run_child(s, 0, 0);
    verify_recovery(s, true, &expected); reset_fixture();
    TEST_ASSERT_TRUE(expected.count > 0);
    size_t failures = 0;
    for (size_t i = 0; i < expected.count; ++i) {
        prepare(s);
        trace actual = run_child(s, i + 1, 0);
        for (size_t j = 0; j <= i; ++j) {
            TEST_ASSERT_EQUAL_STRING(expected.points[j].operation, actual.points[j].operation);
            TEST_ASSERT_EQUAL_INT(expected.points[j].before, actual.points[j].before);
            /* Random fixture basename occurs only in initialization. */
            if (s != INITIALIZE) TEST_ASSERT_EQUAL_STRING(expected.points[j].leaf, actual.points[j].leaf);
        }
        verify_recovery(s, false, &actual); reset_fixture();
        if (!expected.points[i].before) continue;
        prepare(s);
        actual = run_child(s, i + 1, EIO);
        verify_recovery(s, false, &actual); reset_fixture(); ++failures;
        if (!strcmp(expected.points[i].operation, "file.write") || !strcmp(expected.points[i].operation, "atomic.create") ||
            !strcmp(expected.points[i].operation, "segment.create")) {
            prepare(s); actual = run_child(s, i + 1, ENOSPC);
            verify_recovery(s, false, &actual); reset_fixture(); ++failures;
        }
    }
    printf("crash matrix: %s: %zu termination points, %zu syscall failures, acknowledged restart passed\n",
        scenario_names[s], expected.count, failures);
}
static void test_initialization_crash_matrix(void) { matrix(INITIALIZE); }
static void test_table_creation_crash_matrix(void) { matrix(CREATE); }
static void test_first_append_crash_matrix(void) { matrix(FIRST_APPEND); }
static void test_size_rotation_crash_matrix(void) { matrix(ROTATE_SIZE); }
static void test_day_rotation_crash_matrix(void) { matrix(ROTATE_DAY); }
static void test_partial_retention_crash_matrix(void) { matrix(RETIRE_SOME); }
static void test_full_retention_crash_matrix(void) { matrix(RETIRE_ALL); }
static void test_configuration_crash_matrix(void) { matrix(CONFIGURE); }
static void test_tail_repair_crash_matrix(void) { matrix(REPAIR_TAIL); }
int main(void) {
    atomic_init(&armed, false);
    UNITY_BEGIN();
    RUN_TEST(test_initialization_crash_matrix);
    RUN_TEST(test_table_creation_crash_matrix);
    RUN_TEST(test_first_append_crash_matrix);
    RUN_TEST(test_size_rotation_crash_matrix);
    RUN_TEST(test_day_rotation_crash_matrix);
    RUN_TEST(test_partial_retention_crash_matrix);
    RUN_TEST(test_full_retention_crash_matrix);
    RUN_TEST(test_configuration_crash_matrix);
    RUN_TEST(test_tail_repair_crash_matrix);
    return UNITY_END();
}
