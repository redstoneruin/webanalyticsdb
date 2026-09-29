#include "auth.h"
#include "../core/internal.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
struct wadb_auth {
    pthread_mutex_t mutex;
    char token[65];
    wadb_credentials sessions[WADB_SESSION_LIMIT];
    uint64_t login_window;
    unsigned login_attempts;
};
bool wadb_random_hex(char *out, size_t bytes) {
    unsigned char buffer[64];
    if (!out || bytes > sizeof(buffer)) return false;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    size_t got = 0;
    while (got < bytes) {
        ssize_t n = read(fd, buffer + got, bytes - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return false; }
        got += (size_t)n;
    }
    close(fd);
    for (size_t i = 0; i < bytes; ++i) { out[2 * i] = "0123456789abcdef"[buffer[i] >> 4]; out[2 * i + 1] = "0123456789abcdef"[buffer[i] & 15]; }
    out[bytes * 2] = 0; return true;
}
static bool secret_equal(const char *a, const char *b) {
    if (!a || !b || strlen(a) != 64 || strlen(b) != 64) return false;
    volatile unsigned difference = 0;
    for (size_t i = 0; i < 64; ++i) difference |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return difference == 0;
}
static wadb_status load_token(const char *path, char out[65], wadb_error *error) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 && errno == ENOENT) {
        if (!wadb_random_hex(out, 32)) return wadb_fail(error, WADB_IO, errno, "generate admin token");
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) return wadb_fail(error, WADB_IO, errno, "create admin token file");
        char text[65]; memcpy(text, out, 64); text[64] = '\n'; size_t done = 0;
        while (done < sizeof(text)) {
            ssize_t n = write(fd, text + done, sizeof(text) - done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { int saved = errno; close(fd); return wadb_fail(error, WADB_IO, saved, "write admin token file"); }
            done += (size_t)n;
        }
        if (fsync(fd)) { int saved = errno; close(fd); return wadb_fail(error, WADB_IO, saved, "sync admin token file"); }
        close(fd); return WADB_OK;
    }
    if (fd < 0) return wadb_fail(error, WADB_IO, errno, "open admin token file");
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077) || (st.st_size != 64 && st.st_size != 65)) {
        close(fd); return wadb_fail(error, WADB_INVALID, 0, "admin token file must be owner-only, regular, and contain 64 hex characters");
    }
    char text[66]; size_t done = 0;
    while (done < (size_t)st.st_size) {
        ssize_t n = read(fd, text + done, (size_t)st.st_size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return wadb_fail(error, WADB_IO, 0, "read admin token file"); }
        done += (size_t)n;
    }
    close(fd);
    for (size_t i = 0; i < 64; ++i) if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f')))
        return wadb_fail(error, WADB_INVALID, 0, "admin token must contain lowercase hex characters");
    if (done == 65 && text[64] != '\n') return wadb_fail(error, WADB_INVALID, 0, "invalid admin token file terminator");
    memcpy(out, text, 64); out[64] = 0; return WADB_OK;
}
wadb_status wadb_auth_open(const char *path, wadb_auth **out, wadb_error *error) {
    if (!path || !out) return wadb_fail(error, WADB_INVALID, 0, "token path required");
    *out = NULL; wadb_auth *a = calloc(1, sizeof(*a));
    if (!a) return wadb_fail(error, WADB_NOMEM, 0, "authentication state");
    wadb_status status = load_token(path, a->token, error);
    if (status) { free(a); return status; }
    int rc = pthread_mutex_init(&a->mutex, NULL);
    if (rc) { free(a); return wadb_fail(error, WADB_IO, rc, "authentication mutex"); }
    *out = a; return WADB_OK;
}
void wadb_auth_close(wadb_auth *a) { if (a) { pthread_mutex_destroy(&a->mutex); memset(a, 0, sizeof(*a)); free(a); } }
wadb_status wadb_auth_login(wadb_auth *a, const char *token, size_t length, wadb_credentials *out, wadb_error *error) {
    uint64_t now = wadb_monotonic_ns();
    pthread_mutex_lock(&a->mutex);
    if (now - a->login_window >= UINT64_C(1000000000)) { a->login_window = now; a->login_attempts = 0; }
    if (++a->login_attempts > 10) { pthread_mutex_unlock(&a->mutex); return wadb_fail(error, WADB_BACKPRESSURE, 0, "login rate limit; retry in one second"); }
    if (!token || length != 64 || !secret_equal(a->token, token)) { pthread_mutex_unlock(&a->mutex); return wadb_fail(error, WADB_INVALID, 0, "invalid admin token"); }
    size_t slot = 0;
    for (size_t i = 1; i < WADB_SESSION_LIMIT; ++i) if (a->sessions[i].expires_ns < a->sessions[slot].expires_ns) slot = i;
    wadb_credentials c = {0};
    if (!wadb_random_hex(c.session, 32) || !wadb_random_hex(c.csrf, 32)) { pthread_mutex_unlock(&a->mutex); return wadb_fail(error, WADB_IO, errno, "generate session secret"); }
    c.expires_ns = now + UINT64_C(43200000000000); a->sessions[slot] = c; *out = c;
    pthread_mutex_unlock(&a->mutex); return WADB_OK;
}
bool wadb_auth_bearer(wadb_auth *a, const char *header) { return header && !strncmp(header, "Bearer ", 7) && secret_equal(a->token, header + 7); }
bool wadb_auth_session(wadb_auth *a, const char *session, const char *csrf, bool mutation, wadb_credentials *out) {
    bool valid = false; uint64_t now = wadb_monotonic_ns();
    pthread_mutex_lock(&a->mutex);
    for (size_t i = 0; i < WADB_SESSION_LIMIT; ++i) {
        wadb_credentials *c = &a->sessions[i];
        if (c->expires_ns > now && secret_equal(c->session, session) && (!mutation || secret_equal(c->csrf, csrf))) {
            if (out) *out = *c;
            valid = true; break;
        }
    }
    pthread_mutex_unlock(&a->mutex); return valid;
}
void wadb_auth_logout(wadb_auth *a, const char *session) {
    pthread_mutex_lock(&a->mutex);
    for (size_t i = 0; i < WADB_SESSION_LIMIT; ++i) if (secret_equal(a->sessions[i].session, session)) memset(&a->sessions[i], 0, sizeof(a->sessions[i]));
    pthread_mutex_unlock(&a->mutex);
}
