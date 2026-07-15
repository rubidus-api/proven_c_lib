#include "proven/float_format.h"
#include "proven/scan.h"
#include "proven_test.h"
#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t double_bits(double value) {
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static int roundtrips_f64(const char *text, double expected) {
    proven_scan_t scan = proven_scan_init(proven_u8str_view_from_cstr(text));
    proven_result_f64_t parsed = proven_scan_f64(&scan);
    return parsed.err == PROVEN_OK && double_bits(parsed.val) == double_bits(expected);
}

static uint32_t float_bits(float value) {
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static float float_from_bits(uint32_t bits) {
    float value = 0.0f;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static double double_from_bits(uint64_t bits) {
    double value = 0.0;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static int roundtrips_f32(const char *text, float expected) {
    proven_scan_t scan = proven_scan_init(proven_u8str_view_from_cstr(text));
    proven_result_f64_t parsed = proven_scan_f64(&scan);
    return parsed.err == PROVEN_OK && float_bits((float)parsed.val) == float_bits(expected);
}

static void check_expected(double value, const char *expected) {
    char actual[128];
    proven_size_t written = 0;
    proven_err_t err = proven_float_format_f64_policy(
        actual,
        sizeof actual,
        value,
        PROVEN_FLOAT_FORMAT_POLICY_RYU,
        proven_float_format_options_shortest(),
        &written
    );
    PROVEN_TEST_ASSERT(err == PROVEN_OK, "shortest f64 formatting should succeed", "Inspect the shortest formatter if a representative finite value stops formatting.");
    PROVEN_TEST_ASSERT(written > 0, "shortest output should not be empty", "Inspect the shortest formatter if it emits an empty string.");
    PROVEN_TEST_ASSERT(written < sizeof actual, "shortest output should fit the scratch buffer", "Inspect the shortest formatter if it reports a length that exceeds the buffer.");
    PROVEN_TEST_ASSERT(strcmp(actual, expected) == 0, "shortest text should match the documented spelling", "Inspect the shortest formatter if the emitted text drifts from the representative corpus.");
    PROVEN_TEST_ASSERT(roundtrips_f64(actual, value), "shortest text should round-trip through the scanner", "Inspect the shortest formatter if the text no longer parses back to the original value.");
}

static void check_expected_f32(float value, const char *expected) {
    char actual[128];
    proven_size_t written = 0;
    proven_err_t err = proven_float_format_f32_policy(
        actual,
        sizeof actual,
        value,
        PROVEN_FLOAT_FORMAT_POLICY_RYU,
        proven_float_format_options_shortest(),
        &written
    );
    PROVEN_TEST_ASSERT(err == PROVEN_OK, "shortest float32 formatting should succeed", "Inspect the float32 shortest formatter if a representative finite value stops formatting.");
    PROVEN_TEST_ASSERT(written > 0, "float32 shortest output should not be empty", "Inspect the float32 shortest formatter if it emits an empty string.");
    PROVEN_TEST_ASSERT(written < sizeof actual, "float32 shortest output should fit the scratch buffer", "Inspect the float32 shortest formatter if it reports a length that exceeds the buffer.");
    PROVEN_TEST_ASSERT(strcmp(actual, expected) == 0, "float32 shortest text should match the documented spelling", "Inspect the float32 shortest formatter if the emitted text drifts from the representative corpus.");
    PROVEN_TEST_ASSERT(roundtrips_f32(actual, value), "float32 shortest text should round-trip through the scanner", "Inspect the float32 shortest formatter if the text no longer parses back to the original value.");
}

static void check_roundtrips_f64(double value) {
    char actual[128];
    proven_size_t written = 0;
    proven_err_t err = proven_float_format_f64_policy(
        actual,
        sizeof actual,
        value,
        PROVEN_FLOAT_FORMAT_POLICY_RYU,
        proven_float_format_options_shortest(),
        &written
    );
    PROVEN_TEST_ASSERT(err == PROVEN_OK, "shortest f64 formatting should succeed", "Inspect the shortest formatter if a finite subnormal stops formatting.");
    PROVEN_TEST_ASSERT(written > 0, "shortest output should not be empty", "Inspect the shortest formatter if it emits an empty string.");
    PROVEN_TEST_ASSERT(written < sizeof actual, "shortest output should fit the scratch buffer", "Inspect the shortest formatter if it reports a length that exceeds the buffer.");
    PROVEN_TEST_ASSERT(roundtrips_f64(actual, value), "shortest text should round-trip through the scanner", "Inspect the shortest formatter if the text no longer parses back to the original value.");
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_unit_float_shortest_roundtrip",
        "Verify shortest float formatting round-trips through the scanner for representative f64 values.",
        "Inspect src/proven/float_decimal.c and src/proven/float_format.c if the shortest output stops round-tripping or stops matching the representative corpus."
    );

    PROVEN_TEST_SECTION(
        "representative finite values",
        "Confirm shortest f64 output round-trips and keeps the representative corpus spellings stable.",
        "Inspect the shortest formatting path if any representative value stops round-tripping or if the corpus strings drift."
    );
    check_expected(0.0, "0");
    check_expected(-0.0, "-0");
    check_expected(1.0, "1");
    check_expected(0.1, "0.1");
    check_expected(0.01, "0.01");
    check_expected(-0.01, "-0.01");
    check_expected(0.001, "0.001");
    check_expected(-0.001, "-0.001");
    check_expected(0.0001, "1e-04");
    check_expected(-0.0001, "-1e-04");
    check_expected(0.00001, "1e-05");
    check_expected(-0.00001, "-1e-05");
    check_expected(100000.0, "1e05");
    check_expected(-100000.0, "-1e05");
    check_expected(0.9999999999999999, "0.9999999999999999");
    check_expected(1.2345678901234567, "1.2345678901234567");
    check_expected(6.3508876286570945e-242, "6.3508876286570945e-242");
    check_expected(-6.3508876286570945e-242, "-6.3508876286570945e-242");
    check_expected(DBL_MIN, "2.2250738585072014e-308");
    check_expected(-DBL_MIN, "-2.2250738585072014e-308");
    check_expected(0x0.fffffffffffffp-1022, "2.225073858507201e-308");
    check_expected(-0x0.fffffffffffffp-1022, "-2.225073858507201e-308");
    check_expected(DBL_MAX, "1.7976931348623157e308");

    PROVEN_TEST_SECTION(
        "tiny subnormal regression",
        "Confirm shortest f64 formatting still succeeds for a finite tiny subnormal that previously fell through to unsupported.",
        "Inspect the scientific formatting path if a tiny finite value stops round-tripping."
    );
    check_roundtrips_f64(double_from_bits(0x000246fc714d2188ULL));

    PROVEN_TEST_SECTION(
        "large scientific regression",
        "Confirm shortest f64 formatting still round-trips a large scientific value that previously parsed one ULP low.",
        "Inspect decimal scaling in src/proven/float_decimal.c if the large scientific value stops round-tripping."
    );
    check_roundtrips_f64(double_from_bits(0xea8bc28d457c01f2ULL));

    PROVEN_TEST_SECTION(
        "representative float32 values",
        "Confirm shortest float32 output round-trips and keeps the representative corpus spellings stable.",
        "Inspect the float32 shortest formatting path if any representative value stops round-tripping or if the corpus strings drift."
    );
    check_expected_f32(0.0f, "0");
    check_expected_f32(-0.0f, "-0");
    check_expected_f32(0.1f, "0.1");
    check_expected_f32(0.01f, "0.01");
    check_expected_f32(-0.01f, "-0.01");
    check_expected_f32(0.001f, "0.001");
    check_expected_f32(-0.001f, "-0.001");
    check_expected_f32(0.0001f, "1e-04");
    check_expected_f32(-0.0001f, "-1e-04");
    check_expected_f32(0.00001f, "1e-05");
    check_expected_f32(-0.00001f, "-1e-05");
    check_expected_f32(100000.0f, "1e05");
    check_expected_f32(-100000.0f, "-1e05");
    check_expected_f32(0.2f, "0.2");
    check_expected_f32(-0.2f, "-0.2");
    check_expected_f32(0.29999998f, "0.29999998");
    check_expected_f32(-0.29999998f, "-0.29999998");
    check_expected_f32(1.0f, "1");
    check_expected_f32(1.0000001f, "1.0000001");
    check_expected_f32(1.0000002f, "1.0000002");
    check_expected_f32(-1.0000002f, "-1.0000002");
    check_expected_f32(2.5f, "2.5");
    check_expected_f32(-2.5f, "-2.5");
    check_expected_f32(33554432.0f, "33554432");
    check_expected_f32(-33554432.0f, "-33554432");
    check_expected_f32(FLT_MIN, "1.1754944e-38");
    check_expected_f32(float_from_bits(0x007fffffu), "1.1754942e-38");
    check_expected_f32(-FLT_MIN, "-1.1754944e-38");
    check_expected_f32(FLT_TRUE_MIN, "1e-45");
    check_expected_f32(float_from_bits(0x00000002u), "3e-45");
    check_expected_f32(-FLT_TRUE_MIN, "-1e-45");
    check_expected_f32(FLT_MAX, "3.4028235e38");
    check_expected_f32(-FLT_MAX, "-3.4028235e38");

    PROVEN_TEST_PASS("Float shortest round-trip exhaustive checks passed.");
    return 0;
}
