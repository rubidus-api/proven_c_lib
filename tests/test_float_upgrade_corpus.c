#include "proven/fmt.h"
#include "proven/scan.h"
#include "proven_test.h"
#include <float.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint64_t double_bits(double value) {
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static void expect_scan_case(const char *input) {
    char *end = NULL;
    double host = strtod(input, &end);
    PROVEN_TEST_ASSERT(end != NULL && *end == '\0', input, "Inspect the representative scan corpus if host strtod leaves trailing characters.");

    proven_scan_t scan = proven_scan_init(proven_u8str_view_from_cstr(input));
    proven_result_f64_t parsed = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(parsed.err == PROVEN_OK, input, "Inspect proven_scan_f64 if a representative exact-range or subnormal-boundary spelling stops parsing.");
    PROVEN_TEST_ASSERT(double_bits(parsed.val) == double_bits(host), input, "Inspect decimal-to-binary conversion if the parsed value drifts from the host oracle.");
    PROVEN_TEST_ASSERT(scan.cursor == strlen(input), input, "Inspect cursor advancement if a representative scan case stops consuming the full token.");
}

static void expect_shortest_case(double value, const char *expected) {
    char buf[128];
    proven_size_t written = 0;
    proven_err_t err = proven_float_format_f64_policy(
        buf,
        sizeof buf,
        value,
        PROVEN_FLOAT_FORMAT_POLICY_RYU,
        proven_float_format_options_shortest(),
        &written
    );
    PROVEN_TEST_ASSERT(err == PROVEN_OK, expected, "Inspect the shortest float formatter if a representative value stops formatting.");
    PROVEN_TEST_ASSERT(strcmp(buf, expected) == 0, expected, "Inspect the shortest-policy backend if a representative spelling changes.");
    PROVEN_TEST_ASSERT(written == strlen(expected), expected, "Inspect the written-count bookkeeping if the spelled output length changes.");
}

static void expect_shortest_case_f32(float value, const char *expected) {
    char buf[128];
    proven_size_t written = 0;
    proven_err_t err = proven_float_format_f32_policy(
        buf,
        sizeof buf,
        value,
        PROVEN_FLOAT_FORMAT_POLICY_RYU,
        proven_float_format_options_shortest(),
        &written
    );
    PROVEN_TEST_ASSERT(err == PROVEN_OK, expected, "Inspect the float32 shortest formatter if a representative value stops formatting.");
    PROVEN_TEST_ASSERT(strcmp(buf, expected) == 0, expected, "Inspect the float32 shortest-policy backend if a representative spelling changes.");
    PROVEN_TEST_ASSERT(written == strlen(expected), expected, "Inspect the float32 written-count bookkeeping if the spelled output length changes.");
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_float_upgrade_corpus",
        "Pin representative exact-range, subnormal-boundary, and shortest-format float spellings for the ongoing parser and formatter upgrade path.",
        "Inspect src/proven/scan.c and src/proven/float_format.c if the representative corpus drifts or the upgrade target stops matching the documented spellings."
    );

    PROVEN_TEST_SECTION(
        "scan corpus",
        "Confirm representative exact-range and subnormal-boundary decimal spellings still parse to the same host-visible double bits.",
        "Inspect proven_scan_f64 if a representative decimal spelling stops matching the host oracle."
    );
    expect_scan_case("0");
    expect_scan_case("-0");
    expect_scan_case("0.1");
    expect_scan_case("1.0000000000000002");
    expect_scan_case("0.9999999999999999");
    expect_scan_case("9007199254740991");
    expect_scan_case("9007199254740992");
    expect_scan_case("9007199254740993");
    expect_scan_case("2.2250738585072014e-308");
    expect_scan_case("-2.2250738585072014e-308");
    expect_scan_case("2.2250738585072011e-308");
    expect_scan_case("4.9406564584124654e-324");
    expect_scan_case("-4.9406564584124654e-324");
    expect_scan_case("5e-324");
    expect_scan_case("-5e-324");

    PROVEN_TEST_SECTION(
        "shortest format corpus",
        "Confirm the shortest formatter keeps the representative exact-range and subnormal-boundary spellings stable.",
        "Inspect the shortest-policy backend if a representative value changes spelling."
    );
    expect_shortest_case(0.0, "0");
    expect_shortest_case(-0.0, "-0");
    expect_shortest_case(0.1, "0.1");
    expect_shortest_case(0.001, "0.001");
    expect_shortest_case(-0.001, "-0.001");
    expect_shortest_case(0.0001, "1e-04");
    expect_shortest_case(-0.0001, "-1e-04");
    expect_shortest_case(100000.0, "1e05");
    expect_shortest_case(-100000.0, "-1e05");
    expect_shortest_case(1.0, "1");
    expect_shortest_case(1.0000000000000002, "1.0000000000000002");
    expect_shortest_case(0.9999999999999999, "0.9999999999999999");
    expect_shortest_case(9007199254740991.0, "9007199254740991");
    expect_shortest_case(9007199254740992.0, "9007199254740992");
    expect_shortest_case(DBL_MIN, "2.2250738585072014e-308");
    expect_shortest_case(-DBL_MIN, "-2.2250738585072014e-308");
    expect_shortest_case(0x0.fffffffffffffp-1022, "2.2250738585072009e-308");
    expect_shortest_case(-0x0.fffffffffffffp-1022, "-2.2250738585072009e-308");
    expect_shortest_case(DBL_TRUE_MIN, "5e-324");
    expect_shortest_case(-DBL_TRUE_MIN, "-5e-324");
    expect_shortest_case(6.3508876286570945e-242, "6.3508876286570946e-242");
    expect_shortest_case(-6.3508876286570945e-242, "-6.3508876286570946e-242");
    expect_shortest_case(DBL_MAX, "1.7976931348623157e308");
    expect_shortest_case(-DBL_MAX, "-1.7976931348623157e308");

    PROVEN_TEST_SECTION(
        "float32 shortest corpus",
        "Confirm the shortest formatter keeps the representative float32 literals stable alongside the float64 corpus.",
        "Inspect the float32 shortest helper if any documented float32 literal changes spelling."
    );
    expect_shortest_case_f32(FLT_MIN, "1.17549435e-38");
    expect_shortest_case_f32(-FLT_MIN, "-1.17549435e-38");
    expect_shortest_case_f32(0.001f, "0.001");
    expect_shortest_case_f32(-0.001f, "-0.001");
    expect_shortest_case_f32(0.0001f, "1e-04");
    expect_shortest_case_f32(-0.0001f, "-1e-04");
    expect_shortest_case_f32(100000.0f, "1e05");
    expect_shortest_case_f32(-100000.0f, "-1e05");
    expect_shortest_case_f32(0.2f, "0.2");
    expect_shortest_case_f32(-0.2f, "-0.2");
    expect_shortest_case_f32(0.29999998f, "0.29999998");
    expect_shortest_case_f32(-0.29999998f, "-0.29999998");
    expect_shortest_case_f32(1.0000002f, "1.0000002");
    expect_shortest_case_f32(-1.0000002f, "-1.0000002");
    expect_shortest_case_f32(2.5f, "2.5");
    expect_shortest_case_f32(-2.5f, "-2.5");
    expect_shortest_case_f32(33554432.0f, "33554432");
    expect_shortest_case_f32(-33554432.0f, "-33554432");
    expect_shortest_case_f32(FLT_TRUE_MIN, "1e-45");
    expect_shortest_case_f32(-FLT_TRUE_MIN, "-1e-45");
    expect_shortest_case_f32(FLT_MAX, "3.4028235e38");
    expect_shortest_case_f32(-FLT_MAX, "-3.4028235e38");

    PROVEN_TEST_PASS("Float corpus checks passed.");
    return 0;
}