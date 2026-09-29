#include "api.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static bool uint_option(wadb_json *j, yyjson_val *root, const char *name, uint64_t *value) {
    yyjson_val *v = yyjson_obj_get(root, name);
    return !v || wadb_json_u64(j, v, value);
}
static bool int_option(wadb_json *j, yyjson_val *root, const char *name, int64_t *value) {
    yyjson_val *v = yyjson_obj_get(root, name);
    return !v || wadb_json_i64(j, v, value);
}
static bool u32_option(wadb_json *j, yyjson_val *root, const char *name, uint32_t *value) {
    uint64_t n = *value;
    if (!uint_option(j, root, name, &n)) return false;
    if (n > UINT32_MAX) { wadb_fail(&j->error, WADB_INVALID, 0, "%s exceeds uint32", name); return false; }
    *value = (uint32_t)n; return true;
}
static const char *string_option(wadb_json *j, yyjson_val *root, const char *key) {
    yyjson_val *v = yyjson_obj_get(root, key);
    return v ? wadb_json_name(j, v, key) : NULL;
}
static void latency(wadb_json *j, yyjson_mut_val *root, const char *key, const wadb_latency_stats *s) {
    yyjson_mut_val *o = wadb_json_object(j); wadb_json_add(j, root, key, o);
    wadb_json_uint(j, o, "samples", s->samples); wadb_json_uint(j, o, "p50_us", s->p50_us);
    wadb_json_uint(j, o, "p95_us", s->p95_us); wadb_json_uint(j, o, "p99_us", s->p99_us); wadb_json_uint(j, o, "max_us", s->max_us);
}
void wadb_api_stats(wadb_api_context *context, wadb_json *j) {
    wadb_stats s;
    if (wadb_get_stats(context->db, &s, &j->error)) return;
    yyjson_mut_val *root = j->root;
    wadb_json_string(j, root, "boot_id", context->boot_id);
    wadb_json_uint(j, root, "feed_cursor", context->feed_cursor ? context->feed_cursor(context->feed_context) : 0);
#define STAT(name) wadb_json_uint(j, root, #name, s.name)
    STAT(uptime_us); STAT(table_count); STAT(queue_bytes); STAT(queue_limit_bytes); STAT(cache_bytes); STAT(cache_hits); STAT(cache_misses);
    STAT(readers); STAT(snapshot_bytes); STAT(committed_rows); STAT(committed_bytes); STAT(committed_frames); STAT(append_requests);
    STAT(rejected_appends); STAT(failed_appends); STAT(clock_adjustments); STAT(queries); STAT(failed_queries); STAT(rows_scanned); STAT(bytes_scanned);
    STAT(disk_free_bytes); STAT(latest_notification);
#undef STAT
    wadb_json_boolean(j, root, "disk_free_available", s.disk_free_available); wadb_json_boolean(j, root, "read_only", s.read_only);
    latency(j, root, "append_latency", &s.append_latency); latency(j, root, "sync_latency", &s.sync_latency); latency(j, root, "query_latency", &s.query_latency);
}
static void table_info(wadb_json *j, yyjson_mut_val *o, wadb_table *t, bool fields) {
    wadb_table_stats s;
    if (wadb_get_table_stats(t, &s, &j->error)) return;
    wadb_json_string(j, o, "name", wadb_table_name(t));
#define STAT(name) wadb_json_uint(j, o, #name, s.name)
    STAT(id); STAT(retention_us); STAT(rows); STAT(bytes); STAT(segments); STAT(retired_segments); STAT(last_sequence);
    STAT(dictionary_bytes); STAT(active_index_bytes); STAT(row_width);
#undef STAT
    wadb_json_int(j, o, "min_time", s.min_time); wadb_json_int(j, o, "max_time", s.max_time); wadb_json_int(j, o, "last_assigned_time", s.last_assigned_time);
    wadb_json_boolean(j, o, "read_only", s.read_only); wadb_json_boolean(j, o, "pending_retention", s.pending_retention);
    wadb_json_string(j, o, "maintenance_status", wadb_status_name(s.maintenance_status));
    const wadb_schema *schema = wadb_table_schema(t);
    wadb_json_uint(j, o, "field_count", schema->field_count); wadb_json_uint(j, o, "schema_hash", schema->fingerprint);
    if (fields) {
        yyjson_mut_val *array = wadb_json_array(j); wadb_json_add(j, o, "fields", array);
        for (uint32_t i = 0; i < schema->field_count; ++i) {
            const wadb_field *f = &schema->fields[i]; yyjson_mut_val *field = wadb_json_object(j);
            wadb_json_string(j, field, "name", f->name); wadb_json_string(j, field, "type", wadb_type_name(f->type));
            wadb_json_uint(j, field, "size", f->size); wadb_json_uint(j, field, "offset", f->offset); wadb_json_uint(j, field, "width", f->width);
            wadb_json_boolean(j, field, "nullable", f->nullable); wadb_json_push(j, array, field);
        }
    }
}
static void create_table(wadb_api_context *ctx, wadb_json *j, yyjson_val *root) {
    if (!wadb_json_allowed(j, root, "name|fields|retention_us")) return;
    const char *name = wadb_json_name(j, yyjson_obj_get(root, "name"), "table name");
    yyjson_val *fields = yyjson_obj_get(root, "fields");
    size_t count = yyjson_arr_size(fields);
    if (!yyjson_is_arr(fields) || count >= WADB_MAX_FIELDS) { wadb_fail(&j->error, WADB_INVALID, 0, "fields must be an array of at most 255 definitions"); return; }
    wadb_field_def defs[WADB_MAX_FIELDS]; memset(defs, 0, sizeof(defs));
    for (size_t i = 0; i < count && !j->error.code; ++i) {
        yyjson_val *f = yyjson_arr_get(fields, i);
        if (!wadb_json_allowed(j, f, "name|type|size|nullable")) break;
        defs[i].name = wadb_json_name(j, yyjson_obj_get(f, "name"), "field name");
        const char *type = wadb_json_name(j, yyjson_obj_get(f, "type"), "field type");
        defs[i].type = wadb_type_parse(type);
        if (!defs[i].type) wadb_fail(&j->error, WADB_INVALID, 0, "unknown field type");
        if (!u32_option(j, f, "size", &defs[i].size) || !wadb_json_bool(j, f, "nullable", &defs[i].nullable)) break;
    }
    uint64_t retention = 0;
    if (!uint_option(j, root, "retention_us", &retention) || j->error.code) return;
    wadb_table *t;
    if (!wadb_create_table(ctx->db, name, defs, count, retention, &t, &j->error)) table_info(j, j->root, t, true);
}
static void append_rows(wadb_json *j, wadb_table *t, yyjson_val *root) {
    if (!wadb_json_allowed(j, root, "rows")) return;
    yyjson_val *array = yyjson_obj_get(root, "rows"); size_t rows = yyjson_arr_size(array);
    const wadb_schema *s = wadb_table_schema(t); size_t columns = s->field_count - 1;
    if (!yyjson_is_arr(array) || !rows || rows > (WADB_MAX_FRAME_BYTES - 96) / s->row_width) { wadb_fail(&j->error, WADB_LIMIT, 0, "rows must be a nonempty batch within the encoded frame limit"); return; }
    size_t count = rows * columns;
    if (count > WADB_HTTP_JSON_LIMIT / sizeof(wadb_value)) { wadb_fail(&j->error, WADB_LIMIT, 0, "decoded append value budget exceeded"); return; }
    wadb_value *values = calloc(count ? count : 1, sizeof(*values));
    if (!values) { wadb_fail(&j->error, WADB_NOMEM, 0, "append values"); return; }
    /* Values are bounded by encoded row count and maximum schema width, with a
     * separate 32 MiB cap on the decoded value vector for narrow-row schemas. */
    for (size_t row = 0; row < rows && !j->error.code; ++row) {
        yyjson_val *v = yyjson_arr_get(array, row); bool object = yyjson_is_obj(v);
        if (!object && (!yyjson_is_arr(v) || yyjson_arr_size(v) != columns)) { wadb_fail(&j->error, WADB_INVALID, 0, "each row must be an object or an array matching the user fields"); break; }
        if (object) {
            yyjson_obj_iter iter = yyjson_obj_iter_with(v); yyjson_val *key;
            while ((key = yyjson_obj_iter_next(&iter))) {
                int index = wadb_schema_find(s, yyjson_get_str(key));
                if (index <= 0) { wadb_fail(&j->error, WADB_INVALID, 0, "unknown field or supplied _time: %s", yyjson_get_str(key)); break; }
            }
        }
        for (size_t column = 0; column < columns && !j->error.code; ++column) {
            const wadb_field *field = &s->fields[column + 1];
            yyjson_val *value = object ? yyjson_obj_get(v, field->name) : yyjson_arr_get(v, column);
            if (!wadb_json_value(j, field, value, &values[row * columns + column])) break;
        }
    }
    wadb_append_receipt receipt;
    if (!j->error.code && !wadb_append_batch(t, values, rows, &receipt, &j->error)) {
        wadb_json_uint(j, j->root, "first_sequence", receipt.first_sequence); wadb_json_uint(j, j->root, "last_sequence", receipt.last_sequence);
        wadb_json_int(j, j->root, "ingestion_time", receipt.ingestion_time); wadb_json_uint(j, j->root, "row_count", receipt.row_count);
    }
    free(values);
}
void wadb_api_result(wadb_json *j, yyjson_mut_val *o, const wadb_result *r) {
    yyjson_mut_val *columns = wadb_json_array(j), *rows = wadb_json_array(j);
    wadb_json_add(j, o, "columns", columns); wadb_json_add(j, o, "rows", rows);
    for (size_t i = 0; i < r->column_count; ++i) {
        yyjson_mut_val *c = wadb_json_object(j); wadb_json_string(j, c, "name", r->columns[i].name);
        wadb_json_string(j, c, "type", wadb_type_name(r->columns[i].type)); wadb_json_push(j, columns, c);
    }
    for (size_t row = 0; row < r->row_count && !j->error.code; ++row) {
        yyjson_mut_val *values = wadb_json_array(j);
        for (size_t column = 0; column < r->column_count; ++column)
            wadb_json_push(j, values, wadb_json_encode_value(j, r->columns[column].type, &r->values[row * r->column_count + column]));
        wadb_json_push(j, rows, values);
    }
    if (r->sequences) {
        yyjson_mut_val *sequences = wadb_json_array(j); wadb_json_add(j, o, "sequences", sequences);
        for (size_t i = 0; i < r->row_count; ++i) { wadb_value v = {.as.u64 = r->sequences[i]}; wadb_json_push(j, sequences, wadb_json_encode_value(j, WADB_U64, &v)); }
    }
    wadb_json_uint(j, o, "matched_rows", r->matched_rows); wadb_json_uint(j, o, "total_groups", r->total_groups);
    wadb_json_boolean(j, o, "has_more", r->has_more);
    yyjson_mut_val *stats = wadb_json_object(j); wadb_json_add(j, o, "stats", stats);
#define STAT(name) wadb_json_uint(j, stats, #name, r->stats.name)
    STAT(rows_scanned); STAT(rows_returned); STAT(bytes_scanned); STAT(snapshot_sequence); STAT(index_bytes); STAT(dictionary_bytes);
    STAT(frames_scanned); STAT(segments_scanned); STAT(rebuild_bytes); STAT(last_sequence);
#undef STAT
    wadb_json_boolean(j, stats, "limit_reached", r->stats.limit_reached);
}
static bool names(wadb_json *j, yyjson_val *root, const char *key, const char **out, size_t capacity, size_t *count) {
    yyjson_val *a = yyjson_obj_get(root, key);
    if (!a) { *count = 0; return true; }
    if (!yyjson_is_arr(a) || yyjson_arr_size(a) > capacity) { wadb_fail(&j->error, WADB_INVALID, 0, "invalid %s field list", key); return false; }
    *count = yyjson_arr_size(a);
    for (size_t i = 0; i < *count; ++i) if (!(out[i] = wadb_json_name(j, yyjson_arr_get(a, i), key))) return false;
    return true;
}
static uint64_t run_query(wadb_json *j, wadb_db *db, wadb_table *t, yyjson_val *root, bool tail) {
    const char *allowed = tail ? "after_sequence|limit" : "start_us|end_us|after_sequence|limit|max_scan_bytes|timeout_ms|projection|filters|group_by|aggregates|bucket_width_us|bucket_origin_us|max_groups|max_buckets|memory_limit_bytes|order_by|descending|cursor|ttl_ms";
    if (!wadb_json_allowed(j, root, allowed)) return 0;
    wadb_query_options q; wadb_query_options_default(&q);
    const char *projection[WADB_MAX_FIELDS], *groups[WADB_MAX_GROUP_FIELDS];
    wadb_filter filters[WADB_MAX_FILTERS]; wadb_aggregate aggregates[WADB_MAX_AGGREGATES];
    q.projection = projection; q.group_by = groups; q.filters = filters; q.aggregates = aggregates;
    q.scan.limit = tail ? 50 : 1000;
    uint64_t memory = q.memory_limit_bytes; bool cursor = false; uint32_t ttl = 60000;
    if (!int_option(j, root, "start_us", &q.scan.start_time) || !int_option(j, root, "end_us", &q.scan.end_time) ||
        !uint_option(j, root, "after_sequence", &q.scan.after_sequence) || !uint_option(j, root, "max_scan_bytes", &q.scan.max_scan_bytes) ||
        !u32_option(j, root, "limit", &q.scan.limit) || !u32_option(j, root, "timeout_ms", &q.scan.timeout_ms) ||
        !int_option(j, root, "bucket_width_us", &q.bucket_width_us) || !int_option(j, root, "bucket_origin_us", &q.bucket_origin_us) ||
        !u32_option(j, root, "max_groups", &q.max_groups) || !u32_option(j, root, "max_buckets", &q.max_buckets) ||
        !uint_option(j, root, "memory_limit_bytes", &memory) || !wadb_json_bool(j, root, "descending", &q.descending) ||
        !wadb_json_bool(j, root, "cursor", &cursor) || !u32_option(j, root, "ttl_ms", &ttl)) return 0;
    if (memory > 64u * 1024 * 1024 || q.scan.max_scan_bytes > UINT64_C(1024) * 1024 * 1024 || q.scan.timeout_ms > 30000 || q.scan.limit > 10000) {
        wadb_fail(&j->error, WADB_LIMIT, 0, "HTTP query exceeds server limits: 64 MiB memory, 1 GiB scan, 30 seconds, 10000 output rows"); return 0;
    }
    q.memory_limit_bytes = (size_t)memory; q.order_by = string_option(j, root, "order_by");
    if (!names(j, root, "projection", projection, WADB_MAX_FIELDS, &q.projection_count) ||
        !names(j, root, "group_by", groups, WADB_MAX_GROUP_FIELDS, &q.group_count)) return 0;
    yyjson_val *array = yyjson_obj_get(root, "filters");
    if (array && (!yyjson_is_arr(array) || yyjson_arr_size(array) > WADB_MAX_FILTERS)) { wadb_fail(&j->error, WADB_INVALID, 0, "invalid filter array"); return 0; }
    q.filter_count = yyjson_arr_size(array);
    const char *ops[] = {"eq", "ne", "lt", "le", "gt", "ge", "is_null", "is_not_null"};
    for (size_t i = 0; i < q.filter_count && !j->error.code; ++i) {
        yyjson_val *f = yyjson_arr_get(array, i);
        if (!wadb_json_allowed(j, f, "field|op|value")) break;
        filters[i] = (wadb_filter){.field = wadb_json_name(j, yyjson_obj_get(f, "field"), "filter field")};
        const char *op = wadb_json_name(j, yyjson_obj_get(f, "op"), "filter operator");
        int index = wadb_schema_find(wadb_table_schema(t), filters[i].field), operation = -1;
        if (op) for (int k = 0; k < 8; ++k) if (!strcmp(op, ops[k])) operation = k;
        if (index < 0 || operation < 0) { wadb_fail(&j->error, WADB_INVALID, 0, "unknown filter field or operator"); break; }
        filters[i].op = (wadb_filter_op)operation;
        if (operation < WADB_IS_NULL && !wadb_json_value(j, &wadb_table_schema(t)->fields[index], yyjson_obj_get(f, "value"), &filters[i].value)) break;
    }
    array = yyjson_obj_get(root, "aggregates");
    if (array && (!yyjson_is_arr(array) || yyjson_arr_size(array) > WADB_MAX_AGGREGATES)) { wadb_fail(&j->error, WADB_INVALID, 0, "invalid aggregate array"); return 0; }
    q.aggregate_count = yyjson_arr_size(array);
    const char *aggregate_ops[] = {"count_all", "count", "sum", "min", "max", "avg"};
    for (size_t i = 0; i < q.aggregate_count && !j->error.code; ++i) {
        yyjson_val *a = yyjson_arr_get(array, i);
        if (!wadb_json_allowed(j, a, "op|field|name")) break;
        const char *op = wadb_json_name(j, yyjson_obj_get(a, "op"), "aggregate operator");
        int operation = -1; if (op) for (int k = 0; k < 6; ++k) if (!strcmp(op, aggregate_ops[k])) operation = k;
        if (operation < 0) { wadb_fail(&j->error, WADB_INVALID, 0, "unknown aggregate operator"); break; }
        aggregates[i] = (wadb_aggregate){.op = (wadb_aggregate_op)operation, .field = string_option(j, a, "field"),
            .name = wadb_json_name(j, yyjson_obj_get(a, "name"), "aggregate result name")};
    }
    if (tail && !yyjson_obj_get(root, "after_sequence")) {
        wadb_table_stats stats;
        if (wadb_get_table_stats(t, &stats, &j->error)) return 0;
        q.scan.after_sequence = stats.last_sequence > q.scan.limit ? stats.last_sequence - q.scan.limit : 0;
    }
    if (j->error.code) return 0;
    wadb_result *result = NULL; uint64_t id = 0;
    if (cursor) {
        if (!wadb_cursor_open(t, &q, ttl, &id, &j->error)) (void)wadb_cursor_next(db, id, &result, &j->error);
    } else (void)wadb_query(t, &q, &result, &j->error);
    if (result) {
        wadb_api_result(j, j->root, result);
        if (id && result->has_more) wadb_json_uint(j, j->root, "cursor_id", id);
        wadb_result_free(result);
    }
    return id;
}
static void segments(wadb_json *j, wadb_table *t, yyjson_val *root) {
    if (!wadb_json_allowed(j, root, "after_id|limit")) return;
    uint64_t after = 0; uint32_t limit = 100;
    if (!uint_option(j, root, "after_id", &after) || !u32_option(j, root, "limit", &limit)) return;
    if (!limit || limit > 1024) { wadb_fail(&j->error, WADB_INVALID, 0, "segment limit must be in 1..1024"); return; }
    wadb_segment_info items[1024]; size_t count; bool more;
    if (wadb_list_segments(t, after, items, limit, &count, &more, &j->error)) return;
    yyjson_mut_val *a = wadb_json_array(j); wadb_json_add(j, j->root, "segments", a);
    for (size_t i = 0; i < count; ++i) {
        wadb_segment_info *s = &items[i]; yyjson_mut_val *o = wadb_json_object(j);
        wadb_json_uint(j, o, "id", s->id); wadb_json_int(j, o, "day_start", s->day_start);
        wadb_json_uint(j, o, "first_sequence", s->first_sequence); wadb_json_uint(j, o, "last_sequence", s->last_sequence);
        wadb_json_int(j, o, "min_time", s->min_time); wadb_json_int(j, o, "max_time", s->max_time); wadb_json_uint(j, o, "bytes", s->committed_bytes);
        wadb_json_string(j, o, "state", s->state == WADB_SEG_ACTIVE ? "active" : s->state == WADB_SEG_SEALED ? "sealed" : "retired");
        wadb_json_push(j, a, o);
    }
    wadb_json_boolean(j, j->root, "has_more", more);
}
static void retention(wadb_json *j, wadb_table *t, yyjson_val *root, bool preview_only) {
    if (!wadb_json_allowed(j, root, preview_only ? "now_us" : "now_us|retention_us|apply")) return;
    int64_t now = wadb_realtime_us(); bool apply = false;
    if (!int_option(j, root, "now_us", &now) || !wadb_json_bool(j, root, "apply", &apply)) return;
    yyjson_val *duration = yyjson_obj_get(root, "retention_us");
    if (duration) {
        uint64_t us;
        if (!wadb_json_u64(j, duration, &us) || wadb_set_retention(t, us, &j->error)) return;
    }
    wadb_retention_stats s;
    wadb_status status = apply && !preview_only ? wadb_retention_apply(t, now, &s, &j->error) : wadb_retention_preview(t, now, &s, &j->error);
    if (status) return;
    wadb_json_int(j, j->root, "cutoff", s.cutoff);
#define STAT(name) wadb_json_uint(j, j->root, #name, s.name)
    STAT(eligible_segments); STAT(eligible_rows); STAT(eligible_bytes); STAT(partially_expired_segments); STAT(pending_segments); STAT(deleted_segments);
#undef STAT
    wadb_json_boolean(j, j->root, "applied", apply && !preview_only);
}
static void job_info(wadb_json *j, yyjson_mut_val *o, const wadb_job_info *s) {
    const char *states[] = {"starting", "running", "complete", "stopped", "failed"};
    wadb_json_uint(j, o, "id", s->id); wadb_json_uint(j, o, "table_id", s->table_id);
    wadb_json_string(j, o, "kind", s->kind == WADB_JOB_GENERATOR ? "generator" : "integrity");
    wadb_json_string(j, o, "state", states[s->state]);
    wadb_json_uint(j, o, "committed_rows", s->committed_rows); wadb_json_uint(j, o, "attempted_rows", s->attempted_rows);
    wadb_json_uint(j, o, "rejected_batches", s->rejected_batches); wadb_json_uint(j, o, "last_sequence", s->last_sequence);
    wadb_json_uint(j, o, "elapsed_ms", s->elapsed_ms); wadb_json_uint(j, o, "cpu_ns", s->cpu_ns);
    if (s->kind == WADB_JOB_GENERATOR) {
        wadb_json_string(j, o, "table_name", s->generator.table_name);
        wadb_json_uint(j, o, "rows_per_second", s->generator.rows_per_second); wadb_json_uint(j, o, "duration_ms", s->generator.duration_ms);
        wadb_json_uint(j, o, "batch_rows", s->generator.batch_rows); wadb_json_uint(j, o, "cardinality", s->generator.cardinality); wadb_json_uint(j, o, "seed", s->generator.seed);
    } else {
        yyjson_mut_val *result = wadb_json_object(j); wadb_json_add(j, o, "integrity", result);
        wadb_json_uint(j, result, "rows", s->integrity.rows); wadb_json_uint(j, result, "frames", s->integrity.frames);
        wadb_json_uint(j, result, "segments", s->integrity.segments); wadb_json_uint(j, result, "bytes", s->integrity.bytes);
        wadb_json_uint(j, result, "snapshot_sequence", s->integrity.snapshot_sequence);
    }
    if (s->error.code) {
        yyjson_mut_val *error = wadb_json_object(j); wadb_json_add(j, o, "error", error);
        wadb_json_string(j, error, "code", wadb_status_name(s->error.code)); wadb_json_string(j, error, "message", s->error.message);
    }
}
static void start_generator(wadb_api_context *context, wadb_json *j, yyjson_val *root) {
    if (!wadb_json_allowed(j, root, "table_name|rows_per_second|duration_ms|batch_rows|cardinality|seed")) return;
    wadb_generator_options o = {.rows_per_second = 1000, .duration_ms = 10000, .batch_rows = 256, .cardinality = 100, .seed = 1};
    const char *name = string_option(j, root, "table_name");
    if (name) strcpy(o.table_name, name);
    if (j->error.code || !u32_option(j, root, "rows_per_second", &o.rows_per_second) || !u32_option(j, root, "duration_ms", &o.duration_ms) ||
        !u32_option(j, root, "batch_rows", &o.batch_rows) || !u32_option(j, root, "cardinality", &o.cardinality) || !uint_option(j, root, "seed", &o.seed)) return;
    uint64_t id;
    if (!wadb_job_generate(context->jobs, &o, &id, &j->error)) wadb_json_uint(j, j->root, "job_id", id);
}
static bool route_id(const char *path, const char *prefix, uint64_t *id, const char **suffix) {
    size_t n = strlen(prefix);
    if (strncmp(path, prefix, n)) return false;
    const char *p = path + n, *end = strchr(p, '/');
    size_t length = end ? (size_t)(end - p) : strlen(p);
    if (!wadb_decimal_u64(p, length, id) || !*id) return false;
    *suffix = end ? end : p + length; return true;
}
size_t wadb_api_reply_limit(const char *method, const char *path) {
    uint64_t id; const char *suffix;
    /* Appends return only a boot ID and numeric receipt. Even a maximally
     * escaped 255-byte error message fits this cap. Query/schema responses keep
     * the general limit. Share routing with dispatch so the bounds cannot drift. */
    return !strcmp(method, "POST") && route_id(path, "/api/v1/tables/", &id, &suffix) && !strcmp(suffix, "/append") ?
        WADB_HTTP_APPEND_REPLY_LIMIT : WADB_HTTP_REPLY_LIMIT;
}
void wadb_api_dispatch(wadb_api_context *context, const char *method, const char *path,
    const char *body, size_t length, wadb_http_reply *reply) {
    wadb_json j;
    unsigned status = 200; uint64_t consumed_cursor = 0;
    bool initialized = wadb_json_init(&j, body, length);
    j.reply_limit = wadb_api_reply_limit(method, path);
    if (!initialized) { wadb_json_finish(&j, 400, reply); wadb_json_destroy(&j); return; }
    wadb_json_boolean(&j, j.root, "ok", true); wadb_json_string(&j, j.root, "boot_id", context->boot_id);
    yyjson_val *root = yyjson_doc_get_root(j.input);
    bool get = !strcmp(method, "GET"), post = !strcmp(method, "POST");
    uint64_t id = 0; const char *suffix = NULL;
    if (!get && !post) { status = 405; wadb_fail(&j.error, WADB_INVALID, 0, "method not allowed"); }
    else if (!strcmp(path, "/api/v1/health") && get) {
        if (wadb_json_allowed(&j, root, "")) {
            wadb_stats stats;
            if (!wadb_get_stats(context->db, &stats, &j.error)) wadb_json_string(&j, j.root, "status", stats.read_only ? "read_only" : "ready");
        }
    } else if (!strcmp(path, "/api/v1/stats") && get) {
        if (wadb_json_allowed(&j, root, "")) wadb_api_stats(context, &j);
    } else if (!strcmp(path, "/api/v1/tables")) {
        if (post) { create_table(context, &j, root); status = 201; }
        else if (wadb_json_allowed(&j, root, "")) {
            wadb_table *tables[1024]; size_t count;
            if (!wadb_list_tables(context->db, tables, 1024, &count, &j.error)) {
                yyjson_mut_val *a = wadb_json_array(&j); wadb_json_add(&j, j.root, "tables", a);
                for (size_t i = 0; i < count; ++i) { yyjson_mut_val *o = wadb_json_object(&j); table_info(&j, o, tables[i], false); wadb_json_push(&j, a, o); }
            }
        }
    } else if (route_id(path, "/api/v1/tables/", &id, &suffix)) {
        wadb_table *t;
        if (!wadb_get_table(context->db, id, &t, &j.error)) {
            if (!*suffix && get) { if (wadb_json_allowed(&j, root, "")) table_info(&j, j.root, t, true); }
            else if (!strcmp(suffix, "/append") && post) { append_rows(&j, t, root); status = 201; }
            else if (!strcmp(suffix, "/query") && post) consumed_cursor = run_query(&j, context->db, t, root, false);
            else if (!strcmp(suffix, "/tail") && get) consumed_cursor = run_query(&j, context->db, t, root, true);
            else if (!strcmp(suffix, "/segments") && get) segments(&j, t, root);
            else if (!strcmp(suffix, "/retention-preview") && post) retention(&j, t, root, true);
            else if (!strcmp(suffix, "/retention") && post) retention(&j, t, root, false);
            else if (!strcmp(suffix, "/integrity-check") && post) {
                uint64_t bytes = UINT64_C(1024) * 1024 * 1024; uint32_t timeout = 60000;
                if (wadb_json_allowed(&j, root, "max_scan_bytes|timeout_ms") && uint_option(&j, root, "max_scan_bytes", &bytes) && u32_option(&j, root, "timeout_ms", &timeout)) {
                    uint64_t job;
                    if (!wadb_job_integrity(context->jobs, t, bytes, timeout, &job, &j.error)) { wadb_json_uint(&j, j.root, "job_id", job); status = 202; }
                }
            } else wadb_fail(&j.error, WADB_NOT_FOUND, 0, "endpoint not found for this method");
        }
    } else if (route_id(path, "/api/v1/cursors/", &id, &suffix) && post) {
        yyjson_val *boot = yyjson_obj_get(root, "boot_id");
        if (wadb_json_allowed(&j, root, "boot_id")) {
            if (!yyjson_is_str(boot) || yyjson_get_len(boot) != strlen(context->boot_id) || memcmp(yyjson_get_str(boot), context->boot_id, strlen(context->boot_id)))
                wadb_fail(&j.error, WADB_EXPIRED, 0, "cursor belongs to a different server boot");
            else if (!strcmp(suffix, "/close")) { (void)wadb_cursor_close(context->db, id, &j.error); wadb_json_boolean(&j, j.root, "closed", !j.error.code); }
            else if (!strcmp(suffix, "/next")) {
                wadb_result *result = NULL;
                if (!wadb_cursor_next(context->db, id, &result, &j.error)) {
                    consumed_cursor = id; wadb_api_result(&j, j.root, result);
                    if (result->has_more) wadb_json_uint(&j, j.root, "cursor_id", id);
                    wadb_result_free(result);
                }
            } else wadb_fail(&j.error, WADB_NOT_FOUND, 0, "cursor endpoint not found");
        }
    } else if (!strcmp(path, "/api/v1/test-jobs") || !strcmp(path, "/api/v1/jobs")) {
        if (post && !strcmp(path, "/api/v1/test-jobs")) { start_generator(context, &j, root); status = 202; }
        else if (get && wadb_json_allowed(&j, root, "")) {
            wadb_job_info jobs[WADB_JOB_HISTORY]; size_t count;
            if (!wadb_job_list(context->jobs, jobs, WADB_JOB_HISTORY, &count, &j.error)) {
                yyjson_mut_val *a = wadb_json_array(&j); wadb_json_add(&j, j.root, "jobs", a);
                for (size_t i = 0; i < count; ++i) { yyjson_mut_val *o = wadb_json_object(&j); job_info(&j, o, &jobs[i]); wadb_json_push(&j, a, o); }
            }
        } else wadb_fail(&j.error, WADB_NOT_FOUND, 0, "job endpoint not found");
    } else if (route_id(path, "/api/v1/jobs/", &id, &suffix) || route_id(path, "/api/v1/test-jobs/", &id, &suffix)) {
        if (wadb_json_allowed(&j, root, "")) {
            if (!strcmp(suffix, "/stop") && post) (void)wadb_job_stop(context->jobs, id, &j.error);
            else if (*suffix || !get) wadb_fail(&j.error, WADB_NOT_FOUND, 0, "job endpoint not found");
            if (!j.error.code) { wadb_job_info info; if (!wadb_job_get(context->jobs, id, &info, &j.error)) job_info(&j, j.root, &info); }
        }
    } else wadb_fail(&j.error, WADB_NOT_FOUND, 0, "endpoint not found");
    wadb_json_finish(&j, status, reply);
    if (status == 405) reply->status = status;
    if (reply->status >= 400 && consumed_cursor) (void)wadb_cursor_close(context->db, consumed_cursor, NULL);
    wadb_json_destroy(&j);
}
