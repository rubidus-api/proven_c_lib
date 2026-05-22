#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include "proven/fmt.h"
#include "proven/float_format.h"
#include "proven/heap.h"
#include "proven_test.h"

static void expect_policy_exact(proven_float_format_policy_t policy, proven_float_format_options_t opt,
                                const char *label, double value, const char *expected) {
    char buf[128];
    proven_size_t written = 0;
    proven_err_t err = proven_float_format_f64_policy(buf, sizeof buf, value, policy, opt, &written);
    PROVEN_TEST_ASSERT(err == PROVEN_OK, label, "Inspect the float format policy dispatch and fixed-output helper if a supported case stops formatting.");
    PROVEN_TEST_ASSERT(strcmp(buf, expected) == 0, label, "Inspect the fixed float formatter helper if the emitted text drifts.");
    PROVEN_TEST_ASSERT(written == strlen(expected), label, "Inspect the written-out count if the reported length stops matching the emitted text.");
}

static void expect_policy_matches_current_fmt(double value) {
    proven_allocator_t alloc = proven_heap_allocator();
    proven_result_u8str_t res = proven_u8str_create(alloc, 8);
    proven_u8str_t str = res.value;
    char buf[128];
    proven_size_t written = 0;
    proven_float_format_options_t opt = proven_float_format_options_fixed_default();

    PROVEN_TEST_ASSERT(PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, "{}", PROVEN_ARG(value))), "current fmt reference", "Inspect PROVEN_ARG_F64 formatting if the existing formatter stops producing a reference string.");
    PROVEN_TEST_ASSERT(proven_float_format_f64_policy(buf, sizeof buf, value, PROVEN_FLOAT_FORMAT_POLICY_DEFAULT, opt, &written) == PROVEN_OK,
                       "default policy should succeed", "Inspect the float format policy default dispatch.");
    PROVEN_TEST_ASSERT(strcmp(buf, proven_u8str_as_cstr(&str)) == 0, "default policy matches fmt", "Inspect the policy helper if the scaffold stops matching the current formatter output.");
    PROVEN_TEST_ASSERT(written == strlen(proven_u8str_as_cstr(&str)), "written count matches fmt", "Inspect the policy helper if the byte count drifts.");
    proven_u8str_destroy(alloc, &str);
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_float_format_policy",
        "Verify the float format policy scaffold preserves the current simple formatter output, supports the shortest-mode policy, and reports invalid inputs clearly.",
        "Inspect src/proven/float_format.c and the new float policy header if the policy seam or fixed formatter helper regresses."
    );

    PROVEN_TEST_SECTION(
        "default/simple fixed formatting",
        "Confirm the policy API returns the same text as the existing formatter for the current fixed-precision path.",
        "Inspect the fixed float formatter helper if a reference string changes unexpectedly.");
    expect_policy_matches_current_fmt(1.0);
    expect_policy_matches_current_fmt(0.5);
    expect_policy_matches_current_fmt(-2.25);
    expect_policy_matches_current_fmt(0.9999995);
    expect_policy_matches_current_fmt(9.9999995e18);
    expect_policy_matches_current_fmt(-DBL_MIN);
    expect_policy_matches_current_fmt(-DBL_TRUE_MIN);
    expect_policy_matches_current_fmt(-DBL_MAX);

    PROVEN_TEST_SECTION(
        "special values",
        "Confirm the policy API preserves the same special-value text as the existing formatter.",
        "Inspect the special-value branch if NaN or infinity text changes.");
    {
        volatile double zero = 0.0;
        expect_policy_exact(PROVEN_FLOAT_FORMAT_POLICY_DEFAULT, proven_float_format_options_fixed_default(), "NaN", zero / zero, "NaN");
        expect_policy_exact(PROVEN_FLOAT_FORMAT_POLICY_DEFAULT, proven_float_format_options_fixed_default(), "+Inf", 1.0 / zero, "Inf");
        expect_policy_exact(PROVEN_FLOAT_FORMAT_POLICY_DEFAULT, proven_float_format_options_fixed_default(), "-Inf", -1.0 / zero, "-Inf");
    }

    PROVEN_TEST_SECTION(
        "shortest mode and invalid policy cases",
        "Confirm shortest-mode requests succeed for RYU and invalid enum values fail explicitly instead of falling through silently.",
        "Inspect the policy dispatch if supported, unsupported, or invalid requests stop returning the documented error codes.");
    {
        char buf[128];
        proven_size_t written = 0;
        proven_float_format_options_t shortest = proven_float_format_options_shortest();
        proven_err_t err = proven_float_format_f64_policy(buf, sizeof buf, 0.1, PROVEN_FLOAT_FORMAT_POLICY_RYU, shortest, &written);
        PROVEN_TEST_ASSERT(err == PROVEN_OK, "RYU shortest should succeed", "Inspect the RYU policy branch if it stops reaching the active shortest backend.");
        PROVEN_TEST_ASSERT(strcmp(buf, "0.1") == 0, "RYU shortest output should be compact", "Inspect the shortest backend if the emitted text changes.");
        err = proven_float_format_f64_policy(buf, sizeof buf, 0.1, (proven_float_format_policy_t)99, proven_float_format_options_fixed_default(), &written);
        PROVEN_TEST_ASSERT(err == PROVEN_ERR_INVALID_ARG, "invalid policy enum should fail", "Inspect the policy enum validation if an out-of-range value stops being rejected.");
        proven_float_format_options_t bad_mode = proven_float_format_options_fixed_default();
        bad_mode.mode = (proven_float_format_mode_t)99;
        err = proven_float_format_f64_policy(buf, sizeof buf, 0.1, PROVEN_FLOAT_FORMAT_POLICY_SIMPLE, bad_mode, &written);
        PROVEN_TEST_ASSERT(err == PROVEN_ERR_INVALID_ARG, "invalid mode enum should fail", "Inspect the mode enum validation if an out-of-range value stops being rejected.");
        err = proven_float_format_f64_policy(buf, 4, 1.0, PROVEN_FLOAT_FORMAT_POLICY_SIMPLE, proven_float_format_options_fixed_default(), &written);
        PROVEN_TEST_ASSERT(err == PROVEN_ERR_OUT_OF_BOUNDS, "too-small buffer should fail", "Inspect the buffer capacity check if a truncated write is accepted.");
    }

    PROVEN_TEST_SECTION(
        "float32 policy parity",
        "Confirm the float32 entry point follows the same shortest and fixed-precision policy behavior.",
        "Inspect the float32 policy shim if it drifts from the float64 helper or stops matching the documented shortest output.");
    {
        char buf[128];
        proven_size_t written = 0;
        proven_err_t err = proven_float_format_f32_policy(buf, sizeof buf, 1.5f, PROVEN_FLOAT_FORMAT_POLICY_SIMPLE, proven_float_format_options_fixed_default(), &written);
        PROVEN_TEST_ASSERT(err == PROVEN_OK, "float32 simple policy should succeed", "Inspect the float32 policy shim if it stops delegating to the fixed formatter helper.");
        PROVEN_TEST_ASSERT(strcmp(buf, "1.500000") == 0, "float32 output should match fixed formatter", "Inspect the float32 helper if the emitted text changes.");
        PROVEN_TEST_ASSERT(written == strlen("1.500000"), "float32 written count should match", "Inspect the float32 helper if the reported length changes.");
        err = proven_float_format_f32_policy(buf, sizeof buf, 1.5f, PROVEN_FLOAT_FORMAT_POLICY_RYU, proven_float_format_options_shortest(), &written);
        PROVEN_TEST_ASSERT(err == PROVEN_OK, "float32 shortest mode should succeed", "Inspect the float32 policy branch if shortest mode stops reaching the active backend.");
        PROVEN_TEST_ASSERT(strcmp(buf, "1.5") == 0, "float32 shortest output should be compact", "Inspect the float32 shortest backend if the emitted text changes.");
    }

    PROVEN_TEST_PASS("Float format policy scaffold checks passed.");
    return 0;
}
