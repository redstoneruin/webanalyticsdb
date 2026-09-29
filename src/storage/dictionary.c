#include "dictionary.h"
#include <stdlib.h>
#include <string.h>

static void rehash(wadb_dictionary *d) {
    if (!d->slot_count) return;
    memset(d->slots, 0, d->slot_count * sizeof(*d->slots));
    for (size_t i = 0; i < d->count; ++i) {
        size_t slot = d->symbols[i].hash & (d->slot_count - 1);
        while (d->slots[slot]) slot = (slot + 1) & (d->slot_count - 1);
        d->slots[slot] = (uint32_t)i + 1;
    }
}

wadb_status wadb_dictionary_new(const wadb_schema *s, size_t limit,
    wadb_dictionary **out, wadb_error *e) {
    if (!s || !out) return wadb_fail(e, WADB_INVALID, 0, "dictionary schema required");
    *out = NULL;
    size_t base = sizeof(wadb_dictionary) + s->field_count * sizeof(wadb_symbol_column);
    if (base > limit) return wadb_fail(e, WADB_LIMIT, 0, "dictionary budget is smaller than schema metadata");
    wadb_dictionary *d = calloc(1, sizeof(*d));
    if (!d) return wadb_fail(e, WADB_NOMEM, 0, "dictionary allocation");
    d->columns = calloc(s->field_count, sizeof(*d->columns));
    if (!d->columns) { free(d); return wadb_fail(e, WADB_NOMEM, 0, "dictionary columns"); }
    d->schema = s; d->limit = limit; d->allocated_bytes = base;
    *out = d;
    return WADB_OK;
}

void wadb_dictionary_free(wadb_dictionary *d) {
    if (!d) return;
    for (size_t i = 0; i < d->count; ++i) free(d->symbols[i].data);
    for (uint32_t i = 0; i < d->schema->field_count; ++i) free(d->columns[i].indices);
    free(d->symbols); free(d->slots); free(d->columns); free(d);
}

void wadb_dictionary_rollback(wadb_dictionary *d, size_t mark) {
    if (!d || mark >= d->count) return;
    while (d->count > mark) {
        wadb_symbol *symbol = &d->symbols[--d->count];
        --d->columns[symbol->field].count;
        d->allocated_bytes -= (size_t)symbol->length + 1;
        free(symbol->data);
    }
    rehash(d);
}

wadb_status wadb_dictionary_intern(void *context, uint32_t field, const void *data,
    size_t length, uint32_t *id, wadb_error *e) {
    wadb_dictionary *d = context;
    if (!d || !id || !field || field >= d->schema->field_count ||
        d->schema->fields[field].type != WADB_SYMBOL32 || length > WADB_MAX_STRING_BYTES || !wadb_utf8(data, length))
        return wadb_fail(e, WADB_INVALID, 0, "invalid symbol field or string");
    unsigned char encoded_field[4];
    wadb_put_u32(encoded_field, field);
    uint64_t hash = wadb_hash(encoded_field, 4, UINT64_C(14695981039346656037));
    hash = wadb_hash(data, length, hash);
    if (d->slot_count) {
        size_t slot = hash & (d->slot_count - 1);
        while (d->slots[slot]) {
            const wadb_symbol *s = &d->symbols[d->slots[slot] - 1];
            if (s->hash == hash && s->field == field && s->length == length &&
                (!length || !memcmp(s->data, data, length))) { *id = s->id; return WADB_OK; }
            slot = (slot + 1) & (d->slot_count - 1);
        }
    }
    wadb_symbol_column *column = &d->columns[field];
    if (d->count == UINT32_MAX || column->count == UINT32_MAX)
        return wadb_fail(e, WADB_LIMIT, 0, "symbol IDs exhausted");
    size_t capacity = d->count == d->capacity ? (d->capacity ? d->capacity * 2 : 16) : d->capacity;
    size_t slots = d->slot_count < (d->count + 1) * 2 ? (d->slot_count ? d->slot_count * 2 : 32) : d->slot_count;
    size_t column_capacity = column->count == column->capacity ? (column->capacity ? column->capacity * 2 : 8) : column->capacity;
    size_t extra = (capacity - d->capacity) * sizeof(*d->symbols) +
        (slots - d->slot_count) * sizeof(*d->slots) +
        (column_capacity - column->capacity) * sizeof(*column->indices) + length + 1;
    if (extra > d->limit - d->allocated_bytes)
        return wadb_fail(e, WADB_LIMIT, 0, "segment dictionary budget reached");
    if (capacity != d->capacity) {
        wadb_symbol *p = realloc(d->symbols, capacity * sizeof(*p));
        if (!p) return wadb_fail(e, WADB_NOMEM, 0, "symbol entries");
        d->allocated_bytes += (capacity - d->capacity) * sizeof(*p);
        d->symbols = p; d->capacity = capacity;
    }
    if (slots != d->slot_count) {
        uint32_t *p = calloc(slots, sizeof(*p));
        if (!p) return wadb_fail(e, WADB_NOMEM, 0, "symbol hash table");
        d->allocated_bytes += (slots - d->slot_count) * sizeof(*p);
        free(d->slots); d->slots = p; d->slot_count = slots;
        rehash(d);
    }
    if (column_capacity != column->capacity) {
        uint32_t *p = realloc(column->indices, column_capacity * sizeof(*p));
        if (!p) return wadb_fail(e, WADB_NOMEM, 0, "symbol column index");
        d->allocated_bytes += (column_capacity - column->capacity) * sizeof(*p);
        column->indices = p; column->capacity = column_capacity;
    }
    unsigned char *copy = malloc(length + 1);
    if (!copy) return wadb_fail(e, WADB_NOMEM, 0, "symbol string");
    if (length) memcpy(copy, data, length);
    copy[length] = 0;
    *id = (uint32_t)column->count + 1;
    column->indices[column->count++] = (uint32_t)d->count;
    d->symbols[d->count] = (wadb_symbol){.field = field, .id = *id, .length = (uint32_t)length,
        .hash = hash, .data = copy};
    size_t slot = hash & (d->slot_count - 1);
    while (d->slots[slot]) slot = (slot + 1) & (d->slot_count - 1);
    d->slots[slot] = (uint32_t)++d->count;
    d->allocated_bytes += length + 1;
    return WADB_OK;
}

wadb_status wadb_dictionary_resolve(void *context, uint32_t field, uint32_t id,
    const void **data, size_t *length, wadb_error *e) {
    wadb_dictionary *d = context;
    if (!d || !data || !length || !field || field >= d->schema->field_count || !id ||
        d->schema->fields[field].type != WADB_SYMBOL32 || id > d->columns[field].count)
        return wadb_fail(e, WADB_CORRUPT, 0, "row references an undefined symbol");
    const wadb_symbol *s = &d->symbols[d->columns[field].indices[id - 1]];
    *data = s->data; *length = s->length;
    return WADB_OK;
}

wadb_status wadb_dictionary_export(const wadb_dictionary *d, size_t mark,
    unsigned char **data, uint32_t *bytes, uint32_t *count, wadb_error *e) {
    return wadb_dictionary_export_limit(d, mark, WADB_MAX_FRAME_BYTES, data, bytes, count, e);
}

wadb_status wadb_dictionary_export_limit(const wadb_dictionary *d, size_t mark, size_t limit,
    unsigned char **data, uint32_t *bytes, uint32_t *count, wadb_error *e) {
    if (!d || mark > d->count || !data || !bytes || !count)
        return wadb_fail(e, WADB_INVALID, 0, "invalid dictionary checkpoint");
    *data = NULL; *bytes = 0; *count = 0;
    size_t n = 0;
    for (size_t i = mark; i < d->count; ++i) {
        if (!wadb_add_size(n, 12 + (size_t)d->symbols[i].length, &n) || n > limit || n > UINT32_MAX - 7)
            return wadb_fail(e, WADB_LIMIT, 0, "dictionary additions exceed frame limit");
    }
    n = (n + 7) & ~(size_t)7;
    if (n > limit) return wadb_fail(e, WADB_LIMIT, 0, "dictionary exceeds serialization budget");
    if (!n) return WADB_OK;
    unsigned char *p = calloc(1, n);
    if (!p) return wadb_fail(e, WADB_NOMEM, 0, "dictionary serialization");
    size_t offset = 0;
    for (size_t i = mark; i < d->count; ++i) {
        const wadb_symbol *s = &d->symbols[i];
        wadb_put_u16(p + offset, (uint16_t)s->field);
        wadb_put_u32(p + offset + 4, s->id); wadb_put_u32(p + offset + 8, s->length);
        if (s->length) memcpy(p + offset + 12, s->data, s->length);
        offset += 12 + s->length;
    }
    *data = p; *bytes = (uint32_t)n; *count = (uint32_t)(d->count - mark);
    return WADB_OK;
}

wadb_status wadb_dictionary_apply(wadb_dictionary *d, const wadb_frame *f, wadb_error *e) {
    size_t mark = d->count, offset = 0;
    wadb_status status = WADB_OK;
    for (uint32_t i = 0; i < f->dictionary_count; ++i) {
        if (offset > f->dictionary_bytes || f->dictionary_bytes - offset < 12) goto corrupt;
        const unsigned char *p = f->dictionary + offset;
        uint32_t field = wadb_get_u16(p), id = wadb_get_u32(p + 4), length = wadb_get_u32(p + 8), assigned = 0;
        if (!field || field >= d->schema->field_count || d->schema->fields[field].type != WADB_SYMBOL32 ||
            id != d->columns[field].count + 1 || wadb_get_u16(p + 2) || length > f->dictionary_bytes - offset - 12) goto corrupt;
        size_t before = d->count;
        status = wadb_dictionary_intern(d, field, p + 12, length, &assigned, e);
        if (status == WADB_INVALID) goto corrupt;
        if (status) break;
        if (assigned != id || d->count != before + 1) goto corrupt;
        offset += 12 + length;
    }
    if (!status) {
        if (f->dictionary_bytes % 8 || offset > f->dictionary_bytes || f->dictionary_bytes - offset >= 8) goto corrupt;
        while (offset < f->dictionary_bytes) if (f->dictionary[offset++]) goto corrupt;
    }
    if (status) wadb_dictionary_rollback(d, mark);
    return status;
corrupt:
    wadb_dictionary_rollback(d, mark);
    return wadb_fail(e, WADB_CORRUPT, 0, "invalid or repeated symbol definition");
}
