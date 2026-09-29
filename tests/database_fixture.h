#include "../src/core/engine.h"
#include "../test/unity/unity.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char directory[64];
static wadb_db *db;
static void remove_fixture(const char *path) {
    struct stat st;
    TEST_ASSERT_EQUAL_INT(0, lstat(path, &st));
    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        TEST_ASSERT_NOT_NULL(dir);
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            char *child = wadb_path(path, entry->d_name);
            TEST_ASSERT_NOT_NULL(child);
            remove_fixture(child);
            free(child);
        }
        closedir(dir);
        TEST_ASSERT_EQUAL_INT(0, rmdir(path));
    } else TEST_ASSERT_EQUAL_INT(0, unlink(path));
}
void setUp(void) {
    db = NULL;
    strcpy(directory, "/tmp/wadb-database-XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(directory));
}
void tearDown(void) { wadb_close(db); db = NULL; remove_fixture(directory); }

