#include "../core/engine.h"
#include <stdlib.h>
#include <string.h>

static bool zeroes(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; ++i) if (p[i]) return false;
    return true;
}
static bool stored_name(const unsigned char *p) {
    const unsigned char *end = memchr(p, 0, WADB_NAME_CAP);
    return end && wadb_identifier((const char *)p) && zeroes(end, WADB_NAME_CAP - (size_t)(end - p));
}
static wadb_status envelope(const unsigned char *p, size_t n, const char *magic,
    size_t minimum, wadb_error *e) {
    if (n < minimum || memcmp(p, magic, 8) || wadb_crc32c(p, n - 4) != wadb_get_u32(p + n - 4))
        return wadb_fail(e, WADB_CORRUPT, 0, "metadata magic, size, or checksum mismatch");
    if (wadb_get_u32(p + 8) != WADB_FORMAT_VERSION)
        return wadb_fail(e, WADB_VERSION, 0, "unsupported metadata version");
    return WADB_OK;
}
static wadb_status install(const char *dir, const char *name, unsigned char *p,
    size_t n, wadb_error *e) {
    char *path = wadb_path(dir, name);
    if (!path) return wadb_fail(e, WADB_NOMEM, 0, "metadata path");
    wadb_put_u32(p + n - 4, wadb_crc32c(p, n - 4));
    wadb_status status = wadb_atomic_file(path, p, n, e);
    free(path);
    return status;
}
static wadb_status load(const char *dir, const char *name, size_t limit,
    unsigned char **out, size_t *n, wadb_error *e) {
    char *path = wadb_path(dir, name);
    if (!path) return wadb_fail(e, WADB_NOMEM, 0, "metadata path");
    wadb_status status = wadb_read_file(path, limit, out, n, e);
    free(path);
    return status;
}

wadb_status wadb_save_catalog(wadb_db *db, wadb_table *addition, wadb_error *e) {
    size_t count = db->table_count + (addition != NULL), n = 32 + count * 8 + 4;
    unsigned char *p = calloc(1, n);
    if (!p) return wadb_fail(e, WADB_NOMEM, 0, "catalog allocation");
    memcpy(p, "WADBCAT1", 8);
    wadb_put_u32(p + 8, WADB_FORMAT_VERSION);
    wadb_put_u32(p + 12, 32);
    wadb_put_u32(p + 16, (uint32_t)count);
    wadb_put_u64(p + 24, db->next_table_id);
    for (size_t i = 0; i < db->table_count; ++i) wadb_put_u64(p + 32 + i * 8, db->tables[i]->id);
    if (addition) wadb_put_u64(p + 32 + db->table_count * 8, addition->id);
    wadb_status status = install(db->directory, "catalog", p, n, e);
    free(p);
    return status;
}

wadb_status wadb_load_catalog(wadb_db *db, wadb_error *e) {
    unsigned char *p = NULL;
    size_t n = 0;
    wadb_status status = load(db->directory, "catalog", 36 + WADB_MAX_TABLES * 8, &p, &n, e);
    if (status) return status;
    status = envelope(p, n, "WADBCAT1", 36, e);
    if (status) goto done;
    uint32_t count = wadb_get_u32(p + 16);
    uint64_t next = wadb_get_u64(p + 24), previous = 0;
    if (wadb_get_u32(p + 12) != 32 || !zeroes(p + 20, 4) || !next ||
        count > WADB_MAX_TABLES || n != 36 + (size_t)count * 8) goto corrupt;
    if (count > db->options.max_tables) { status = wadb_fail(e, WADB_LIMIT, 0, "configured table limit is below catalog size"); goto done; }
    char *tables = wadb_path(db->directory, "tables");
    if (!tables) { status = wadb_fail(e, WADB_NOMEM, 0, "tables path"); goto done; }
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t id = wadb_get_u64(p + 32 + (size_t)i * 8);
        if (id <= previous || id >= next) { free(tables); goto corrupt; }
        previous = id;
        wadb_table *t = calloc(1, sizeof(*t));
        if (!t) { status = wadb_fail(e, WADB_NOMEM, 0, "table allocation"); break; }
        t->active_fd = -1;
        t->id = id; t->db = db;
        t->directory = wadb_id_path(tables, id);
        if (!t->directory) status = wadb_fail(e, WADB_NOMEM, 0, "table directory");
        else status = wadb_load_schema(t, e);
        if (!status) status = wadb_load_manifest(t, e);
        if (!status) status = wadb_storage_recover(t, e);
        if (status == WADB_NOT_FOUND) status = wadb_fail(e, WADB_CORRUPT, 0, "catalog references missing table metadata");
        if (!status) for (size_t j = 0; j < db->table_count; ++j) if (!strcmp(db->tables[j]->name, t->name)) {
            status = wadb_fail(e, WADB_CORRUPT, 0, "duplicate table name in catalog"); break;
        }
        if (status) { wadb_table_free(t); break; }
        db->tables[db->table_count++] = t;
    }
    free(tables);
    if (!status) db->next_table_id = next;
    goto done;
corrupt:
    status = wadb_fail(e, WADB_CORRUPT, 0, "invalid catalog records");
done:
    free(p);
    return status;
}

wadb_status wadb_save_schema(wadb_table *t, wadb_error *e) {
    uint32_t count = t->schema.field_count - 1;
    size_t n = 132 + (size_t)count * 80;
    unsigned char *p = calloc(1, n);
    if (!p) return wadb_fail(e, WADB_NOMEM, 0, "schema allocation");
    memcpy(p, "WADBSCH1", 8);
    wadb_put_u32(p + 8, WADB_FORMAT_VERSION);
    wadb_put_u32(p + 12, count);
    wadb_put_u64(p + 16, t->id);
    wadb_put_u64(p + 24, t->schema.fingerprint);
    wadb_put_u32(p + 32, t->schema.row_width);
    wadb_put_u32(p + 36, t->schema.null_bytes);
    memcpy(p + 40, t->name, strlen(t->name));
    for (uint32_t i = 0; i < count; ++i) {
        unsigned char *record = p + 128 + (size_t)i * 80;
        const wadb_field *f = &t->schema.fields[i + 1];
        memcpy(record, f->name, strlen(f->name));
        wadb_put_u32(record + 64, (uint32_t)f->type);
        wadb_put_u32(record + 68, f->size);
        wadb_put_u32(record + 72, f->nullable);
    }
    wadb_status status = install(t->directory, "schema", p, n, e);
    free(p);
    return status;
}

wadb_status wadb_load_schema(wadb_table *t, wadb_error *e) {
    unsigned char *p = NULL;
    size_t n;
    wadb_status status = load(t->directory, "schema", 132 + (WADB_MAX_FIELDS - 1) * 80, &p, &n, e);
    if (status) return status;
    status = envelope(p, n, "WADBSCH1", 132, e);
    if (status) goto done;
    uint32_t count = wadb_get_u32(p + 12);
    if (count >= WADB_MAX_FIELDS || n != 132 + (size_t)count * 80 ||
        wadb_get_u64(p + 16) != t->id || !stored_name(p + 40) || !zeroes(p + 104, 24)) goto corrupt;
    wadb_field_def defs[WADB_MAX_FIELDS];
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char *r = p + 128 + (size_t)i * 80;
        if (!stored_name(r) || wadb_get_u32(r + 72) > 1 || !zeroes(r + 76, 4)) goto corrupt;
        defs[i] = (wadb_field_def){.name = (const char *)r, .type = (wadb_type)wadb_get_u32(r + 64),
            .size = wadb_get_u32(r + 68), .nullable = wadb_get_u32(r + 72) != 0};
    }
    if (wadb_schema_build(&t->schema, defs, count, NULL) ||
        t->schema.fingerprint != wadb_get_u64(p + 24) || t->schema.row_width != wadb_get_u32(p + 32) ||
        t->schema.null_bytes != wadb_get_u32(p + 36)) goto corrupt;
    strcpy(t->name, (const char *)p + 40);
    goto done;
corrupt:
    status = wadb_fail(e, WADB_CORRUPT, 0, "invalid persisted table schema");
done:
    free(p);
    return status;
}

wadb_status wadb_save_manifest(wadb_table *t, wadb_error *e) {
    if (t->segment_count > WADB_MAX_SEGMENTS) return wadb_fail(e, WADB_LIMIT, 0, "segment metadata limit");
    size_t n = 84 + t->segment_count * 64;
    unsigned char *p = calloc(1, n);
    if (!p) return wadb_fail(e, WADB_NOMEM, 0, "manifest allocation");
    memcpy(p, "WADBMAN1", 8);
    wadb_put_u32(p + 8, WADB_FORMAT_VERSION);
    wadb_put_u32(p + 12, 80);
    wadb_put_u64(p + 16, t->id);
    wadb_put_u64(p + 24, t->next_segment_id);
    wadb_put_u64(p + 32, t->preserved_sequence);
    wadb_put_u64(p + 40, (uint64_t)t->preserved_time);
    wadb_put_u64(p + 48, t->retention_us);
    wadb_put_u64(p + 56, t->manifest_generation);
    wadb_put_u32(p + 64, (uint32_t)t->segment_count);
    for (size_t i = 0; i < t->segment_count; ++i) {
        const wadb_segment_meta *s = &t->segments[i];
        unsigned char *r = p + 80 + i * 64;
        wadb_put_u64(r, s->id); wadb_put_u64(r + 8, (uint64_t)s->day_start);
        wadb_put_u64(r + 16, s->first_sequence); wadb_put_u64(r + 24, s->last_sequence);
        wadb_put_u64(r + 32, (uint64_t)s->min_time); wadb_put_u64(r + 40, (uint64_t)s->max_time);
        wadb_put_u64(r + 48, s->committed_bytes); wadb_put_u32(r + 56, (uint32_t)s->state);
    }
    wadb_status status = install(t->directory, "manifest", p, n, e);
    free(p);
    return status;
}

wadb_status wadb_load_manifest(wadb_table *t, wadb_error *e) {
    unsigned char *p = NULL;
    size_t n;
    wadb_status status = load(t->directory, "manifest", 84 + (size_t)WADB_MAX_SEGMENTS * 64, &p, &n, e);
    if (status) return status;
    status = envelope(p, n, "WADBMAN1", 84, e);
    if (status) goto done;
    uint32_t count = wadb_get_u32(p + 64);
    uint64_t next = wadb_get_u64(p + 24), previous = 0;
    unsigned active = 0;
    if (count > WADB_MAX_SEGMENTS || n != 84 + (size_t)count * 64 || wadb_get_u32(p + 12) != 80 ||
        wadb_get_u64(p + 16) != t->id || !next || !zeroes(p + 68, 12) ||
        wadb_get_i64(p + 40) < 0 || wadb_get_u64(p + 48) > INT64_MAX || !wadb_get_u64(p + 56)) goto corrupt;
    wadb_segment_meta *segments = count ? calloc(count, sizeof(*segments)) : NULL;
    if (count && !segments) { status = wadb_fail(e, WADB_NOMEM, 0, "segment metadata allocation"); goto done; }
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char *r = p + 80 + (size_t)i * 64;
        wadb_segment_meta s = {.id = wadb_get_u64(r), .day_start = wadb_get_i64(r + 8),
            .first_sequence = wadb_get_u64(r + 16), .last_sequence = wadb_get_u64(r + 24),
            .min_time = wadb_get_i64(r + 32), .max_time = wadb_get_i64(r + 40),
            .committed_bytes = wadb_get_u64(r + 48), .state = (wadb_segment_state)wadb_get_u32(r + 56)};
        if (s.id <= previous || s.id >= next || s.day_start < 0 || s.day_start % WADB_DAY_US ||
            !s.first_sequence || s.committed_bytes < WADB_SEGMENT_HEADER_BYTES || s.committed_bytes > INT64_MAX ||
            !zeroes(r + 60, 4) || s.state < WADB_SEG_ACTIVE || s.state > WADB_SEG_RETIRED ||
            (s.last_sequence && (s.last_sequence < s.first_sequence || s.min_time < s.day_start ||
                s.max_time < s.min_time || s.max_time / WADB_DAY_US != s.day_start / WADB_DAY_US)) ||
            (!s.last_sequence && (s.min_time || s.max_time || s.committed_bytes != WADB_SEGMENT_HEADER_BYTES)) ||
            (s.state == WADB_SEG_ACTIVE && (++active > 1 || i + 1 != count))) {
            free(segments); goto corrupt;
        }
        previous = s.id; segments[i] = s;
    }
    free(t->segments); t->segments = segments; t->segment_count = count;
    t->next_segment_id = next;
    t->preserved_sequence = wadb_get_u64(p + 32); t->preserved_time = wadb_get_i64(p + 40);
    t->retention_us = wadb_get_u64(p + 48); t->manifest_generation = wadb_get_u64(p + 56);
    goto done;
corrupt:
    status = wadb_fail(e, WADB_CORRUPT, 0, "invalid segment manifest");
done:
    free(p);
    return status;
}
