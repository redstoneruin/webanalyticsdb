/* Standalone benchmark executable. It links the production core archive.
 * Internal access is limited to clocks, cache eviction between cold probes,
 * and the platform sync primitive used by the raw-write reference. */
#include "../src/core/engine.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#endif

#define MAX_PRODUCERS 32
#define MAX_TABLES 32
#define ARRIVAL_CAP 1024
#define HIST_BINS 2048
#define MAX_ROWS UINT64_C(1000000000)
typedef struct { uint64_t bins[HIST_BINS], count, maximum; } histogram;
typedef struct {
    const char *directory;
    uint64_t rows, rate, step, segment, bytes;
    unsigned seconds, width, batch, producers, tables, cardinality, delay, frame, repeats;
    bool burst, queries, synthetic, cold;
} configuration;
typedef struct { uint64_t number, due; unsigned count; } arrival;
typedef struct workload workload;
typedef struct {
    workload *owner;
    uint64_t attempted, committed, rejected, failed, indeterminate;
    histogram latency, service;
} producer;
struct workload {
    configuration config;
    wadb_db *db;
    wadb_table *tables[MAX_TABLES];
    atomic_uint_fast64_t next, clock;
    atomic_uint_fast64_t counts[MAX_TABLES], sums[MAX_TABLES];
    atomic_bool abort, finished;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    bool started, closed;
    unsigned ready;
    uint64_t start, end, offered, dropped;
    arrival arrivals[ARRIVAL_CAP];
    size_t head, count;
    histogram query_latency;
    uint64_t query_count, query_failures;
};
typedef struct { double cpu; uint64_t rss, io_read, io_write; bool has_io; } resources;

static void fail(const char *message) { fprintf(stderr, "benchmark: %s\n", message); exit(1); }
static void check(wadb_status status, const wadb_error *error) {
    if (status) { fprintf(stderr, "benchmark: %s: %s\n", wadb_status_name(status), error ? error->message : ""); exit(1); }
}
static uint64_t number(const char *text) {
    char *end; errno = 0;
    if (!text[0] || text[0] == '-') fail("invalid unsigned argument");
    unsigned long long n = strtoull(text, &end, 10);
    if (errno || *end) fail("invalid unsigned argument");
    return (uint64_t)n;
}
static configuration parse(int argc, char **argv) {
    configuration c = {.rows = MAX_ROWS, .seconds = 5, .width = 48, .batch = 64, .producers = 1,
        .tables = 1, .cardinality = 100, .delay = 5, .frame = 256 * 1024, .segment = 256 * 1024 * 1024,
        .repeats = 3, .bytes = 64 * 1024 * 1024};
    for (int i = 2; i < argc; ++i) {
        const char *key = argv[i];
        if (!strcmp(key, "--burst")) { c.burst = true; continue; }
        if (!strcmp(key, "--queries")) { c.queries = true; continue; }
        if (!strcmp(key, "--cold")) { c.cold = true; continue; }
        if (++i == argc) fail("argument requires value");
        if (!strcmp(key, "--data")) { c.directory = argv[i]; continue; }
        uint64_t n = number(argv[i]);
        if (!strcmp(key, "--rows")) c.rows = n;
        else if (!strcmp(key, "--rate")) c.rate = n;
        else if (!strcmp(key, "--synthetic-clock-step-us")) { c.synthetic = true; c.step = n; }
        else if (!strcmp(key, "--segment-bytes")) c.segment = n;
        else if (!strcmp(key, "--bytes")) c.bytes = n;
        else {
            if (n > UINT32_MAX) fail("argument exceeds uint32");
            if (!strcmp(key, "--seconds")) c.seconds = (unsigned)n;
            else if (!strcmp(key, "--width")) c.width = (unsigned)n;
            else if (!strcmp(key, "--batch")) c.batch = (unsigned)n;
            else if (!strcmp(key, "--producers")) c.producers = (unsigned)n;
            else if (!strcmp(key, "--tables")) c.tables = (unsigned)n;
            else if (!strcmp(key, "--cardinality")) c.cardinality = (unsigned)n;
            else if (!strcmp(key, "--batch-delay-ms")) c.delay = (unsigned)n;
            else if (!strcmp(key, "--frame-bytes")) c.frame = (unsigned)n;
            else if (!strcmp(key, "--repeats")) c.repeats = (unsigned)n;
            else fail("unknown option");
        }
    }
    if (!c.directory || !c.rows || c.rows > MAX_ROWS || c.seconds > 3600 || (!c.seconds && c.rows == MAX_ROWS) ||
        c.width < 32 || c.width > WADB_MAX_ROW_BYTES || c.width % 8 || !c.producers || c.producers > MAX_PRODUCERS ||
        !c.tables || c.tables > MAX_TABLES || !c.cardinality || c.cardinality > 1000000 || c.delay > 1000 ||
        c.frame < 256 || c.frame > WADB_MAX_FRAME_BYTES || c.segment < 256 || c.segment > INT64_MAX ||
        c.rate > 100000000 || c.step > UINT64_C(1000000000) || !c.repeats || c.repeats > 100 ||
        !c.bytes || c.bytes > UINT64_C(1024) * 1024 * 1024 * 1024) fail("invalid benchmark bounds");
    /* /p/ plus ten digits: 13 bytes, a 12-byte definition, padded to 8.
     * Batch zero selects the largest frame legal even in an empty dictionary. */
    unsigned maximum = (WADB_MAX_FRAME_BYTES - 96) / c.width;
    while (maximum && (uint64_t)maximum * c.width + 96 +
        (((uint64_t)(maximum < c.cardinality ? maximum : c.cardinality) * 25 + 7) & ~UINT64_C(7)) > WADB_MAX_FRAME_BYTES) --maximum;
    if (!c.batch) c.batch = maximum;
    if (!c.batch || c.batch > maximum) fail("batch exceeds frame including worst-case dictionary additions");
    return c;
}
static void sleep_until(uint64_t due) {
    for (;;) {
        uint64_t now = wadb_monotonic_ns(); if (now >= due) return;
        uint64_t gap = due - now;
        struct timespec delay = {.tv_sec = (time_t)(gap / 1000000000), .tv_nsec = (long)(gap % 1000000000)};
        if (!nanosleep(&delay, NULL) || errno != EINTR) return;
    }
}
static void record(histogram *h, uint64_t ns) {
    uint64_t us = (ns + 999) / 1000;
    unsigned index;
    if (us < 32) index = (unsigned)us;
    else {
        unsigned e = 0; for (uint64_t n = us; n > 1; n >>= 1) ++e;
        index = 32 + (e - 5) * 32 + (unsigned)((us >> (e - 5)) - 32);
    }
    ++h->bins[index]; ++h->count;
    if (us > h->maximum) h->maximum = us;
}
static uint64_t percentile(const histogram *h, unsigned pct) {
    if (!h->count) return 0;
    uint64_t needed = (h->count * pct + 99) / 100, count = 0;
    for (unsigned i = 0; i < HIST_BINS; ++i) {
        count += h->bins[i];
        if (count < needed) continue;
        if (i < 32) return i;
        unsigned e = (i - 32) / 32 + 5, part = (i - 32) % 32;
        if (e >= 63) return UINT64_MAX;
        return ((uint64_t)(33 + part) << (e - 5)) - 1;
    }
    return h->maximum;
}
static void print_histogram(const histogram *h) {
    printf("{\"samples\":%" PRIu64 ",\"p50_us\":%" PRIu64 ",\"p95_us\":%" PRIu64
        ",\"p99_us\":%" PRIu64 ",\"max_us\":%" PRIu64 "}", h->count,
        percentile(h, 50), percentile(h, 95), percentile(h, 99), h->maximum);
}
static resources resource_snapshot(void) {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage)) fail("getrusage");
    resources r = {.cpu = usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 +
        usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6, .rss = (uint64_t)usage.ru_maxrss};
#ifndef __APPLE__
    r.rss *= 1024;
#else
    struct rusage_info_v4 info;
    if (!proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&info)) {
        r.has_io = true; r.io_read = info.ri_diskio_bytesread; r.io_write = info.ri_diskio_byteswritten;
    }
#endif
#ifdef __linux__
    FILE *file = fopen("/proc/self/io", "r");
    if (file) {
        char line[128]; r.has_io = true;
        while (fgets(line, sizeof(line), file)) {
            uint64_t value;
            if (sscanf(line, "read_bytes: %" SCNu64, &value) == 1) r.io_read = value;
            if (sscanf(line, "write_bytes: %" SCNu64, &value) == 1) r.io_write = value;
        }
        fclose(file);
    }
#endif
    return r;
}
static void print_resources(resources before, resources after) {
    printf("\"cpu_seconds\":%.6f,\"peak_rss_bytes\":%" PRIu64 ",\"kernel_read_bytes\":", after.cpu - before.cpu, after.rss);
    if (after.has_io) printf("%" PRIu64, after.io_read - before.io_read); else printf("null");
    printf(",\"kernel_write_bytes\":");
    if (after.has_io) printf("%" PRIu64, after.io_write - before.io_write); else printf("null");
}
static wadb_options engine_options(configuration c) {
    wadb_options o; wadb_options_default(&o);
    o.segment_target_bytes = c.segment; o.batch_target_bytes = c.frame; o.batch_delay_ms = c.delay;
    return o;
}
static void benchmark_marker(const char *directory, bool create) {
    const char marker[] = "WebAnalyticsDB benchmark fixture v1\n";
    char *path = wadb_path(directory, ".wadb-benchmark"); if (!path) fail("marker path");
    int fd = open(path, (create ? O_WRONLY | O_CREAT | O_EXCL : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW, 0600);
    free(path);
    if (fd < 0) fail("requires a benchmark-owned fixture");
    if (create) { if (write(fd, marker, sizeof(marker)) != sizeof(marker)) fail("write benchmark marker"); }
    else {
        char bytes[sizeof(marker)];
        if (read(fd, bytes, sizeof(bytes)) != sizeof(bytes) || memcmp(marker, bytes, sizeof(bytes))) fail("invalid fixture marker");
    }
    close(fd);
}
static int64_t synthetic_clock(void *context) {
    workload *w = context;
    return (int64_t)atomic_fetch_add(&w->clock, w->config.step);
}
static bool job_for(workload *w, uint64_t number_value, arrival *job) {
    uint64_t first = number_value * w->config.batch;
    if (first >= w->config.rows) return false;
    uint64_t left = w->config.rows - first;
    *job = (arrival){.number = number_value, .count = left < w->config.batch ? (unsigned)left : w->config.batch};
    return true;
}
static void await_start(workload *w) {
    pthread_mutex_lock(&w->mutex); ++w->ready; pthread_cond_broadcast(&w->wake);
    while (!w->started) pthread_cond_wait(&w->wake, &w->mutex);
    pthread_mutex_unlock(&w->mutex);
}
static bool next_job(workload *w, arrival *job) {
    if (atomic_load(&w->abort)) return false;
    if (!w->config.rate) {
        uint64_t now = wadb_monotonic_ns();
        if (now >= w->end || !job_for(w, atomic_fetch_add(&w->next, 1), job)) return false;
        job->due = now; return true;
    }
    pthread_mutex_lock(&w->mutex);
    while (!w->count && !w->closed) pthread_cond_wait(&w->wake, &w->mutex);
    bool found = w->count != 0;
    if (found) { *job = w->arrivals[w->head]; w->head = (w->head + 1) % ARRIVAL_CAP; --w->count; }
    pthread_mutex_unlock(&w->mutex);
    return found;
}
static void *produce(void *context) {
    producer *p = context; workload *w = p->owner; configuration c = w->config;
    size_t fields = c.width > 32 ? 7 : 6;
    wadb_value *values = calloc((size_t)c.batch * fields, sizeof(*values));
    char (*paths)[14] = calloc(c.batch, sizeof(*paths));
    unsigned char *padding = calloc(c.width, 1);
    if (!values || !paths || !padding) fail("allocate producer inputs");
    /* Nonzero deterministic padding avoids a zero-filled raw fixture. */
    for (unsigned i = 0; i < c.width; ++i) padding[i] = (unsigned char)(i * 37 + 11);
    await_start(w);
    arrival job;
    while (next_job(w, &job)) {
        uint64_t first = job.number * c.batch + 1;
        for (unsigned row = 0; row < job.count; ++row) {
            uint64_t id = first + row;
            snprintf(paths[row], 14, "/p/%010" PRIu64, id % c.cardinality);
            wadb_value *v = values + row * fields;
            v[0].as.u64 = id;
            v[1].as.u64 = id % 10 ? 1 : 2 + (id / 10) % 99;
            v[2].as.bytes.data = paths[row]; v[2].as.bytes.length = 13;
            v[3].as.u64 = id % 1000;
            v[4].as.u64 = id % 100 ? 200 : 500;
            v[5].as.bytes.data = "US"; v[5].as.bytes.length = 2;
            if (fields == 7) { v[6].as.bytes.data = padding; v[6].as.bytes.length = c.width - 32; }
        }
        unsigned table = (unsigned)(job.number % c.tables);
        wadb_error error; uint64_t begin = wadb_monotonic_ns();
        wadb_status status = wadb_append_batch(w->tables[table], values, job.count, NULL, &error);
        uint64_t end = wadb_monotonic_ns();
        p->attempted += job.count; record(&p->latency, end - job.due); record(&p->service, end - begin);
        if (!status) {
            p->committed += job.count;
            atomic_fetch_add(&w->counts[table], job.count);
            atomic_fetch_add(&w->sums[table], job.count * (2 * first + job.count - 1) / 2);
        } else if (status == WADB_BACKPRESSURE) p->rejected += job.count;
        else {
            p->failed += job.count;
            if (status == WADB_INDETERMINATE) p->indeterminate += job.count;
            fprintf(stderr, "append: %s: %s\n", wadb_status_name(status), error.message);
            atomic_store(&w->abort, true);
        }
    }
    free(values); free(paths); free(padding);
    return NULL;
}
static void *query_during_load(void *context) {
    workload *w = context; await_start(w);
    wadb_query_options q; wadb_query_options_default(&q);
    wadb_aggregate a = {WADB_COUNT_ALL, NULL, "count"}; q.aggregates = &a; q.aggregate_count = 1;
    q.scan.max_scan_bytes = UINT64_C(16) * 1024 * 1024 * 1024;
    unsigned table = 0;
    while (!atomic_load(&w->finished)) {
        uint64_t start = wadb_monotonic_ns(); wadb_result *r = NULL;
        wadb_status status = wadb_query(w->tables[table++ % w->config.tables], &q, &r, NULL);
        record(&w->query_latency, wadb_monotonic_ns() - start);
        ++w->query_count; if (status) ++w->query_failures;
        wadb_result_free(r); sleep_until(wadb_monotonic_ns() + 100000000);
    }
    return NULL;
}
static void schedule(workload *w) {
    for (uint64_t i = 0; !atomic_load(&w->abort); ++i) {
        arrival job; if (!job_for(w, i, &job)) break;
        uint64_t offset = (i * w->config.batch * UINT64_C(1000000000)) / w->config.rate;
        if (w->config.burst) offset = offset / 100000000 * 100000000;
        job.due = w->start + offset;
        if (job.due >= w->end) break;
        sleep_until(job.due); w->offered += job.count;
        pthread_mutex_lock(&w->mutex);
        if (w->count == ARRIVAL_CAP) w->dropped += job.count;
        else { w->arrivals[(w->head + w->count) % ARRIVAL_CAP] = job; ++w->count; pthread_cond_signal(&w->wake); }
        pthread_mutex_unlock(&w->mutex);
    }
    pthread_mutex_lock(&w->mutex); w->closed = true; pthread_cond_broadcast(&w->wake); pthread_mutex_unlock(&w->mutex);
}
static void ingest(configuration c) {
    if (mkdir(c.directory, 0700)) fail("ingest requires a new, nonexistent directory");
    benchmark_marker(c.directory, true);
    workload *w = calloc(1, sizeof(*w)); if (!w) fail("allocate workload");
    w->config = c;
    atomic_init(&w->abort, false); atomic_init(&w->finished, false); atomic_init(&w->next, 0);
    atomic_init(&w->clock, UINT64_C(1700000000000000));
    if (pthread_mutex_init(&w->mutex, NULL) || pthread_cond_init(&w->wake, NULL)) fail("initialize producer queue");
    wadb_options o = engine_options(c); wadb_error error;
    check(wadb_open(c.directory, &o, &w->db, &error), &error);
    if (c.synthetic) { w->db->realtime = synthetic_clock; w->db->clock_context = w; }
    wadb_field_def fields[] = {{"value", WADB_U64, 0, false}, {"site", WADB_U32, 0, false},
        {"path", WADB_SYMBOL32, 0, false}, {"duration", WADB_U32, 0, false},
        {"status", WADB_U16, 0, false}, {"country", WADB_BYTES, 2, false}, {"padding", WADB_BYTES, c.width - 32, false}};
    for (unsigned i = 0; i < c.tables; ++i) {
        char name[32]; snprintf(name, sizeof(name), "bench_%u", i);
        check(wadb_create_table(w->db, name, fields, c.width > 32 ? 7 : 6, 0, &w->tables[i], &error), &error);
        if (wadb_table_schema(w->tables[i])->row_width != c.width) fail("unexpected compiled row width");
        atomic_init(&w->counts[i], 0); atomic_init(&w->sums[i], 0);
    }
    producer *producers = calloc(c.producers, sizeof(*producers)); if (!producers) fail("allocate producer counters");
    pthread_t threads[MAX_PRODUCERS], query;
    for (unsigned i = 0; i < c.producers; ++i) {
        producers[i].owner = w;
        if (pthread_create(&threads[i], NULL, produce, &producers[i])) fail("start producer");
    }
    if (c.queries && pthread_create(&query, NULL, query_during_load, w)) fail("start query worker");
    pthread_mutex_lock(&w->mutex);
    while (w->ready != c.producers + (unsigned)c.queries) pthread_cond_wait(&w->wake, &w->mutex);
    resources before = resource_snapshot();
    w->start = wadb_monotonic_ns(); w->end = c.seconds ? w->start + (uint64_t)c.seconds * 1000000000 : UINT64_MAX;
    w->started = true; pthread_cond_broadcast(&w->wake); pthread_mutex_unlock(&w->mutex);
    if (c.rate) schedule(w);
    for (unsigned i = 0; i < c.producers; ++i) if (pthread_join(threads[i], NULL)) fail("join producer");
    uint64_t ingestion_end = wadb_monotonic_ns();
    atomic_store(&w->finished, true);
    if (c.queries && pthread_join(query, NULL)) fail("join query worker");
    resources after = resource_snapshot();
    producer total = {0};
    for (unsigned i = 0; i < c.producers; ++i) {
        producer *p = &producers[i];
        total.attempted += p->attempted; total.committed += p->committed; total.rejected += p->rejected;
        total.failed += p->failed; total.indeterminate += p->indeterminate;
        for (unsigned j = 0; j < HIST_BINS; ++j) { total.latency.bins[j] += p->latency.bins[j]; total.service.bins[j] += p->service.bins[j]; }
        total.latency.count += p->latency.count; total.service.count += p->service.count;
        if (p->latency.maximum > total.latency.maximum) total.latency.maximum = p->latency.maximum;
        if (p->service.maximum > total.service.maximum) total.service.maximum = p->service.maximum;
    }
    wadb_stats stats; check(wadb_get_stats(w->db, &stats, &error), &error);
    double elapsed = (ingestion_end - w->start) / 1e9;
    printf("{\"mode\":\"engine\",\"row_width\":%u,\"batch_rows\":%u,\"producers\":%u,\"table_count\":%u,"
        "\"cardinality\":%u,\"batch_delay_ms\":%u,\"frame_target_bytes\":%u,\"segment_target_bytes\":%" PRIu64
        ",\"target_rows_per_second\":%" PRIu64 ",\"burst\":%s,\"synthetic_clock\":%s,\"clock_step_us\":%" PRIu64
        ",\"elapsed_seconds\":%.6f,\"offered_rows\":%" PRIu64 ",\"generator_dropped_rows\":%" PRIu64
        ",\"submitted_rows\":%" PRIu64 ",\"committed_rows\":%" PRIu64 ",\"rejected_rows\":%" PRIu64
        ",\"failed_rows\":%" PRIu64 ",\"indeterminate_rows\":%" PRIu64 ",\"committed_rows_per_second\":%.3f,"
        "\"committed_frame_bytes\":%" PRIu64 ",\"logical_row_bytes\":%" PRIu64 ",\"frames\":%" PRIu64 ",",
        c.width, c.batch, c.producers, c.tables, c.cardinality, c.delay, c.frame, c.segment, c.rate,
        c.burst ? "true" : "false", c.synthetic ? "true" : "false", c.step, elapsed,
        c.rate ? w->offered : total.attempted, w->dropped, total.attempted, total.committed, total.rejected,
        total.failed, total.indeterminate, total.committed / elapsed, stats.committed_bytes, total.committed * c.width, stats.committed_frames);
    print_resources(before, after);
    printf(",\"arrival_to_ack_latency\":"); print_histogram(&total.latency);
    printf(",\"api_call_latency\":"); print_histogram(&total.service);
    printf(",\"concurrent_queries\":%" PRIu64 ",\"query_failures\":%" PRIu64 ",\"query_latency\":", w->query_count, w->query_failures);
    print_histogram(&w->query_latency);
    printf(",\"tables\":[");
    for (unsigned i = 0; i < c.tables; ++i) {
        wadb_table_stats ts; check(wadb_get_table_stats(w->tables[i], &ts, &error), &error);
        if (i) printf(",");
        printf("{\"id\":%" PRIu64 ",\"rows\":%" PRIu64 ",\"sum_value\":%" PRIuFAST64
            ",\"source_bytes\":%" PRIu64 ",\"segments\":%" PRIu64 ",\"dictionary_memory_bytes\":%" PRIu64 "}",
            ts.id, ts.rows, atomic_load(&w->sums[i]), ts.bytes, ts.segments, ts.dictionary_bytes);
        if (ts.rows != atomic_load(&w->counts[i])) fail("receipt counts disagree with retained rows");
    }
    printf("]}\n");
    bool failed = atomic_load(&w->abort);
    wadb_close(w->db); free(producers); pthread_cond_destroy(&w->wake); pthread_mutex_destroy(&w->mutex); free(w);
    if (failed) exit(1);
}

typedef struct { uint64_t segment_bytes, index_bytes, metadata_bytes, dictionary_bytes, framing_bytes,
    payload_bytes, resident_before, resident_after; } disk_stats;
#ifdef __linux__
static uint64_t resident_pages(int fd, size_t bytes) {
    if (!bytes) return 0;
    long page = sysconf(_SC_PAGESIZE); if (page <= 0) fail("page size");
    size_t pages = (bytes + (size_t)page - 1) / (size_t)page;
    unsigned char *vector = malloc(pages); if (!vector) fail("mincore vector");
    void *mapping = mmap(NULL, bytes, PROT_NONE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED || mincore(mapping, bytes, vector)) fail("inspect OS page residency");
    uint64_t count = 0; for (size_t i = 0; i < pages; ++i) if (vector[i] & 1) ++count;
    munmap(mapping, bytes); free(vector); return count * (uint64_t)page;
}
#endif
static void inspect_disk(const char *path, bool evict, disk_stats *stats) {
    struct stat st; if (lstat(path, &st)) fail("stat benchmark fixture");
    if (S_ISDIR(st.st_mode)) {
        DIR *directory = opendir(path); if (!directory) fail("open fixture directory");
        struct dirent *entry;
        while ((entry = readdir(directory))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            char *child = wadb_path(path, entry->d_name); if (!child) fail("fixture path");
            inspect_disk(child, evict, stats); free(child);
        }
        closedir(directory); return;
    }
    if (!S_ISREG(st.st_mode)) fail("unexpected nonregular fixture file");
    const char *ext = strrchr(path, '.');
    bool segment = ext && !strcmp(ext, ".seg"), index = ext && !strcmp(ext, ".idx");
    if (segment) stats->segment_bytes += (uint64_t)st.st_size;
    else if (index) stats->index_bytes += (uint64_t)st.st_size;
    else stats->metadata_bytes += (uint64_t)st.st_size;
    if (segment && !evict) {
        int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW); if (fd < 0) fail("open frame accounting file");
        uint64_t offset = WADB_SEGMENT_HEADER_BYTES; stats->framing_bytes += WADB_SEGMENT_HEADER_BYTES;
        while (offset < (uint64_t)st.st_size) {
            unsigned char header[WADB_FRAME_HEADER_BYTES]; wadb_frame frame; wadb_error error;
            check(wadb_pread_all(fd, header, sizeof(header), offset, &error), &error);
            check(wadb_frame_inspect(header, sizeof(header), &frame, &error), &error);
            if (frame.frame_bytes > (uint64_t)st.st_size - offset) fail("incomplete benchmark frame");
            uint64_t payload = (uint64_t)frame.row_count * frame.row_width;
            stats->payload_bytes += payload; stats->dictionary_bytes += frame.dictionary_bytes;
            stats->framing_bytes += frame.frame_bytes - frame.dictionary_bytes - payload;
            offset += frame.frame_bytes;
        }
        close(fd);
    }
#ifdef __linux__
    if (evict && (segment || index)) {
        int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW); if (fd < 0) fail("open cache-eviction file");
        stats->resident_before += resident_pages(fd, (size_t)st.st_size);
        if (posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED)) fail("evict fixture cache pages");
        stats->resident_after += resident_pages(fd, (size_t)st.st_size); close(fd);
    }
#else
    if (evict) fail("cold probe requires Linux posix_fadvise and mincore");
#endif
}
static void probe(configuration c) {
    benchmark_marker(c.directory, false);
    wadb_db *db; wadb_error error; uint64_t begin = wadb_monotonic_ns();
    check(wadb_open(c.directory, NULL, &db, &error), &error);
    uint64_t restart = wadb_monotonic_ns() - begin;
    wadb_table *tables[MAX_TABLES]; size_t count;
    check(wadb_list_tables(db, tables, MAX_TABLES, &count, &error), &error);
    disk_stats disk = {0}; inspect_disk(c.directory, false, &disk);
    printf("{\"restart_us\":%" PRIu64 ",\"segment_bytes\":%" PRIu64 ",\"index_bytes\":%" PRIu64
        ",\"metadata_bytes\":%" PRIu64 ",\"dictionary_definition_bytes\":%" PRIu64 ",\"framing_bytes\":%" PRIu64
        ",\"row_payload_bytes\":%" PRIu64 ",\"cold_requested\":%s,\"queries\":[", restart / 1000,
        disk.segment_bytes, disk.index_bytes, disk.metadata_bytes, disk.dictionary_bytes, disk.framing_bytes,
        disk.payload_bytes, c.cold ? "true" : "false");
    bool comma = false;
    const char *names[] = {"tail", "minute", "hour", "day", "count_by_minute", "top_paths_site_1", "broad"};
    for (unsigned repeat = 0; repeat < c.repeats; ++repeat) for (size_t table = 0; table < count; ++table) {
        wadb_table_stats ts; check(wadb_get_table_stats(tables[table], &ts, &error), &error);
        for (unsigned kind = 0; kind < sizeof(names) / sizeof(*names); ++kind) {
            disk_stats cold = {0};
            if (c.cold) { wadb_cache_clear(db); inspect_disk(c.directory, true, &cold); }
            wadb_query_options q; wadb_query_options_default(&q);
            if (disk.segment_bytes > UINT64_MAX / 4 || disk.index_bytes > UINT64_MAX / 4) fail("fixture scan budget overflow");
            q.scan.max_scan_bytes = disk.segment_bytes * 3 + disk.index_bytes + 64 * 1024 * 1024;
            q.scan.timeout_ms = 300000; q.scan.end_time = ts.max_time + 1;
            wadb_aggregate aggs[] = {{WADB_COUNT_ALL, NULL, "count"}, {WADB_SUM, "value", "sum_value"}};
            const char *projection[] = {"value", "site", "path"}, *group[] = {"path"};
            wadb_filter site = {.field = "site", .op = WADB_EQ, .value = {.as.u64 = 1}};
            if (!kind) { q.projection = projection; q.projection_count = 3; q.scan.limit = 100; q.scan.after_sequence = ts.last_sequence > 100 ? ts.last_sequence - 100 : 0; }
            else {
                q.aggregates = aggs; q.aggregate_count = 2;
                if (kind <= 3) {
                    int64_t span = kind == 1 ? INT64_C(60000000) : kind == 2 ? INT64_C(3600000000) : INT64_C(86400000000);
                    q.scan.start_time = q.scan.end_time > span ? q.scan.end_time - span : 0;
                }
                if (kind == 4) { q.bucket_width_us = 60000000; q.scan.start_time = ts.min_time; }
                if (kind == 5) {
                    q.group_by = group; q.group_count = 1; q.filters = &site; q.filter_count = 1;
                    q.scan.limit = 10; q.order_by = "count"; q.descending = true;
                }
            }
            wadb_result *result = NULL; resources before = resource_snapshot(); begin = wadb_monotonic_ns();
            wadb_status status = wadb_query(tables[table], &q, &result, &error);
            uint64_t elapsed = wadb_monotonic_ns() - begin; resources after = resource_snapshot();
            if (comma) printf(",");
            comma = true;
            printf("{\"name\":\"%s\",\"table_id\":%" PRIu64 ",\"repeat\":%u,\"status\":\"%s\",\"elapsed_us\":%" PRIu64
                ",\"os_resident_before_eviction_bytes\":%" PRIu64 ",\"os_resident_after_eviction_bytes\":%" PRIu64 ",",
                names[kind], ts.id, repeat, wadb_status_name(status), elapsed / 1000, cold.resident_before, cold.resident_after);
            print_resources(before, after);
            if (result) {
                printf(",\"output_rows\":%zu,\"matched_rows\":%" PRIu64 ",\"scan_bytes\":%" PRIu64
                    ",\"index_bytes\":%" PRIu64 ",\"dictionary_bytes\":%" PRIu64 ",\"frames\":%" PRIu64,
                    result->row_count, result->matched_rows, result->stats.bytes_scanned, result->stats.index_bytes,
                    result->stats.dictionary_bytes, result->stats.frames_scanned);
                if (kind == 6 && result->row_count == 1) {
                    uint64_t actual = result->values[0].as.u64;
                    if (actual != ts.rows) fail("broad count disagrees with manifest totals");
                    printf(",\"count\":%" PRIu64 ",\"sum_value\":%" PRIu64, actual,
                        result->values[1].is_null ? 0 : result->values[1].as.u64);
                }
            }
            printf("}"); wadb_result_free(result);
            if (status && status != WADB_LIMIT) check(status, &error);
        }
    }
    printf("]}\n"); wadb_close(db);
}
static void retention(configuration c) {
    benchmark_marker(c.directory, false);
    wadb_db *db; wadb_error error;
    check(wadb_open(c.directory, NULL, &db, &error), &error);
    wadb_table *tables[MAX_TABLES]; size_t count;
    check(wadb_list_tables(db, tables, MAX_TABLES, &count, &error), &error);
    if (!count) fail("retention requires populated benchmark tables");
    wadb_table_stats before[MAX_TABLES]; wadb_result *samples[MAX_TABLES];
    uint64_t cursors[MAX_TABLES], apply_us[MAX_TABLES], release_us[MAX_TABLES], preview_us[MAX_TABLES];
    wadb_retention_stats previews[MAX_TABLES];
    disk_stats original = {0}; inspect_disk(c.directory, false, &original);
    resources resources_before = resource_snapshot();
    for (size_t i = 0; i < count; ++i) {
        check(wadb_get_table_stats(tables[i], &before[i], &error), &error);
        if (before[i].rows < 2 || before[i].max_time > INT64_MAX - 2) fail("retention fixture bounds");
        check(wadb_set_retention(tables[i], 1, &error), &error);
        wadb_query_options q; wadb_query_options_default(&q); q.scan.limit = 1;
        check(wadb_cursor_open(tables[i], &q, 300000, &cursors[i], &error), &error);
        uint64_t begin = wadb_monotonic_ns();
        check(wadb_retention_preview(tables[i], before[i].max_time + 2, &previews[i], &error), &error);
        preview_us[i] = (wadb_monotonic_ns() - begin) / 1000;
        if (previews[i].eligible_rows != before[i].rows || previews[i].eligible_segments != before[i].segments)
            fail("retention preview disagrees with source totals");
        wadb_retention_stats applied; begin = wadb_monotonic_ns();
        check(wadb_retention_apply(tables[i], before[i].max_time + 2, &applied, &error), &error);
        apply_us[i] = (wadb_monotonic_ns() - begin) / 1000;
        if (applied.deleted_segments || applied.pending_segments != before[i].segments)
            fail("retention did not preserve pinned files");
        wadb_result *empty = NULL;
        check(wadb_query(tables[i], NULL, &empty, &error), &error);
        if (empty->row_count) fail("new reader saw retired rows");
        wadb_result_free(empty);
        check(wadb_cursor_next(db, cursors[i], &samples[i], &error), &error);
        if (samples[i]->row_count != 1 || !samples[i]->has_more) fail("pinned reader lost its snapshot");
    }
    disk_stats pinned = {0}; inspect_disk(c.directory, false, &pinned);
    if (pinned.segment_bytes != original.segment_bytes) fail("pinned source files were deleted");
    for (size_t i = 0; i < count; ++i) {
        uint64_t begin = wadb_monotonic_ns();
        check(wadb_cursor_close(db, cursors[i], &error), &error);
        release_us[i] = (wadb_monotonic_ns() - begin) / 1000;
    }
    disk_stats deleted = {0}; inspect_disk(c.directory, false, &deleted);
    if (deleted.segment_bytes || deleted.index_bytes) fail("retired files survived release of last reader");
    resources resources_after = resource_snapshot();
    wadb_close(db); uint64_t begin = wadb_monotonic_ns();
    check(wadb_open(c.directory, NULL, &db, &error), &error);
    uint64_t restart_us = (wadb_monotonic_ns() - begin) / 1000;
    check(wadb_list_tables(db, tables, MAX_TABLES, &count, &error), &error);
    printf("{\"source_bytes_before\":%" PRIu64 ",\"source_bytes_pinned\":%" PRIu64
        ",\"source_bytes_after_release\":%" PRIu64 ",\"restart_us\":%" PRIu64 ",",
        original.segment_bytes, pinned.segment_bytes, deleted.segment_bytes, restart_us);
    print_resources(resources_before, resources_after); printf(",\"tables\":[");
    for (size_t i = 0; i < count; ++i) {
        wadb_table_stats ts; check(wadb_get_table_stats(tables[i], &ts, &error), &error);
        if (ts.id != before[i].id || ts.rows || ts.pending_retention || ts.last_sequence != before[i].last_sequence ||
            ts.last_assigned_time != before[i].last_assigned_time) fail("retention restart watermark mismatch");
        wadb_append_receipt receipt;
        check(wadb_append_batch(tables[i], samples[i]->values + 1, 1, &receipt, &error), &error);
        if (receipt.first_sequence != before[i].last_sequence + 1 || receipt.ingestion_time < before[i].last_assigned_time)
            fail("post-retention append lost ordering");
        wadb_result_free(samples[i]);
        printf("%s{\"id\":%" PRIu64 ",\"retired_rows\":%" PRIu64 ",\"retired_segments\":%" PRIu64
            ",\"preview_us\":%" PRIu64 ",\"apply_pinned_us\":%" PRIu64 ",\"release_delete_us\":%" PRIu64
            ",\"next_sequence\":%" PRIu64 "}", i ? "," : "", ts.id, before[i].rows, before[i].segments,
            preview_us[i], apply_us[i], release_us[i], receipt.first_sequence);
    }
    wadb_close(db); check(wadb_open(c.directory, NULL, &db, &error), &error);
    check(wadb_list_tables(db, tables, MAX_TABLES, &count, &error), &error);
    for (size_t i = 0; i < count; ++i) {
        wadb_table_stats ts; check(wadb_get_table_stats(tables[i], &ts, &error), &error);
        if (ts.rows != 1 || ts.last_sequence != before[i].last_sequence + 1) fail("new row not durable after retention");
    }
    wadb_close(db); printf("],\"verified_after_restart\":true}\n");
}
static void sequential(configuration c) {
    if (mkdir(c.directory, 0700)) fail("sequential reference requires new directory");
    benchmark_marker(c.directory, true);
    char *path = wadb_path(c.directory, "sequential.bin"); if (!path) fail("raw path");
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600); free(path);
    if (fd < 0) fail("open raw reference");
    unsigned char *bytes = malloc(c.frame); if (!bytes) fail("raw buffer");
    uint32_t seed = 1; for (unsigned i = 0; i < c.frame; ++i) { seed = seed * 1664525 + 1013904223; bytes[i] = (unsigned char)(seed >> 24); }
    resources before = resource_snapshot(); uint64_t begin = wadb_monotonic_ns();
    wadb_error error; histogram latency = {0};
    for (uint64_t written = 0; written < c.bytes;) {
        size_t n = c.bytes - written < c.frame ? (size_t)(c.bytes - written) : c.frame;
        uint64_t start = wadb_monotonic_ns();
        check(wadb_write_all(NULL, fd, bytes, n, &error), &error);
        check(wadb_sync_file(NULL, fd, &error), &error); record(&latency, wadb_monotonic_ns() - start);
        written += n;
    }
    check(wadb_sync_directory(c.directory, &error), &error);
    double elapsed = (wadb_monotonic_ns() - begin) / 1e9;
    printf("{\"mode\":\"sequential_durable_reference\",\"bytes\":%" PRIu64 ",\"block_bytes\":%u,\"elapsed_seconds\":%.6f,"
        "\"bytes_per_second\":%.3f,", c.bytes, c.frame, elapsed, c.bytes / elapsed);
    print_resources(before, resource_snapshot()); printf(",\"sync_block_latency\":"); print_histogram(&latency); printf("}\n");
    close(fd); free(bytes);
}
int main(int argc, char **argv) {
    if (argc < 2) fail("usage: bench-core ingest|probe|retention|sequential --data DIRECTORY [options]");
    configuration c = parse(argc, argv);
    if (!strcmp(argv[1], "ingest")) ingest(c);
    else if (!strcmp(argv[1], "probe")) probe(c);
    else if (!strcmp(argv[1], "retention")) retention(c);
    else if (!strcmp(argv[1], "sequential")) sequential(c);
    else fail("unknown benchmark command");
    return 0;
}
