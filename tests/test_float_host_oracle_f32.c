#include "proven_test.h"
#include "proven/float_format.h"
#include <float.h>
#include <stdio.h>
#include <string.h>

static void require_format_matches_host_f32(float value) {
    char expected[128];
    double abs_v = value < 0.0f ? -(double)value : (double)value;
    bool use_scientific = (abs_v >= 1e18 || (abs_v > 0.0 && abs_v < 1e-4));
    const char *fmt = use_scientific ? "%.6e" : "%.6f";
    int expected_len = snprintf(expected, sizeof expected, fmt, value);
    PROVEN_TEST_ASSERT(expected_len >= 0 && (size_t)expected_len < sizeof expected, "host snprintf should fit the oracle buffer", "Inspect the host oracle corpus if formatting overflows the local expectation buffer.");

    char actual[128];
    proven_size_t written = 0;
    proven_err_t err = proven_float_format_f32_policy(actual, sizeof actual, value, PROVEN_FLOAT_FORMAT_POLICY_DEFAULT, proven_float_format_options_fixed_default(), &written);
    PROVEN_TEST_ASSERT(err == PROVEN_OK, "library formatter should succeed for the float32 oracle value", "Inspect the float32 formatter policy path if a finite oracle value fails to format.");
    PROVEN_TEST_ASSERT(strcmp(actual, expected) == 0, "library formatter should match the host float32 oracle spelling", "Inspect the float32 fixed formatter if the library text drifts away from host snprintf.");
    PROVEN_TEST_ASSERT(written == strlen(expected), "formatter should report the written text length", "Inspect written-count bookkeeping if the formatter text matches but the length does not.");
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_float_host_oracle_f32",
        "Compare representative finite float32 fixed-format rendering against the platform C library without sharing implementation code.",
        "Inspect src/proven/float_format.c if the float32 host oracle and library disagree on the representative finite corpus."
    );

    PROVEN_TEST_SECTION(
        "host-oracle float32 formatting",
        "Verify the float32 fixed formatter matches the platform C library on representative finite float values and branch thresholds.",
        "Inspect the float32 fixed formatter path if the oracle text or written count drifts."
    );
    require_format_matches_host_f32(0.0f);
    require_format_matches_host_f32(-0.0f);
    require_format_matches_host_f32(0.1f);
    require_format_matches_host_f32(0.001f);
    require_format_matches_host_f32(0.0001f);
    require_format_matches_host_f32(1.0f);
    require_format_matches_host_f32(1.234567f);
    require_format_matches_host_f32(123.45679f);
    require_format_matches_host_f32(1e17f);
    require_format_matches_host_f32(1e18f);
    require_format_matches_host_f32(1e-4f);
    require_format_matches_host_f32(9.999999e-5f);
    require_format_matches_host_f32(FLT_MIN);
    require_format_matches_host_f32(-FLT_MIN);
    require_format_matches_host_f32(FLT_TRUE_MIN);
    require_format_matches_host_f32(-FLT_TRUE_MIN);
    require_format_matches_host_f32(FLT_MAX);
    require_format_matches_host_f32(-FLT_MAX);

    PROVEN_TEST_PASS("Float32 host oracle comparison completed.");
    return 0;
}
