#include "../core/engine.h"
#include <stdlib.h>

static bool matches(const wadb_cache_entry *p, const wadb_index_reader *r) {
    return p->table_id == r->table->id && p->segment_id == r->segment.id &&
        p->covered_bytes == r->segment.committed_bytes && p->dictionary_crc == r->dictionary_crc;
}
static void destroy(wadb_cache_entry *p) { wadb_dictionary_free(p->dictionary); free(p); }
void wadb_cache_clear(wadb_db *db) {
    while (db->cache) { wadb_cache_entry *p = db->cache; db->cache = p->next; destroy(p); }
    db->cache_bytes = 0;
}
wadb_status wadb_cache_acquire(wadb_index_reader *r, wadb_cache_entry **out, wadb_error *e) {
    wadb_db *db = r->table->db;
    *out = NULL;
    pthread_mutex_lock(&db->mutex);
    for (wadb_cache_entry *p = db->cache; p; p = p->next) if (matches(p, r)) {
        ++p->references; p->used = ++db->cache_clock; ++db->cache_hits; *out = p;
        pthread_mutex_unlock(&db->mutex); return WADB_OK;
    }
    ++db->cache_misses;
    size_t reservation = db->options.dictionary_limit_bytes + sizeof(wadb_cache_entry);
    while (reservation > db->options.read_cache_bytes - db->cache_bytes) {
        wadb_cache_entry **oldest = NULL;
        for (wadb_cache_entry **p = &db->cache; *p; p = &(*p)->next)
            if (!(*p)->references && (!oldest || (*p)->used < (*oldest)->used)) oldest = p;
        if (!oldest) {
            pthread_mutex_unlock(&db->mutex);
            return wadb_fail(e, WADB_BACKPRESSURE, 0, "dictionary cache is pinned by other readers");
        }
        wadb_cache_entry *p = *oldest;
        *oldest = p->next; db->cache_bytes -= p->charge; destroy(p);
    }
    db->cache_bytes += reservation;
    pthread_mutex_unlock(&db->mutex);
    wadb_cache_entry *entry = calloc(1, sizeof(*entry));
    wadb_status status = entry ? wadb_index_dictionary(r, &entry->dictionary, e) :
        wadb_fail(e, WADB_NOMEM, 0, "dictionary cache entry");
    pthread_mutex_lock(&db->mutex);
    db->cache_bytes -= reservation;
    if (!status) {
        entry->table_id = r->table->id; entry->segment_id = r->segment.id;
        entry->covered_bytes = r->segment.committed_bytes; entry->dictionary_crc = r->dictionary_crc;
        entry->charge = sizeof(*entry) + entry->dictionary->allocated_bytes;
        entry->references = 1; entry->used = ++db->cache_clock;
        /* Two misses may load concurrently. Keep only one immutable copy. */
        for (wadb_cache_entry *p = db->cache; p; p = p->next) if (matches(p, r)) {
            ++p->references; p->used = entry->used; *out = p; break;
        }
        if (!*out) {
            entry->next = db->cache; db->cache = entry; db->cache_bytes += entry->charge;
            *out = entry; entry = NULL;
        }
    }
    pthread_mutex_unlock(&db->mutex);
    if (entry) destroy(entry);
    return status;
}
void wadb_cache_release(wadb_db *db, wadb_cache_entry *entry) {
    if (!entry) return;
    pthread_mutex_lock(&db->mutex);
    --entry->references;
    pthread_mutex_unlock(&db->mutex);
}
