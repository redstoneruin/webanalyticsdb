#include "../core/internal.h"
#include "fault.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int64_t wadb_realtime_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts)) return -1;
    if (ts.tv_sec < 0 || (uint64_t)ts.tv_sec > ((uint64_t)INT64_MAX - 999999) / 1000000) return -1;
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
uint64_t wadb_monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;
}
wadb_status wadb_write_all(const wadb_io *io, int fd, const void *data, size_t n, wadb_error *e) {
    const unsigned char *p = data;
    size_t done = 0;
    while (done < n) {
        ssize_t wrote = io && io->write ? io->write(io->context, fd, p + done, n - done) :
            WADB_IO_CALL("file.write", NULL, write(fd, p + done, n - done));
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0 || (size_t)wrote > n - done)
            return wadb_fail(e, WADB_IO, wrote < 0 ? errno : EIO, "write failed after %zu of %zu bytes", done, n);
        done += (size_t)wrote;
    }
    return WADB_OK;
}
wadb_status wadb_pread_all(int fd, void *data, size_t n, uint64_t offset, wadb_error *e) {
    if (offset > INT64_MAX || n > (uint64_t)INT64_MAX - offset)
        return wadb_fail(e, WADB_LIMIT, 0, "file offset exceeds signed 64-bit range");
    size_t done = 0;
    unsigned char *p = data;
    while (done < n) {
        ssize_t got = pread(fd, p + done, n - done, (off_t)(offset + done));
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) return wadb_fail(e, WADB_IO, errno, "pread failed");
        if (!got) return wadb_fail(e, WADB_CORRUPT, 0, "unexpected end of file");
        done += (size_t)got;
    }
    return WADB_OK;
}
wadb_status wadb_sync_file(const wadb_io *io, int fd, wadb_error *e) {
    int rc;
    do {
        if (io && io->sync) rc = io->sync(io->context, fd);
#ifdef __APPLE__
        else rc = WADB_IO_CALL("file.sync", NULL, fcntl(fd, F_FULLFSYNC));
#else
        else rc = WADB_IO_CALL("file.sync", NULL, fdatasync(fd));
#endif
    } while (rc && errno == EINTR);
    if (rc) return wadb_fail(e, WADB_IO, errno, "durable file synchronization failed");
    return WADB_OK;
}
wadb_status wadb_mkdir(const char *path, wadb_error *e) {
    if (!WADB_IO_CALL("directory.create", path, mkdir(path, 0700))) return WADB_OK;
    if (errno != EEXIST) return wadb_fail(e, WADB_IO, errno, "cannot create directory %s", path);
    struct stat st;
    if (lstat(path, &st)) return wadb_fail(e, WADB_IO, errno, "cannot inspect directory %s", path);
    if (!S_ISDIR(st.st_mode)) return wadb_fail(e, WADB_IO, ENOTDIR, "path is not a directory: %s", path);
    return WADB_OK;
}
wadb_status wadb_sync_directory(const char *path, wadb_error *e) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return wadb_fail(e, WADB_IO, errno, "cannot open directory %s", path);
    int rc;
    do { rc = WADB_IO_CALL("directory.sync", path, fsync(fd)); } while (rc && errno == EINTR);
    int saved = errno;
    close(fd);
    if (rc) return wadb_fail(e, WADB_IO, saved, "cannot synchronize directory %s", path);
    return WADB_OK;
}
wadb_status wadb_atomic_file(const char *path, const void *data, size_t n, wadb_error *e) {
    size_t len = strlen(path);
    if (!len || len > 4096) return wadb_fail(e, WADB_LIMIT, 0, "metadata path length invalid");
    char *temp = malloc(len + 16), *parent = malloc(len + 2);
    if (!temp || !parent) { free(temp); free(parent); return wadb_fail(e, WADB_NOMEM, 0, "metadata path allocation"); }
    strcpy(parent, path);
    snprintf(temp, len + 16, "%s.tmp.XXXXXX", path);
    char *slash = strrchr(parent, '/');
    if (!slash) strcpy(parent, ".");
    else if (slash == parent) parent[1] = '\0';
    else *slash = '\0';
    int fd = WADB_IO_CALL("atomic.create", path, mkstemp(temp));
    if (fd < 0) { int saved = errno; free(temp); free(parent); return wadb_fail(e, WADB_IO, saved, "create temporary metadata file"); }
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    wadb_status status = wadb_write_all(NULL, fd, data, n, e);
    if (!status) status = wadb_sync_file(NULL, fd, e);
    if (close(fd) && !status) status = wadb_fail(e, WADB_IO, errno, "close metadata file");
    if (!status && WADB_IO_CALL("atomic.rename", path, rename(temp, path)))
        status = wadb_fail(e, WADB_IO, errno, "install metadata file");
    if (!status) {
        status = wadb_sync_directory(parent, e);
        if (status) status = wadb_fail(e, WADB_INDETERMINATE, e ? e->system_errno : 0,
            "metadata installed but directory synchronization failed");
    }
    if (status) unlink(temp);
    free(temp); free(parent);
    return status;
}
wadb_status wadb_read_file(const char *path, size_t limit, unsigned char **out,
    size_t *length, wadb_error *e) {
    *out = NULL; *length = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return wadb_fail(e, errno == ENOENT ? WADB_NOT_FOUND : WADB_IO, errno, "open %s", path);
    struct stat st;
    if (fstat(fd, &st)) { int saved = errno; close(fd); return wadb_fail(e, WADB_IO, saved, "stat %s", path); }
    if (!S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > limit) {
        close(fd); return wadb_fail(e, WADB_LIMIT, 0, "invalid file type or oversized file %s", path);
    }
    size_t n = (size_t)st.st_size;
    unsigned char *p = malloc(n ? n : 1);
    if (!p) { close(fd); return wadb_fail(e, WADB_NOMEM, 0, "allocate file buffer"); }
    wadb_status status = wadb_pread_all(fd, p, n, 0, e);
    close(fd);
    if (status) { free(p); return status; }
    *out = p; *length = n;
    return WADB_OK;
}
