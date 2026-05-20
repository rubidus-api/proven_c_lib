#include "proven/fmt.h"
#include "proven/heap.h"
#include "proven_test.h"
#include <float.h>
#include <stdio.h>
#include <string.h>

static void require_impl(bool cond, const char *cond_text, const char *msg, const char *file, int line) {
    if (!cond) {
        fprintf(stderr, "\n[PROVEN][CHECK][FAIL] file=%s line=%d\n", file, line);
        fprintf(stderr, "[PROVEN][CHECK][COND] %s\n", cond_text);
        fprintf(stderr, "[PROVEN][CHECK][INTENT] %s\n", msg);
        fprintf(stderr, "[PROVEN][CHECK][FAIL_HINT] Inspect the shortest float formatting path in src/proven/float_format.c and keep the policy branch enabled for RYU requests.\n");
        exit(1);
    }
}

#define require(cond, msg) require_impl((cond), #cond, (msg), __FILE__, __LINE__)

static void check_f64(double value, const char *expected) {
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
    require(err == PROVEN_OK, "shortest f64 formatting should succeed");
    require(strcmp(buf, expected) == 0, expected);
    require(written == strlen(expected), expected);
}

static void check_f32(float value, const char *expected) {
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
    require(err == PROVEN_OK, "shortest f32 formatting should succeed");
    require(strcmp(buf, expected) == 0, expected);
    require(written == strlen(expected), expected);
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_float_format_shortest_known",
        "Verify the shortest float format policy emits the documented exact spellings for representative f64 and f32 values.",
        "Inspect src/proven/float_format.c if the shortest-policy output drifts or if RYU requests stop reaching the active backend."
    );

    PROVEN_TEST_SECTION(
        "f64 shortest spellings",
        "Confirm representative double values format to the expected shortest strings.",
        "Inspect the shortest formatting path if any of these known values stop matching the documented spellings."
    );
    check_f64(0.0, "0");
    check_f64(-0.0, "-0");
    check_f64(1.0, "1");
    check_f64(-1.0, "-1");
    check_f64(0.1, "0.1");
    check_f64(123456789.0, "123456789");
    check_f64(1e20, "1e20");
    check_f64(DBL_MIN, "2.2250738585072014e-308");
    check_f64(DBL_MAX, "1.7976931348623157e308");
    check_f64(4.9e-324, "5e-324");

    PROVEN_TEST_SECTION(
        "f32 shortest spellings",
        "Confirm representative float values format to compact shortest strings through the float32 policy shim.",
        "Inspect the float32 policy branch if any representative float stops formatting with the same shortest policy."
    );
    check_f32(0.0f, "0");
    check_f32(-0.0f, "-0");
    check_f32(1.0f, "1");
    check_f32(0.1f, "0.1");
    check_f32(1.0e10f, "1e10");
    check_f32(16777216.0f, "16777216");

    PROVEN_TEST_PASS("Shortest known-value checks passed.");
    return 0;
}
