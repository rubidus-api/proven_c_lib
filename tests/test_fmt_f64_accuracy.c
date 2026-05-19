#include "proven/fmt.h"
#include "proven/heap.h"
#include "proven_test.h"
#include <string.h>

static void expect_fmt_exact(proven_allocator_t alloc, const char *label, double value, const char *expected) {
    proven_result_u8str_t res = proven_u8str_create(alloc, 8);
    proven_u8str_t str = res.value;

    PROVEN_TEST_ASSERT(PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, "{}", PROVEN_ARG(value))), label, "Inspect PROVEN_ARG_F64 formatting and carry handling.");
    PROVEN_TEST_ASSERT(strcmp(proven_u8str_as_cstr(&str), expected) == 0, label, "Inspect float digit extraction, rounding, and scientific normalization.");

    proven_u8str_destroy(alloc, &str);
}

static void expect_fmt_special(proven_allocator_t alloc, const char *label, double value, const char *expected) {
    proven_result_u8str_t res = proven_u8str_create(alloc, 8);
    proven_u8str_t str = res.value;

    PROVEN_TEST_ASSERT(PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, "{}", PROVEN_ARG(value))), label, "Inspect NaN and infinity formatting paths.");
    PROVEN_TEST_ASSERT(strcmp(proven_u8str_as_cstr(&str), expected) == 0, label, "Inspect special floating-point output and sign handling.");

    proven_u8str_destroy(alloc, &str);
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();
    volatile double zero = 0.0;

    PROVEN_TEST_SUITE(
        "test_fmt_f64_accuracy",
        "Validate float formatting for fixed precision, carry into the integer part, scientific normalization, and special values.",
        "Inspect PROVEN_ARG_F64 digit extraction and the scientific branch if any formatted value drifts from the documented output."
    );

    PROVEN_TEST_SECTION(
        "fixed precision",
        "Confirm that the normal path prints exactly six fractional digits with round-half-up behavior.",
        "Inspect the integer/fraction split and zero-padding logic if a value emits the wrong width or misses a carry.");
    expect_fmt_exact(alloc, "1.0", 1.0, "1.000000");
    expect_fmt_exact(alloc, "0.5", 0.5, "0.500000");
    expect_fmt_exact(alloc, "-2.25", -2.25, "-2.250000");
    expect_fmt_exact(alloc, "0.9999995 carry", 0.9999995, "1.000000");
    expect_fmt_exact(alloc, "0.9999994 no carry", 0.9999994, "0.999999");
    expect_fmt_exact(alloc, "123.4567899 rounding", 123.4567899, "123.456790");

    PROVEN_TEST_SECTION(
        "scientific carry",
        "Confirm that values rendered in scientific notation still carry a rounded mantissa into the exponent when needed.",
        "Inspect the scientific normalization loop and mantissa rounding path if a near-10 value prints a malformed digit string.");
    expect_fmt_exact(alloc, "scientific carry", 9.9999995e18, "9.999999e+18");

    PROVEN_TEST_SECTION(
        "special values",
        "Confirm that NaN and infinities keep their existing textual forms.",
        "Inspect the special-value branch if these outputs change.");
    expect_fmt_special(alloc, "NaN", zero / zero, "NaN");
    expect_fmt_special(alloc, "+Inf", 1.0 / zero, "Inf");
    expect_fmt_special(alloc, "-Inf", -1.0 / zero, "-Inf");

    PROVEN_TEST_PASS("Float formatter accuracy checks passed.");
    return 0;
}
