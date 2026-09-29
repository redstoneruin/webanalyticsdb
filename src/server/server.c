#include "server.h"
#include "api.h"
#include "auth.h"
#include "../core/engine.h"
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

#if MHD_VERSION != 0x01001000
#error "Use the pinned GNU libmicrohttpd 1.0.10 dependency"
#endif
#define WORKERS 4u
#define QUEUE_LIMIT 32u
#define UPLOAD_BUDGET (64u * 1024 * 1024)
#define RESPONSE_BUDGET (32u * 1024 * 1024)
#define FEED_CAPACITY 128u
#define FEED_MESSAGE_LIMIT 32768u
#define STREAM_LIMIT 8u

typedef struct request request;
typedef struct stream stream;
typedef struct { char *bytes; size_t length; uint64_t id; } feed_message;
struct wadb_server {
    struct MHD_Daemon *daemon;
    wadb_api_context api;
    wadb_auth *auth;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t workers[WORKERS], ticker;
    unsigned worker_count;
    bool ticker_started, stopping, ready;
    request *queue[QUEUE_LIMIT]; size_t queue_head, queue_count;
    size_t upload_bytes, response_bytes;
    char origin[256], host[240], boot_id[33]; uint16_t port; bool secure;
    feed_message feed[FEED_CAPACITY]; uint64_t feed_id, notification_id;
    stream *streams; unsigned stream_count;
};
struct request {
    wadb_server *server; struct MHD_Connection *connection;
    char path[256], method[8], session[65], csrf[65];
    bool bearer, authorized, queued, ready;
    size_t response_charge;
    char *body; size_t length, capacity;
    wadb_http_reply reply;
    char set_cookie[256];
};
struct stream {
    wadb_server *server; struct MHD_Connection *connection; stream *next;
    uint64_t after, last_progress; bool suspended, reset, closing, bearer;
    char session[65]; char buffer[FEED_MESSAGE_LIMIT + 256]; size_t length, offset;
};
typedef struct { wadb_server *server; wadb_http_reply reply; bool reserved; } response_holder;
static uint64_t feed_cursor(void *context) {
    wadb_server *s = context;
    pthread_mutex_lock(&s->mutex); uint64_t id = s->feed_id; pthread_mutex_unlock(&s->mutex); return id;
}
static void headers(struct MHD_Response *r, const char *type) {
    MHD_add_response_header(r, MHD_HTTP_HEADER_CONTENT_TYPE, type);
    MHD_add_response_header(r, MHD_HTTP_HEADER_CACHE_CONTROL, "no-store");
    MHD_add_response_header(r, "X-Content-Type-Options", "nosniff");
    MHD_add_response_header(r, "Referrer-Policy", "no-referrer");
    MHD_add_response_header(r, "Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
}
static void release_response(void *context) {
    response_holder *h = context;
    if (h->reserved) { pthread_mutex_lock(&h->server->mutex); h->server->response_bytes -= h->reply.length; pthread_mutex_unlock(&h->server->mutex); }
    wadb_http_reply_free(&h->reply); free(h);
}
static enum MHD_Result send_reply(request *r) {
    wadb_server *s = r->server;
    if (!r->response_charge) {
        pthread_mutex_lock(&s->mutex);
        if (r->reply.length > RESPONSE_BUDGET - s->response_bytes) { pthread_mutex_unlock(&s->mutex); return MHD_NO; }
        s->response_bytes += r->reply.length; r->response_charge = r->reply.length; pthread_mutex_unlock(&s->mutex);
    }
    response_holder *h = malloc(sizeof(*h));
    if (!h) return MHD_NO;
    *h = (response_holder){.server = s, .reply = r->reply, .reserved = r->response_charge != 0};
    memset(&r->reply, 0, sizeof(r->reply)); r->response_charge = 0;
    struct MHD_Response *response = MHD_create_response_from_buffer_with_free_callback_cls(h->reply.length, h->reply.body, release_response, h);
    if (!response) { release_response(h); return MHD_NO; }
    headers(response, "application/json; charset=utf-8");
    if (r->set_cookie[0]) MHD_add_response_header(response, MHD_HTTP_HEADER_SET_COOKIE, r->set_cookie);
    if (h->reply.status == 429) MHD_add_response_header(response, MHD_HTTP_HEADER_RETRY_AFTER, "1");
    enum MHD_Result result = MHD_queue_response(r->connection, h->reply.status, response);
    MHD_destroy_response(response); return result;
}
static enum MHD_Result error_reply(request *r, unsigned status, wadb_status code, const char *message) {
    wadb_http_error(status, code, message, &r->reply); return send_reply(r);
}
static void completed(void *context, struct MHD_Connection *connection, void **cls, enum MHD_RequestTerminationCode code) {
    (void)connection; (void)code; wadb_server *s = context; request *r = *cls;
    if (!r) return;
    pthread_mutex_lock(&s->mutex); s->upload_bytes -= r->capacity;
    s->response_bytes -= r->response_charge;
    pthread_mutex_unlock(&s->mutex);
    free(r->body); wadb_http_reply_free(&r->reply); free(r); *cls = NULL;
}
static void *worker(void *context) {
    wadb_server *s = context;
    for (;;) {
        pthread_mutex_lock(&s->mutex);
        while (!s->queue_count && !s->stopping) pthread_cond_wait(&s->changed, &s->mutex);
        if (!s->queue_count) { pthread_mutex_unlock(&s->mutex); break; }
        request *r = s->queue[s->queue_head]; s->queue_head = (s->queue_head + 1) % QUEUE_LIMIT; --s->queue_count;
        pthread_mutex_unlock(&s->mutex);
        wadb_api_dispatch(&s->api, r->method, r->path, r->body, r->length, &r->reply);
        pthread_mutex_lock(&s->mutex);
        /* Reserve the endpoint's enforced reply limit before mutation, then refund
         * unused space. A successful commit cannot become a retryable overload
         * response merely because other clients stopped reading. */
        s->response_bytes -= r->response_charge - r->reply.length;
        r->response_charge = r->reply.length;
        r->ready = true;
        /* Suspension guarantees r stays alive. After resume, the HTTP thread
         * can complete/free it, so this worker never accesses r again. */
        MHD_resume_connection(r->connection); pthread_mutex_unlock(&s->mutex);
    }
    return NULL;
}
static bool publish(wadb_server *s, const char *event, const wadb_http_reply *reply) {
    if (reply->status >= 400 || reply->length > FEED_MESSAGE_LIMIT - 256) return false;
    char *bytes = malloc(reply->length + 256);
    if (!bytes) return false;
    pthread_mutex_lock(&s->mutex);
    uint64_t id = ++s->feed_id;
    int n = snprintf(bytes, reply->length + 256, "id: %s:%" PRIu64 "\nevent: %s\ndata: %.*s\n\n", s->boot_id, id, event, (int)reply->length, reply->body);
    feed_message *m = &s->feed[(id - 1) % FEED_CAPACITY]; free(m->bytes);
    *m = (feed_message){.bytes = bytes, .length = (size_t)n, .id = id};
    pthread_mutex_unlock(&s->mutex); return true;
}
static void publish_commits(wadb_server *s) {
    wadb_commit_event events[128]; size_t count; uint64_t latest; bool gap;
    if (wadb_read_notifications(s->api.db, s->notification_id, events, 128, &count, &latest, &gap, NULL) || (!count && !gap)) return;
    wadb_json j;
    if (!wadb_json_init(&j, NULL, 0)) { wadb_json_destroy(&j); return; }
    wadb_json_boolean(&j, j.root, "gap", gap); wadb_json_uint(&j, j.root, "latest_notification", latest);
    yyjson_mut_val *a = wadb_json_array(&j); wadb_json_add(&j, j.root, "commits", a);
    for (size_t i = 0; i < count; ++i) {
        yyjson_mut_val *o = wadb_json_object(&j);
        wadb_json_uint(&j, o, "table_id", events[i].table_id); wadb_json_uint(&j, o, "first_sequence", events[i].first_sequence);
        wadb_json_uint(&j, o, "last_sequence", events[i].last_sequence); wadb_json_uint(&j, o, "rows", events[i].rows);
        wadb_json_int(&j, o, "max_time", events[i].max_time); wadb_json_push(&j, a, o);
    }
    wadb_http_reply reply; wadb_json_finish(&j, 200, &reply); wadb_json_destroy(&j);
    if (publish(s, "commits", &reply)) s->notification_id = count ? events[count - 1].id : latest;
    wadb_http_reply_free(&reply);
}
static void *tick(void *context) {
    wadb_server *s = context; uint64_t next_stats = 0;
    for (;;) {
        uint64_t now = wadb_monotonic_ns();
        publish_commits(s);
        if (now >= next_stats) {
            wadb_http_reply reply; wadb_api_dispatch(&s->api, "GET", "/api/v1/stats", NULL, 0, &reply);
            (void)publish(s, "stats", &reply); wadb_http_reply_free(&reply); next_stats = now + UINT64_C(1000000000);
            wadb_cursor_expire(s->api.db);
        }
        pthread_mutex_lock(&s->mutex);
        for (stream *c = s->streams; c; c = c->next) {
            if (s->stopping || now - c->last_progress > UINT64_C(15000000000)) c->closing = true;
            if (c->suspended) { c->suspended = false; MHD_resume_connection(c->connection); }
        }
        if (s->stopping) { pthread_mutex_unlock(&s->mutex); break; }
        uint64_t deadline = now + UINT64_C(250000000);
        while (!s->stopping && wadb_monotonic_ns() < deadline)
            (void)wadb_condition_wait_until(&s->changed, &s->mutex, deadline);
        pthread_mutex_unlock(&s->mutex);
    }
    return NULL;
}
static ssize_t stream_read(void *context, uint64_t position, char *buffer, size_t capacity) {
    (void)position; stream *c = context; wadb_server *s = c->server;
    pthread_mutex_lock(&s->mutex);
    if (s->stopping || c->closing || (!c->bearer && !wadb_auth_session(s->auth, c->session, NULL, false, NULL))) {
        pthread_mutex_unlock(&s->mutex); return MHD_CONTENT_READER_END_OF_STREAM;
    }
    if (c->offset == c->length) {
        c->offset = c->length = 0;
        uint64_t oldest = s->feed_id >= FEED_CAPACITY ? s->feed_id - FEED_CAPACITY + 1 : 1;
        bool gap = c->after > s->feed_id || (c->after < s->feed_id && c->after + 1 < oldest);
        if (c->reset || gap) {
            c->length = (size_t)snprintf(c->buffer, sizeof(c->buffer), "id: %s:%" PRIu64 "\nevent: reset\ndata: {\"boot_id\":\"%s\",\"feed_cursor\":\"%" PRIu64 "\",\"reason\":\"replay_unavailable\"}\n\n", s->boot_id, s->feed_id, s->boot_id, s->feed_id);
            /* A connection that falls behind after opening gets one reset and
             * closes. Reconnect fetches a new snapshot; writes never wait. */
            if (gap && !c->reset) c->closing = true;
            c->reset = false; c->after = s->feed_id;
        } else if (c->after < s->feed_id) {
            feed_message *m = &s->feed[c->after % FEED_CAPACITY];
            memcpy(c->buffer, m->bytes, m->length); c->length = m->length; c->after = m->id;
        } else {
            c->suspended = true; MHD_suspend_connection(c->connection); pthread_mutex_unlock(&s->mutex); return 0;
        }
    }
    size_t n = c->length - c->offset;
    if (n > capacity) n = capacity;
    memcpy(buffer, c->buffer + c->offset, n); c->offset += n; c->last_progress = wadb_monotonic_ns();
    pthread_mutex_unlock(&s->mutex); return (ssize_t)n;
}
static void stream_free(void *context) {
    stream *c = context; wadb_server *s = c->server;
    pthread_mutex_lock(&s->mutex);
    /* A peer disconnect can destroy a streaming response while its connection
     * is still suspended. Release that suspension before dropping our last
     * tracking entry, otherwise daemon shutdown can retain an orphaned socket. */
    if (c->suspended) { c->suspended = false; MHD_resume_connection(c->connection); }
    stream **p = &s->streams;
    while (*p && *p != c) p = &(*p)->next;
    if (*p) { *p = c->next; --s->stream_count; }
    pthread_mutex_unlock(&s->mutex); free(c);
}
static enum MHD_Result open_stream(request *r) {
    wadb_server *s = r->server;
    /* Keep OS autotuning from hiding a slow consumer behind a large send
     * buffer. The kernel may round/double this value; application queues stay
     * independently bounded by the shared ring and one message per stream. */
    const union MHD_ConnectionInfo *info = MHD_get_connection_info(r->connection, MHD_CONNECTION_INFO_CONNECTION_FD);
    int send_bytes = 16 * 1024;
    if (!info || setsockopt(info->connect_fd, SOL_SOCKET, SO_SNDBUF, &send_bytes, sizeof(send_bytes)))
        return error_reply(r, 503, WADB_IO, "could not bound the stream socket buffer");
    stream *c = calloc(1, sizeof(*c));
    if (!c) return error_reply(r, 503, WADB_NOMEM, "stream allocation");
    c->server = s; c->connection = r->connection; c->last_progress = wadb_monotonic_ns(); c->bearer = r->bearer; memcpy(c->session, r->session, sizeof(c->session));
    const char *last = MHD_lookup_connection_value(r->connection, MHD_HEADER_KIND, "Last-Event-ID");
    wadb_json j;
    if (!wadb_json_init(&j, r->body, r->length)) { wadb_json_finish(&j, 400, &r->reply); wadb_json_destroy(&j); free(c); return send_reply(r); }
    yyjson_val *root = yyjson_doc_get_root(j.input), *boot = yyjson_obj_get(root, "boot"), *after = yyjson_obj_get(root, "after");
    if (!wadb_json_allowed(&j, root, "boot|after")) { wadb_json_finish(&j, 400, &r->reply); wadb_json_destroy(&j); free(c); return send_reply(r); }
    if (last) {
        const char *colon = strchr(last, ':');
        c->reset = !colon || (size_t)(colon - last) != 32 || memcmp(last, s->boot_id, 32) || !wadb_decimal_u64(colon + 1, strlen(colon + 1), &c->after);
    } else if (boot || after) {
        c->reset = !yyjson_is_str(boot) || yyjson_get_len(boot) != 32 || memcmp(yyjson_get_str(boot), s->boot_id, 32) || !wadb_json_u64(&j, after, &c->after);
    } else c->reset = true;
    wadb_json_destroy(&j);
    pthread_mutex_lock(&s->mutex);
    if (s->stream_count >= STREAM_LIMIT || s->stopping) { pthread_mutex_unlock(&s->mutex); free(c); return error_reply(r, 429, WADB_BACKPRESSURE, "stream connection limit reached"); }
    c->next = s->streams; s->streams = c; ++s->stream_count;
    pthread_mutex_unlock(&s->mutex);
    struct MHD_Response *response = MHD_create_response_from_callback(MHD_SIZE_UNKNOWN, 8192, stream_read, c, stream_free);
    if (!response) { stream_free(c); return MHD_NO; }
    headers(response, "text/event-stream; charset=utf-8"); MHD_add_response_header(response, "X-Accel-Buffering", "no");
    enum MHD_Result result = MHD_queue_response(r->connection, 200, response); MHD_destroy_response(response); return result;
}

typedef struct { bool bad; unsigned count[9]; } header_check;
static enum MHD_Result check_header(void *context, enum MHD_ValueKind kind, const char *key, size_t kn, const char *value, size_t vn) {
    (void)kind; header_check *c = context;
    if (!key || strlen(key) != kn || !value || strlen(value) != vn) { c->bad = true; return MHD_NO; }
    const char *names[] = {"Host", "Origin", "Authorization", "X-CSRF-Token", "Content-Type", "Cookie", "Last-Event-ID", "Content-Length", "Content-Encoding"};
    for (size_t i = 0; i < 9; ++i) if (!strcasecmp(key, names[i]) && ++c->count[i] > 1) { c->bad = true; return MHD_NO; }
    return MHD_YES;
}
static enum MHD_Result query_argument(void *context, enum MHD_ValueKind kind, const char *key, size_t kn, const char *value, size_t vn) {
    (void)kind; wadb_json *j = context;
    if (!key || !value || !kn || kn >= WADB_NAME_CAP || vn > 128 || strlen(key) != kn || strlen(value) != vn ||
        yyjson_mut_obj_size(j->root) >= 16 || yyjson_mut_obj_get(j->root, key)) {
        wadb_fail(&j->error, WADB_INVALID, 0, "invalid or duplicate query argument"); return MHD_NO;
    }
    wadb_json_string(j, j->root, key, value); return MHD_YES;
}
static bool reserve_body(request *r, size_t need) {
    if (need > WADB_HTTP_BODY_LIMIT) return false;
    if (need <= r->capacity) return true;
    size_t cap = r->capacity ? r->capacity * 2 : 4096;
    if (cap < need) cap = need;
    if (cap > WADB_HTTP_BODY_LIMIT) cap = WADB_HTTP_BODY_LIMIT;
    wadb_server *s = r->server; pthread_mutex_lock(&s->mutex);
    if (cap - r->capacity > UPLOAD_BUDGET - s->upload_bytes) { pthread_mutex_unlock(&s->mutex); return false; }
    char *p = realloc(r->body, cap);
    if (!p) { pthread_mutex_unlock(&s->mutex); return false; }
    s->upload_bytes += cap - r->capacity; r->body = p; r->capacity = cap;
    pthread_mutex_unlock(&s->mutex); return true;
}
static bool arguments(request *r) {
    wadb_json j;
    if (!wadb_json_init(&j, NULL, 0)) { wadb_json_finish(&j, 400, &r->reply); wadb_json_destroy(&j); return false; }
    MHD_get_connection_values_n(r->connection, MHD_GET_ARGUMENT_KIND, query_argument, &j);
    if (strcmp(r->method, "GET") && yyjson_mut_obj_size(j.root)) wadb_fail(&j.error, WADB_INVALID, 0, "POST arguments belong in the JSON body");
    if (!strcmp(r->method, "GET") && !j.error.code) {
        wadb_http_reply reply; wadb_json_finish(&j, 200, &reply);
        if (reply.status >= 400) r->reply = reply;
        else {
            if (!reserve_body(r, reply.length)) wadb_http_error(429, WADB_BACKPRESSURE, "request body budget exhausted", &r->reply);
            else { memcpy(r->body, reply.body, reply.length); r->length = reply.length; }
            wadb_http_reply_free(&reply);
        }
    } else if (j.error.code) wadb_json_finish(&j, 400, &r->reply);
    wadb_json_destroy(&j); return !r->reply.status;
}
static void cookie(request *r, const char *session, bool clear) {
    snprintf(r->set_cookie, sizeof(r->set_cookie), "wadb_session=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=%u%s", session, clear ? 0 : 43200, r->server->secure ? "; Secure" : "");
}
static enum MHD_Result session_action(request *r) {
    wadb_server *s = r->server;
    if (!strcmp(r->path, "/api/v1/logout")) {
        wadb_auth_logout(s->auth, r->session); cookie(r, "", true);
        r->reply = (wadb_http_reply){.status = 200, .body = strdup("{\"ok\":true}"), .length = 11};
        if (!r->reply.body) return MHD_NO;
        return send_reply(r);
    }
    wadb_json j;
    if (!wadb_json_init(&j, r->body, r->length)) { wadb_json_finish(&j, 400, &r->reply); wadb_json_destroy(&j); return send_reply(r); }
    yyjson_val *root = yyjson_doc_get_root(j.input); wadb_credentials c = {0}; unsigned status = 200;
    if (!strcmp(r->method, "POST")) {
        if (wadb_json_allowed(&j, root, "token")) {
            yyjson_val *token = yyjson_obj_get(root, "token");
            wadb_status code = wadb_auth_login(s->auth, yyjson_get_str(token), yyjson_get_len(token), &c, &j.error);
            if (!code) cookie(r, c.session, false);
            else status = code == WADB_BACKPRESSURE ? 429 : code == WADB_INVALID ? 401 : 503;
        }
    } else if (wadb_json_allowed(&j, root, "")) {
        if (!wadb_auth_session(s->auth, r->session, NULL, false, &c)) { wadb_fail(&j.error, WADB_INVALID, 0, "browser session required"); status = 401; }
    }
    if (!j.error.code) { wadb_json_boolean(&j, j.root, "ok", true); wadb_json_string(&j, j.root, "csrf", c.csrf); wadb_json_string(&j, j.root, "boot_id", s->boot_id); }
    wadb_json_finish(&j, status, &r->reply);
    if (status >= 400) r->reply.status = status;
    wadb_json_destroy(&j); return send_reply(r);
}
/* Embedded assets are generated from web/ during the build. */
#include "web_assets.h"
static enum MHD_Result static_response(request *r) {
    const char *type = NULL; const unsigned char *data = NULL; size_t length = 0;
    if (!strcmp(r->path, "/")) { data = wadb_web_index; length = sizeof(wadb_web_index); type = "text/html; charset=utf-8"; }
    else if (!strcmp(r->path, "/app.js")) { data = wadb_web_app; length = sizeof(wadb_web_app); type = "text/javascript; charset=utf-8"; }
    else if (!strcmp(r->path, "/style.css")) { data = wadb_web_style; length = sizeof(wadb_web_style); type = "text/css; charset=utf-8"; }
    else return error_reply(r, 404, WADB_NOT_FOUND, "asset not found");
    struct MHD_Response *response = MHD_create_response_from_buffer(length, (void *)data, MHD_RESPMEM_PERSISTENT);
    if (!response) return MHD_NO;
    headers(response, type); enum MHD_Result result = MHD_queue_response(r->connection, 200, response);
    MHD_destroy_response(response); return result;
}
static enum MHD_Result access_request(void *context, struct MHD_Connection *connection, const char *path, const char *method,
    const char *version, const char *upload, size_t *upload_size, void **cls) {
    (void)version; wadb_server *s = context; request *r = *cls;
    if (!r) {
        r = calloc(1, sizeof(*r)); if (!r) return MHD_NO;
        *cls = r; r->server = s; r->connection = connection;
        pthread_mutex_lock(&s->mutex); bool accepting = s->ready && !s->stopping; pthread_mutex_unlock(&s->mutex);
        if (!accepting) return error_reply(r, 503, WADB_BACKPRESSURE, "server is starting or stopping");
        if (!path || strlen(path) >= sizeof(r->path) || strlen(method) >= sizeof(r->method)) return error_reply(r, 400, WADB_INVALID, "invalid request target");
        strcpy(r->path, path); strcpy(r->method, method);
        if (strcmp(method, "GET") && strcmp(method, "POST")) return error_reply(r, 405, WADB_INVALID, "method not allowed");
        header_check check = {0}; MHD_get_connection_values_n(connection, MHD_HEADER_KIND, check_header, &check);
        if (check.bad || check.count[0] != 1) return error_reply(r, 400, WADB_INVALID, "missing or ambiguous request headers");
        const char *host = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Host");
        const char *origin = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Origin");
        const char *site = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Sec-Fetch-Site");
        if (strcmp(host, s->host) || (origin && strcmp(origin, s->origin)) || (site && strcmp(site, "same-origin") && strcmp(site, "none")))
            return error_reply(r, 403, WADB_INVALID, "request must use the configured origin");
        if (check.count[8]) return error_reply(r, 415, WADB_INVALID, "compressed request bodies are not supported");
        const char *size = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Content-Length"); uint64_t bytes = 0;
        if (size && (!wadb_decimal_u64(size, strlen(size), &bytes) || bytes > WADB_HTTP_BODY_LIMIT)) return error_reply(r, 413, WADB_LIMIT, "request body exceeds 4 MiB");
        bool post = !strcmp(method, "POST");
        if (!post && (bytes || MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Transfer-Encoding"))) return error_reply(r, 400, WADB_INVALID, "GET body is not supported");
        const char *type = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Content-Type");
        if (post && (!type || (strcmp(type, "application/json") && strcmp(type, "application/json; charset=utf-8")))) return error_reply(r, 415, WADB_INVALID, "POST requires application/json");
        const char *authorization = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Authorization");
        const char *session = MHD_lookup_connection_value(connection, MHD_COOKIE_KIND, "wadb_session");
        const char *csrf = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "X-CSRF-Token");
        if (session && strlen(session) == 64) memcpy(r->session, session, 65);
        if (csrf && strlen(csrf) == 64) memcpy(r->csrf, csrf, 65);
        r->bearer = wadb_auth_bearer(s->auth, authorization);
        r->authorized = r->bearer || wadb_auth_session(s->auth, r->session, r->csrf, post, NULL);
        bool public = (!post && (!strcmp(path, "/") || !strcmp(path, "/app.js") || !strcmp(path, "/style.css"))) || (post && !strcmp(path, "/api/v1/session"));
        if (!public && !r->authorized) return error_reply(r, 401, WADB_INVALID, "valid session and CSRF token or bearer token required");
        if (!arguments(r)) return send_reply(r);
        return MHD_YES;
    }
    if (*upload_size) {
        if (strcmp(r->method, "POST")) { *upload_size = 0; return MHD_NO; }
        if (!r->reply.status) {
            if (*upload_size > WADB_HTTP_BODY_LIMIT - r->length) wadb_http_error(413, WADB_LIMIT, "request body exceeds 4 MiB", &r->reply);
            else if (!reserve_body(r, r->length + *upload_size)) wadb_http_error(429, WADB_BACKPRESSURE, "request body budget exhausted", &r->reply);
            else { memcpy(r->body + r->length, upload, *upload_size); r->length += *upload_size; }
        }
        *upload_size = 0;
        /* MHD cannot queue a response from an upload-data callback. Close an
         * over-budget streaming upload immediately instead of draining an
         * unbounded chunked body. No database work has been submitted. */
        return r->reply.status ? MHD_NO : MHD_YES;
    }
    pthread_mutex_lock(&s->mutex); bool stopping = s->stopping, ready = r->ready; pthread_mutex_unlock(&s->mutex);
    if (ready || r->reply.status) return send_reply(r);
    if (stopping) return error_reply(r, 503, WADB_BACKPRESSURE, "server is stopping");
    if (!strcmp(r->path, "/api/v1/session")) return session_action(r);
    if (!strcmp(r->path, "/api/v1/logout") && !strcmp(r->method, "POST")) return session_action(r);
    if (!strcmp(r->path, "/api/v1/stream") && !strcmp(r->method, "GET")) return open_stream(r);
    if (strncmp(r->path, "/api/", 5) && !strcmp(r->method, "GET")) return static_response(r);
    size_t reply_limit = wadb_api_reply_limit(r->method, r->path);
    pthread_mutex_lock(&s->mutex);
    if (s->stopping || s->queue_count >= QUEUE_LIMIT || reply_limit + 65536 > RESPONSE_BUDGET - s->response_bytes) {
        pthread_mutex_unlock(&s->mutex); return error_reply(r, 429, WADB_BACKPRESSURE, "HTTP work queue or response budget is full");
    }
    s->response_bytes += reply_limit; r->response_charge = reply_limit;
    r->queued = true; MHD_suspend_connection(connection);
    s->queue[(s->queue_head + s->queue_count) % QUEUE_LIMIT] = r; ++s->queue_count;
    pthread_cond_broadcast(&s->changed); pthread_mutex_unlock(&s->mutex); return MHD_YES;
}
static bool configure_origin(wadb_server *s, const char *origin) {
    size_t n = strlen(origin);
    if (n >= sizeof(s->origin)) return false;
    const char *host;
    if (!strncmp(origin, "https://", 8)) { host = origin + 8; s->secure = true; }
    else if (!strncmp(origin, "http://", 7)) host = origin + 7;
    else return false;
    if (!*host || strlen(host) >= sizeof(s->host)) return false;
    for (const char *p = host; *p; ++p) if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == ':')) return false;
    strcpy(s->origin, origin); strcpy(s->host, host); return true;
}
wadb_status wadb_server_start(wadb_db *db, const wadb_server_options *options, wadb_server **out, wadb_error *error) {
    if (!db || !options || !out || !options->token_file) return wadb_fail(error, WADB_INVALID, 0, "server requires database and token file");
    *out = NULL; struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(options->port)};
    const char *bind = options->bind_address ? options->bind_address : "127.0.0.1";
    if (inet_pton(AF_INET, bind, &address.sin_addr) != 1) return wadb_fail(error, WADB_INVALID, 0, "bind address must be numeric IPv4");
    bool loopback = (ntohl(address.sin_addr.s_addr) >> 24) == 127;
    if (!loopback && (!options->public_origin || strncmp(options->public_origin, "https://", 8))) return wadb_fail(error, WADB_INVALID, 0, "remote binding requires an explicit HTTPS public origin and TLS termination");
    wadb_server *s = calloc(1, sizeof(*s));
    if (!s) return wadb_fail(error, WADB_NOMEM, 0, "server state");
    int rc = pthread_mutex_init(&s->mutex, NULL);
    if (rc) { free(s); return wadb_fail(error, WADB_IO, rc, "server mutex"); }
    rc = wadb_condition_init(&s->changed);
    if (rc) { pthread_mutex_destroy(&s->mutex); free(s); return wadb_fail(error, WADB_IO, rc, "server condition"); }
    wadb_status status = wadb_auth_open(options->token_file, &s->auth, error);
    if (status) goto fail;
    if (!wadb_random_hex(s->boot_id, 16)) { status = wadb_fail(error, WADB_IO, errno, "server boot identity"); goto fail; }
    s->api = (wadb_api_context){.db = db, .boot_id = s->boot_id, .feed_cursor = feed_cursor, .feed_context = s};
    status = wadb_jobs_open(db, &s->api.jobs, error); if (status) goto fail;
    /* ready stays false until the origin and all workers are initialized. */
    s->daemon = MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_POLL | MHD_ALLOW_SUSPEND_RESUME, options->port, NULL, NULL, access_request, s,
        MHD_OPTION_SOCK_ADDR_LEN, (socklen_t)sizeof(address), (const struct sockaddr *)&address,
        MHD_OPTION_CONNECTION_LIMIT, (unsigned)128,
        MHD_OPTION_CONNECTION_MEMORY_LIMIT, (size_t)65536,
        MHD_OPTION_CONNECTION_TIMEOUT, (unsigned)15,
        MHD_OPTION_CLIENT_DISCIPLINE_LVL, (int)1,
        MHD_OPTION_NOTIFY_COMPLETED, completed, s,
        MHD_OPTION_END);
    if (!s->daemon) { status = wadb_fail(error, WADB_IO, errno, "start HTTP listener"); goto fail; }
    s->port = MHD_get_daemon_info(s->daemon, MHD_DAEMON_INFO_BIND_PORT)->port;
    char default_origin[256]; snprintf(default_origin, sizeof(default_origin), "http://%s:%u", bind, (unsigned)s->port);
    if (!configure_origin(s, options->public_origin ? options->public_origin : default_origin)) { status = wadb_fail(error, WADB_INVALID, 0, "invalid public origin; use scheme://host[:port] without a path"); goto fail; }
    for (unsigned i = 0; i < WORKERS; ++i) {
        /* Workers initially wait despite listener initialization. */
        rc = pthread_create(&s->workers[i], NULL, worker, s);
        if (rc) { status = wadb_fail(error, WADB_IO, rc, "start HTTP worker"); goto fail; }
        ++s->worker_count;
    }
    rc = pthread_create(&s->ticker, NULL, tick, s);
    if (rc) { status = wadb_fail(error, WADB_IO, rc, "start monitoring ticker"); goto fail; }
    s->ticker_started = true;
    pthread_mutex_lock(&s->mutex); s->ready = true; pthread_mutex_unlock(&s->mutex);
    *out = s; return WADB_OK;
fail:
    wadb_server_stop(s); return status;
}
void wadb_server_stop(wadb_server *s) {
    if (!s) return;
    pthread_mutex_lock(&s->mutex); s->stopping = true; pthread_cond_broadcast(&s->changed); pthread_mutex_unlock(&s->mutex);
    for (unsigned i = 0; i < s->worker_count; ++i) pthread_join(s->workers[i], NULL);
    if (s->ticker_started) pthread_join(s->ticker, NULL);
    pthread_mutex_lock(&s->mutex);
    for (stream *c = s->streams; c; c = c->next) if (c->suspended) { c->suspended = false; MHD_resume_connection(c->connection); }
    pthread_mutex_unlock(&s->mutex);
    if (s->daemon) MHD_stop_daemon(s->daemon);
    wadb_jobs_close(s->api.jobs); wadb_auth_close(s->auth);
    for (size_t i = 0; i < FEED_CAPACITY; ++i) free(s->feed[i].bytes);
    pthread_cond_destroy(&s->changed); pthread_mutex_destroy(&s->mutex); free(s);
}
uint16_t wadb_server_port(const wadb_server *s) { return s->port; }
const char *wadb_server_origin(const wadb_server *s) { return s->origin; }
