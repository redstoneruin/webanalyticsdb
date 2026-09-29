#ifndef WADB_FAULT_H
#define WADB_FAULT_H

/* The crash-test archive alone enables syscall boundary instrumentation. Normal
 * libraries compile this to the original call, with no hooks, state, or strings.
 * A test may terminate the process before/after a successful call, or make a
 * before boundary return -1 with an injected errno. */
#ifdef WADB_TEST_FAULTS
#include <stdint.h>
int wadb_test_io_before(const char *operation, const char *path);
int64_t wadb_test_io_after(const char *operation, const char *path, int64_t result);
#define WADB_IO_CALL(operation, path, call) \
    (wadb_test_io_before(operation, path) ? -1 : wadb_test_io_after(operation, path, (call)))
#else
#define WADB_IO_CALL(operation, path, call) (call)
#endif

#endif
