#ifndef WADB_DICTIONARY_H
#define WADB_DICTIONARY_H
#include "../core/internal.h"

typedef struct {
    uint32_t field, id, length;
    uint64_t hash;
    unsigned char *data;
} wadb_symbol;
typedef struct { uint32_t *indices; size_t count, capacity; } wadb_symbol_column;
typedef struct {
    const wadb_schema *schema;
    wadb_symbol *symbols;
    size_t count, capacity;
    uint32_t *slots;
    size_t slot_count;
    wadb_symbol_column *columns;
    size_t allocated_bytes, limit;
} wadb_dictionary;

wadb_status wadb_dictionary_new(const wadb_schema *schema, size_t limit,
    wadb_dictionary **out, wadb_error *error);
void wadb_dictionary_free(wadb_dictionary *dictionary);
void wadb_dictionary_rollback(wadb_dictionary *dictionary, size_t mark);
wadb_status wadb_dictionary_intern(void *context, uint32_t field,
    const void *data, size_t length, uint32_t *id, wadb_error *error);
wadb_status wadb_dictionary_resolve(void *context, uint32_t field,
    uint32_t id, const void **data, size_t *length, wadb_error *error);
wadb_status wadb_dictionary_export(const wadb_dictionary *dictionary, size_t mark,
    unsigned char **data, uint32_t *bytes, uint32_t *count, wadb_error *error);
wadb_status wadb_dictionary_export_limit(const wadb_dictionary *dictionary, size_t mark, size_t limit,
    unsigned char **data, uint32_t *bytes, uint32_t *count, wadb_error *error);
wadb_status wadb_dictionary_apply(wadb_dictionary *dictionary, const wadb_frame *frame,
    wadb_error *error);
#endif
