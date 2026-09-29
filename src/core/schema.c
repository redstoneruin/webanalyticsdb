#include "internal.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <string.h>

_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128,
               "IEEE-754 binary32 required");
_Static_assert(sizeof(double) == 8 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
               "IEEE-754 binary64 required");

const char *wadb_type_name(wadb_type type) {
    static const char *names[] = {"invalid", "i8", "u8", "i16", "u16", "i32", "u32",
        "i64", "u64", "f32", "f64", "bool", "timestamp_us", "uuid", "bytes", "text", "symbol32"};
    return (unsigned)type < sizeof(names) / sizeof(*names) ? names[type] : "invalid";
}
wadb_type wadb_type_parse(const char *name) {
    if (name) for (unsigned i = WADB_I8; i <= WADB_SYMBOL32; ++i)
        if (!strcmp(name, wadb_type_name((wadb_type)i))) return (wadb_type)i;
    return 0;
}

static uint32_t type_width(wadb_type t, uint32_t size) {
    switch (t) {
        case WADB_I8: case WADB_U8: case WADB_BOOL: return 1;
        case WADB_I16: case WADB_U16: return 2;
        case WADB_I32: case WADB_U32: case WADB_F32: case WADB_SYMBOL32: return 4;
        case WADB_I64: case WADB_U64: case WADB_F64: case WADB_TIMESTAMP_US: return 8;
        case WADB_UUID: return 16;
        case WADB_BYTES: return size;
        case WADB_TEXT: return size + 4;
        default: return 0;
    }
}

wadb_status wadb_schema_build(wadb_schema *out, const wadb_field_def *defs,
                             size_t n, wadb_error *e) {
    wadb_error_clear(e);
    if (!out || (n && !defs) || n >= WADB_MAX_FIELDS)
        return wadb_fail(e, WADB_INVALID, 0, "schema requires at most %u user fields", WADB_MAX_FIELDS - 1);
    wadb_schema schema = {0};
    schema.field_count = (uint32_t)n + 1;
    uint32_t nullable = 0;
    for (size_t i = 0; i < n; ++i) {
        const wadb_field_def *d = &defs[i];
        if (!wadb_identifier(d->name) || !strcmp(d->name, "_time"))
            return wadb_fail(e, WADB_INVALID, 0, "invalid or reserved field name at index %zu", i);
        for (size_t j = 0; j < i; ++j) if (!strcmp(d->name, defs[j].name))
            return wadb_fail(e, WADB_INVALID, 0, "duplicate field: %s", d->name);
        bool sized = d->type == WADB_BYTES || d->type == WADB_TEXT;
        if ((sized && (!d->size || d->size > WADB_MAX_ROW_BYTES)) || (!sized && d->size))
            return wadb_fail(e, WADB_INVALID, 0, "invalid size for %s", d->name);
        if (!type_width(d->type, d->size))
            return wadb_fail(e, WADB_INVALID, 0, "unknown field type for %s", d->name);
        nullable += d->nullable;
    }
    schema.null_bytes = (nullable + 7) / 8;
    /* Keep _time at offset zero for indexing; the bitmap follows it. */
    wadb_field *time = &schema.fields[0];
    strcpy(time->name, "_time");
    time->type = WADB_TIMESTAMP_US; time->width = 8; time->null_bit = UINT32_MAX;
    uint32_t offset = 8 + schema.null_bytes, null_bit = 0;
    for (size_t i = 0; i < n; ++i) {
        wadb_field *f = &schema.fields[i + 1];
        strcpy(f->name, defs[i].name);
        f->type = defs[i].type; f->size = defs[i].size; f->nullable = defs[i].nullable;
        f->offset = offset; f->width = type_width(f->type, f->size);
        f->null_bit = f->nullable ? null_bit++ : UINT32_MAX;
        if (f->width > WADB_MAX_ROW_BYTES - offset)
            return wadb_fail(e, WADB_LIMIT, 0, "row exceeds %u bytes", WADB_MAX_ROW_BYTES);
        offset += f->width;
    }
    schema.row_width = (offset + 7u) & ~7u;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (uint32_t i = 0; i < schema.field_count; ++i) {
        const wadb_field *f = &schema.fields[i];
        unsigned char spec[12] = {0};
        wadb_put_u32(spec, (uint32_t)f->type);
        wadb_put_u32(spec + 4, f->size);
        wadb_put_u32(spec + 8, f->nullable);
        hash = wadb_hash(f->name, strlen(f->name) + 1, hash);
        hash = wadb_hash(spec, sizeof(spec), hash);
    }
    schema.fingerprint = hash;
    *out = schema;
    return WADB_OK;
}

int wadb_schema_find(const wadb_schema *s, const char *name) {
    if (s && name) for (uint32_t i = 0; i < s->field_count; ++i)
        if (!strcmp(s->fields[i].name, name)) return (int)i;
    return -1;
}

wadb_status wadb_encode_field(const wadb_field *f, const wadb_value *v,
    unsigned char *p, uint32_t field, wadb_symbol_encode_fn resolve, void *ctx, wadb_error *e) {
    uint64_t u = 0;
    switch (f->type) {
        case WADB_I8: case WADB_I16: case WADB_I32: case WADB_I64: case WADB_TIMESTAMP_US: {
            unsigned bits = f->width * 8;
            if (bits < 64) {
                int64_t bound = INT64_C(1) << (bits - 1);
                if (v->as.i64 < -bound || v->as.i64 >= bound) goto range;
            }
            u = (uint64_t)v->as.i64;
            break;
        }
        case WADB_U8: case WADB_U16: case WADB_U32: case WADB_U64: case WADB_BOOL:
            u = v->as.u64;
            if ((f->width < 8 && u >= (UINT64_C(1) << (f->width * 8))) ||
                (f->type == WADB_BOOL && u > 1)) goto range;
            break;
        case WADB_F32: {
            if (!isfinite(v->as.f64) || fabs(v->as.f64) > FLT_MAX) goto range;
            float x = (float)v->as.f64;
            uint32_t bits;
            memcpy(&bits, &x, sizeof(bits)); u = bits;
            break;
        }
        case WADB_F64:
            if (!isfinite(v->as.f64)) goto range;
            memcpy(&u, &v->as.f64, sizeof(u));
            break;
        case WADB_UUID: case WADB_BYTES:
            if (v->as.bytes.length != f->width || !v->as.bytes.data) goto range;
            memcpy(p, v->as.bytes.data, f->width);
            return WADB_OK;
        case WADB_TEXT:
            if (v->as.bytes.length > f->size || !wadb_utf8(v->as.bytes.data, v->as.bytes.length)) goto range;
            wadb_put_u32(p, (uint32_t)v->as.bytes.length);
            if (v->as.bytes.length) memcpy(p + 4, v->as.bytes.data, v->as.bytes.length);
            return WADB_OK;
        case WADB_SYMBOL32: {
            if (v->as.bytes.length > WADB_MAX_STRING_BYTES || !wadb_utf8(v->as.bytes.data, v->as.bytes.length)) goto range;
            if (!resolve) return wadb_fail(e, WADB_INVALID, 0, "symbol resolver required");
            uint32_t id;
            wadb_status status = resolve(ctx, field, v->as.bytes.data, v->as.bytes.length, &id, e);
            if (status) return status;
            if (!id) return wadb_fail(e, WADB_INVALID, 0, "symbol ID zero is reserved");
            wadb_put_u32(p, id);
            return WADB_OK;
        }
        default: return wadb_fail(e, WADB_INVALID, 0, "unknown type");
    }
    for (uint32_t j = 0; j < f->width; ++j) p[j] = (unsigned char)(u >> (j * 8));
    return WADB_OK;
range:
    return wadb_fail(e, WADB_INVALID, 0, "value is invalid or outside the capacity of %s", f->name);
}

wadb_status wadb_encode_row(const wadb_schema *s, const wadb_value *values, int64_t time,
    wadb_symbol_encode_fn resolve, void *ctx, void *row, size_t cap, wadb_error *e) {
    wadb_error_clear(e);
    if (!s || !row || cap < s->row_width || (s->field_count > 1 && !values))
        return wadb_fail(e, WADB_INVALID, 0, "invalid row buffer or values");
    unsigned char *p = row;
    memset(p, 0, s->row_width);
    wadb_put_u64(p, (uint64_t)time);
    for (uint32_t i = 1; i < s->field_count; ++i) {
        const wadb_field *f = &s->fields[i];
        const wadb_value *v = &values[i - 1];
        if (v->is_null) {
            if (!f->nullable) return wadb_fail(e, WADB_INVALID, 0, "field %s cannot be null", f->name);
            p[8 + f->null_bit / 8] |= (unsigned char)(1u << (f->null_bit % 8));
        } else {
            wadb_status status = wadb_encode_field(f, v, p + f->offset, i, resolve, ctx, e);
            if (status) return status;
        }
    }
    return WADB_OK;
}

wadb_status wadb_decode_value(const wadb_schema *s, uint32_t index, const void *row,
    wadb_symbol_decode_fn resolve, void *ctx, wadb_value *out, wadb_error *e) {
    if (!s || !row || !out || index >= s->field_count)
        return wadb_fail(e, WADB_INVALID, 0, "invalid field or row");
    memset(out, 0, sizeof(*out));
    const wadb_field *f = &s->fields[index];
    const unsigned char *base = row, *p = base + f->offset;
    if (f->nullable && (base[8 + f->null_bit / 8] & (1u << (f->null_bit % 8)))) {
        out->is_null = true;
        return WADB_OK;
    }
    uint64_t u = 0;
    if (f->width <= 8) for (uint32_t i = 0; i < f->width; ++i) u |= (uint64_t)p[i] << (i * 8);
    switch (f->type) {
        case WADB_I8: case WADB_I16: case WADB_I32: {
            unsigned bits = f->width * 8;
            out->as.i64 = (u & (UINT64_C(1) << (bits - 1))) ?
                -1 - (int64_t)(((UINT64_C(1) << bits) - 1) - u) : (int64_t)u;
            break;
        }
        case WADB_I64: case WADB_TIMESTAMP_US: out->as.i64 = wadb_get_i64(p); break;
        case WADB_U8: case WADB_U16: case WADB_U32: case WADB_U64: out->as.u64 = u; break;
        case WADB_BOOL:
            if (u > 1) goto corrupt;
            out->as.u64 = u; break;
        case WADB_F32: {
            uint32_t bits = (uint32_t)u;
            float x;
            memcpy(&x, &bits, 4);
            if (!isfinite(x)) goto corrupt;
            out->as.f64 = x; break;
        }
        case WADB_F64:
            memcpy(&out->as.f64, &u, 8);
            if (!isfinite(out->as.f64)) goto corrupt;
            break;
        case WADB_UUID: case WADB_BYTES:
            out->as.bytes.data = p; out->as.bytes.length = f->width; break;
        case WADB_TEXT:
            out->as.bytes.length = wadb_get_u32(p);
            if (out->as.bytes.length > f->size || !wadb_utf8(p + 4, out->as.bytes.length)) goto corrupt;
            out->as.bytes.data = p + 4; break;
        case WADB_SYMBOL32:
            if (!resolve || !u) goto corrupt;
            return resolve(ctx, index, (uint32_t)u, &out->as.bytes.data, &out->as.bytes.length, e);
        default: goto corrupt;
    }
    return WADB_OK;
corrupt:
    return wadb_fail(e, WADB_CORRUPT, 0, "invalid encoded field %s", f->name);
}

wadb_status wadb_validate_row(const wadb_schema *s, const void *row,
    wadb_symbol_decode_fn resolve, void *ctx, wadb_error *e) {
    if (!s || !row) return wadb_fail(e, WADB_INVALID, 0, "missing schema or row");
    const unsigned char *p = row;
    uint32_t nullable = 0;
    for (uint32_t i = 0; i < s->field_count; ++i) {
        const wadb_field *f = &s->fields[i];
        nullable += f->nullable;
        wadb_value v;
        wadb_status status = wadb_decode_value(s, i, row, resolve, ctx, &v, e);
        if (status) return status;
        uint32_t start = f->width;
        if (v.is_null) start = 0;
        else if (f->type == WADB_TEXT) start = 4 + (uint32_t)v.as.bytes.length;
        for (uint32_t j = start; j < f->width; ++j) if (p[f->offset + j])
            return wadb_fail(e, WADB_CORRUPT, 0, "nonzero null/text padding");
    }
    if (nullable % 8 && (p[8 + s->null_bytes - 1] & (unsigned char)(0xffu << (nullable % 8))))
        return wadb_fail(e, WADB_CORRUPT, 0, "nonzero unused null bits");
    const wadb_field *last = &s->fields[s->field_count - 1];
    uint32_t end = last->offset + last->width;
    for (uint32_t i = end; i < s->row_width; ++i) if (p[i])
        return wadb_fail(e, WADB_CORRUPT, 0, "nonzero row padding");
    return WADB_OK;
}
