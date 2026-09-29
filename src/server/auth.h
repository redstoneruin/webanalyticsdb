#ifndef WADB_SERVER_AUTH_H
#define WADB_SERVER_AUTH_H
#include "wadb.h"
#define WADB_SECRET_LENGTH 64u
#define WADB_SESSION_LIMIT 32u
typedef struct wadb_auth wadb_auth;
typedef struct { char session[65], csrf[65]; uint64_t expires_ns; } wadb_credentials;
/* Existing token files must be owner-only regular files containing 64 hex
 * characters, optionally followed by a newline. Missing files are created 0600. */
wadb_status wadb_auth_open(const char *token_file, wadb_auth **auth, wadb_error *error);
void wadb_auth_close(wadb_auth *auth);
wadb_status wadb_auth_login(wadb_auth *auth, const char *token, size_t length, wadb_credentials *credentials, wadb_error *error);
bool wadb_auth_bearer(wadb_auth *auth, const char *authorization);
bool wadb_auth_session(wadb_auth *auth, const char *session, const char *csrf, bool mutation, wadb_credentials *credentials);
void wadb_auth_logout(wadb_auth *auth, const char *session);
bool wadb_random_hex(char *out, size_t bytes);
#endif
