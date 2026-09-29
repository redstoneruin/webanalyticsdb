#include "wadb.h"
#include "server/server.h"
#include "server/json.h"
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *file) {
    fputs("WebAnalyticsDB — append-only analytics database\n\n"
        "  webanalyticsdb serve [--data DIR] [--bind IPv4] [--port PORT]\n"
        "                       [--origin https://HOST] [--token-file FILE]\n"
        "  webanalyticsdb check [--data DIR] [--max-scan-bytes N] [--timeout-ms N]\n\n"
        "Defaults: data/, 127.0.0.1:8080, data/admin.token. Remote binding requires\n"
        "an HTTPS public origin and a TLS-terminating reverse proxy. check requires\n"
        "exclusive access and verifies all retained tables (1 GiB / 60 s per table\n"
        "by default). It does not repair corrupted source data.\n", file);
}
int main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "--help") || !strcmp(argv[1], "help")) { usage(stdout); return 0; }
    bool serve = !strcmp(argv[1], "serve"), check = !strcmp(argv[1], "check");
    if (!serve && !check) { usage(stderr); return 2; }
    const char *directory = "data", *token = NULL;
    wadb_server_options server_options = {.bind_address = "127.0.0.1", .port = 8080};
    wadb_integrity_options integrity; wadb_integrity_options_default(&integrity);
    for (int i = 2; i < argc; ++i) {
        const char *option = argv[i];
        if (!strcmp(option, "--help")) { usage(stdout); return 0; }
        if (++i >= argc) { fprintf(stderr, "Missing value for %s\n", option); return 2; }
        const char *value = argv[i]; uint64_t number;
        if (!strcmp(option, "--data")) directory = value;
        else if (serve && !strcmp(option, "--bind")) server_options.bind_address = value;
        else if (serve && !strcmp(option, "--origin")) server_options.public_origin = value;
        else if (serve && !strcmp(option, "--token-file")) token = value;
        else if (serve && !strcmp(option, "--port") && wadb_decimal_u64(value, strlen(value), &number) && number <= 65535) server_options.port = (uint16_t)number;
        else if (check && !strcmp(option, "--max-scan-bytes") && wadb_decimal_u64(value, strlen(value), &number) && number) integrity.max_scan_bytes = number;
        else if (check && !strcmp(option, "--timeout-ms") && wadb_decimal_u64(value, strlen(value), &number) && number && number <= UINT32_MAX) integrity.timeout_ms = (uint32_t)number;
        else { fprintf(stderr, "Invalid option or value: %s\n", option); return 2; }
    }
    sigset_t signals; sigemptyset(&signals); sigaddset(&signals, SIGINT); sigaddset(&signals, SIGTERM);
    if (serve && pthread_sigmask(SIG_BLOCK, &signals, NULL)) { fputs("Cannot configure signal handling\n", stderr); return 1; }
    struct sigaction ignore = {.sa_handler = SIG_IGN}; sigemptyset(&ignore.sa_mask); sigaction(SIGPIPE, &ignore, NULL);
    wadb_db *db = NULL; wadb_error error; wadb_status status = wadb_open(directory, NULL, &db, &error);
    if (status) { fprintf(stderr, "%s: %s\n", wadb_status_name(status), error.message); return 1; }
    int exit_status = 0;
    if (serve) {
        char *default_token = NULL;
        if (!token) {
            size_t length = strlen(directory) + sizeof("/admin.token"); default_token = malloc(length);
            if (default_token) snprintf(default_token, length, "%s/admin.token", directory);
            token = default_token;
        }
        server_options.token_file = token; wadb_server *server = NULL;
        status = wadb_server_start(db, &server_options, &server, &error);
        if (status) { fprintf(stderr, "%s: %s\n", wadb_status_name(status), error.message); exit_status = 1; }
        else {
            printf("WebAnalyticsDB listening at %s\nAdmin token file: %s\n", wadb_server_origin(server), token); fflush(stdout);
            int caught; if (sigwait(&signals, &caught)) exit_status = 1;
            wadb_server_stop(server);
        }
        free(default_token);
    } else {
        wadb_table *tables[1024]; size_t count = 0;
        status = wadb_list_tables(db, tables, 1024, &count, &error);
        if (status) { fprintf(stderr, "%s\n", error.message); exit_status = 1; }
        for (size_t i = 0; !status && i < count; ++i) {
            wadb_integrity_stats stats; wadb_status result = wadb_integrity_check(tables[i], &integrity, &stats, &error);
            printf("%s: %s; %" PRIu64 " rows, %" PRIu64 " frames, %" PRIu64 " bytes verified\n", wadb_table_name(tables[i]), wadb_status_name(result), stats.rows, stats.frames, stats.bytes);
            if (result) { fprintf(stderr, "%s\n", error.message); exit_status = 1; }
        }
    }
    wadb_close(db); return exit_status;
}
