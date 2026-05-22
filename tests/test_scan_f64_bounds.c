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
    PROVEN_TEST_ASSERT(double_bits(res.val) == double_bits(expected), label, "Inspect decimal-to-binary conversion accuracy and underflow handling.");
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
        "test_scan_f64_bounds",
        "Validate float scanning around underflow and overflow boundaries, including sign-preserving zero and failure-atomic rollback.",
        "Inspect proven_scan_f64 exponent scaling, underflow policy, and overflow handling before changing the decimal parser."
    );

    PROVEN_TEST_SECTION(
        "underflow boundaries",
        "Confirm that values below the smallest subnormal round to signed zero instead of failing early.",
        "Inspect the exponent-range check and the final finite-value path if a tiny token returns the wrong error or loses its sign.");
    expect_scan_ok_bits("1e-330", "1e-330", 0.0);
    expect_scan_ok_bits("-1e-330", "-1e-330", -0.0);
    {
        char tiny_decimal[336];
        tiny_decimal[0] = '0';
        tiny_decimal[1] = '.';
        memset(tiny_decimal + 2, '0', 330);
        tiny_decimal[332] = '1';
        tiny_decimal[333] = '\0';
        expect_scan_ok_bits("tiny decimal underflow", tiny_decimal, 0.0);
    }

    PROVEN_TEST_SECTION(
        "overflow boundaries",
        "Confirm that genuine overflow reports a deterministic overflow error and preserves the cursor.",
        "Inspect the true-value exponent path if large inputs return out-of-bounds or advance the cursor on failure.");
    expect_scan_ok_bits("largest finite", "1.7976931348623157e308", strtod("1.7976931348623157e308", NULL));
    expect_scan_ok_bits("smallest normal", "2.2250738585072014e-308", 2.2250738585072014e-308);
    expect_scan_ok_bits("smallest subnormal", "4.9e-324", 4.9e-324);
    expect_scan_fail_restore("1e309 overflow", "1e309", PROVEN_ERR_OVERFLOW);
    {
        char large_int[402];
        large_int[0] = '1';
        memset(large_int + 1, '0', 400);
        large_int[401] = '\0';
        expect_scan_fail_restore("long integer overflow", large_int, PROVEN_ERR_OVERFLOW);
    }

    PROVEN_TEST_SECTION(
        "malformed input and rollback",
        "Confirm that invalid tokens do not advance the caller cursor.",
        "Inspect the start_cursor save/restore path if malformed input leaves the scanner advanced.");
    expect_scan_fail_restore("empty token", "", PROVEN_ERR_INVALID_ARG);
    expect_scan_fail_restore("non-numeric token", "abc", PROVEN_ERR_INVALID_ARG);
    expect_scan_fail_restore("dangling exponent", "1e", PROVEN_ERR_INVALID_ARG);
    expect_scan_fail_restore("dangling exponent sign", "1e+", PROVEN_ERR_INVALID_ARG);

    PROVEN_TEST_PASS("Float scanner boundary checks passed.");
    return 0;
}
