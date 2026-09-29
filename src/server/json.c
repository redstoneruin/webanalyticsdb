#include "json.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

union wadb_json_block {
    max_align_t alignment;
    struct { size_t bytes; wadb_json_block *previous, *next; } header;
};
static void *allocate(void *ctx, size_t n) {
    wadb_json_memory *m = ctx;
    if (n > SIZE_MAX - sizeof(wadb_json_block) || n + sizeof(wadb_json_block) > m->limit - m->used) {
        m->failure = WADB_LIMIT; return NULL;
    }
    wadb_json_block *p = malloc(sizeof(*p) + n);
    if (!p) { m->failure = WADB_NOMEM; return NULL; }
    p->header.bytes = n + sizeof(*p); p->header.previous = NULL; p->header.next = m->blocks;
    if (m->blocks) m->blocks->header.previous = p;
    m->blocks = p; m->used += p->header.bytes;
    return p + 1;
}
static void release(void *ctx, void *ptr) {
    if (!ptr) return;
    wadb_json_memory *m = ctx; wadb_json_block *p = (wadb_json_block *)ptr - 1;
    if (p->header.previous) p->header.previous->header.next = p->header.next; else m->blocks = p->header.next;
    if (p->header.next) p->header.next->header.previous = p->header.previous;
    m->used -= p->header.bytes; free(p);
}
static void *resize(void *ctx, void *ptr, size_t old_size, size_t n) {
    (void)old_size;
    if (!ptr) return allocate(ctx, n);
    if (!n) { release(ctx, ptr); return NULL; }
    wadb_json_memory *m = ctx; wadb_json_block *p = (wadb_json_block *)ptr - 1;
    size_t old = p->header.bytes;
    if (n > SIZE_MAX - sizeof(*p) || n + sizeof(*p) > m->limit - (m->used - old)) { m->failure = WADB_LIMIT; return NULL; }
    wadb_json_block *next = p->header.next, *previous = p->header.previous;
    p = realloc(p, sizeof(*p) + n);
    if (!p) { m->failure = WADB_NOMEM; return NULL; }
    p->header.bytes = n + sizeof(*p);
    if (previous) previous->header.next = p; else m->blocks = p;
    if (next) next->header.previous = p;
    m->used = m->used - old + p->header.bytes;
    return p + 1;
}
static bool bad(wadb_json *j, const char *message) { wadb_fail(&j->error, WADB_INVALID, 0, "%s", message); return false; }
static bool validate_tree(wadb_json *j, yyjson_val *v, unsigned depth, size_t *nodes) {
    if (depth > 32 || ++*nodes > 200000) return bad(j, "JSON depth or value count exceeded");
    if (yyjson_is_arr(v)) {
        yyjson_arr_iter iter = yyjson_arr_iter_with(v); yyjson_val *item;
        while ((item = yyjson_arr_iter_next(&iter))) if (!validate_tree(j, item, depth + 1, nodes)) return false;
    } else if (yyjson_is_obj(v)) {
        if (yyjson_obj_size(v) > 256) return bad(j, "JSON object has too many fields");
        yyjson_val *keys[256]; size_t count = 0;
        yyjson_obj_iter iter = yyjson_obj_iter_with(v); yyjson_val *key;
        while ((key = yyjson_obj_iter_next(&iter))) {
            size_t n = yyjson_get_len(key);
            if (!n || n >= WADB_NAME_CAP || strlen(yyjson_get_str(key)) != n) return bad(j, "invalid JSON object key");
            for (size_t i = 0; i < count; ++i)
                if (yyjson_get_len(keys[i]) == n && !memcmp(yyjson_get_str(keys[i]), yyjson_get_str(key), n)) return bad(j, "duplicate JSON object key");
            keys[count++] = key;
            if (!validate_tree(j, yyjson_obj_iter_get_val(key), depth + 1, nodes)) return false;
        }
    }
    return true;
}
bool wadb_json_init(wadb_json *j, const char *body, size_t n) {
    memset(j, 0, sizeof(*j)); j->memory.limit = WADB_HTTP_JSON_LIMIT;
    j->reply_limit = WADB_HTTP_REPLY_LIMIT;
    j->allocator = (yyjson_alc){.malloc = allocate, .realloc = resize, .free = release, .ctx = &j->memory};
    if (n > WADB_HTTP_BODY_LIMIT || (n && !body)) { wadb_fail(&j->error, WADB_LIMIT, 0, "HTTP body limit exceeded"); return false; }
    if (!n) { body = "{}"; n = 2; }
    yyjson_read_err error;
    j->input = yyjson_read_opts((char *)(void *)body, n, 0, &j->allocator, &error);
    if (!j->input) { wadb_fail(&j->error, j->memory.failure ? j->memory.failure : WADB_INVALID, 0, "invalid JSON near byte %zu", error.pos); return false; }
    size_t nodes = 0;
    if (!validate_tree(j, yyjson_doc_get_root(j->input), 0, &nodes)) return false;
    if (!yyjson_is_obj(yyjson_doc_get_root(j->input))) return bad(j, "request body must be an object");
    j->output = yyjson_mut_doc_new(&j->allocator);
    if (!j->output) { wadb_fail(&j->error, j->memory.failure, 0, "JSON allocation failed"); return false; }
    j->root = wadb_json_object(j); yyjson_mut_doc_set_root(j->output, j->root);
    if (!j->root) wadb_fail(&j->error, j->memory.failure ? j->memory.failure : WADB_NOMEM, 0, "JSON root allocation");
    return !j->error.code;
}
void wadb_json_destroy(wadb_json *j) {
    yyjson_doc_free(j->input); yyjson_mut_doc_free(j->output);
    while (j->memory.blocks) release(&j->memory, j->memory.blocks + 1);
    memset(j, 0, sizeof(*j));
}
bool wadb_json_allowed(wadb_json *j, yyjson_val *object, const char *fields) {
    if (!yyjson_is_obj(object)) return bad(j, "expected an object");
    yyjson_obj_iter it = yyjson_obj_iter_with(object); yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&it))) {
        size_t n = yyjson_get_len(key); const char *p = fields; bool found = false;
        while (*p) {
            const char *end = strchr(p, '|'); size_t size = end ? (size_t)(end - p) : strlen(p);
            if (size == n && !memcmp(p, yyjson_get_str(key), n)) { found = true; break; }
            if (!end) break;
            p = end + 1;
        }
        if (!found) { wadb_fail(&j->error, WADB_INVALID, 0, "unknown argument: %s", yyjson_get_str(key)); return false; }
    }
    return true;
}
const char *wadb_json_name(wadb_json *j, yyjson_val *v, const char *what) {
    const char *s = yyjson_get_str(v);
    if (!s || yyjson_get_len(v) != strlen(s) || !wadb_identifier(s)) {
        wadb_fail(&j->error, WADB_INVALID, 0, "invalid %s", what); return NULL;
    }
    return s;
}
bool wadb_decimal_u64(const char *s, size_t n, uint64_t *out) {
    if (!s || !n || n > 20) return false;
    uint64_t value = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned digit = (unsigned char)s[i] - (unsigned)'0';
        if (digit > 9 || value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *out = value; return true;
}
bool wadb_json_u64(wadb_json *j, yyjson_val *v, uint64_t *out) {
    if (yyjson_is_uint(v) && yyjson_get_uint(v) <= UINT64_C(9007199254740991)) { *out = yyjson_get_uint(v); return true; }
    if (yyjson_is_str(v) && wadb_decimal_u64(yyjson_get_str(v), yyjson_get_len(v), out)) return true;
    return bad(j, "expected a nonnegative integer; use decimal strings beyond JavaScript's exact range");
}
bool wadb_json_i64(wadb_json *j, yyjson_val *v, int64_t *out) {
    if (yyjson_is_int(v)) {
        if (yyjson_is_uint(v) && yyjson_get_uint(v) <= UINT64_C(9007199254740991)) { *out = (int64_t)yyjson_get_uint(v); return true; }
        if (yyjson_is_sint(v) && yyjson_get_sint(v) >= -INT64_C(9007199254740991) && yyjson_get_sint(v) <= INT64_C(9007199254740991)) { *out = yyjson_get_sint(v); return true; }
    }
    if (yyjson_is_str(v)) {
        const char *s = yyjson_get_str(v); size_t n = yyjson_get_len(v); bool negative = n && *s == '-'; uint64_t value;
        if (negative) { ++s; --n; }
        if (wadb_decimal_u64(s, n, &value) && value <= (uint64_t)INT64_MAX + (negative ? 1u : 0u)) {
            *out = value == (UINT64_C(1) << 63) ? INT64_MIN : negative ? -(int64_t)value : (int64_t)value;
            return true;
        }
    }
    return bad(j, "expected a signed integer; use decimal strings beyond JavaScript's exact range");
}
bool wadb_json_bool(wadb_json *j, yyjson_val *obj, const char *key, bool *out) {
    yyjson_val *v = yyjson_obj_get(obj, key);
    if (!v) return true;
    if (!yyjson_is_bool(v)) return bad(j, "expected boolean argument");
    *out = yyjson_get_bool(v); return true;
}
static int hex(unsigned char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
bool wadb_json_value(wadb_json *j, const wadb_field *f, yyjson_val *v, wadb_value *out) {
    memset(out, 0, sizeof(*out));
    if (!v || yyjson_is_null(v)) {
        if (!f->nullable) return bad(j, "missing or null value for a required field");
        out->is_null = true; return true;
    }
    switch (f->type) {
        case WADB_I8: case WADB_I16: case WADB_I32: case WADB_I64: case WADB_TIMESTAMP_US:
            return wadb_json_i64(j, v, &out->as.i64);
        case WADB_U8: case WADB_U16: case WADB_U32: case WADB_U64: return wadb_json_u64(j, v, &out->as.u64);
        case WADB_BOOL:
            if (!yyjson_is_bool(v)) return bad(j, "boolean field requires true or false");
            out->as.u64 = yyjson_get_bool(v); return true;
        case WADB_F32: case WADB_F64:
            if (!yyjson_is_num(v) || !isfinite(yyjson_get_num(v))) return bad(j, "floating field requires a finite number");
            out->as.f64 = yyjson_get_num(v); return true;
        case WADB_TEXT: case WADB_SYMBOL32:
            if (!yyjson_is_str(v)) return bad(j, "text field requires a string");
            out->as.bytes.data = yyjson_get_str(v); out->as.bytes.length = yyjson_get_len(v); return true;
        case WADB_UUID: case WADB_BYTES: {
            if (!yyjson_is_str(v) || yyjson_get_len(v) != (size_t)f->width * 2) return bad(j, "fixed bytes require a hex string of exactly twice the byte width");
            unsigned char *bytes = allocate(&j->memory, f->width);
            if (!bytes) { wadb_fail(&j->error, j->memory.failure, 0, "binary field allocation"); return false; }
            const unsigned char *s = (const unsigned char *)yyjson_get_str(v);
            for (uint32_t i = 0; i < f->width; ++i) {
                int hi = hex(s[i * 2]), lo = hex(s[i * 2 + 1]);
                if (hi < 0 || lo < 0) return bad(j, "invalid hex byte string");
                bytes[i] = (unsigned char)(hi * 16 + lo);
            }
            out->as.bytes.data = bytes; out->as.bytes.length = f->width; return true;
        }
        default: return bad(j, "unsupported field type");
    }
}
yyjson_mut_val *wadb_json_object(wadb_json *j) { return yyjson_mut_obj(j->output); }
yyjson_mut_val *wadb_json_array(wadb_json *j) { return yyjson_mut_arr(j->output); }
void wadb_json_add(wadb_json *j, yyjson_mut_val *o, const char *key, yyjson_mut_val *v) {
    if (!yyjson_mut_obj_add(o, yyjson_mut_strcpy(j->output, key), v) && !j->error.code)
        wadb_fail(&j->error, j->memory.failure ? j->memory.failure : WADB_NOMEM, 0, "JSON output allocation");
}
void wadb_json_push(wadb_json *j, yyjson_mut_val *a, yyjson_mut_val *v) {
    if (!yyjson_mut_arr_append(a, v) && !j->error.code)
        wadb_fail(&j->error, j->memory.failure ? j->memory.failure : WADB_NOMEM, 0, "JSON output allocation");
}
void wadb_json_string(wadb_json *j, yyjson_mut_val *o, const char *k, const char *v) { wadb_json_add(j, o, k, yyjson_mut_strcpy(j->output, v ? v : "")); }
void wadb_json_uint(wadb_json *j, yyjson_mut_val *o, const char *k, uint64_t v) { char s[32]; snprintf(s, sizeof(s), "%" PRIu64, v); wadb_json_string(j, o, k, s); }
void wadb_json_int(wadb_json *j, yyjson_mut_val *o, const char *k, int64_t v) { char s[32]; snprintf(s, sizeof(s), "%" PRId64, v); wadb_json_string(j, o, k, s); }
void wadb_json_boolean(wadb_json *j, yyjson_mut_val *o, const char *k, bool v) { wadb_json_add(j, o, k, yyjson_mut_bool(j->output, v)); }
yyjson_mut_val *wadb_json_encode_value(wadb_json *j, wadb_type type, const wadb_value *v) {
    if (v->is_null) return yyjson_mut_null(j->output);
    char text[32];
    switch (type) {
        case WADB_I64: case WADB_TIMESTAMP_US: snprintf(text, sizeof(text), "%" PRId64, v->as.i64); return yyjson_mut_strcpy(j->output, text);
        case WADB_U64: snprintf(text, sizeof(text), "%" PRIu64, v->as.u64); return yyjson_mut_strcpy(j->output, text);
        case WADB_I8: case WADB_I16: case WADB_I32: return yyjson_mut_sint(j->output, v->as.i64);
        case WADB_U8: case WADB_U16: case WADB_U32: return yyjson_mut_uint(j->output, v->as.u64);
        case WADB_BOOL: return yyjson_mut_bool(j->output, v->as.u64 != 0);
        case WADB_F32: case WADB_F64: return yyjson_mut_real(j->output, v->as.f64);
        case WADB_TEXT: case WADB_SYMBOL32: return yyjson_mut_strncpy(j->output, v->as.bytes.data, v->as.bytes.length);
        case WADB_UUID: case WADB_BYTES: {
            char bytes[WADB_MAX_ROW_BYTES * 2]; const unsigned char *src = v->as.bytes.data;
            for (size_t i = 0; i < v->as.bytes.length; ++i) { bytes[i * 2] = "0123456789abcdef"[src[i] >> 4]; bytes[i * 2 + 1] = "0123456789abcdef"[src[i] & 15]; }
            return yyjson_mut_strncpy(j->output, bytes, v->as.bytes.length * 2);
        }
        default: return NULL;
    }
}
unsigned wadb_http_status(wadb_status s) {
    switch (s) {
        case WADB_OK: return 200;
        case WADB_INVALID: case WADB_SCHEMA_MISMATCH: return 400;
        case WADB_NOT_FOUND: return 404;
        case WADB_EXISTS: case WADB_LOCKED: return 409;
        case WADB_EXPIRED: return 410;
        case WADB_LIMIT: return 422;
        case WADB_BACKPRESSURE: return 429;
        default: return 503;
    }
}
void wadb_http_reply_free(wadb_http_reply *r) { free(r->body); memset(r, 0, sizeof(*r)); }
void wadb_http_error(unsigned status, wadb_status code, const char *message, wadb_http_reply *r) {
    memset(r, 0, sizeof(*r)); r->status = status;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (doc) {
        yyjson_mut_val *root = yyjson_mut_obj(doc), *error = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
        yyjson_mut_obj_add_bool(doc, root, "ok", false); yyjson_mut_obj_add_val(doc, root, "error", error);
        yyjson_mut_obj_add_strcpy(doc, error, "code", wadb_status_name(code));
        yyjson_mut_obj_add_strcpy(doc, error, "message", message ? message : wadb_status_name(code));
        r->body = yyjson_mut_write(doc, 0, &r->length); yyjson_mut_doc_free(doc);
    }
    if (!r->body) { r->status = 503; r->body = strdup("{\"ok\":false,\"error\":{\"code\":\"out_of_memory\",\"message\":\"Response allocation failed\"}}"); r->length = r->body ? strlen(r->body) : 0; }
}
void wadb_json_finish(wadb_json *j, unsigned status, wadb_http_reply *r) {
    memset(r, 0, sizeof(*r));
    if (!j->error.code && j->memory.failure) wadb_fail(&j->error, j->memory.failure, 0, "JSON memory budget exhausted");
    if (!j->error.code) {
        size_t length = 0;
        char *body = yyjson_mut_write_opts(j->output, 0, &j->allocator, &length, NULL);
        if (!body || length > j->reply_limit) wadb_fail(&j->error, WADB_LIMIT, 0, "response size limit exceeded; request fewer rows");
        else {
            r->body = malloc(length + 1);
            if (!r->body) wadb_fail(&j->error, WADB_NOMEM, 0, "response allocation");
            else { memcpy(r->body, body, length + 1); r->length = length; r->status = status; }
        }
    }
    if (j->error.code) wadb_http_error(wadb_http_status(j->error.code), j->error.code, j->error.message, r);
}
