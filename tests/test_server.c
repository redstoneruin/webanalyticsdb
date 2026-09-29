#define tearDown database_tear_down
#include "database_fixture.h"
#undef tearDown
#include "../src/server/server.h"
#include "../vendor/yyjson/yyjson.h"
#include <arpa/inet.h>
#include <signal.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
static wadb_server *server;
static char token[65], origin[128], host[128], token_path[128];
static int sockets[64]; static size_t socket_count;
static pthread_mutex_t sync_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sync_changed = PTHREAD_COND_INITIALIZER;
static bool hold_sync, entered_sync;
static int delayed_sync(void *unused, int fd) {
    (void)unused; pthread_mutex_lock(&sync_mutex); entered_sync = true;
    while (hold_sync) pthread_cond_wait(&sync_changed, &sync_mutex);
    pthread_mutex_unlock(&sync_mutex);
    return wadb_sync_file(NULL, fd, NULL) ? -1 : 0;
}
void tearDown(void) {
    pthread_mutex_lock(&sync_mutex); hold_sync = false; pthread_cond_broadcast(&sync_changed); pthread_mutex_unlock(&sync_mutex);
    for (size_t i = 0; i < socket_count; ++i) if (sockets[i] >= 0) close(sockets[i]);
    socket_count = 0; wadb_server_stop(server); server = NULL; database_tear_down();
}
static void start_server(void) {
    signal(SIGPIPE, SIG_IGN);
    wadb_options o; wadb_options_default(&o); o.batch_delay_ms = 0;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, &o, &db, NULL));
    snprintf(token_path, sizeof(token_path), "%s/admin.token", directory);
    wadb_server_options options = {.port = 0, .token_file = token_path};
    wadb_error error; TEST_ASSERT_EQUAL_MESSAGE(WADB_OK, wadb_server_start(db, &options, &server, &error), error.message);
    snprintf(origin, sizeof(origin), "%s", wadb_server_origin(server)); snprintf(host, sizeof(host), "127.0.0.1:%u", wadb_server_port(server));
    int fd = open(token_path, O_RDONLY); TEST_ASSERT_TRUE(fd >= 0); TEST_ASSERT_EQUAL_INT(64, read(fd, token, 64)); close(fd); token[64] = 0;
}
static int connect_client(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0); TEST_ASSERT_TRUE(fd >= 0);
    struct timeval timeout = {.tv_sec = 5};
    TEST_ASSERT_EQUAL_INT(0, setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    TEST_ASSERT_EQUAL_INT(0, setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(wadb_server_port(server)), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    TEST_ASSERT_EQUAL_INT(0, connect(fd, (struct sockaddr *)&addr, sizeof(addr)));
    for (size_t i = 0; i < socket_count; ++i) if (sockets[i] < 0) { sockets[i] = fd; return fd; }
    TEST_ASSERT_TRUE(socket_count < 64); sockets[socket_count++] = fd; return fd;
}
static void close_client(int fd) {
    for (size_t i = 0; i < socket_count; ++i) if (sockets[i] == fd) { sockets[i] = -1; break; }
    close(fd);
}
static void write_request(int fd, const char *method, const char *path, const char *headers, const char *body) {
    char bytes[8192]; size_t length = body ? strlen(body) : 0;
    int n = snprintf(bytes, sizeof(bytes), "%s %s HTTP/1.1\r\nHost: %s\r\n%sContent-Length: %zu\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n%s", method, path, host, headers ? headers : "", length, body ? body : "");
    TEST_ASSERT_TRUE(n > 0 && n < (int)sizeof(bytes)); size_t sent = 0;
    while (sent < (size_t)n) { ssize_t written = send(fd, bytes + sent, (size_t)n - sent, 0); TEST_ASSERT_TRUE(written > 0); sent += (size_t)written; }
}
static void read_response(int fd, char *out, size_t capacity) {
    size_t used = 0;
    while (used < capacity - 1) { ssize_t n = recv(fd, out + used, capacity - 1 - used, 0); TEST_ASSERT_TRUE(n >= 0); if (!n) break; used += (size_t)n; }
    out[used] = 0; TEST_ASSERT_TRUE(used < capacity - 1);
}
static void expect(const char *method, const char *path, const char *headers, const char *body, unsigned status, char *out, size_t capacity) {
    int fd = connect_client(); write_request(fd, method, path, headers, body); read_response(fd, out, capacity); close_client(fd);
    unsigned actual = 0; TEST_ASSERT_EQUAL_INT(1, sscanf(out, "HTTP/1.1 %u", &actual)); TEST_ASSERT_EQUAL_UINT_MESSAGE(status, actual, strstr(out, "\r\n\r\n"));
}
static void bearer(char out[128]) { snprintf(out, 128, "Authorization: Bearer %s\r\n", token); }
static void test_http_authentication_origin_csrf_and_durable_requests(void) {
    start_server(); char response[16384], authorization[128]; bearer(authorization);
    expect("GET", "/api/v1/stats", NULL, NULL, 401, response, sizeof(response));
    expect("GET", "/", NULL, NULL, 200, response, sizeof(response)); TEST_ASSERT_NOT_NULL(strstr(response, "WebAnalyticsDB"));
    char headers[512]; snprintf(headers, sizeof(headers), "%sOrigin: https://untrusted.example\r\n", authorization);
    expect("GET", "/api/v1/stats", headers, NULL, 403, response, sizeof(response));
    char body[128]; snprintf(body, sizeof(body), "{\"token\":\"%s\"}", token);
    expect("POST", "/api/v1/session", NULL, body, 200, response, sizeof(response));
    char *cookie = strstr(response, "wadb_session="); TEST_ASSERT_NOT_NULL(cookie); cookie += strlen("wadb_session=");
    char session[65]; memcpy(session, cookie, 64); session[64] = 0;
    TEST_ASSERT_NOT_NULL(strstr(response, "HttpOnly; SameSite=Strict"));
    char *json = strstr(response, "\r\n\r\n"); TEST_ASSERT_NOT_NULL(json); json += 4;
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0); TEST_ASSERT_NOT_NULL(doc);
    char csrf[65]; snprintf(csrf, sizeof(csrf), "%s", yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "csrf"))); yyjson_doc_free(doc);
    snprintf(headers, sizeof(headers), "Cookie: wadb_session=%s\r\n", session);
    expect("POST", "/api/v1/tables", headers, "{\"name\":\"t\",\"fields\":[]}", 401, response, sizeof(response));
    snprintf(headers, sizeof(headers), "Cookie: wadb_session=%s\r\nX-CSRF-Token: %s\r\nOrigin: %s\r\n", session, csrf, origin);
    expect("POST", "/api/v1/tables", headers, "{\"name\":\"t\",\"fields\":[]}", 201, response, sizeof(response));
    expect("POST", "/api/v1/tables/1/append", authorization, "{\"rows\":[[],[],[]]}", 201, response, sizeof(response));
    expect("GET", "/api/v1/tables/1/tail?after_sequence=1&limit=1", authorization, NULL, 200, response, sizeof(response)); TEST_ASSERT_NOT_NULL(strstr(response, "\"sequences\":[\"2\"]"));
    expect("GET", "/api/v1/tables/1/tail?limit=1&limit=2", authorization, NULL, 400, response, sizeof(response));
    expect("GET", "/api/v1/tables/1/tail?limit=1%00x", authorization, NULL, 400, response, sizeof(response));
    expect("POST", "/api/v1/logout", headers, "{}", 200, response, sizeof(response));
    expect("GET", "/api/v1/stats", headers, NULL, 401, response, sizeof(response));
    wadb_server_stop(server); server = NULL; wadb_close(db); db = NULL;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, NULL, &db, NULL));
    wadb_table *t; wadb_table_stats stats; TEST_ASSERT_EQUAL(WADB_OK, wadb_get_table(db, 1, &t, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_get_table_stats(t, &stats, NULL)); TEST_ASSERT_EQUAL_UINT64(3, stats.rows);
}
static size_t read_until(int fd, char *buffer, size_t cap, const char *needle) {
    size_t used = 0; buffer[0] = 0;
    while (!strstr(buffer, needle) && used < cap - 1) {
        ssize_t n = recv(fd, buffer + used, cap - 1 - used, 0); TEST_ASSERT_TRUE(n > 0); used += (size_t)n; buffer[used] = 0;
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buffer, needle), buffer); return used;
}
static void json_string(yyjson_val *root, const char *key, char *out, size_t cap) {
    yyjson_val *value = yyjson_obj_get(root, key);
    const char *text = yyjson_get_str(value);
    if (!text) { TEST_FAIL_MESSAGE("expected JSON string field"); return; }
    size_t length = yyjson_get_len(value);
    if (length >= cap) { TEST_FAIL_MESSAGE("JSON string exceeds test buffer"); return; }
    memcpy(out, text, length); out[length] = 0;
}
static void test_sse_replay_reset_limits_and_suspended_shutdown(void) {
    start_server(); char authorization[128], response[16384]; bearer(authorization);
    expect("GET", "/api/v1/stats", authorization, NULL, 200, response, sizeof(response));
    char *body = strstr(response, "\r\n\r\n") + 4; yyjson_doc *doc = yyjson_read(body, strlen(body), 0); TEST_ASSERT_NOT_NULL(doc);
    yyjson_val *root = yyjson_doc_get_root(doc); char boot[33], cursor[32];
    json_string(root, "boot_id", boot, sizeof(boot));
    json_string(root, "feed_cursor", cursor, sizeof(cursor)); yyjson_doc_free(doc);
    char path[160]; snprintf(path, sizeof(path), "/api/v1/stream?boot=%s&after=%s", boot, cursor);
    int stream = connect_client(); write_request(stream, "GET", path, authorization, NULL);
    read_until(stream, response, sizeof(response), "event: stats");
    TEST_ASSERT_NOT_NULL(strstr(response, "text/event-stream")); TEST_ASSERT_NULL(strstr(response, "event: reset"));
    expect("POST", "/api/v1/tables", authorization, "{\"name\":\"t\",\"fields\":[]}", 201, response, sizeof(response));
    expect("POST", "/api/v1/tables/1/append", authorization, "{\"rows\":[[]]}", 201, response, sizeof(response));
    read_until(stream, response, sizeof(response), "event: commits"); TEST_ASSERT_NOT_NULL(strstr(response, "\"last_sequence\":\"1\""));
    int reset = connect_client(); write_request(reset, "GET", "/api/v1/stream?boot=old&after=0", authorization, NULL);
    read_until(reset, response, sizeof(response), "event: reset");
    int remaining[6]; for (int i = 0; i < 6; ++i) { remaining[i] = connect_client(); write_request(remaining[i], "GET", "/api/v1/stream", authorization, NULL); read_until(remaining[i], response, sizeof(response), "event: reset"); }
    expect("GET", "/api/v1/stream", authorization, NULL, 429, response, sizeof(response));
    expect("POST", "/api/v1/tables/1/append", authorization, "{\"rows\":[[],[]]}", 201, response, sizeof(response));
    uint64_t start = wadb_monotonic_ns(); wadb_server_stop(server); server = NULL;
    TEST_ASSERT_TRUE(wadb_monotonic_ns() - start < UINT64_C(3000000000));
    close_client(stream); close_client(reset); for (int i = 0; i < 6; ++i) close_client(remaining[i]);
}
static void test_http_rejects_remote_bind_without_origin_and_stops_live_job(void) {
    start_server(); char authorization[128], response[16384]; bearer(authorization);
    expect("POST", "/api/v1/test-jobs", authorization, "{\"rows_per_second\":1000,\"duration_ms\":60000}", 202, response, sizeof(response));
    expect("GET", "/api/v1/jobs", authorization, NULL, 200, response, sizeof(response)); TEST_ASSERT_NOT_NULL(strstr(response, "\"generator\""));
    uint64_t start = wadb_monotonic_ns(); wadb_server_stop(server); server = NULL;
    TEST_ASSERT_TRUE(wadb_monotonic_ns() - start < UINT64_C(3000000000));
    wadb_server_options options = {.bind_address = "0.0.0.0", .token_file = token_path};
    TEST_ASSERT_EQUAL(WADB_INVALID, wadb_server_start(db, &options, &server, NULL)); TEST_ASSERT_NULL(server);
}
static void test_disconnected_keepalive_streams_release_suspension(void) {
    start_server(); char response[8192];
    for (unsigned i = 0; i < 3; ++i) {
        int fd = connect_client(); char request[1024];
        int n = snprintf(request, sizeof(request), "GET /api/v1/stream HTTP/1.1\r\nHost: %s\r\nAuthorization: Bearer %s\r\n\r\n", host, token);
        TEST_ASSERT_EQUAL_INT(n, send(fd, request, (size_t)n, 0));
        read_until(fd, response, sizeof(response), "event: reset"); close_client(fd);
    }
    /* Allow the next monitor tick to observe the disconnect. Unlike the
     * connection-close test above, these are browser-style keepalive streams. */
    struct timespec pause = {.tv_nsec = 400000000}; nanosleep(&pause, NULL);
    wadb_server_stop(server); server = NULL;
}
static void test_http_request_limits_and_disconnected_clients(void) {
    start_server(); char authorization[128], response[16384]; bearer(authorization);
    expect("DELETE", "/api/v1/tables", authorization, NULL, 405, response, sizeof(response));
    expect("POST", "/api/v1/tables?name=bad", authorization, "{}", 400, response, sizeof(response));
    const char *extra[] = {"Content-Length: 4194305\r\n", "Content-Encoding: gzip\r\nContent-Length: 0\r\n", "Host: bad.example\r\nContent-Length: 0\r\n"};
    unsigned statuses[] = {413, 415, 400};
    for (size_t i = 0; i < 3; ++i) {
        int fd = connect_client(); char request[1024];
        int n = snprintf(request, sizeof(request), "POST /api/v1/tables HTTP/1.1\r\nHost: %s\r\n%s%sContent-Type: application/json\r\nConnection: close\r\n\r\n", host, authorization, extra[i]);
        TEST_ASSERT_EQUAL_INT(n, send(fd, request, (size_t)n, 0));
        read_response(fd, response, sizeof(response)); close_client(fd);
        unsigned status = 0; TEST_ASSERT_EQUAL_INT(1, sscanf(response, "HTTP/1.1 %u", &status)); TEST_ASSERT_EQUAL_UINT(statuses[i], status);
    }
    expect("POST", "/api/v1/tables", authorization, "{\"name\":\"disconnects\",\"fields\":[]}", 201, response, sizeof(response));
    for (unsigned i = 0; i < 20; ++i) {
        int fd = connect_client(); write_request(fd, "POST", "/api/v1/tables/1/append", authorization, "{\"rows\":[[]]}"); close_client(fd);
    }
    /* A disconnected caller has an uncertain outcome. Shutdown must still
     * release requests only after their workers finish and resume them. */
    wadb_server_stop(server); server = NULL;
    wadb_table *t; wadb_integrity_stats result; TEST_ASSERT_EQUAL(WADB_OK, wadb_get_table(db, 1, &t, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_integrity_check(t, NULL, &result, NULL)); TEST_ASSERT_TRUE(result.rows <= 20);
}
static void test_http_overload_is_rejected_before_durable_mutation(void) {
    start_server(); char authorization[128], response[8192]; bearer(authorization);
    expect("POST", "/api/v1/tables", authorization, "{\"name\":\"pressure\",\"fields\":[]}", 201, response, sizeof(response));
    pthread_mutex_lock(&sync_mutex); hold_sync = true; entered_sync = false; pthread_mutex_unlock(&sync_mutex);
    db->io.sync = delayed_sync;
    int pending[48]; size_t count = 0; unsigned rejected = 0, committed = 0;
    for (; count < 48; ++count) {
        int fd = connect_client(); pending[count] = fd;
        write_request(fd, "POST", "/api/v1/tables/1/append", authorization, "{\"rows\":[[]]}");
        struct pollfd poll_fd = {.fd = fd, .events = POLLIN};
        if (poll(&poll_fd, 1, 50) > 0) {
            read_response(fd, response, sizeof(response)); close_client(fd); pending[count] = -1;
            unsigned status = 0; TEST_ASSERT_EQUAL_INT(1, sscanf(response, "HTTP/1.1 %u", &status)); TEST_ASSERT_EQUAL_UINT(429, status);
            ++rejected; ++count; break;
        }
    }
    pthread_mutex_lock(&sync_mutex); bool entered = entered_sync; hold_sync = false; pthread_cond_broadcast(&sync_changed); pthread_mutex_unlock(&sync_mutex);
    TEST_ASSERT_TRUE(entered); TEST_ASSERT_TRUE(rejected > 0);
    for (size_t i = 0; i < count; ++i) if (pending[i] >= 0) {
        read_response(pending[i], response, sizeof(response)); close_client(pending[i]);
        unsigned status = 0; TEST_ASSERT_EQUAL_INT(1, sscanf(response, "HTTP/1.1 %u", &status));
        TEST_ASSERT_TRUE(status == 201 || status == 429);
        if (status == 201) ++committed; else ++rejected;
    }
    /* Tiny append receipts must not hit the seven-large-response limit. The
     * bounded worker queue still rejects overload before it can create a row. */
    TEST_ASSERT_TRUE(committed >= 8); TEST_ASSERT_EQUAL_UINT(count, committed + rejected);
    wadb_table *t; wadb_table_stats stats; TEST_ASSERT_EQUAL(WADB_OK, wadb_get_table(db, 1, &t, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_get_table_stats(t, &stats, NULL)); TEST_ASSERT_EQUAL_UINT64(committed, stats.rows);
}
int main(void) {
    UNITY_BEGIN(); RUN_TEST(test_http_authentication_origin_csrf_and_durable_requests);
    RUN_TEST(test_sse_replay_reset_limits_and_suspended_shutdown);
    RUN_TEST(test_http_rejects_remote_bind_without_origin_and_stops_live_job);
    RUN_TEST(test_http_request_limits_and_disconnected_clients);
    RUN_TEST(test_disconnected_keepalive_streams_release_suspension);
    RUN_TEST(test_http_overload_is_rejected_before_durable_mutation); return UNITY_END();
}
