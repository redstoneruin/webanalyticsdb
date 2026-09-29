#include "internal.h"

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

_Static_assert(CHAR_BIT == 8, "WebAnalyticsDB requires 8-bit bytes");

wadb_status wadb_fail(wadb_error *e, wadb_status code, int n, const char *fmt, ...) {
    if (e) {
        e->code = code;
        e->system_errno = n;
        va_list args;
        va_start(args, fmt);
        vsnprintf(e->message, sizeof(e->message), fmt, args);
        va_end(args);
    }
    return code;
}

void wadb_error_clear(wadb_error *e) { if (e) memset(e, 0, sizeof(*e)); }

const char *wadb_status_name(wadb_status status) {
    static const char *names[] = {
        "ok", "invalid", "out_of_memory", "io_error", "corrupt", "unsupported_version",
        "already_exists", "not_found", "locked", "schema_mismatch", "backpressure",
        "resource_limit", "indeterminate_commit", "expired", "read_only", "cancelled"
    };
    return (unsigned)status < sizeof(names) / sizeof(*names) ? names[status] : "unknown";
}

bool wadb_add_size(size_t a, size_t b, size_t *out) {
    if (a > SIZE_MAX - b) return false;
    *out = a + b;
    return true;
}
bool wadb_mul_size(size_t a, size_t b, size_t *out) {
    if (b && a > SIZE_MAX / b) return false;
    *out = a * b;
    return true;
}

bool wadb_identifier(const char *name) {
    if (!name || !name[0]) return false;
    for (size_t i = 0; i < WADB_NAME_CAP; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (!c) return true;
        if (!(c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (i && c >= '0' && c <= '9'))) return false;
    }
    return false;
}

bool wadb_utf8(const void *data, size_t n) {
    if (n && !data) return false;
    const unsigned char *p = data;
    for (size_t i = 0; i < n;) {
        uint32_t cp = p[i++], min;
        unsigned more;
        if (cp < 0x80) continue;
        if (cp >= 0xc2 && cp <= 0xdf) { more = 1; min = 0x80; cp &= 0x1f; }
        else if (cp >= 0xe0 && cp <= 0xef) { more = 2; min = 0x800; cp &= 0x0f; }
        else if (cp >= 0xf0 && cp <= 0xf4) { more = 3; min = 0x10000; cp &= 7; }
        else return false;
        if (more > n - i) return false;
        while (more--) {
            if ((p[i] & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (p[i++] & 0x3f);
        }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    }
    return true;
}

uint16_t wadb_get_u16(const void *v) {
    const unsigned char *p = v;
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}
uint32_t wadb_get_u32(const void *v) {
    const unsigned char *p = v;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
uint64_t wadb_get_u64(const void *v) {
    const unsigned char *p = v;
    return (uint64_t)wadb_get_u32(p) | (uint64_t)wadb_get_u32(p + 4) << 32;
}
int64_t wadb_get_i64(const void *p) {
    uint64_t u = wadb_get_u64(p);
    return u <= INT64_MAX ? (int64_t)u : -1 - (int64_t)(UINT64_MAX - u);
}
void wadb_put_u16(void *v, uint16_t n) {
    unsigned char *p = v;
    p[0] = (unsigned char)n; p[1] = (unsigned char)(n >> 8);
}
void wadb_put_u32(void *v, uint32_t n) {
    unsigned char *p = v;
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(n >> (i * 8));
}
void wadb_put_u64(void *v, uint64_t n) {
    unsigned char *p = v;
    for (unsigned i = 0; i < 8; ++i) p[i] = (unsigned char)(n >> (i * 8));
}

static uint32_t crc_table[256];
static pthread_once_t crc_once = PTHREAD_ONCE_INIT;
static void crc_init(void) {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t x = i;
        for (unsigned j = 0; j < 8; ++j) x = (x >> 1) ^ ((x & 1) ? UINT32_C(0x82f63b78) : 0);
        crc_table[i] = x;
    }
}
uint32_t wadb_crc32c(const void *data, size_t n) {
    pthread_once(&crc_once, crc_init);
    const unsigned char *p = data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < n; ++i) crc = crc_table[(crc ^ p[i]) & 255] ^ (crc >> 8);
    return ~crc;
}
uint64_t wadb_hash(const void *data, size_t n, uint64_t hash) {
    const unsigned char *p = data;
    for (size_t i = 0; i < n; ++i) hash = (hash ^ p[i]) * UINT64_C(1099511628211);
    return hash;
}
