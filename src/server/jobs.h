#ifndef WADB_SERVER_JOBS_H
#define WADB_SERVER_JOBS_H
#include "wadb.h"
#define WADB_JOB_HISTORY 32u
#define WADB_JOB_CONCURRENCY 2u
typedef struct wadb_jobs wadb_jobs;
typedef enum { WADB_JOB_GENERATOR, WADB_JOB_INTEGRITY } wadb_job_kind;
typedef enum { WADB_JOB_STARTING, WADB_JOB_RUNNING, WADB_JOB_COMPLETE, WADB_JOB_STOPPED, WADB_JOB_FAILED } wadb_job_state;
typedef struct {
    char table_name[WADB_NAME_CAP];
    uint32_t rows_per_second, duration_ms, batch_rows, cardinality;
    uint64_t seed;
} wadb_generator_options;
typedef struct {
    uint64_t id, table_id, committed_rows, attempted_rows, rejected_batches, last_sequence;
    uint64_t started_ns, finished_ns, elapsed_ms, cpu_ns;
    wadb_job_kind kind;
    wadb_job_state state;
    wadb_generator_options generator;
    wadb_integrity_stats integrity;
    wadb_error error;
} wadb_job_info;
wadb_status wadb_jobs_open(wadb_db *db, wadb_jobs **jobs, wadb_error *error);
void wadb_jobs_close(wadb_jobs *jobs);
wadb_status wadb_job_generate(wadb_jobs *jobs, const wadb_generator_options *options, uint64_t *id, wadb_error *error);
wadb_status wadb_job_integrity(wadb_jobs *jobs, wadb_table *table, uint64_t bytes, uint32_t timeout_ms, uint64_t *id, wadb_error *error);
wadb_status wadb_job_get(wadb_jobs *jobs, uint64_t id, wadb_job_info *info, wadb_error *error);
wadb_status wadb_job_list(wadb_jobs *jobs, wadb_job_info *infos, size_t capacity, size_t *count, wadb_error *error);
wadb_status wadb_job_stop(wadb_jobs *jobs, uint64_t id, wadb_error *error);
#endif
