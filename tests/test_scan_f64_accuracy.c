#include "proven_test.h"
#include "proven/scan.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

static uint64_t double_bits(double v) {
    uint64_t bits = 0;
    memcpy(&bits, &v, sizeof bits);
    return bits;
}

static void expect_scan_ok_bits(const char *label, const char *input, double expected) {
    proven_scan_t scan = proven_scan_init(proven_u8str_view_from_cstr(input));
    proven_result_f64_t res = proven_scan_f64(&scan);
    size_t input_len = strlen(input);

    PROVEN_TEST_ASSERT(res.err == PROVEN_OK, label, "Inspect proven_scan_f64 parsing and exponent handling.");
    PROVEN_TEST_ASSERT(double_bits(res.val) == double_bits(expected), label, "Inspect decimal-to-binary conversion accuracy and rounding.");
    PROVEN_TEST_ASSERT(scan.cursor == input_len, label, "Inspect cursor advancement after a successful parse.");
}

static void expect_scan_fail_restore(const char *label, const char *input, proven_err_t expected_err) {
    proven_scan_t scan = proven_scan_init(proven_u8str_view_from_cstr(input));
    proven_result_f64_t res = proven_scan_f64(&scan);

    PROVEN_TEST_ASSERT(res.err == expected_err, label, "Inspect the invalid input or out-of-range path in proven_scan_f64.");
    PROVEN_TEST_ASSERT(scan.cursor == 0, label, "Inspect cursor restoration on every failure path.");
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_scan_f64_accuracy",
        "Validate float scanning for exact small values, signed zero, exponent edges, malformed input, and failure-atomic cursor restore.",
        "Inspect proven_scan_f64 decimal accumulation, exponent scaling, and failure rollback before changing parsing logic."
    );

    PROVEN_TEST_SECTION(
        "exact small values",
        "Confirm that common finite values parse to the expected double bit pattern.",
        "Inspect the basic decimal-to-binary conversion path if any of these values drift.");
    expect_scan_ok_bits("0.0", "0.0", 0.0);
    expect_scan_ok_bits("-0.0", "-0.0", -0.0);
    expect_scan_ok_bits("1.0", "1.0", 1.0);
    expect_scan_ok_bits("-1.0", "-1.0", -1.0);
    expect_scan_ok_bits("0.5", "0.5", 0.5);
    expect_scan_ok_bits("0.1", "0.1", 0.1);
    expect_scan_ok_bits("123456789.0", "123456789.0", 123456789.0);

    PROVEN_TEST_SECTION(
        "round-trip style decimal",
        "Confirm that the exact decimal token used in the regression round-trips to the same double value.",
        "Inspect the mantissa accumulation path if the parsed value loses the final digit.");
    expect_scan_ok_bits("0.30000000000000004", "0.30000000000000004", 0.30000000000000004);

    PROVEN_TEST_SECTION(
        "exponent edges",
        "Confirm that finite extremes remain finite and that out-of-range input returns a deterministic error.",
        "Inspect exponent range checks and the final finite-value validation if a large token overflows too early or a tiny token collapses to zero.");
    expect_scan_ok_bits("1.7976931348623157e308", "1.7976931348623157e308", strtod("1.7976931348623157e308", NULL));
    expect_scan_ok_bits("2.2250738585072014e-308", "2.2250738585072014e-308", 2.2250738585072014e-308);
    expect_scan_ok_bits("4.9e-324", "4.9e-324", 4.9e-324);
    expect_scan_fail_restore("1e309 overflow", "1e309", PROVEN_ERR_OVERFLOW);

    PROVEN_TEST_SECTION(
        "malformed input and rollback",
        "Confirm that invalid tokens do not advance the caller cursor.",
        "Inspect the start_cursor save/restore path if malformed input leaves the scanner advanced.");
    expect_scan_fail_restore("empty token", "", PROVEN_ERR_INVALID_ARG);
    expect_scan_fail_restore("non-numeric token", "abc", PROVEN_ERR_INVALID_ARG);
    {
        /*
         * A dangling exponent marker is not part of the number. Match strtod:
         * the scanner accepts the mantissa and stops at the `e`.
         */
        proven_scan_t scan = proven_scan_init(proven_u8str_view_from_cstr("1e"));
        proven_result_f64_t res = proven_scan_f64(&scan);
        PROVEN_TEST_ASSERT(res.err == PROVEN_OK && double_bits(res.val) == double_bits(1.0) && scan.cursor == 1,
                           "dangling exponent keeps mantissa", "Inspect incomplete-exponent backtracking in proven_scan_f64.");
        proven_scan_t scan2 = proven_scan_init(proven_u8str_view_from_cstr("1e+"));
        proven_result_f64_t res2 = proven_scan_f64(&scan2);
        PROVEN_TEST_ASSERT(res2.err == PROVEN_OK && double_bits(res2.val) == double_bits(1.0) && scan2.cursor == 1,
                           "dangling exponent sign keeps mantissa", "Inspect incomplete-exponent backtracking in proven_scan_f64.");
    }

    PROVEN_TEST_PASS("Float scanner accuracy checks passed.");
    return 0;
}
