#include "../core/engine.h"
#include "../platform/fault.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char *partition_path(wadb_table *t, int64_t day_start) {
    time_t seconds = (time_t)(day_start / 1000000);
    struct tm utc;
    char date[32];
    if (!gmtime_r(&seconds, &utc) || !strftime(date, sizeof(date), "%Y-%m-%d", &utc)) return NULL;
    return wadb_path(t->directory, date);
}
char *wadb_segment_path(wadb_table *t, const wadb_segment_meta *s, const char *ext) {
    char *dir = partition_path(t, s->day_start);
    if (!dir) return NULL;
    char name[32];
    snprintf(name, sizeof(name), "%020" PRIu64 ".%s", s->id, ext);
    char *path = wadb_path(dir, name);
    free(dir);
    return path;
}

static wadb_status create_active(wadb_table *t, int64_t now, wadb_error *e) {
    if (t->next_segment_id == UINT64_MAX || t->segment_count >= WADB_MAX_SEGMENTS || t->manifest_generation == UINT64_MAX)
        return wadb_fail(e, WADB_LIMIT, 0, "segment metadata exhausted");
    if (t->segment_count && t->segments[t->segment_count - 1].state == WADB_SEG_ACTIVE) {
        wadb_status status = wadb_index_save(t, &t->segments[t->segment_count - 1], &t->index, t->dictionary, e);
        if (status) return status;
    }
    wadb_segment_meta *items = calloc(t->segment_count + 1, sizeof(*items));
    if (!items) return wadb_fail(e, WADB_NOMEM, 0, "segment directory allocation");
    if (t->segment_count) memcpy(items, t->segments, t->segment_count * sizeof(*items));
    if (t->segment_count && items[t->segment_count - 1].state == WADB_SEG_ACTIVE)
        items[t->segment_count - 1].state = WADB_SEG_SEALED;
    wadb_segment_meta *s = &items[t->segment_count];
    *s = (wadb_segment_meta){.id = t->next_segment_id, .day_start = now - now % WADB_DAY_US,
        .first_sequence = t->last_sequence + 1, .committed_bytes = WADB_SEGMENT_HEADER_BYTES, .state = WADB_SEG_ACTIVE};
    char *partition = partition_path(t, s->day_start), *path = NULL;
    wadb_status status = partition ? wadb_mkdir(partition, e) : wadb_fail(e, WADB_NOMEM, 0, "partition path");
    if (!status) status = wadb_sync_directory(t->directory, e);
    int fd = -1;
    if (!status) {
        /* Skip orphan segment files left before manifest installation. */
        for (;;) {
            if (s->id == UINT64_MAX) { status = wadb_fail(e, WADB_LIMIT, 0, "segment IDs exhausted"); break; }
            free(path); path = wadb_segment_path(t, s, "seg");
            if (!path) { status = wadb_fail(e, WADB_NOMEM, 0, "segment path"); break; }
            fd = WADB_IO_CALL("segment.create", path,
                open(path, O_RDWR | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600));
            if (fd >= 0) break;
            if (errno != EEXIST) { status = wadb_fail(e, WADB_IO, errno, "create segment"); break; }
            ++s->id;
        }
    }
    unsigned char header[WADB_SEGMENT_HEADER_BYTES];
    wadb_segment_header h = {.table_id = t->id, .segment_id = s->id,
        .schema_hash = t->schema.fingerprint, .row_width = t->schema.row_width,
        .day_start = s->day_start, .first_sequence = s->first_sequence};
    if (!status) status = wadb_segment_encode(&h, header, e);
    if (!status) status = wadb_write_all(NULL, fd, header, sizeof(header), e);
    if (!status) status = wadb_sync_file(NULL, fd, e);
    if (!status) status = wadb_sync_directory(partition, e);
    if (!status) {
        pthread_mutex_lock(&t->db->mutex);
        wadb_table staged = *t;
        pthread_mutex_unlock(&t->db->mutex);
        staged.segments = items; ++staged.segment_count;
        staged.next_segment_id = s->id + 1; ++staged.manifest_generation;
        status = wadb_save_manifest(&staged, e);
        if (!status) {
            pthread_mutex_lock(&t->db->mutex);
            free(t->segments); t->segments = items; items = NULL;
            t->segment_count = staged.segment_count;
            t->next_segment_id = staged.next_segment_id;
            t->manifest_generation = staged.manifest_generation;
            ++t->retained_segments; t->retained_bytes += WADB_SEGMENT_HEADER_BYTES;
            t->published_dictionary_bytes = 0;
            wadb_index_free(&t->index);
            if (t->active_fd >= 0) close(t->active_fd);
            t->active_fd = fd; fd = -1;
            pthread_mutex_unlock(&t->db->mutex);
        }
    }
    if (status == WADB_INDETERMINATE) {
        pthread_mutex_lock(&t->db->mutex);
        t->read_only = true;
        pthread_mutex_unlock(&t->db->mutex);
    }
    if (fd >= 0) close(fd);
    free(items); free(partition); free(path);
    return status;
}

static wadb_status recover_segment(wadb_table *t, wadb_segment_meta *s, bool force_scan, uint64_t deadline, wadb_error *e) {
    char *path = wadb_segment_path(t, s, "seg");
    if (!path) return wadb_fail(e, WADB_NOMEM, 0, "segment path");
    bool active = s->state == WADB_SEG_ACTIVE;
    int fd = open(path, (active ? O_RDWR | O_APPEND : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW);
    free(path);
    if (fd < 0) return wadb_fail(e, WADB_CORRUPT, errno, "manifest references unavailable segment");
    struct stat st;
    wadb_status status = WADB_OK;
    wadb_dictionary *dictionary = NULL;
    wadb_frame_index index = {0};
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < WADB_SEGMENT_HEADER_BYTES) {
        status = wadb_fail(e, WADB_CORRUPT, errno, "invalid segment file"); goto done;
    }
    uint64_t file_bytes = (uint64_t)st.st_size;
    if ((!active && file_bytes != s->committed_bytes) || file_bytes < s->committed_bytes) {
        status = wadb_fail(e, WADB_CORRUPT, 0, "segment size contradicts manifest"); goto done;
    }
    unsigned char header[WADB_SEGMENT_HEADER_BYTES];
    wadb_segment_header h;
    status = wadb_pread_all(fd, header, sizeof(header), 0, e);
    if (!status) status = wadb_segment_decode(header, sizeof(header), &h, e);
    if (status) goto done;
    if (h.table_id != t->id || h.segment_id != s->id || h.schema_hash != t->schema.fingerprint ||
        h.row_width != t->schema.row_width || h.day_start != s->day_start || h.first_sequence != s->first_sequence ||
        t->last_sequence == UINT64_MAX || s->first_sequence != t->last_sequence + 1) {
        status = wadb_fail(e, WADB_CORRUPT, 0, "segment identity or sequence mismatch"); goto done;
    }
    if (!active && !force_scan) {
        wadb_index_reader reader;
        status = wadb_index_open(t, s, &reader, false, e);
        if (!status) {
            wadb_index_close(&reader);
            if (s->last_sequence) {
                if (s->min_time < t->last_time) { status = wadb_fail(e, WADB_CORRUPT, 0, "unordered sealed segments"); goto done; }
                t->last_sequence = s->last_sequence; t->last_time = s->max_time;
            }
            goto done;
        }
        if (status == WADB_NOMEM || status == WADB_LIMIT) goto done;
        status = WADB_OK;
        wadb_error_clear(e);
    }
    uint64_t offset = WADB_SEGMENT_HEADER_BYTES, last = 0;
    int64_t min_time = 0, max_time = 0;
    status = wadb_dictionary_new(&t->schema, t->db->options.dictionary_limit_bytes, &dictionary, e);
    if (status) goto done;
    while (offset < file_bytes) {
        if (wadb_monotonic_ns() >= deadline) { status = wadb_fail(e, WADB_LIMIT, 0, "index rebuild deadline exceeded"); goto done; }
        wadb_frame f;
        if (file_bytes - offset < WADB_FRAME_HEADER_BYTES) goto partial;
        status = wadb_pread_all(fd, header, WADB_FRAME_HEADER_BYTES, offset, e);
        if (!status) status = wadb_frame_inspect(header, WADB_FRAME_HEADER_BYTES, &f, e);
        if (status) goto done;
        if (f.frame_bytes > file_bytes - offset) goto partial;
        unsigned char *bytes = malloc(f.frame_bytes);
        if (!bytes) { status = wadb_fail(e, WADB_NOMEM, 0, "recovery frame buffer"); goto done; }
        status = wadb_pread_all(fd, bytes, f.frame_bytes, offset, e);
        if (!status) status = wadb_frame_decode(bytes, f.frame_bytes, &f, e);
        if (!status && (f.schema_hash != h.schema_hash || f.row_width != h.row_width ||
            t->last_sequence == UINT64_MAX || f.first_sequence != t->last_sequence + 1 ||
            f.min_time < t->last_time || f.min_time / WADB_DAY_US != s->day_start / WADB_DAY_US))
            status = wadb_fail(e, WADB_CORRUPT, 0, "frame schema, sequence or timestamp mismatch");
        if (!status) status = wadb_dictionary_apply(dictionary, &f, e);
        if (!status) for (uint32_t i = 0; i < f.row_count; ++i) {
            status = wadb_validate_row(&t->schema, f.rows + (size_t)i * f.row_width,
                wadb_dictionary_resolve, dictionary, e);
            if (status) break;
        }
        if (!status) status = wadb_index_reserve(&index, e);
        if (!status) {
            index.entries[index.count++] = wadb_index_for_frame(&f, bytes, offset);
            if (!last) min_time = f.min_time;
            max_time = f.max_time;
            last = f.first_sequence + f.row_count - 1;
            t->last_sequence = last; t->last_time = max_time;
            offset += f.frame_bytes;
        }
        free(bytes);
        if (status) goto done;
        continue;
partial:
        if (!active || offset < s->committed_bytes) {
            status = wadb_fail(e, WADB_CORRUPT, 0, "partial frame inside committed segment"); goto done;
        }
        if (WADB_IO_CALL("segment.truncate", NULL, ftruncate(fd, (off_t)offset))) {
            status = wadb_fail(e, WADB_IO, errno, "truncate interrupted tail"); goto done;
        }
        status = wadb_sync_file(NULL, fd, e);
        if (status) goto done;
        break;
    }
    if ((!active && (s->last_sequence != last || s->min_time != min_time || s->max_time != max_time)) ||
        (active && last < s->last_sequence)) {
        status = wadb_fail(e, WADB_CORRUPT, 0, "segment summary contradicts durable manifest"); goto done;
    }
    s->last_sequence = last; s->min_time = min_time; s->max_time = max_time; s->committed_bytes = offset;
    if (active) {
        t->active_fd = fd; fd = -1; t->dictionary = dictionary; dictionary = NULL;
        t->index = index; memset(&index, 0, sizeof(index));
    } else if (force_scan) {
        t->dictionary = dictionary; dictionary = NULL;
        t->index = index; memset(&index, 0, sizeof(index));
    } else {
        status = wadb_index_save(t, s, &index, dictionary, e);
        /* Derived files may be unavailable on a full disk. Valid authoritative
         * rows remain readable through a bounded transient index. */
        if (status == WADB_IO || status == WADB_INDETERMINATE) { status = WADB_OK; wadb_error_clear(e); }
    }
done:
    wadb_index_free(&index);
    wadb_dictionary_free(dictionary);
    if (fd >= 0) close(fd);
    return status;
}

wadb_status wadb_storage_recover(wadb_table *t, wadb_error *e) {
    t->last_sequence = t->preserved_sequence; t->last_time = t->preserved_time;
    for (size_t i = 0; i < t->segment_count; ++i) {
        if (t->segments[i].state == WADB_SEG_RETIRED) { t->pending_retention = true; ++t->retired_segments; continue; }
        wadb_status status = recover_segment(t, &t->segments[i], false, UINT64_MAX, e);
        if (status) return status;
        ++t->retained_segments; t->retained_bytes += t->segments[i].committed_bytes;
        if (t->segments[i].last_sequence) {
            t->retained_rows += t->segments[i].last_sequence - t->segments[i].first_sequence + 1;
            status = wadb_readable_reserve(t, e);
            if (status) return status;
            t->readable_segments[t->readable_count++] = i;
        }
    }
    t->published_dictionary_bytes = t->dictionary ? t->dictionary->allocated_bytes : 0;
    if (t->pending_retention) (void)wadb_retention_cleanup(t, NULL, NULL);
    return WADB_OK;
}

wadb_status wadb_readable_reserve(wadb_table *t, wadb_error *e) {
    if (t->readable_count < t->readable_capacity) return WADB_OK;
    size_t cap = t->readable_capacity ? t->readable_capacity * 2 : 16;
    if (cap > WADB_MAX_SEGMENTS) cap = WADB_MAX_SEGMENTS;
    if (cap <= t->readable_count) return wadb_fail(e, WADB_LIMIT, 0, "segment directory limit");
    size_t *p = realloc(t->readable_segments, cap * sizeof(*p));
    if (!p) return wadb_fail(e, WADB_NOMEM, 0, "readable segment directory");
    t->readable_segments = p; t->readable_capacity = cap;
    return WADB_OK;
}

wadb_status wadb_rebuild_index(wadb_table *t, const wadb_segment_meta *segment, wadb_error *e) {
    wadb_frame_index index = {0};
    wadb_dictionary *dictionary = NULL;
    wadb_status status = wadb_rebuild_index_data(t, segment, &index, &dictionary, UINT64_MAX, e);
    if (!status) status = wadb_index_save(t, segment, &index, dictionary, e);
    wadb_index_free(&index); wadb_dictionary_free(dictionary);
    return status;
}
wadb_status wadb_rebuild_index_data(wadb_table *t, const wadb_segment_meta *segment,
    wadb_frame_index *index, wadb_dictionary **dictionary, uint64_t deadline, wadb_error *e) {
    if (segment->state == WADB_SEG_ACTIVE) return wadb_fail(e, WADB_INVALID, 0, "active indexes are memory snapshots");
    wadb_table view = {.db = t->db, .id = t->id, .directory = t->directory, .schema = t->schema};
    wadb_segment_meta copy = *segment;
    view.last_sequence = segment->first_sequence - 1;
    view.last_time = segment->min_time;
    view.active_fd = -1; view.dictionary = NULL;
    memset(&view.index, 0, sizeof(view.index));
    wadb_status status = recover_segment(&view, &copy, true, deadline, e);
    if (!status) {
        view.dictionary->schema = &t->schema;
        *index = view.index; *dictionary = view.dictionary;
    }
    return status;
}

/* The dedicated writer owns provisional dictionary mutations. Readers replay
 * their own committed dictionary view; failed groups roll back all additions. */
wadb_status wadb_commit_requests(wadb_table *t, wadb_append_request **requests,
    size_t request_count, wadb_error *e) {
    size_t row_count = 0, row_bytes = 0;
    for (size_t i = 0; i < request_count; ++i) {
        if (!wadb_add_size(row_count, requests[i]->row_count, &row_count) ||
            !wadb_add_size(row_bytes, requests[i]->row_bytes, &row_bytes))
            return wadb_fail(e, WADB_LIMIT, 0, "append group overflow");
    }
    if (!request_count || row_bytes > WADB_MAX_FRAME_BYTES - 96 || row_count > UINT32_MAX)
        return wadb_fail(e, WADB_LIMIT, 0, "append group exceeds frame size");
    int64_t first_time = requests[0]->ingestion_time;
    int64_t last_time = requests[request_count - 1]->ingestion_time;
    if (first_time < 0 || last_time < first_time || first_time / WADB_DAY_US != last_time / WADB_DAY_US)
        return wadb_fail(e, WADB_IO, 0, "invalid ingestion clock for append group");
    unsigned char *rows = malloc(row_bytes);
    if (!rows) return wadb_fail(e, WADB_NOMEM, 0, "append group row buffer");
    size_t offset = 0;
    for (size_t i = 0; i < request_count; ++i) {
        wadb_append_request *r = requests[i];
        memcpy(rows + offset, r->rows, r->row_bytes);
        for (size_t j = 0; j < r->row_count; ++j)
            wadb_put_u64(rows + offset + j * t->schema.row_width, (uint64_t)r->ingestion_time);
        offset += r->row_bytes;
    }
    pthread_mutex_lock(&t->db->writer_mutex);
    pthread_mutex_lock(&t->db->mutex);
    bool read_only = t->read_only || t->db->read_only;
    pthread_mutex_unlock(&t->db->mutex);
    wadb_status status = WADB_OK;
    if (read_only) { status = wadb_fail(e, WADB_READ_ONLY, 0, "writer requires recovery"); goto done; }
    if (t->last_sequence > UINT64_MAX - row_count) { status = wadb_fail(e, WADB_LIMIT, 0, "row sequences exhausted"); goto done; }
    wadb_segment_meta *active = t->segment_count ? &t->segments[t->segment_count - 1] : NULL;
    bool fresh = !active || active->state != WADB_SEG_ACTIVE ||
        active->day_start != first_time - first_time % WADB_DAY_US || t->index.count >= WADB_MAX_INDEX_ENTRIES;
    for (;;) {
        wadb_dictionary *dictionary = t->dictionary;
        if (fresh) {
            status = wadb_dictionary_new(&t->schema, t->db->options.dictionary_limit_bytes, &dictionary, e);
            if (status) break;
        }
        size_t mark = dictionary->count;
        offset = 0;
        for (size_t i = 0; i < request_count && !status; ++i) {
            wadb_append_request *r = requests[i];
            for (size_t row = 0; row < r->row_count && !status; ++row) {
                for (uint32_t field = 1; field < t->schema.field_count; ++field) {
                    if (t->schema.fields[field].type != WADB_SYMBOL32) continue;
                    const wadb_value *v = &r->values[row * (t->schema.field_count - 1) + field - 1];
                    if (v->is_null) continue;
                    uint32_t id;
                    status = wadb_dictionary_intern(dictionary, field, v->as.bytes.data, v->as.bytes.length, &id, e);
                    if (status) break;
                    wadb_put_u32(rows + offset + row * t->schema.row_width + t->schema.fields[field].offset, id);
                }
            }
            offset += r->row_bytes;
        }
        unsigned char *definitions = NULL, *bytes = NULL;
        uint32_t definition_bytes = 0, definition_count = 0;
        if (!status) status = wadb_dictionary_export(dictionary, mark, &definitions, &definition_bytes, &definition_count, e);
        if (!status && definition_bytes > WADB_MAX_FRAME_BYTES - 96 - row_bytes)
            status = wadb_fail(e, WADB_LIMIT, 0, "rows and symbol additions exceed frame size");
        size_t n = 0;
        wadb_frame frame = {.schema_hash = t->schema.fingerprint, .first_sequence = t->last_sequence + 1,
            .min_time = first_time, .max_time = last_time, .row_count = (uint32_t)row_count,
            .row_width = t->schema.row_width, .rows = rows, .dictionary = definitions,
            .dictionary_bytes = definition_bytes, .dictionary_count = definition_count};
        if (!status) status = wadb_frame_encode(&frame, &bytes, &n, e);
        free(definitions);
        uint64_t target = t->db->options.segment_target_bytes;
        bool too_large = !status && !fresh && active->last_sequence &&
            (active->committed_bytes >= target || n > target - active->committed_bytes);
        if (!fresh && (status == WADB_LIMIT || too_large)) {
            wadb_dictionary_rollback(dictionary, mark);
            free(bytes); fresh = true; status = WADB_OK;
            continue;
        }
        if (!status && fresh) {
            status = create_active(t, first_time, e);
            if (!status) {
                wadb_dictionary_free(t->dictionary);
                t->dictionary = dictionary;
                fresh = false;
            }
        }
        if (!status) {
            active = &t->segments[t->segment_count - 1];
            pthread_mutex_lock(&t->db->mutex);
            status = wadb_index_reserve(&t->index, e);
            if (!status && !active->last_sequence) status = wadb_readable_reserve(t, e);
            pthread_mutex_unlock(&t->db->mutex);
        }
        if (!status) {
            status = wadb_write_all(&t->db->io, t->active_fd, bytes, n, e);
            if (!status) {
                uint64_t started = wadb_monotonic_ns();
                status = wadb_sync_file(&t->db->io, t->active_fd, e);
                uint64_t elapsed = wadb_monotonic_ns() - started;
                pthread_mutex_lock(&t->db->mutex);
                wadb_histogram_record(&t->db->sync_latency, elapsed);
                pthread_mutex_unlock(&t->db->mutex);
            }
            if (status) {
                pthread_mutex_lock(&t->db->mutex);
                t->read_only = true;
                pthread_mutex_unlock(&t->db->mutex);
                status = wadb_fail(e, WADB_INDETERMINATE, e ? e->system_errno : 0,
                    "append durability is uncertain; close and recover before retrying");
            } else {
                pthread_mutex_lock(&t->db->mutex);
                t->index.entries[t->index.count++] = wadb_index_for_frame(&frame, bytes, active->committed_bytes);
                if (!active->last_sequence) {
                    active->min_time = first_time;
                    t->readable_segments[t->readable_count++] = t->segment_count - 1;
                }
                active->max_time = last_time; active->last_sequence = frame.first_sequence + row_count - 1;
                active->committed_bytes += n;
                t->last_sequence = active->last_sequence; t->last_time = last_time;
                ++t->committed_frames; t->append_requests += request_count;
                t->retained_rows += row_count; t->retained_bytes += n;
                t->published_dictionary_bytes = dictionary->allocated_bytes;
                wadb_db *db = t->db;
                ++db->committed_frames; db->append_requests += request_count;
                db->committed_rows += row_count; db->committed_bytes += n;
                if (db->latest_notification < UINT64_MAX) {
                    uint64_t id = ++db->latest_notification;
                    db->notifications[(id - 1) % WADB_NOTIFICATION_CAPACITY] = (wadb_commit_event){
                        .id = id, .table_id = t->id, .first_sequence = frame.first_sequence,
                        .last_sequence = active->last_sequence, .frame_bytes = n, .max_time = last_time, .rows = (uint32_t)row_count};
                }
                pthread_mutex_unlock(&t->db->mutex);
                uint64_t sequence = frame.first_sequence;
                for (size_t i = 0; i < request_count; ++i) {
                    wadb_append_request *r = requests[i];
                    r->receipt = (wadb_append_receipt){.first_sequence = sequence,
                        .last_sequence = sequence + r->row_count - 1,
                        .ingestion_time = r->ingestion_time, .row_count = (uint32_t)r->row_count};
                    sequence += r->row_count;
                }
            }
        }
        if (status) wadb_dictionary_rollback(dictionary, mark);
        if (fresh) wadb_dictionary_free(dictionary);
        free(bytes);
        break;
    }
done:
    pthread_mutex_unlock(&t->db->writer_mutex);
    free(rows);
    return status;
}
