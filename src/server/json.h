#ifndef WADB_SERVER_JSON_H
#define WADB_SERVER_JSON_H
#include "../core/internal.h"
#include "../../vendor/yyjson/yyjson.h"

#define WADB_HTTP_BODY_LIMIT (4u * 1024 * 1024)
#define WADB_HTTP_REPLY_LIMIT (4u * 1024 * 1024)
#define WADB_HTTP_APPEND_REPLY_LIMIT 4096u
#define WADB_HTTP_JSON_LIMIT (32u * 1024 * 1024)
typedef union wadb_json_block wadb_json_block;
typedef struct { size_t used, limit; wadb_json_block *blocks; wadb_status failure; } wadb_json_memory;
typedef struct {
    wadb_json_memory memory;
    yyjson_alc allocator;
    yyjson_doc *input;
    yyjson_mut_doc *output;
    yyjson_mut_val *root;
    size_t reply_limit;
    wadb_error error;
} wadb_json;
typedef struct { unsigned status; char *body; size_t length; } wadb_http_reply;
bool wadb_json_init(wadb_json *json, const char *body, size_t length);
void wadb_json_destroy(wadb_json *json);
bool wadb_json_allowed(wadb_json *json, yyjson_val *object, const char *fields);
const char *wadb_json_name(wadb_json *json, yyjson_val *value, const char *what);
bool wadb_json_u64(wadb_json *json, yyjson_val *value, uint64_t *out);
bool wadb_json_i64(wadb_json *json, yyjson_val *value, int64_t *out);
bool wadb_json_bool(wadb_json *json, yyjson_val *object, const char *key, bool *out);
bool wadb_json_value(wadb_json *json, const wadb_field *field, yyjson_val *value, wadb_value *out);
yyjson_mut_val *wadb_json_encode_value(wadb_json *json, wadb_type type, const wadb_value *value);
yyjson_mut_val *wadb_json_object(wadb_json *json);
yyjson_mut_val *wadb_json_array(wadb_json *json);
void wadb_json_add(wadb_json *json, yyjson_mut_val *object, const char *key, yyjson_mut_val *value);
void wadb_json_push(wadb_json *json, yyjson_mut_val *array, yyjson_mut_val *value);
void wadb_json_string(wadb_json *json, yyjson_mut_val *object, const char *key, const char *value);
void wadb_json_uint(wadb_json *json, yyjson_mut_val *object, const char *key, uint64_t value);
void wadb_json_int(wadb_json *json, yyjson_mut_val *object, const char *key, int64_t value);
void wadb_json_boolean(wadb_json *json, yyjson_mut_val *object, const char *key, bool value);
void wadb_json_finish(wadb_json *json, unsigned status, wadb_http_reply *reply);
void wadb_http_error(unsigned status, wadb_status code, const char *message, wadb_http_reply *reply);
unsigned wadb_http_status(wadb_status status);
void wadb_http_reply_free(wadb_http_reply *reply);
bool wadb_decimal_u64(const char *text, size_t length, uint64_t *out);
#endif
