#ifndef WADB_QUERY_INTERNAL_H
#define WADB_QUERY_INTERNAL_H
#include "../core/engine.h"
typedef struct { uint32_t field; wadb_filter_op op; wadb_value value; } wadb_predicate;
typedef struct { uint32_t field; wadb_aggregate_op op; } wadb_reduction;
typedef struct {
    wadb_table *table;
    wadb_query_options options; /* all pointer members cleared after compilation */
    uint32_t projection[WADB_MAX_FIELDS], groups[WADB_MAX_GROUP_FIELDS];
    wadb_predicate filters[WADB_MAX_FILTERS];
    wadb_reduction aggregates[WADB_MAX_AGGREGATES];
    wadb_result_column columns[WADB_MAX_FIELDS];
    size_t column_count, key_count, allocated_bytes;
    int order;
    int64_t first_bucket;
    uint32_t bucket_count;
} wadb_query_plan;
wadb_status wadb_query_compile(wadb_table *table, const wadb_query_options *options,
    wadb_query_plan **plan, wadb_error *error);
void wadb_query_plan_free(wadb_query_plan *plan);
wadb_status wadb_query_execute(const wadb_query_plan *plan, const wadb_snapshot *snapshot,
    uint64_t after, uint64_t byte_budget, uint32_t timeout_ms, wadb_result **out, wadb_error *error);
bool wadb_type_bytes(wadb_type type);
bool wadb_type_signed(wadb_type type);
bool wadb_type_float(wadb_type type);
int wadb_value_compare(wadb_type type, const wadb_value *a, const wadb_value *b);
wadb_status wadb_bucket_floor(int64_t time, int64_t width, int64_t origin, int64_t *bucket, wadb_error *error);
#endif
