#include "../src/core/internal.h"
#include "../test/unity/unity.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char directory[64], path[96];
void setUp(void) {
    strcpy(directory, "/tmp/wadb-files-XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(directory));
    snprintf(path, sizeof(path), "%s/value", directory);
}
void tearDown(void) {
    unlink(path);
    TEST_ASSERT_EQUAL_INT(0, rmdir(directory));
}

typedef struct { unsigned calls; size_t written; size_t fail_after; } fault_state;
static ssize_t short_write(void *context, int fd, const void *data, size_t length) {
    fault_state *f = context;
    if (!f->calls++) { errno = EINTR; return -1; }
    if (f->written >= f->fail_after) { errno = ENOSPC; return -1; }
    size_t n = length < 3 ? length : 3;
    if (n > f->fail_after - f->written) n = f->fail_after - f->written;
    ssize_t result = write(fd, data, n);
    if (result > 0) f->written += (size_t)result;
    return result;
}
static int failed_sync(void *context, int fd) {
    (void)context; (void)fd;
    errno = EIO;
    return -1;
}

static void test_atomic_replace_and_read_limits(void) {
    wadb_error e;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_atomic_file(path, "first", 5, &e));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_atomic_file(path, "replacement", 11, &e));
    unsigned char *data = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL(WADB_LIMIT, wadb_read_file(path, 10, &data, &n, &e));
    TEST_ASSERT_NULL(data);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_read_file(path, 11, &data, &n, &e));
    TEST_ASSERT_EQUAL_UINT32(11, n);
    TEST_ASSERT_EQUAL_MEMORY("replacement", data, n);
    free(data);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_sync_directory(directory, &e));
}

static void test_short_writes_and_interruption(void) {
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    TEST_ASSERT_TRUE(fd >= 0);
    fault_state state = {.fail_after = SIZE_MAX};
    wadb_io io = {.context = &state, .write = short_write};
    const char input[] = "each byte must be written once";
    char output[sizeof(input)];
    TEST_ASSERT_EQUAL(WADB_OK, wadb_write_all(&io, fd, input, sizeof(input), NULL));
    TEST_ASSERT_TRUE(state.calls > 2);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_sync_file(NULL, fd, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_pread_all(fd, output, sizeof(output), 0, NULL));
    TEST_ASSERT_EQUAL_MEMORY(input, output, sizeof(input));
    TEST_ASSERT_EQUAL(WADB_CORRUPT, wadb_pread_all(fd, output, 1, sizeof(input), NULL));
    TEST_ASSERT_EQUAL(WADB_LIMIT, wadb_pread_all(fd, output, 1, UINT64_MAX, NULL));
    close(fd);
}

static void test_partial_write_and_sync_failure_are_errors(void) {
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    TEST_ASSERT_TRUE(fd >= 0);
    fault_state state = {.fail_after = 7};
    wadb_io io = {.context = &state, .write = short_write, .sync = failed_sync};
    wadb_error e;
    TEST_ASSERT_EQUAL(WADB_IO, wadb_write_all(&io, fd, "0123456789", 10, &e));
    TEST_ASSERT_EQUAL_INT(ENOSPC, e.system_errno);
    TEST_ASSERT_EQUAL_UINT32(7, state.written);
    TEST_ASSERT_EQUAL(WADB_IO, wadb_sync_file(&io, fd, &e));
    TEST_ASSERT_EQUAL_INT(EIO, e.system_errno);
    close(fd);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_atomic_replace_and_read_limits);
    RUN_TEST(test_short_writes_and_interruption);
    RUN_TEST(test_partial_write_and_sync_failure_are_errors);
    return UNITY_END();
}
