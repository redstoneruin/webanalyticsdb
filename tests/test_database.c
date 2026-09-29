#include "database_fixture.h"

static void test_create_and_reopen_typed_tables(void) {
    wadb_error e;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, NULL, &db, &e));
    wadb_field_def fields[] = {{"site_id", WADB_U32, 0, false}, {"path", WADB_SYMBOL32, 0, true}};
    wadb_table *table, *other;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_create_table(db, "page_views", fields, 2, 12345, &table, &e));
    uint64_t id = wadb_table_id(table), hash = wadb_table_schema(table)->fingerprint;
    TEST_ASSERT_EQUAL(WADB_EXISTS, wadb_create_table(db, "page_views", fields, 2, 0, &other, &e));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_create_table(db, "ticks", NULL, 0, 0, &other, &e));
    TEST_ASSERT_TRUE(wadb_table_id(other) > id);
    size_t count;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_list_tables(db, NULL, 0, &count, &e));
    TEST_ASSERT_EQUAL_UINT32(2, count);
    wadb_table *one[1];
    TEST_ASSERT_EQUAL(WADB_LIMIT, wadb_list_tables(db, one, 1, &count, &e));
    wadb_close(db); db = NULL;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, NULL, &db, &e));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_find_table(db, "page_views", &table, &e));
    TEST_ASSERT_EQUAL_UINT64(id, wadb_table_id(table));
    TEST_ASSERT_EQUAL_UINT64(hash, wadb_table_schema(table)->fingerprint);
    TEST_ASSERT_EQUAL_UINT64(12345, table->retention_us);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_get_table(db, id, &other, &e));
    TEST_ASSERT_EQUAL_PTR(table, other);
    TEST_ASSERT_EQUAL(WADB_NOT_FOUND, wadb_get_table(db, UINT64_MAX, &other, &e));
}

static void test_exclusive_ownership_across_processes(void) {
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, NULL, &db, NULL));
    pid_t child = fork();
    TEST_ASSERT_TRUE(child >= 0);
    if (!child) {
        wadb_db *second = NULL;
        wadb_status status = wadb_open(directory, NULL, &second, NULL);
        wadb_close(second);
        _exit(status == WADB_LOCKED ? 0 : 1);
    }
    int status;
    TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
    TEST_ASSERT_TRUE(WIFEXITED(status));
    TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

static void test_missing_referenced_schema_does_not_replace_catalog(void) {
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, NULL, &db, NULL));
    wadb_table *t;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_create_table(db, "events", NULL, 0, 0, &t, NULL));
    char *schema = wadb_path(t->directory, "schema"), *catalog = wadb_path(directory, "catalog");
    unsigned char *before, *after;
    size_t a, b;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_read_file(catalog, 1000, &before, &a, NULL));
    wadb_close(db); db = NULL;
    TEST_ASSERT_EQUAL_INT(0, unlink(schema));
    TEST_ASSERT_EQUAL(WADB_CORRUPT, wadb_open(directory, NULL, &db, NULL));
    TEST_ASSERT_NULL(db);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_read_file(catalog, 1000, &after, &b, NULL));
    TEST_ASSERT_EQUAL_UINT32(a, b);
    TEST_ASSERT_EQUAL_MEMORY(before, after, a);
    free(before); free(after); free(schema); free(catalog);
}

static void test_missing_catalog_and_checksum_failure_fail_closed(void) {
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, NULL, &db, NULL));
    wadb_table *t;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_create_table(db, "events", NULL, 0, 0, &t, NULL));
    char *catalog = wadb_path(directory, "catalog");
    wadb_close(db); db = NULL;
    int fd = open(catalog, O_RDWR);
    TEST_ASSERT_TRUE(fd >= 0);
    unsigned char byte = 0xff;
    TEST_ASSERT_EQUAL_INT(1, pwrite(fd, &byte, 1, 24));
    close(fd);
    TEST_ASSERT_EQUAL(WADB_CORRUPT, wadb_open(directory, NULL, &db, NULL));
    TEST_ASSERT_EQUAL_INT(0, unlink(catalog));
    TEST_ASSERT_EQUAL(WADB_CORRUPT, wadb_open(directory, NULL, &db, NULL));
    TEST_ASSERT_EQUAL_INT(-1, access(catalog, F_OK));
    free(catalog);
}

static void test_orphan_table_directory_is_never_reused(void) {
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, NULL, &db, NULL));
    char *tables = wadb_path(directory, "tables"), *orphan = wadb_id_path(tables, 1);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_mkdir(orphan, NULL));
    wadb_table *t;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_create_table(db, "events", NULL, 0, 0, &t, NULL));
    TEST_ASSERT_EQUAL_UINT64(2, t->id);
    free(tables); free(orphan);
    wadb_close(db); db = NULL;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, NULL, &db, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_find_table(db, "events", &t, NULL));
    TEST_ASSERT_EQUAL_UINT64(2, t->id);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_create_and_reopen_typed_tables);
    RUN_TEST(test_exclusive_ownership_across_processes);
    RUN_TEST(test_missing_referenced_schema_does_not_replace_catalog);
    RUN_TEST(test_missing_catalog_and_checksum_failure_fail_closed);
    RUN_TEST(test_orphan_table_directory_is_never_reused);
    return UNITY_END();
}
