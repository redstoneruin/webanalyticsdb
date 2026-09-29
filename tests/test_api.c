#include "database_fixture.h"
#include "../src/server/api.h"
static wadb_api_context context;
static yyjson_doc *request(const char *method, const char *path, const char *body, unsigned expected) {
    wadb_http_reply reply;
    wadb_api_dispatch(&context, method, path, body, body ? strlen(body) : 0, &reply);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(expected, reply.status, reply.body);
    yyjson_doc *doc = yyjson_read(reply.body, reply.length, 0);
    TEST_ASSERT_NOT_NULL(doc); wadb_http_reply_free(&reply); return doc;
}
static void send(const char *method, const char *path, const char *body, unsigned status) {
    yyjson_doc_free(request(method, path, body, status));
}
static void open_api(void) {
    wadb_options o; wadb_options_default(&o); o.batch_delay_ms = 0;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, &o, &db, NULL));
    context = (wadb_api_context){.db = db, .boot_id = "0123456789abcdef"};
}
static void test_api_fixed_schema_exact_values_and_atomic_rejection(void) {
    open_api();
    send("POST", "/api/v1/tables", "{\"name\":\"views\",\"fields\":[{\"name\":\"id\",\"type\":\"u64\"},{\"name\":\"path\",\"type\":\"symbol32\"},{\"name\":\"tag\",\"type\":\"text\",\"size\":12,\"nullable\":true},{\"name\":\"raw\",\"type\":\"bytes\",\"size\":2}]}", 201);
    send("POST", "/api/v1/tables/1/append", "{\"rows\":[{\"id\":\"18446744073709551615\",\"path\":\"a\\u0000b\",\"raw\":\"00ff\"},[\"9007199254740992\",\"a\\u0000b\",\"z\",\"1234\"]]}", 201);
    send("POST", "/api/v1/tables/1/append", "{\"rows\":[[1,\"ok\",null,\"0000\"],[9007199254740992,\"bad\",null,\"0000\"]]}", 400);
    send("POST", "/api/v1/tables/1/append", "{\"rows\":[{\"_time\":1,\"id\":1,\"path\":\"x\",\"raw\":\"0000\"}]}", 400);
    yyjson_doc *doc = request("POST", "/api/v1/tables/1/query", "{\"projection\":[\"id\",\"path\",\"tag\",\"raw\"]}", 200);
    yyjson_val *rows = yyjson_obj_get(yyjson_doc_get_root(doc), "rows");
    TEST_ASSERT_EQUAL_UINT64(2, yyjson_arr_size(rows));
    yyjson_val *row = yyjson_arr_get(rows, 0);
    TEST_ASSERT_EQUAL_STRING("18446744073709551615", yyjson_get_str(yyjson_arr_get(row, 0)));
    TEST_ASSERT_EQUAL_UINT64(3, yyjson_get_len(yyjson_arr_get(row, 1)));
    TEST_ASSERT_EQUAL_MEMORY("a\0b", yyjson_get_str(yyjson_arr_get(row, 1)), 3);
    TEST_ASSERT_TRUE(yyjson_is_null(yyjson_arr_get(row, 2)));
    TEST_ASSERT_EQUAL_STRING("00ff", yyjson_get_str(yyjson_arr_get(row, 3)));
    yyjson_doc_free(doc);
}
static void test_api_rejects_ambiguous_and_malformed_requests(void) {
    open_api();
    send("GET", "/api/v1/stats", "{\"x\":1,\"x\":2}", 400);
    send("GET", "/api/v1/stats", "{\"x\\u0000y\":1}", 400);
    send("GET", "/api/v1/stats", "[]", 400);
    send("GET", "/api/v1/stats", "{} trailing", 400);
    send("POST", "/api/v1/tables", "{\"name\":\"broken\",\"fields\":[{\"name\":\"x\"}]}", 400);
    send("POST", "/api/v1/tables", "{\"name\":\"broken\",\"fields\":[{\"name\":\"x\",\"type\":\"u8\",\"nullable\":1}]}", 400);
    send("GET", "/api/v1/tables/18446744073709551616", NULL, 404);
    send("DELETE", "/api/v1/tables", NULL, 405);
    send("POST", "/api/v1/tables", "{\"name\":\"empty\",\"fields\":[]}", 201);
    send("POST", "/api/v1/tables/1/append", "{\"rows\":[{},[]]}", 201);
    send("POST", "/api/v1/tables/1/query", "{\"memory_limit_bytes\":\"18446744073709551615\"}", 422);
    wadb_stats s; TEST_ASSERT_EQUAL(WADB_OK, wadb_get_stats(db, &s, NULL)); TEST_ASSERT_EQUAL_UINT64(2, s.committed_rows);
}
static void new_numbers(void) {
    open_api();
    send("POST", "/api/v1/tables", "{\"name\":\"numbers\",\"fields\":[{\"name\":\"n\",\"type\":\"i64\"},{\"name\":\"group\",\"type\":\"symbol32\"}]}", 201);
    send("POST", "/api/v1/tables/1/append", "{\"rows\":[[1,\"a\"],[2,\"b\"],[3,\"a\"]]}", 201);
}
static void test_api_grouped_query_and_tail(void) {
    new_numbers();
    yyjson_doc *doc = request("POST", "/api/v1/tables/1/query", "{\"filters\":[{\"field\":\"n\",\"op\":\"ge\",\"value\":2}],\"group_by\":[\"group\"],\"aggregates\":[{\"op\":\"sum\",\"field\":\"n\",\"name\":\"total\"}],\"order_by\":\"total\",\"descending\":true}", 200);
    yyjson_val *row = yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(doc), "rows"), 0);
    TEST_ASSERT_EQUAL_STRING("a", yyjson_get_str(yyjson_arr_get(row, 0)));
    TEST_ASSERT_EQUAL_STRING("3", yyjson_get_str(yyjson_arr_get(row, 1))); yyjson_doc_free(doc);
    doc = request("GET", "/api/v1/tables/1/tail", "{\"limit\":1}", 200);
    row = yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(doc), "rows"), 0);
    TEST_ASSERT_EQUAL_STRING("3", yyjson_get_str(yyjson_arr_get(row, 1))); yyjson_doc_free(doc);
    send("POST", "/api/v1/tables/1/query", "{\"filters\":[{\"op\":\"eq\",\"value\":1}]}", 400);
}
static void test_api_cursors_require_boot_and_preserve_snapshot(void) {
    new_numbers();
    yyjson_doc *doc = request("POST", "/api/v1/tables/1/query", "{\"cursor\":true,\"limit\":2}", 200);
    const char *id = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "cursor_id")); TEST_ASSERT_NOT_NULL(id);
    char path[128]; snprintf(path, sizeof(path), "/api/v1/cursors/%s/next", id); yyjson_doc_free(doc);
    send("POST", path, "{\"boot_id\":\"old\"}", 410);
    send("POST", "/api/v1/tables/1/append", "{\"rows\":[[4,\"a\"]]}", 201);
    doc = request("POST", path, "{\"boot_id\":\"0123456789abcdef\"}", 200);
    TEST_ASSERT_EQUAL_UINT64(1, yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(doc), "rows")));
    TEST_ASSERT_FALSE(yyjson_get_bool(yyjson_obj_get(yyjson_doc_get_root(doc), "has_more")));
    yyjson_doc_free(doc); send("POST", path, "{\"boot_id\":\"0123456789abcdef\"}", 410);
    wadb_stats s; wadb_get_stats(db, &s, NULL); TEST_ASSERT_EQUAL_UINT64(0, s.readers);
}
static void test_api_retention_and_segment_metadata(void) {
    new_numbers();
    send("POST", "/api/v1/tables/1/retention", "{\"retention_us\":1}", 200);
    send("POST", "/api/v1/tables/1/retention-preview", "{\"now_us\":\"9223372036854775807\"}", 200);
    yyjson_doc *doc = request("GET", "/api/v1/tables/1/segments", NULL, 200);
    TEST_ASSERT_EQUAL_UINT64(1, yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(doc), "segments"))); yyjson_doc_free(doc);
    send("POST", "/api/v1/tables/1/retention", "{\"now_us\":\"9223372036854775807\",\"apply\":true}", 200);
    doc = request("GET", "/api/v1/tables/1", NULL, 200);
    TEST_ASSERT_EQUAL_STRING("0", yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "rows")));
    TEST_ASSERT_EQUAL_STRING("3", yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "last_sequence"))); yyjson_doc_free(doc);
}
static void test_append_reply_bound_covers_receipts_and_escaped_errors(void) {
    open_api();
    send("POST", "/api/v1/tables", "{\"name\":\"receipts\",\"fields\":[]}", 201);
    const char *path = "/api/v1/tables/1/append";
    TEST_ASSERT_EQUAL_UINT64(4096, wadb_api_reply_limit("POST", path));
    TEST_ASSERT_EQUAL_UINT64(WADB_HTTP_REPLY_LIMIT, wadb_api_reply_limit("GET", path));
    TEST_ASSERT_EQUAL_UINT64(WADB_HTTP_REPLY_LIMIT, wadb_api_reply_limit("POST", "/api/v1/tables/1/query"));
    TEST_ASSERT_EQUAL_UINT64(WADB_HTTP_REPLY_LIMIT, wadb_api_reply_limit("POST", "/api/v1/tables/1/append/extra"));
    wadb_http_reply reply;
    const char *body = "{\"rows\":[[],[],[]]}";
    wadb_api_dispatch(&context, "POST", path, body, strlen(body), &reply);
    TEST_ASSERT_EQUAL_UINT(201, reply.status); TEST_ASSERT_TRUE(reply.length <= WADB_HTTP_APPEND_REPLY_LIMIT);
    wadb_http_reply_free(&reply);
    wadb_api_dispatch(&context, "POST", path, "{", 1, &reply);
    TEST_ASSERT_EQUAL_UINT(400, reply.status); TEST_ASSERT_TRUE(reply.length <= WADB_HTTP_APPEND_REPLY_LIMIT);
    wadb_http_reply_free(&reply);
    wadb_json json; TEST_ASSERT_TRUE(wadb_json_init(&json, NULL, 0)); json.reply_limit = WADB_HTTP_APPEND_REPLY_LIMIT;
    memset(json.error.message, 1, sizeof(json.error.message) - 1); json.error.message[sizeof(json.error.message) - 1] = 0;
    json.error.code = WADB_INVALID;
    wadb_json_finish(&json, 400, &reply); TEST_ASSERT_EQUAL_UINT(400, reply.status);
    TEST_ASSERT_TRUE(reply.length <= WADB_HTTP_APPEND_REPLY_LIMIT); wadb_http_reply_free(&reply); wadb_json_destroy(&json);
    TEST_ASSERT_TRUE(wadb_json_init(&json, NULL, 0)); json.reply_limit = WADB_HTTP_APPEND_REPLY_LIMIT;
    char oversized[WADB_HTTP_APPEND_REPLY_LIMIT + 1]; memset(oversized, 'x', sizeof(oversized) - 1); oversized[sizeof(oversized) - 1] = 0;
    wadb_json_string(&json, json.root, "future_field", oversized);
    wadb_json_finish(&json, 201, &reply); TEST_ASSERT_EQUAL_UINT(422, reply.status);
    TEST_ASSERT_TRUE(reply.length <= WADB_HTTP_APPEND_REPLY_LIMIT); wadb_http_reply_free(&reply); wadb_json_destroy(&json);
}
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_api_fixed_schema_exact_values_and_atomic_rejection);
    RUN_TEST(test_api_rejects_ambiguous_and_malformed_requests);
    RUN_TEST(test_api_grouped_query_and_tail);
    RUN_TEST(test_api_cursors_require_boot_and_preserve_snapshot);
    RUN_TEST(test_api_retention_and_segment_metadata);
    RUN_TEST(test_append_reply_bound_covers_receipts_and_escaped_errors);
    return UNITY_END();
}
