#include "engine.h"
#include "../platform/fault.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

char *wadb_path(const char *base, const char *name) {
    size_t a = strlen(base), b = strlen(name), n;
    if (!wadb_add_size(a, b, &n) || !wadb_add_size(n, 2, &n) || n > 4096) return NULL;
    char *p = malloc(n);
    if (p) snprintf(p, n, "%s/%s", base, name);
    return p;
}
char *wadb_id_path(const char *base, uint64_t id) {
    char name[32];
    snprintf(name, sizeof(name), "%020" PRIu64, id);
    return wadb_path(base, name);
}

void wadb_options_default(wadb_options *o) {
    if (!o) return;
    *o = (wadb_options){.segment_target_bytes = UINT64_C(256) * 1024 * 1024,
        .queue_limit_bytes = 64u * 1024 * 1024, .dictionary_limit_bytes = 8u * 1024 * 1024,
        .read_cache_bytes = 64u * 1024 * 1024, .snapshot_limit_bytes = 64u * 1024 * 1024, .max_readers = 32,
        .batch_target_bytes = 256u * 1024, .batch_delay_ms = 5, .max_tables = WADB_MAX_TABLES};
}

void wadb_table_free(wadb_table *t) {
    if (!t) return;
    if (t->active_fd >= 0) close(t->active_fd);
    wadb_dictionary_free(t->dictionary);
    wadb_index_free(&t->index);
    free(t->readable_segments);
    free(t->directory); free(t->segments); free(t);
}
void wadb_close(wadb_db *db) {
    if (!db) return;
    wadb_writer_stop(db);
    wadb_cursor_clear(db);
    wadb_cache_clear(db);
    for (size_t i = 0; i < db->table_count; ++i) wadb_table_free(db->tables[i]);
    free(db->tables);
    if (db->lock_fd >= 0) close(db->lock_fd);
    if (db->mutex_initialized) pthread_mutex_destroy(&db->mutex);
    if (db->writer_mutex_initialized) pthread_mutex_destroy(&db->writer_mutex);
    if (db->work_initialized) pthread_cond_destroy(&db->work);
    free(db->directory); free(db);
}

wadb_status wadb_open(const char *directory, const wadb_options *options,
    wadb_db **out, wadb_error *e) {
    wadb_error_clear(e);
    if (!directory || !directory[0] || !out) return wadb_fail(e, WADB_INVALID, 0, "database directory required");
    *out = NULL;
    wadb_options opts;
    wadb_options_default(&opts);
    if (options) opts = *options;
    if (!opts.max_tables || opts.max_tables > WADB_MAX_TABLES || opts.segment_target_bytes < 256 ||
        opts.segment_target_bytes > INT64_MAX || opts.queue_limit_bytes < WADB_MAX_FRAME_BYTES ||
        opts.dictionary_limit_bytes < 1024 || opts.dictionary_limit_bytes > 64u * 1024 * 1024 || opts.batch_target_bytes < 256 ||
        opts.batch_target_bytes > WADB_MAX_FRAME_BYTES || opts.batch_delay_ms > 1000 ||
        opts.read_cache_bytes < opts.dictionary_limit_bytes + sizeof(wadb_cache_entry) ||
        opts.snapshot_limit_bytes < 1024 || !opts.max_readers || opts.max_readers > 1024)
        return wadb_fail(e, WADB_INVALID, 0, "invalid database limits");
    wadb_status status = wadb_mkdir(directory, e);
    if (status) return status;
    wadb_db *db = calloc(1, sizeof(*db));
    if (!db) return wadb_fail(e, WADB_NOMEM, 0, "allocate database");
    db->lock_fd = -1;
    db->boot_ns = wadb_monotonic_ns();
    db->directory = realpath(directory, NULL);
    db->options = opts;
    db->next_table_id = 1;
    db->tables = calloc(opts.max_tables, sizeof(*db->tables));
    int rc = pthread_mutex_init(&db->mutex, NULL);
    if (!rc) db->mutex_initialized = true;
    if (!rc) {
        rc = pthread_mutex_init(&db->writer_mutex, NULL);
        if (!rc) db->writer_mutex_initialized = true;
    }
    if (!db->directory || !db->tables || rc) {
        status = wadb_fail(e, WADB_NOMEM, rc, "initialize database"); goto fail;
    }
    char *parent = strdup(db->directory);
    if (!parent) { status = wadb_fail(e, WADB_NOMEM, 0, "directory path"); goto fail; }
    char *slash = strrchr(parent, '/');
    if (slash == parent) parent[1] = 0; else *slash = 0;
    status = wadb_sync_directory(parent, e);
    free(parent);
    if (status) goto fail;
    char *path = wadb_path(db->directory, "LOCK");
    if (!path) { status = wadb_fail(e, WADB_NOMEM, 0, "lock path"); goto fail; }
    db->lock_fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    free(path);
    if (db->lock_fd < 0) { status = wadb_fail(e, WADB_IO, errno, "open database lock"); goto fail; }
    if (flock(db->lock_fd, LOCK_EX | LOCK_NB)) {
        status = wadb_fail(e, WADB_LOCKED, errno, "database is already open by another owner"); goto fail;
    }
    path = wadb_path(db->directory, "tables");
    if (!path) { status = wadb_fail(e, WADB_NOMEM, 0, "tables path"); goto fail; }
    status = wadb_mkdir(path, e);
    free(path);
    if (!status) status = wadb_sync_directory(db->directory, e);
    if (status) goto fail;
    status = wadb_load_catalog(db, e);
    if (status == WADB_NOT_FOUND) {
        path = wadb_path(db->directory, "tables");
        DIR *dir = path ? opendir(path) : NULL;
        free(path);
        if (!dir) { status = wadb_fail(e, WADB_IO, errno, "inspect tables before initializing catalog"); goto fail; }
        bool empty = true;
        struct dirent *entry;
        while ((entry = readdir(dir))) if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) empty = false;
        closedir(dir);
        if (!empty) { status = wadb_fail(e, WADB_CORRUPT, 0, "catalog missing from nonempty database"); goto fail; }
        status = wadb_save_catalog(db, NULL, e);
    }
    if (status) goto fail;
    status = wadb_writer_start(db, e);
    if (status) goto fail;
    wadb_error_clear(e);
    *out = db;
    return WADB_OK;
fail:
    wadb_close(db);
    return status;
}

wadb_status wadb_create_table(wadb_db *db, const char *name, const wadb_field_def *fields,
    size_t count, uint64_t retention_us, wadb_table **out, wadb_error *e) {
    wadb_error_clear(e);
    if (!db || !out || !wadb_identifier(name) || retention_us > INT64_MAX)
        return wadb_fail(e, WADB_INVALID, 0, "invalid table name or retention");
    *out = NULL;
    wadb_table *t = calloc(1, sizeof(*t));
    if (!t) return wadb_fail(e, WADB_NOMEM, 0, "allocate table");
    t->active_fd = -1;
    wadb_status status = wadb_schema_build(&t->schema, fields, count, e);
    if (status) { wadb_table_free(t); return status; }
    t->db = db; strcpy(t->name, name);
    t->next_segment_id = 1; t->manifest_generation = 1; t->retention_us = retention_us;
    pthread_mutex_lock(&db->mutex);
    if (db->read_only) { status = wadb_fail(e, WADB_READ_ONLY, 0, "database metadata requires recovery"); goto done; }
    if (db->table_count >= db->options.max_tables) { status = wadb_fail(e, WADB_LIMIT, 0, "table limit reached"); goto done; }
    for (size_t i = 0; i < db->table_count; ++i) if (!strcmp(db->tables[i]->name, name)) {
        status = wadb_fail(e, WADB_EXISTS, 0, "table already exists: %s", name); goto done;
    }
    char *tables = wadb_path(db->directory, "tables");
    if (!tables) { status = wadb_fail(e, WADB_NOMEM, 0, "table path"); goto done; }
    /* Unreferenced directories from interrupted creates remain quarantined by
     * absence from the catalog. Never overwrite or auto-import their contents. */
    for (;;) {
        if (db->next_table_id == UINT64_MAX) { status = wadb_fail(e, WADB_LIMIT, 0, "table IDs exhausted"); break; }
        t->id = db->next_table_id++;
        free(t->directory);
        t->directory = wadb_id_path(tables, t->id);
        if (!t->directory) { status = wadb_fail(e, WADB_NOMEM, 0, "table directory path"); break; }
        if (!WADB_IO_CALL("table.create", t->directory, mkdir(t->directory, 0700))) break;
        if (errno != EEXIST) { status = wadb_fail(e, WADB_IO, errno, "create table directory"); break; }
    }
    if (!status) status = wadb_sync_directory(tables, e);
    free(tables);
    if (!status) status = wadb_save_schema(t, e);
    if (!status) status = wadb_save_manifest(t, e);
    if (!status) status = wadb_save_catalog(db, t, e);
    if (status == WADB_INDETERMINATE) db->read_only = true;
    if (!status) { db->tables[db->table_count++] = t; *out = t; }
done:
    pthread_mutex_unlock(&db->mutex);
    if (status) wadb_table_free(t);
    return status;
}

wadb_status wadb_get_table(wadb_db *db, uint64_t id, wadb_table **out, wadb_error *e) {
    if (!db || !out) return wadb_fail(e, WADB_INVALID, 0, "database and output required");
    *out = NULL;
    pthread_mutex_lock(&db->mutex);
    for (size_t i = 0; i < db->table_count; ++i) if (db->tables[i]->id == id) { *out = db->tables[i]; break; }
    pthread_mutex_unlock(&db->mutex);
    return *out ? WADB_OK : wadb_fail(e, WADB_NOT_FOUND, 0, "table not found");
}
wadb_status wadb_find_table(wadb_db *db, const char *name, wadb_table **out, wadb_error *e) {
    if (!db || !name || !out) return wadb_fail(e, WADB_INVALID, 0, "database, name and output required");
    *out = NULL;
    pthread_mutex_lock(&db->mutex);
    for (size_t i = 0; i < db->table_count; ++i) if (!strcmp(db->tables[i]->name, name)) { *out = db->tables[i]; break; }
    pthread_mutex_unlock(&db->mutex);
    return *out ? WADB_OK : wadb_fail(e, WADB_NOT_FOUND, 0, "table not found");
}
wadb_status wadb_list_tables(wadb_db *db, wadb_table **tables, size_t cap, size_t *count, wadb_error *e) {
    if (!db || !count || (!tables && cap)) return wadb_fail(e, WADB_INVALID, 0, "invalid table list arguments");
    pthread_mutex_lock(&db->mutex);
    *count = db->table_count;
    wadb_status status = WADB_OK;
    if (tables) {
        if (cap < db->table_count) status = wadb_fail(e, WADB_LIMIT, 0, "table list capacity too small");
        else memcpy(tables, db->tables, db->table_count * sizeof(*tables));
    }
    pthread_mutex_unlock(&db->mutex);
    return status;
}
uint64_t wadb_table_id(const wadb_table *t) { return t ? t->id : 0; }
const char *wadb_table_name(const wadb_table *t) { return t ? t->name : NULL; }
const wadb_schema *wadb_table_schema(const wadb_table *t) { return t ? &t->schema : NULL; }
