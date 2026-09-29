#include "query.h"
#include <stdlib.h>
#include <string.h>

bool wadb_type_bytes(wadb_type t) { return t == WADB_UUID || t == WADB_BYTES || t == WADB_TEXT || t == WADB_SYMBOL32; }
bool wadb_type_signed(wadb_type t) { return t == WADB_I8 || t == WADB_I16 || t == WADB_I32 || t == WADB_I64 || t == WADB_TIMESTAMP_US; }
bool wadb_type_float(wadb_type t) { return t == WADB_F32 || t == WADB_F64; }
int wadb_value_compare(wadb_type t, const wadb_value *a, const wadb_value *b) {
    if (a->is_null || b->is_null) return (int)a->is_null - (int)b->is_null;
    if (wadb_type_bytes(t)) {
        size_t n = a->as.bytes.length < b->as.bytes.length ? a->as.bytes.length : b->as.bytes.length;
        int cmp = n ? memcmp(a->as.bytes.data, b->as.bytes.data, n) : 0;
        return cmp ? (cmp > 0 ? 1 : -1) : (a->as.bytes.length > b->as.bytes.length) - (a->as.bytes.length < b->as.bytes.length);
    }
    if (wadb_type_signed(t)) return (a->as.i64 > b->as.i64) - (a->as.i64 < b->as.i64);
    if (wadb_type_float(t)) return (a->as.f64 > b->as.f64) - (a->as.f64 < b->as.f64);
    return (a->as.u64 > b->as.u64) - (a->as.u64 < b->as.u64);
}
wadb_status wadb_bucket_floor(int64_t time, int64_t width, int64_t origin, int64_t *out, wadb_error *e) {
    int64_t a = time % width, b = origin % width;
    if (a < 0) a += width;
    if (b < 0) b += width;
    int64_t distance = a >= b ? a - b : width - (b - a);
    if (time < INT64_MIN + distance) return wadb_fail(e, WADB_LIMIT, 0, "bucket boundary underflows timestamp");
    *out = time - distance;
    return WADB_OK;
}
void wadb_query_options_default(wadb_query_options *o) {
    if (!o) return;
    memset(o, 0, sizeof(*o)); wadb_scan_options_default(&o->scan);
    o->max_groups = 10000; o->max_buckets = 10000; o->memory_limit_bytes = 64u * 1024 * 1024;
}
static wadb_status placeholder(void *ctx, uint32_t field, const void *p, size_t n, uint32_t *id, wadb_error *e) {
    (void)ctx; (void)field; (void)p; (void)n; (void)e; *id = 1; return WADB_OK;
}
static wadb_status add_column(wadb_query_plan *p, const char *name, wadb_type type, wadb_error *e) {
    if (!wadb_identifier(name) || p->column_count >= WADB_MAX_FIELDS)
        return wadb_fail(e, WADB_INVALID, 0, "invalid result column name or count");
    for (size_t i = 0; i < p->column_count; ++i) if (!strcmp(p->columns[i].name, name))
        return wadb_fail(e, WADB_INVALID, 0, "duplicate result column: %s", name);
    wadb_result_column *column = &p->columns[p->column_count++];
    strcpy(column->name, name); column->type = type;
    return WADB_OK;
}
void wadb_query_plan_free(wadb_query_plan *p) {
    if (!p) return;
    for (size_t i = 0; i < p->options.filter_count; ++i)
        if (wadb_type_bytes(p->table->schema.fields[p->filters[i].field].type)) free((void *)p->filters[i].value.as.bytes.data);
    free(p);
}
wadb_status wadb_query_compile(wadb_table *t, const wadb_query_options *options,
    wadb_query_plan **out, wadb_error *e) {
    *out = NULL;
    wadb_query_options o;
    wadb_query_options_default(&o); if (options) o = *options;
    if (!t || o.filter_count > WADB_MAX_FILTERS || o.group_count > WADB_MAX_GROUP_FIELDS ||
        o.aggregate_count > WADB_MAX_AGGREGATES || o.projection_count > WADB_MAX_FIELDS ||
        (o.filter_count && !o.filters) || (o.group_count && !o.group_by) ||
        (o.aggregate_count && !o.aggregates) || (o.projection_count && !o.projection) ||
        o.scan.end_time < o.scan.start_time || !o.scan.limit || o.scan.limit > 1000000 ||
        !o.scan.max_scan_bytes || !o.scan.timeout_ms || o.scan.timeout_ms > 3600000 ||
        o.bucket_width_us < 0 || !o.max_groups || o.max_groups > 1000000 || !o.max_buckets || o.max_buckets > 1000000 ||
        o.memory_limit_bytes < sizeof(wadb_query_plan) || o.memory_limit_bytes > 1024u * 1024 * 1024 ||
        (o.aggregate_count && o.projection_count) || (!o.aggregate_count && (o.group_count || o.bucket_width_us || o.order_by)))
        return wadb_fail(e, WADB_INVALID, 0, "invalid query shape or resource limits");
    wadb_query_plan *p = calloc(1, sizeof(*p));
    if (!p) return wadb_fail(e, WADB_NOMEM, 0, "query plan");
    p->table = t; p->options = o; p->allocated_bytes = sizeof(*p); p->order = -1;
    p->options.filters = NULL; p->options.group_by = NULL; p->options.aggregates = NULL;
    p->options.projection = NULL; p->options.order_by = NULL;
    wadb_status status = WADB_OK;
    for (size_t i = 0; i < o.filter_count && !status; ++i) {
        const wadb_filter *f = &o.filters[i];
        int field = wadb_schema_find(&t->schema, f->field);
        if (field < 0 || f->op < WADB_EQ || f->op > WADB_IS_NOT_NULL ||
            (f->op < WADB_IS_NULL && f->value.is_null)) {
            status = wadb_fail(e, WADB_INVALID, 0, "filter requires an existing field and typed value; use IS_NULL for null"); break;
        }
        wadb_predicate *dst = &p->filters[i];
        dst->field = (uint32_t)field; dst->op = f->op;
        if (f->op >= WADB_IS_NULL) continue;
        unsigned char scratch[WADB_MAX_ROW_BYTES];
        status = wadb_encode_field(&t->schema.fields[field], &f->value, scratch, (uint32_t)field, placeholder, NULL, e);
        if (status) break;
        dst->value = f->value;
        if (t->schema.fields[field].type == WADB_F32) dst->value.as.f64 = (double)(float)dst->value.as.f64;
        if (wadb_type_bytes(t->schema.fields[field].type)) {
            size_t n = f->value.as.bytes.length;
            dst->value.as.bytes.data = NULL;
            if (n + 1 > o.memory_limit_bytes - p->allocated_bytes) { status = wadb_fail(e, WADB_LIMIT, 0, "filter memory budget"); break; }
            void *copy = malloc(n + 1);
            if (!copy) { status = wadb_fail(e, WADB_NOMEM, 0, "filter string"); break; }
            if (n) memcpy(copy, f->value.as.bytes.data, n);
            dst->value.as.bytes.data = copy; p->allocated_bytes += n + 1;
        }
    }
    if (!o.aggregate_count) {
        size_t count = o.projection_count ? o.projection_count : t->schema.field_count;
        for (size_t i = 0; i < count && !status; ++i) {
            int field = o.projection_count ? wadb_schema_find(&t->schema, o.projection[i]) : (int)i;
            if (field < 0) { status = wadb_fail(e, WADB_INVALID, 0, "unknown projection field"); break; }
            p->projection[i] = (uint32_t)field;
            status = add_column(p, t->schema.fields[field].name, t->schema.fields[field].type, e);
        }
    } else {
        if (o.bucket_width_us && !status) {
            status = add_column(p, "_bucket", WADB_TIMESTAMP_US, e);
            if (!status && o.scan.start_time != o.scan.end_time) {
                int64_t last;
                status = wadb_bucket_floor(o.scan.start_time, o.bucket_width_us, o.bucket_origin_us, &p->first_bucket, e);
                if (!status) status = wadb_bucket_floor(o.scan.end_time - 1, o.bucket_width_us, o.bucket_origin_us, &last, e);
                if (!status) {
                    uint64_t intervals = ((uint64_t)last - (uint64_t)p->first_bucket) / (uint64_t)o.bucket_width_us;
                    if (intervals >= o.max_buckets) status = wadb_fail(e, WADB_LIMIT, 0, "bucket count exceeds query budget");
                    else p->bucket_count = (uint32_t)intervals + 1;
                }
            }
        }
        for (size_t i = 0; i < o.group_count && !status; ++i) {
            int field = wadb_schema_find(&t->schema, o.group_by[i]);
            if (field < 0) { status = wadb_fail(e, WADB_INVALID, 0, "unknown grouping field"); break; }
            p->groups[i] = (uint32_t)field;
            status = add_column(p, t->schema.fields[field].name, t->schema.fields[field].type, e);
        }
        p->key_count = p->column_count;
        for (size_t i = 0; i < o.aggregate_count && !status; ++i) {
            const wadb_aggregate *a = &o.aggregates[i];
            int field = a->op == WADB_COUNT_ALL ? 0 : wadb_schema_find(&t->schema, a->field);
            if (a->op < WADB_COUNT_ALL || a->op > WADB_AVG || field < 0 || (a->op == WADB_COUNT_ALL && a->field)) {
                status = wadb_fail(e, WADB_INVALID, 0, "invalid aggregate operation or field"); break;
            }
            wadb_type type = t->schema.fields[field].type;
            if (a->op >= WADB_SUM && wadb_type_bytes(type)) { status = wadb_fail(e, WADB_INVALID, 0, "numeric aggregate requires numeric field"); break; }
            p->aggregates[i] = (wadb_reduction){.field = (uint32_t)field, .op = a->op};
            if (a->op <= WADB_COUNT) type = WADB_U64;
            else if (a->op == WADB_AVG || (a->op == WADB_SUM && wadb_type_float(type))) type = WADB_F64;
            else if (a->op == WADB_SUM) type = wadb_type_signed(type) ? WADB_I64 : WADB_U64;
            status = add_column(p, a->name, type, e);
        }
        if (o.order_by && !status) {
            for (size_t i = 0; i < p->column_count; ++i) if (!strcmp(o.order_by, p->columns[i].name)) p->order = (int)i;
            if (p->order < 0) status = wadb_fail(e, WADB_INVALID, 0, "unknown order_by result column");
        }
    }
    if (status) wadb_query_plan_free(p); else *out = p;
    return status;
}
