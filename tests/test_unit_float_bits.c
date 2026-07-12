#include "proven_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include "../src/proven/float_decimal.h"

static void require_impl(bool cond, const char *cond_text, const char *msg, const char *file, int line) {
    if (!cond) {
        fprintf(stderr, "\n[PROVEN][CHECK][FAIL] file=%s line=%d\n", file, line);
        fprintf(stderr, "[PROVEN][CHECK][COND] %s\n", cond_text);
        fprintf(stderr, "[PROVEN][CHECK][INTENT] %s\n", msg);
        fprintf(stderr, "[PROVEN][CHECK][FAIL_HINT] Inspect the shared float bit helpers in src/proven/float_decimal.c and keep them using safe byte copies rather than unions or pointer casts.\n");
        exit(1);
    }
}

#define require(cond, msg) require_impl((cond), #cond, (msg), __FILE__, __LINE__)

static float f32_from_bits(uint32_t bits) {
    float v = 0.0f;
    memcpy(&v, &bits, sizeof v);
    return v;
}

static double f64_from_bits(uint64_t bits) {
    double v = 0.0;
    memcpy(&v, &bits, sizeof v);
    return v;
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_unit_float_bits",
        "Verify the internal float bit extraction helpers preserve the raw IEEE-754 bit patterns used by the future formatter and parser helpers.",
        "Inspect src/proven/float_decimal.h and src/proven/float_decimal.c if the bit helpers stop matching the raw object representation."
    );

    PROVEN_TEST_SECTION(
        "f64 bit extraction",
        "Confirm the double helper returns the expected bit patterns for zero, sign, one, infinity, and a NaN payload.",
        "Inspect the safe byte-copy path in proven_float_bits_f64 if any expected bit pattern changes.");
    require(proven_float_bits_f64(f64_from_bits(0x0000000000000000ULL)) == 0x0000000000000000ULL, "+0.0 should keep the all-zero bit pattern");
    require(proven_float_bits_f64(f64_from_bits(0x8000000000000000ULL)) == 0x8000000000000000ULL, "-0.0 should keep the sign bit set");
    require(proven_float_bits_f64(1.0) == 0x3ff0000000000000ULL, "1.0 should map to the canonical double representation");
    require(proven_float_bits_f64(f64_from_bits(0x7ff0000000000000ULL)) == 0x7ff0000000000000ULL, "+Inf should keep the all-ones exponent and zero mantissa");
    require((proven_float_bits_f64(f64_from_bits(0x7ff8000000000001ULL)) & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL, "NaN should keep the all-ones exponent");
    require((proven_float_bits_f64(f64_from_bits(0x7ff8000000000001ULL)) & 0x000fffffffffffffULL) != 0, "NaN should keep a non-zero mantissa");

    PROVEN_TEST_SECTION(
        "f32 bit extraction",
        "Confirm the float helper returns the expected bit patterns for zero, sign, one, infinity, and a NaN payload.",
        "Inspect the safe byte-copy path in proven_float_bits_f32 if any expected bit pattern changes.");
    require(proven_float_bits_f32(f32_from_bits(0x00000000u)) == 0x00000000u, "+0.0f should keep the all-zero bit pattern");
    require(proven_float_bits_f32(f32_from_bits(0x80000000u)) == 0x80000000u, "-0.0f should keep the sign bit set");
    require(proven_float_bits_f32(1.0f) == 0x3f800000u, "1.0f should map to the canonical float representation");
    require(proven_float_bits_f32(f32_from_bits(0x7f800000u)) == 0x7f800000u, "+Inf should keep the all-ones exponent and zero mantissa");
    require((proven_float_bits_f32(f32_from_bits(0x7fc00001u)) & 0x7f800000u) == 0x7f800000u, "NaN should keep the all-ones exponent");
    require((proven_float_bits_f32(f32_from_bits(0x7fc00001u)) & 0x007fffffu) != 0u, "NaN should keep a non-zero mantissa");

    PROVEN_TEST_PASS("Float bit extraction checks passed.");
    return 0;
}
