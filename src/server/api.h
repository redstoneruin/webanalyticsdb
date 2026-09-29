#ifndef WADB_SERVER_API_H
#define WADB_SERVER_API_H
#include "json.h"
#include "jobs.h"
typedef struct {
    wadb_db *db;
    wadb_jobs *jobs;
    const char *boot_id;
    uint64_t (*feed_cursor)(void *context);
    void *feed_context;
} wadb_api_context;
/* The dispatcher enforces this bound; reserve it before dispatching mutations. */
size_t wadb_api_reply_limit(const char *method, const char *path);
void wadb_api_dispatch(wadb_api_context *context, const char *method, const char *path,
    const char *body, size_t length, wadb_http_reply *reply);
void wadb_api_stats(wadb_api_context *context, wadb_json *json);
void wadb_api_result(wadb_json *json, yyjson_mut_val *object, const wadb_result *result);
#endif
