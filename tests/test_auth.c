#include "database_fixture.h"
#include "../src/server/auth.h"
static void test_authentication_sessions_csrf_and_revocation(void) {
    char path[128]; snprintf(path, sizeof(path), "%s/admin.token", directory);
    wadb_auth *auth; TEST_ASSERT_EQUAL(WADB_OK, wadb_auth_open(path, &auth, NULL));
    struct stat st; TEST_ASSERT_EQUAL_INT(0, stat(path, &st)); TEST_ASSERT_EQUAL_INT(0600, st.st_mode & 0777);
    int fd = open(path, O_RDONLY); char token[65] = {0}; TEST_ASSERT_EQUAL_INT(64, read(fd, token, 64)); close(fd);
    wadb_credentials c; TEST_ASSERT_EQUAL(WADB_OK, wadb_auth_login(auth, token, 64, &c, NULL));
    TEST_ASSERT_TRUE(wadb_auth_session(auth, c.session, NULL, false, NULL));
    TEST_ASSERT_FALSE(wadb_auth_session(auth, c.session, NULL, true, NULL));
    TEST_ASSERT_TRUE(wadb_auth_session(auth, c.session, c.csrf, true, NULL));
    char bearer[80]; snprintf(bearer, sizeof(bearer), "Bearer %s", token);
    TEST_ASSERT_TRUE(wadb_auth_bearer(auth, bearer)); bearer[8] ^= 1; TEST_ASSERT_FALSE(wadb_auth_bearer(auth, bearer));
    wadb_auth_logout(auth, c.session); TEST_ASSERT_FALSE(wadb_auth_session(auth, c.session, c.csrf, true, NULL));
    wadb_auth_close(auth);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_auth_open(path, &auth, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_auth_login(auth, token, 64, &c, NULL)); wadb_auth_close(auth);
}
static void test_auth_rejects_unsafe_files_and_throttles_login(void) {
    char path[128], link[128]; snprintf(path, sizeof(path), "%s/admin.token", directory); snprintf(link, sizeof(link), "%s/link", directory);
    wadb_auth *auth; TEST_ASSERT_EQUAL(WADB_OK, wadb_auth_open(path, &auth, NULL));
    wadb_credentials c;
    for (int i = 0; i < 10; ++i) TEST_ASSERT_EQUAL(WADB_INVALID, wadb_auth_login(auth, "wrong", 5, &c, NULL));
    TEST_ASSERT_EQUAL(WADB_BACKPRESSURE, wadb_auth_login(auth, "wrong", 5, &c, NULL)); wadb_auth_close(auth);
    TEST_ASSERT_EQUAL_INT(0, chmod(path, 0644)); TEST_ASSERT_EQUAL(WADB_INVALID, wadb_auth_open(path, &auth, NULL));
    TEST_ASSERT_EQUAL_INT(0, chmod(path, 0600)); TEST_ASSERT_EQUAL_INT(0, symlink(path, link));
    TEST_ASSERT_EQUAL(WADB_IO, wadb_auth_open(link, &auth, NULL));
}
int main(void) { UNITY_BEGIN(); RUN_TEST(test_authentication_sessions_csrf_and_revocation); RUN_TEST(test_auth_rejects_unsafe_files_and_throttles_login); return UNITY_END(); }
