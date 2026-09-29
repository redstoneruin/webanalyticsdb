#include "database_fixture.h"

static wadb_table *symbol_table(size_t budget, uint64_t segment_bytes) {
    wadb_options o;
    wadb_options_default(&o); o.dictionary_limit_bytes = budget; o.segment_target_bytes = segment_bytes;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, &o, &db, NULL));
    wadb_field_def fields[] = {{"path", WADB_SYMBOL32, 0, false}, {"referrer", WADB_SYMBOL32, 0, true}};
    wadb_table *t;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_create_table(db, "visits", fields, 2, 0, &t, NULL));
    return t;
}
typedef struct { size_t rows; unsigned char first[16]; size_t first_length; } strings_seen;
static wadb_status read_strings(void *context, uint64_t sequence, const wadb_value *v, size_t count) {
    (void)sequence;
    strings_seen *s = context;
    if (count != 3 || v[1].is_null || !v[1].as.bytes.data) return WADB_CORRUPT;
    if (!s->rows) {
        s->first_length = v[1].as.bytes.length;
        size_t n = s->first_length < sizeof(s->first) ? s->first_length : sizeof(s->first);
        memcpy(s->first, v[1].as.bytes.data, n);
    }
    ++s->rows;
    return WADB_OK;
}

static void test_symbols_reuse_ids_and_recover_definitions(void) {
    wadb_table *t = symbol_table(1024 * 1024, 4096);
    wadb_value v[] = {{.as.bytes = {"/home", 5}}, {.as.bytes = {"/home", 5}},
        {.as.bytes = {"/about", 6}}, {.is_null = true}};
    TEST_ASSERT_EQUAL(WADB_OK, wadb_append_batch(t, v, 2, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(3, t->dictionary->count);
    uint64_t first_end = t->segments[0].committed_bytes;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_append_batch(t, v, 2, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(3, t->dictionary->count);
    unsigned char header[WADB_FRAME_HEADER_BYTES];
    TEST_ASSERT_EQUAL(WADB_OK, wadb_pread_all(t->active_fd, header, sizeof(header), first_end, NULL));
    wadb_frame f;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_frame_inspect(header, sizeof(header), &f, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, f.dictionary_count);
    strings_seen s = {0};
    TEST_ASSERT_EQUAL(WADB_OK, wadb_scan(t, NULL, read_strings, &s, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(4, s.rows);
    TEST_ASSERT_EQUAL_MEMORY("/home", s.first, 5);
    wadb_close(db); db = NULL;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, NULL, &db, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_find_table(db, "visits", &t, NULL));
    TEST_ASSERT_EQUAL_UINT32(3, t->dictionary->count);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_append_batch(t, v, 2, NULL, NULL));
    memset(&s, 0, sizeof(s));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_scan(t, NULL, read_strings, &s, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(6, s.rows);
}

static void test_dictionary_budget_rotates_without_invalid_ids(void) {
    wadb_table *t = symbol_table(2048, 1024 * 1024);
    char text[100];
    memset(text, 'x', sizeof(text));
    for (unsigned i = 0; i < 30; ++i) {
        text[0] = (char)('A' + i);
        wadb_value v[] = {{.as.bytes = {text, sizeof(text)}}, {.is_null = true}};
        TEST_ASSERT_EQUAL(WADB_OK, wadb_append_batch(t, v, 1, NULL, NULL));
        TEST_ASSERT_TRUE(t->dictionary->allocated_bytes <= 2048);
    }
    TEST_ASSERT_TRUE(t->segment_count > 1);
    strings_seen s = {0};
    TEST_ASSERT_EQUAL(WADB_OK, wadb_scan(t, NULL, read_strings, &s, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(30, s.rows);
    wadb_close(db); db = NULL;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_open(directory, NULL, &db, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_find_table(db, "visits", &t, NULL));
    memset(&s, 0, sizeof(s));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_scan(t, NULL, read_strings, &s, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(30, s.rows);
}

static void test_rejected_symbol_batch_rolls_back_every_definition(void) {
    wadb_table *t = symbol_table(8 * 1024 * 1024, 8 * 1024 * 1024);
    wadb_value initial[] = {{.as.bytes = {"initial", 7}}, {.is_null = true}};
    TEST_ASSERT_EQUAL(WADB_OK, wadb_append_batch(t, initial, 1, NULL, NULL));
    const size_t count = 20, length = WADB_MAX_STRING_BYTES;
    char *strings = malloc(count * length);
    wadb_value *values = calloc(count * 2, sizeof(*values));
    TEST_ASSERT_NOT_NULL(strings); TEST_ASSERT_NOT_NULL(values);
    memset(strings, 'x', count * length);
    for (size_t i = 0; i < count; ++i) {
        strings[i * length] = (char)('A' + i);
        values[i * 2].as.bytes.data = strings + i * length;
        values[i * 2].as.bytes.length = length;
        values[i * 2 + 1].is_null = true;
    }
    TEST_ASSERT_EQUAL(WADB_LIMIT, wadb_append_batch(t, values, count, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, t->dictionary->count);
    TEST_ASSERT_EQUAL_UINT64(1, t->last_sequence);
    TEST_ASSERT_EQUAL_UINT32(1, t->segment_count);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_append_batch(t, initial, 1, NULL, NULL));
    free(strings); free(values);
}

static void test_dictionary_replay_rejects_duplicate_and_undefined_ids(void) {
    wadb_table *t = symbol_table(1024 * 1024, 4096);
    wadb_dictionary *d;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_dictionary_new(&t->schema, 4096, &d, NULL));
    uint32_t id;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_dictionary_intern(d, 1, "x", 1, &id, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, id);
    unsigned char *bytes;
    uint32_t n, count;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_dictionary_export(d, 0, &bytes, &n, &count, NULL));
    wadb_frame f = {.dictionary = bytes, .dictionary_bytes = n, .dictionary_count = count};
    TEST_ASSERT_EQUAL(WADB_CORRUPT, wadb_dictionary_apply(d, &f, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, d->count);
    const void *data;
    size_t length;
    TEST_ASSERT_EQUAL(WADB_CORRUPT, wadb_dictionary_resolve(d, 2, 1, &data, &length, NULL));
    wadb_dictionary_rollback(d, 0);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_dictionary_apply(d, &f, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_dictionary_resolve(d, 1, 1, &data, &length, NULL));
    TEST_ASSERT_EQUAL_MEMORY("x", data, 1);
    free(bytes); wadb_dictionary_free(d);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_symbols_reuse_ids_and_recover_definitions);
    RUN_TEST(test_dictionary_budget_rotates_without_invalid_ids);
    RUN_TEST(test_rejected_symbol_batch_rolls_back_every_definition);
    RUN_TEST(test_dictionary_replay_rejects_duplicate_and_undefined_ids);
    return UNITY_END();
}
