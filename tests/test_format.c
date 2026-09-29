#include "../src/core/internal.h"
#include "../test/unity/unity.h"
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_crc_known_vector_and_segment(void) {
    TEST_ASSERT_EQUAL_HEX32(0xe3069283, wadb_crc32c("123456789", 9));
    wadb_segment_header h = {.table_id = 1, .segment_id = 2, .schema_hash = 3,
        .row_width = 48, .day_start = 2 * WADB_DAY_US, .first_sequence = 1}, out;
    unsigned char bytes[128];
    TEST_ASSERT_EQUAL(WADB_OK, wadb_segment_encode(&h, bytes, NULL));
    TEST_ASSERT_EQUAL_MEMORY("WADBSEG1", bytes, 8);
    TEST_ASSERT_EQUAL_UINT8(48, bytes[40]);
    TEST_ASSERT_EQUAL(WADB_OK, wadb_segment_decode(bytes, sizeof(bytes), &out, NULL));
    TEST_ASSERT_EQUAL_UINT64(2, out.segment_id);
    bytes[50] ^= 1;
    TEST_ASSERT_EQUAL(WADB_CORRUPT, wadb_segment_decode(bytes, sizeof(bytes), &out, NULL));
    bytes[50] ^= 1; wadb_put_u32(bytes + 8, 2);
    wadb_put_u32(bytes + 124, wadb_crc32c(bytes, 124));
    TEST_ASSERT_EQUAL(WADB_VERSION, wadb_segment_decode(bytes, sizeof(bytes), &out, NULL));
}

static unsigned char *sample_frame(size_t *n) {
    unsigned char rows[32] = {0}, dict[16] = {0};
    wadb_put_u64(rows, 100); wadb_put_u64(rows + 16, 101);
    wadb_put_u32(rows + 8, 1); wadb_put_u32(rows + 24, 1);
    wadb_put_u16(dict, 1); wadb_put_u32(dict + 4, 1); wadb_put_u32(dict + 8, 1); dict[12] = 'x';
    wadb_frame f = {.schema_hash = 99, .first_sequence = 5, .min_time = 100, .max_time = 101,
        .row_count = 2, .row_width = 16, .dictionary_count = 1, .dictionary_bytes = 16,
        .dictionary = dict, .rows = rows};
    unsigned char *p = NULL;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_frame_encode(&f, &p, n, NULL));
    return p;
}

static void test_frame_roundtrip_and_every_truncation(void) {
    size_t n;
    unsigned char *p = sample_frame(&n);
    TEST_ASSERT_EQUAL_UINT32(144, n);
    wadb_frame f;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_frame_decode(p, n, &f, NULL));
    TEST_ASSERT_EQUAL_UINT32(96, f.rows_offset);
    TEST_ASSERT_EQUAL_UINT64(5, f.first_sequence);
    TEST_ASSERT_EQUAL_UINT64(101, wadb_get_u64(f.rows + 16));
    for (size_t i = 0; i < n; ++i)
        TEST_ASSERT_EQUAL(WADB_CORRUPT, wadb_frame_decode(p, i, &f, NULL));
    for (size_t i = 0; i < n; ++i) {
        p[i] ^= 0x80;
        TEST_ASSERT_EQUAL(WADB_CORRUPT, wadb_frame_decode(p, n, &f, NULL));
        p[i] ^= 0x80;
    }
    free(p);
}

static void test_semantic_corruption_with_valid_checksum(void) {
    size_t n;
    unsigned char *p = sample_frame(&n);
    wadb_frame f;
    /* A checksum alone does not establish valid lengths or ordering. */
    wadb_put_u32(p + 16, UINT32_MAX);
    wadb_put_u32(p + 72, wadb_crc32c(p, 72));
    wadb_put_u32(p + n - 4, wadb_crc32c(p, n - 4));
    TEST_ASSERT_EQUAL(WADB_CORRUPT, wadb_frame_decode(p, n, &f, NULL));
    wadb_put_u32(p + 16, 2);
    wadb_put_u32(p + 72, wadb_crc32c(p, 72));
    wadb_put_u64(p + 112, 99);
    wadb_put_u32(p + n - 4, wadb_crc32c(p, n - 4));
    TEST_ASSERT_EQUAL(WADB_CORRUPT, wadb_frame_decode(p, n, &f, NULL));
    wadb_put_u64(p + 112, 101);
    p[95] = 1;
    wadb_put_u32(p + n - 4, wadb_crc32c(p, n - 4));
    TEST_ASSERT_EQUAL(WADB_CORRUPT, wadb_frame_decode(p, n, &f, NULL));
    free(p);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_crc_known_vector_and_segment);
    RUN_TEST(test_frame_roundtrip_and_every_truncation);
    RUN_TEST(test_semantic_corruption_with_valid_checksum);
    return UNITY_END();
}
