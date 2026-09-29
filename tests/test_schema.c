#include "../src/core/internal.h"
#include "../test/unity/unity.h"
#include <float.h>
#include <math.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_golden_row_and_nulls(void) {
    wadb_field_def fields[] = {{"status", WADB_U16, 0, false},
        {"count", WADB_I64, 0, true}, {"label", WADB_TEXT, 8, false}};
    wadb_schema s;
    wadb_error e;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_schema_build(&s, fields, 3, &e));
    TEST_ASSERT_EQUAL_UINT32(32, s.row_width);
    TEST_ASSERT_EQUAL_UINT32(9, s.fields[1].offset);
    wadb_value v[] = {{.as.u64 = 0x1234}, {.is_null = true},
        {.as.bytes = {.data = "a\xe2\x82\xac", .length = 4}}};
    unsigned char row[32], expected[32] = {
        8,7,6,5,4,3,2,1, 1, 0x34,0x12, 0,0,0,0,0,0,0,0,
        4,0,0,0, 'a',0xe2,0x82,0xac, 0,0,0,0,0
    };
    TEST_ASSERT_EQUAL(WADB_OK, wadb_encode_row(&s, v, INT64_C(0x0102030405060708), NULL, NULL, row, sizeof(row), &e));
    TEST_ASSERT_EQUAL_MEMORY(expected, row, sizeof(row));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_validate_row(&s, row, NULL, NULL, &e));
    wadb_value out;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_decode_value(&s, 2, row, NULL, NULL, &out, &e));
    TEST_ASSERT_TRUE(out.is_null);
    row[11] = 1;
    TEST_ASSERT_EQUAL(WADB_CORRUPT, wadb_validate_row(&s, row, NULL, NULL, &e));
    row[11] = 0; row[8] |= 128;
    TEST_ASSERT_EQUAL(WADB_CORRUPT, wadb_validate_row(&s, row, NULL, NULL, &e));
    v[1].is_null = false; v[1].as.i64 = INT64_MIN;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_encode_row(&s, v, 0, NULL, NULL, row, sizeof(row), &e));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_decode_value(&s, 2, row, NULL, NULL, &out, &e));
    TEST_ASSERT_EQUAL_INT64(INT64_MIN, out.as.i64);
}

static void test_schema_rejections_and_stability(void) {
    wadb_schema a, b;
    wadb_field_def defs[] = {{"value", WADB_U32, 0, false}, {"value", WADB_U32, 0, false}};
    TEST_ASSERT_EQUAL(WADB_INVALID, wadb_schema_build(&a, defs, 2, NULL));
    defs[0].name = "_time";
    TEST_ASSERT_EQUAL(WADB_INVALID, wadb_schema_build(&a, defs, 1, NULL));
    defs[0].name = "../escape";
    TEST_ASSERT_EQUAL(WADB_INVALID, wadb_schema_build(&a, defs, 1, NULL));
    defs[0].name = "value"; defs[0].size = 1;
    TEST_ASSERT_EQUAL(WADB_INVALID, wadb_schema_build(&a, defs, 1, NULL));
    defs[0].type = WADB_TEXT; defs[0].size = WADB_MAX_ROW_BYTES;
    TEST_ASSERT_EQUAL(WADB_LIMIT, wadb_schema_build(&a, defs, 1, NULL));
    defs[0].size = 32;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_schema_build(&a, defs, 1, NULL));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_schema_build(&b, defs, 1, NULL));
    TEST_ASSERT_EQUAL_UINT64(a.fingerprint, b.fingerprint);
    defs[0].nullable = true;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_schema_build(&b, defs, 1, NULL));
    TEST_ASSERT_NOT_EQUAL(a.fingerprint, b.fingerprint);
    TEST_ASSERT_EQUAL_INT(1, wadb_schema_find(&a, "value"));
    TEST_ASSERT_EQUAL_INT(-1, wadb_schema_find(&a, "missing"));
    TEST_ASSERT_EQUAL(WADB_OK, wadb_schema_build(&a, NULL, 0, NULL));
    TEST_ASSERT_EQUAL_UINT32(8, a.row_width);
}

static void test_numeric_ranges_and_nonfinite_values(void) {
    const wadb_type types[] = {WADB_I8, WADB_U8, WADB_I16, WADB_U16, WADB_I32, WADB_U32,
        WADB_I64, WADB_U64, WADB_F32, WADB_F64, WADB_BOOL, WADB_TIMESTAMP_US};
    for (size_t i = 0; i < sizeof(types) / sizeof(*types); ++i) {
        wadb_field_def d = {"value", types[i], 0, false};
        wadb_schema s;
        TEST_ASSERT_EQUAL(WADB_OK, wadb_schema_build(&s, &d, 1, NULL));
        wadb_value v = {0}, out;
        bool signed_type = types[i] == WADB_I8 || types[i] == WADB_I16 || types[i] == WADB_I32 ||
                           types[i] == WADB_I64 || types[i] == WADB_TIMESTAMP_US;
        if (signed_type) v.as.i64 = -17;
        else if (types[i] == WADB_F32 || types[i] == WADB_F64) v.as.f64 = 1.5;
        else v.as.u64 = 1;
        unsigned char row[32];
        TEST_ASSERT_EQUAL(WADB_OK, wadb_encode_row(&s, &v, 0, NULL, NULL, row, sizeof(row), NULL));
        TEST_ASSERT_EQUAL(WADB_OK, wadb_decode_value(&s, 1, row, NULL, NULL, &out, NULL));
        if (signed_type) TEST_ASSERT_EQUAL_INT64(-17, out.as.i64);
        else if (types[i] == WADB_F32 || types[i] == WADB_F64) TEST_ASSERT_TRUE(out.as.f64 == 1.5);
        else TEST_ASSERT_EQUAL_UINT64(1, out.as.u64);
        v.is_null = true;
        TEST_ASSERT_EQUAL(WADB_INVALID, wadb_encode_row(&s, &v, 0, NULL, NULL, row, sizeof(row), NULL));
        v.is_null = false;
        if (types[i] == WADB_F32 || types[i] == WADB_F64) v.as.f64 = INFINITY;
        else if (signed_type && s.fields[1].width < 8) v.as.i64 = INT64_MAX;
        else if (!signed_type && s.fields[1].width < 8) v.as.u64 = UINT64_MAX;
        else continue;
        TEST_ASSERT_EQUAL(WADB_INVALID, wadb_encode_row(&s, &v, 0, NULL, NULL, row, sizeof(row), NULL));
    }
}

static void test_utf8_and_fixed_buffers(void) {
    TEST_ASSERT_TRUE(wadb_utf8("\xf0\x9f\x98\x80", 4));
    TEST_ASSERT_FALSE(wadb_utf8("\xc0\x80", 2));
    TEST_ASSERT_FALSE(wadb_utf8("\xed\xa0\x80", 3));
    TEST_ASSERT_FALSE(wadb_utf8("\xf4\x90\x80\x80", 4));
    TEST_ASSERT_FALSE(wadb_utf8("\xe2\x82", 2));
    wadb_schema s;
    wadb_field_def d = {"label", WADB_TEXT, 4, false};
    wadb_value v = {.as.bytes = {.data = "large", .length = 5}};
    unsigned char row[24];
    TEST_ASSERT_EQUAL(WADB_OK, wadb_schema_build(&s, &d, 1, NULL));
    TEST_ASSERT_EQUAL(WADB_INVALID, wadb_encode_row(&s, &v, 0, NULL, NULL, row, sizeof(row), NULL));
    v.as.bytes.data = "\xed\xa0\x80"; v.as.bytes.length = 3;
    TEST_ASSERT_EQUAL(WADB_INVALID, wadb_encode_row(&s, &v, 0, NULL, NULL, row, sizeof(row), NULL));
    v.as.bytes.data = NULL; v.as.bytes.length = 0;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_encode_row(&s, &v, 0, NULL, NULL, row, sizeof(row), NULL));
    row[8] = 255;
    TEST_ASSERT_EQUAL(WADB_CORRUPT, wadb_validate_row(&s, row, NULL, NULL, NULL));
    d.type = WADB_BYTES;
    TEST_ASSERT_EQUAL(WADB_OK, wadb_schema_build(&s, &d, 1, NULL));
    TEST_ASSERT_EQUAL(WADB_INVALID, wadb_encode_row(&s, &v, 0, NULL, NULL, row, sizeof(row), NULL));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_golden_row_and_nulls);
    RUN_TEST(test_schema_rejections_and_stability);
    RUN_TEST(test_numeric_ranges_and_nonfinite_values);
    RUN_TEST(test_utf8_and_fixed_buffers);
    return UNITY_END();
}
