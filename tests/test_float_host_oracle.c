#include "proven_test.h"
#include "proven/float_format.h"
#include "proven/scan.h"
#include "proven/u8str.h"
#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long float_bits_f64(double value) {
    unsigned long long bits = 0;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static void require_scan_matches_host(const char *text) {
    char *end = NULL;
    double host = strtod(text, &end);
    PROVEN_TEST_ASSERT(end != NULL && *end == '\0', "host strtod should consume the entire oracle input", "Inspect the test corpus if host strtod leaves trailing characters.");

    proven_scan_t scan = proven_scan_init(proven_u8str_view_from_cstr(text));
    proven_result_f64_t parsed = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(parsed.err == PROVEN_OK, "library scan should parse the oracle input", "Inspect proven_scan_f64 if the host-oracle scan check rejects a valid decimal spelling.");
    PROVEN_TEST_ASSERT(float_bits_f64(parsed.val) == float_bits_f64(host), "library scan should match host strtod bit patterns", "Inspect decimal accumulation and exponent scaling if the host-oracle bits diverge.");
}

static void require_format_matches_host(double value) {
    char expected[128];
    double abs_v = value < 0.0 ? -value : value;
    bool use_scientific = (abs_v >= 1e18 || (abs_v > 0.0 && abs_v < 1e-4));
    const char *fmt = use_scientific ? "%.6e" : "%.6f";
    int expected_len = snprintf(expected, sizeof expected, fmt, value);
    PROVEN_TEST_ASSERT(expected_len >= 0 && (size_t)expected_len < sizeof expected, "host snprintf should fit the oracle buffer", "Inspect the host oracle corpus if formatting overflows the local expectation buffer.");

    char actual[128];
    proven_size_t written = 0;
    proven_err_t err = proven_float_format_f64_policy(actual, sizeof actual, value, PROVEN_FLOAT_FORMAT_POLICY_DEFAULT, proven_float_format_options_fixed_default(), &written);
    PROVEN_TEST_ASSERT(err == PROVEN_OK, "library formatter should succeed for the oracle value", "Inspect the float formatter policy path if a finite oracle value fails to format.");
    PROVEN_TEST_ASSERT(strcmp(actual, expected) == 0, "library formatter should match the host oracle spelling", "Inspect the fixed formatter branch if the library text drifts away from host snprintf.");
    PROVEN_TEST_ASSERT(written == strlen(expected), "formatter should report the written text length", "Inspect written-count bookkeeping if the formatter text matches but the length does not.");
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_float_host_oracle",
        "Compare representative finite float parse and format cases against the platform C library without linking the implementation together.",
        "Inspect src/proven/scan.c and src/proven/float_format.c if the host oracle and library stop agreeing on the representative corpus."
    );

    PROVEN_TEST_SECTION(
        "host-oracle scanning",
        "Verify the parser matches host strtod on representative finite decimal spellings.",
        "Inspect decimal accumulation, exponent scaling, or underflow handling if the parsed bits diverge from host strtod."
    );
    require_scan_matches_host("0");
    require_scan_matches_host("-0");
    require_scan_matches_host("0.1");
    require_scan_matches_host("0.01");
    require_scan_matches_host("0.001");
    require_scan_matches_host("0.0001");
    require_scan_matches_host("0.00001");
    require_scan_matches_host("1");
    require_scan_matches_host("1.234567");
    require_scan_matches_host("123.4567899");
    require_scan_matches_host("1e17");
    require_scan_matches_host("1e18");
    require_scan_matches_host("1e-4");
    require_scan_matches_host("9.999999e-5");
    require_scan_matches_host("2.2250738585072000e-308");
    require_scan_matches_host("2.2250738585072001e-308");
    require_scan_matches_host("2.2250738585072002e-308");
    require_scan_matches_host("2.2250738585072010e-308");
    require_scan_matches_host("2.2250738585072012e-308");
    require_scan_matches_host("2.2250738585072013e-308");
    require_scan_matches_host("2.2250738585072014e-308");
    require_scan_matches_host("2.2250738585072017e-308");
    require_scan_matches_host("2.2250738585072018e-308");
    require_scan_matches_host("2.2250738585072020e-308");
    require_scan_matches_host("-2.2250738585072014e-308");
    require_scan_matches_host("4.9406564584124654e-324");
    require_scan_matches_host("-4.9406564584124654e-324");
    require_scan_matches_host("-5e-324");
    require_scan_matches_host("-6.3508876286570945e-242");

    PROVEN_TEST_SECTION(
        "host-oracle formatting",
        "Verify the fixed formatter matches the platform C library on the same representative values and branch threshold.",
        "Inspect the simple formatter threshold or carry path if the formatted spelling diverges from host snprintf."
    );
    require_format_matches_host(0.0);
    require_format_matches_host(-0.0);
    require_format_matches_host(0.1);
    require_format_matches_host(0.01);
    require_format_matches_host(0.001);
    require_format_matches_host(0.0001);
    require_format_matches_host(0.00001);
    require_format_matches_host(1.0);
    require_format_matches_host(1.234567);
    require_format_matches_host(123.4567899);
    require_format_matches_host(1e17);
    require_format_matches_host(1e18);
    require_format_matches_host(1e-4);
    require_format_matches_host(-6.3508876286570945e-242);

    PROVEN_TEST_PASS("host oracle comparison completed.");
    return 0;
}
