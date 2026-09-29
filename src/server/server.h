#ifndef WADB_SERVER_SERVER_H
#define WADB_SERVER_SERVER_H
#include "wadb.h"
typedef struct wadb_server wadb_server;
typedef struct {
    const char *bind_address; /* Numeric IPv4; default 127.0.0.1. */
    uint16_t port; /* Zero chooses an ephemeral port. */
    const char *public_origin; /* Required HTTPS origin for non-loopback bind. */
    const char *token_file;
} wadb_server_options;
wadb_status wadb_server_start(wadb_db *db, const wadb_server_options *options, wadb_server **server, wadb_error *error);
void wadb_server_stop(wadb_server *server);
uint16_t wadb_server_port(const wadb_server *server);
const char *wadb_server_origin(const wadb_server *server);
#endif
