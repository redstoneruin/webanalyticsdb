#include "query.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    wadb_result public;
    size_t capacity;
} query_result;
typedef struct {
    uint64_t hash;
    wadb_value keys[WADB_MAX_GROUP_FIELDS + 1];
    wadb_value aggregates[WADB_MAX_AGGREGATES];
    uint64_t nonnull[WADB_MAX_AGGREGATES];
} query_group;
typedef struct {
    const wadb_query_plan *plan;
    query_result *result;
    query_group *groups;
    uint32_t *slots;
    size_t group_count, group_capacity, slot_count, used;
    uint64_t last_sequence, deadline;
    bool stopped;
    wadb_error error;
} execution;

static wadb_status charge(execution *x, size_t n) {
    if (n > x->plan->options.memory_limit_bytes - x->used)
        return wadb_fail(&x->error, WADB_LIMIT, 0, "query memory budget exceeded");
    x->used += n;
    return WADB_OK;
}
static wadb_status copy_value(execution *x, wadb_type type, const wadb_value *src, wadb_value *dst) {
    *dst = *src;
    if (!src->is_null && wadb_type_bytes(type)) {
        dst->as.bytes.data = NULL;
        size_t n = src->as.bytes.length;
        wadb_status status = charge(x, n + 1);
        if (status) return status;
        void *copy = malloc(n + 1);
        if (!copy) return wadb_fail(&x->error, WADB_NOMEM, 0, "query string");
        if (n) memcpy(copy, src->as.bytes.data, n);
        dst->as.bytes.data = copy;
    }
    return WADB_OK;
}
void wadb_result_free(wadb_result *r) {
    if (!r) return;
    for (size_t row = 0; row < r->row_count; ++row)
        for (size_t col = 0; col < r->column_count; ++col) {
            wadb_value *v = &r->values[row * r->column_count + col];
            if (wadb_type_bytes(r->columns[col].type) && !v->is_null) free((void *)v->as.bytes.data);
        }
    free(r->columns); free(r->values); free(r->sequences); free(r);
}
static wadb_status result_row(execution *x, const wadb_value *values, uint64_t sequence) {
    query_result *owner = x->result;
    wadb_result *r = &owner->public;
    if (r->row_count == owner->capacity) {
        size_t cap = owner->capacity ? owner->capacity * 2 : 16;
        if (cap > x->plan->options.scan.limit) cap = x->plan->options.scan.limit;
        size_t width = r->column_count * sizeof(wadb_value) + (x->plan->options.aggregate_count ? 0 : sizeof(uint64_t));
        wadb_status status = charge(x, (cap - owner->capacity) * width);
        if (status) return status;
        wadb_value *p = realloc(r->values, cap * r->column_count * sizeof(*p));
        if (!p) return wadb_fail(&x->error, WADB_NOMEM, 0, "query result cells");
        r->values = p;
        if (!x->plan->options.aggregate_count) {
            uint64_t *seq = realloc(r->sequences, cap * sizeof(*seq));
            if (!seq) return wadb_fail(&x->error, WADB_NOMEM, 0, "query result sequences");
            r->sequences = seq;
        }
        owner->capacity = cap;
    }
    wadb_value *row = r->values + r->row_count * r->column_count;
    memset(row, 0, r->column_count * sizeof(*row));
    for (size_t i = 0; i < r->column_count; ++i) row[i].is_null = true;
    if (r->sequences) r->sequences[r->row_count] = sequence;
    ++r->row_count;
    for (size_t i = 0; i < r->column_count; ++i) {
        wadb_status status = copy_value(x, r->columns[i].type, &values[i], &row[i]);
        if (status) return status;
    }
    return WADB_OK;
}
static bool matches(const wadb_query_plan *p, const wadb_value *values) {
    for (size_t i = 0; i < p->options.filter_count; ++i) {
        const wadb_predicate *f = &p->filters[i];
        const wadb_value *v = &values[f->field];
        if (f->op == WADB_IS_NULL) { if (!v->is_null) return false; continue; }
        if (f->op == WADB_IS_NOT_NULL) { if (v->is_null) return false; continue; }
        if (v->is_null) return false;
        int c = wadb_value_compare(p->table->schema.fields[f->field].type, v, &f->value);
        bool yes = f->op == WADB_EQ ? c == 0 : f->op == WADB_NE ? c != 0 :
            f->op == WADB_LT ? c < 0 : f->op == WADB_LE ? c <= 0 : f->op == WADB_GT ? c > 0 : c >= 0;
        if (!yes) return false;
    }
    return true;
}
static uint64_t hash_keys(const wadb_query_plan *p, const wadb_value *keys) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < p->key_count; ++i) {
        const wadb_value *v = &keys[i];
        unsigned char bytes[9] = {v->is_null ? 1 : 0};
        if (v->is_null) { hash = wadb_hash(bytes, 1, hash); continue; }
        wadb_type type = p->columns[i].type;
        uint64_t bits;
        if (wadb_type_bytes(type)) bits = v->as.bytes.length;
        else if (wadb_type_float(type)) { double value = v->as.f64 == 0 ? 0 : v->as.f64; memcpy(&bits, &value, 8); }
        else bits = wadb_type_signed(type) ? (uint64_t)v->as.i64 : v->as.u64;
        wadb_put_u64(bytes + 1, bits); hash = wadb_hash(bytes, sizeof(bytes), hash);
        if (wadb_type_bytes(type)) hash = wadb_hash(v->as.bytes.data, v->as.bytes.length, hash);
    }
    return hash;
}
static wadb_status group_for(execution *x, const wadb_value *keys, query_group **out) {
    const wadb_query_plan *p = x->plan;
    uint64_t hash = hash_keys(p, keys);
    if (x->slot_count) {
        size_t slot = hash & (x->slot_count - 1);
        while (x->slots[slot]) {
            query_group *g = &x->groups[x->slots[slot] - 1];
            bool equal = g->hash == hash;
            for (size_t i = 0; equal && i < p->key_count; ++i)
                equal = !wadb_value_compare(p->columns[i].type, &g->keys[i], &keys[i]);
            if (equal) { *out = g; return WADB_OK; }
            slot = (slot + 1) & (x->slot_count - 1);
        }
    }
    if (x->group_count == p->options.max_groups)
        return wadb_fail(&x->error, WADB_LIMIT, 0, "group count exceeds query budget");
    if (x->group_count == x->group_capacity) {
        size_t cap = x->group_capacity ? x->group_capacity * 2 : 16;
        if (cap > p->options.max_groups) cap = p->options.max_groups;
        wadb_status status = charge(x, (cap - x->group_capacity) * sizeof(query_group));
        if (status) return status;
        query_group *groups = realloc(x->groups, cap * sizeof(*groups));
        if (!groups) return wadb_fail(&x->error, WADB_NOMEM, 0, "query groups");
        x->groups = groups; x->group_capacity = cap;
    }
    if (x->slot_count < (x->group_count + 1) * 2) {
        size_t count = x->slot_count ? x->slot_count * 2 : 32;
        /* The old slots coexist during rehash, and count against peak memory. */
        wadb_status status = charge(x, count * sizeof(uint32_t));
        if (status) return status;
        uint32_t *slots = calloc(count, sizeof(*slots));
        if (!slots) return wadb_fail(&x->error, WADB_NOMEM, 0, "group hash index");
        for (size_t i = 0; i < x->group_count; ++i) {
            size_t slot = x->groups[i].hash & (count - 1);
            while (slots[slot]) slot = (slot + 1) & (count - 1);
            slots[slot] = (uint32_t)i + 1;
        }
        x->used -= x->slot_count * sizeof(*slots);
        free(x->slots); x->slots = slots; x->slot_count = count;
    }
    size_t index = x->group_count++;
    query_group *g = &x->groups[index];
    memset(g, 0, sizeof(*g)); g->hash = hash;
    for (size_t i = 0; i < p->options.aggregate_count; ++i) g->aggregates[i].is_null = p->aggregates[i].op > WADB_COUNT;
    for (size_t i = 0; i < p->key_count; ++i) {
        wadb_status status = copy_value(x, p->columns[i].type, &keys[i], &g->keys[i]);
        if (status) return status;
    }
    size_t slot = hash & (x->slot_count - 1);
    while (x->slots[slot]) slot = (slot + 1) & (x->slot_count - 1);
    x->slots[slot] = (uint32_t)index + 1;
    *out = g;
    return WADB_OK;
}
static wadb_status reduce(execution *x, query_group *g, const wadb_value *values) {
    const wadb_query_plan *p = x->plan;
    for (size_t i = 0; i < p->options.aggregate_count; ++i) {
        wadb_reduction a = p->aggregates[i];
        const wadb_value *v = &values[a.field];
        wadb_value *out = &g->aggregates[i];
        if (a.op != WADB_COUNT_ALL && v->is_null) continue;
        if (g->nonnull[i] == UINT64_MAX) goto overflow;
        uint64_t n = ++g->nonnull[i];
        wadb_type type = p->table->schema.fields[a.field].type;
        if (a.op <= WADB_COUNT) { out->as.u64 = n; continue; }
        if (a.op == WADB_AVG) {
            double value = wadb_type_float(type) ? v->as.f64 : wadb_type_signed(type) ? (double)v->as.i64 : (double)v->as.u64;
            out->as.f64 = n == 1 ? value : out->as.f64 * ((double)(n - 1) / (double)n) + value / (double)n;
            if (!isfinite(out->as.f64)) goto overflow;
        } else if (n == 1) *out = *v;
        else if (a.op == WADB_SUM) {
            if (wadb_type_float(type)) { out->as.f64 += v->as.f64; if (!isfinite(out->as.f64)) goto overflow; }
            else if (wadb_type_signed(type)) {
                if ((v->as.i64 > 0 && out->as.i64 > INT64_MAX - v->as.i64) ||
                    (v->as.i64 < 0 && out->as.i64 < INT64_MIN - v->as.i64)) goto overflow;
                out->as.i64 += v->as.i64;
            } else {
                if (v->as.u64 > UINT64_MAX - out->as.u64) goto overflow;
                out->as.u64 += v->as.u64;
            }
        } else {
            int cmp = wadb_value_compare(type, v, out);
            if ((a.op == WADB_MIN && cmp < 0) || (a.op == WADB_MAX && cmp > 0)) *out = *v;
        }
        out->is_null = false;
    }
    return WADB_OK;
overflow:
    return wadb_fail(&x->error, WADB_LIMIT, 0, "aggregate arithmetic overflow");
}
static wadb_status consume(void *context, uint64_t sequence, const wadb_value *values, size_t count) {
    (void)count;
    execution *x = context;
    const wadb_query_plan *p = x->plan;
    x->last_sequence = sequence;
    if (!matches(p, values)) return WADB_OK;
    if (x->result->public.matched_rows == UINT64_MAX) return wadb_fail(&x->error, WADB_LIMIT, 0, "matched row count overflow");
    ++x->result->public.matched_rows;
    if (!p->options.aggregate_count) {
        wadb_value projected[WADB_MAX_FIELDS];
        for (size_t i = 0; i < p->column_count; ++i) projected[i] = values[p->projection[i]];
        wadb_status status = result_row(x, projected, sequence);
        if (status) return status;
        if (x->result->public.row_count == p->options.scan.limit) { x->stopped = true; return WADB_CANCELLED; }
        return WADB_OK;
    }
    wadb_value keys[WADB_MAX_GROUP_FIELDS + 1] = {{0}};
    size_t key = 0;
    if (p->options.bucket_width_us) {
        wadb_status status = wadb_bucket_floor(values[0].as.i64, p->options.bucket_width_us,
            p->options.bucket_origin_us, &keys[key++].as.i64, &x->error);
        if (status) return status;
    }
    for (size_t i = 0; i < p->options.group_count; ++i) keys[key++] = values[p->groups[i]];
    query_group *g;
    wadb_status status = group_for(x, keys, &g);
    return status ? status : reduce(x, g, values);
}
static int compare_groups(const execution *x, size_t a, size_t b) {
    const wadb_query_plan *p = x->plan;
    size_t col = (size_t)p->order;
    const wadb_value *va = col < p->key_count ? &x->groups[a].keys[col] : &x->groups[a].aggregates[col - p->key_count];
    const wadb_value *vb = col < p->key_count ? &x->groups[b].keys[col] : &x->groups[b].aggregates[col - p->key_count];
    int cmp = wadb_value_compare(p->columns[col].type, va, vb);
    if (!va->is_null && !vb->is_null && p->options.descending) cmp = -cmp;
    return cmp ? cmp : (a > b) - (a < b);
}
static wadb_status emit_groups(execution *x) {
    size_t n = x->group_count;
    x->result->public.total_groups = n;
    if (!n) return WADB_OK;
    wadb_status status = charge(x, n * 2 * sizeof(size_t));
    if (status) return status;
    size_t *order = malloc(n * sizeof(*order)), *tmp = malloc(n * sizeof(*tmp));
    if (!order || !tmp) { free(order); free(tmp); return wadb_fail(&x->error, WADB_NOMEM, 0, "group ordering"); }
    for (size_t i = 0; i < n; ++i) order[i] = i;
    for (size_t width = 1; x->plan->order >= 0 && width < n; width *= 2) {
        if (wadb_monotonic_ns() >= x->deadline) { status = wadb_fail(&x->error, WADB_LIMIT, 0, "query sort deadline exceeded"); break; }
        for (size_t start = 0; start < n; start += 2 * width) {
            size_t mid = start + width < n ? start + width : n, end = start + 2 * width < n ? start + 2 * width : n;
            size_t a = start, b = mid;
            for (size_t j = start; j < end; ++j)
                tmp[j] = a < mid && (b == end || compare_groups(x, order[a], order[b]) <= 0) ? order[a++] : order[b++];
        }
        size_t *swap = order; order = tmp; tmp = swap;
    }
    size_t limit = n < x->plan->options.scan.limit ? n : x->plan->options.scan.limit;
    for (size_t i = 0; !status && i < limit; ++i) {
        if (!(i % 256) && wadb_monotonic_ns() >= x->deadline) { status = wadb_fail(&x->error, WADB_LIMIT, 0, "query output deadline exceeded"); break; }
        query_group *g = &x->groups[order[i]];
        wadb_value row[WADB_MAX_GROUP_FIELDS + 1 + WADB_MAX_AGGREGATES];
        memcpy(row, g->keys, x->plan->key_count * sizeof(*row));
        memcpy(row + x->plan->key_count, g->aggregates, x->plan->options.aggregate_count * sizeof(*row));
        status = result_row(x, row, 0);
    }
    free(order); free(tmp);
    return status;
}
wadb_status wadb_query_execute(const wadb_query_plan *p, const wadb_snapshot *snapshot,
    uint64_t after, uint64_t bytes, uint32_t timeout_ms, wadb_result **out, wadb_error *e) {
    uint64_t started = wadb_monotonic_ns();
    *out = NULL;
    execution x = {.plan = p, .used = p->allocated_bytes, .last_sequence = after,
        .deadline = wadb_monotonic_ns() + (uint64_t)timeout_ms * 1000000};
    wadb_status status = charge(&x, sizeof(query_result) + p->column_count * sizeof(wadb_result_column));
    if (status) { if (e) *e = x.error; wadb_record_query(p->table->db, started, status, NULL); return status; }
    x.result = calloc(1, sizeof(*x.result));
    if (!x.result) {
        wadb_record_query(p->table->db, started, WADB_NOMEM, NULL);
        return wadb_fail(e, WADB_NOMEM, 0, "query result");
    }
    wadb_result *r = &x.result->public;
    r->column_count = p->column_count;
    r->columns = malloc(p->column_count * sizeof(*r->columns));
    if (!r->columns) {
        wadb_result_free(r); wadb_record_query(p->table->db, started, WADB_NOMEM, NULL);
        return wadb_fail(e, WADB_NOMEM, 0, "result columns");
    }
    memcpy(r->columns, p->columns, p->column_count * sizeof(*r->columns));
    if (p->options.aggregate_count && !p->options.group_count) {
        uint32_t count = p->options.bucket_width_us ? p->bucket_count : 1;
        int64_t bucket = p->first_bucket;
        for (uint32_t i = 0; !status && i < count; ++i) {
            if (!(i % 256) && wadb_monotonic_ns() >= x.deadline) { status = wadb_fail(&x.error, WADB_LIMIT, 0, "query bucket deadline exceeded"); break; }
            wadb_value key = {.as.i64 = bucket};
            query_group *unused;
            status = group_for(&x, &key, &unused);
            if (i + 1 < count) bucket += p->options.bucket_width_us;
        }
    }
    wadb_scan_options scan = p->options.scan;
    scan.limit = UINT32_MAX; scan.after_sequence = after; scan.max_scan_bytes = bytes;
    scan.timeout_ms = timeout_ms;
    if (!status) status = wadb_scan_snapshot(snapshot, &scan, consume, &x, &r->stats, e);
    if (status == WADB_CANCELLED && x.stopped) { status = WADB_OK; wadb_error_clear(e); }
    if (!status && !x.stopped && r->stats.limit_reached) status = wadb_fail(e, WADB_LIMIT, 0, "query examined too many rows");
    if (!status && p->options.aggregate_count) status = emit_groups(&x);
    r->has_more = x.stopped && x.last_sequence < snapshot->sequence;
    r->stats.limit_reached = p->options.aggregate_count ? r->total_groups > r->row_count : r->has_more;
    r->stats.rows_returned = r->row_count; r->stats.last_sequence = x.last_sequence;
    for (size_t i = 0; i < x.group_count; ++i)
        for (size_t key = 0; key < p->key_count; ++key)
            if (!x.groups[i].keys[key].is_null && wadb_type_bytes(p->columns[key].type)) free((void *)x.groups[i].keys[key].as.bytes.data);
    free(x.groups); free(x.slots);
    wadb_record_query(p->table->db, started, status, &r->stats);
    if (status) { wadb_result_free(r); if (e && x.error.code) *e = x.error; }
    else *out = r;
    return status;
}
wadb_status wadb_query(wadb_table *t, const wadb_query_options *options, wadb_result **out, wadb_error *e) {
    wadb_error_clear(e);
    if (!out) return wadb_fail(e, WADB_INVALID, 0, "query result output required");
    *out = NULL;
    wadb_query_plan *plan = NULL;
    wadb_status status = wadb_query_compile(t, options, &plan, e);
    if (status) return status;
    wadb_cursor_expire(t->db);
    wadb_snapshot snapshot;
    status = wadb_snapshot_acquire(t, &plan->options.scan, &snapshot, e);
    if (!status) status = wadb_query_execute(plan, &snapshot, plan->options.scan.after_sequence,
        plan->options.scan.max_scan_bytes, plan->options.scan.timeout_ms, out, e);
    wadb_snapshot_release(&snapshot); wadb_query_plan_free(plan);
    return status;
}
