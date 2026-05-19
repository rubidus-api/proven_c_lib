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
        fprintf(stderr, "[PROVEN][CHECK][FAIL_HINT] Inspect the shared 64x64->128 multiply helper in src/proven/float_decimal.c and keep the implementation bounded and free of UB.\n");
        exit(1);
    }
}

#define require(cond, msg) require_impl((cond), #cond, (msg), __FILE__, __LINE__)

static proven_u128_parts_t ref_mul(proven_u64 a, proven_u64 b) {
#if defined(__SIZEOF_INT128__)
    unsigned __int128 prod = (unsigned __int128)a * (unsigned __int128)b;
    proven_u128_parts_t out;
    out.lo = (proven_u64)prod;
    out.hi = (proven_u64)(prod >> 64);
    return out;
#else
#error "This test expects unsigned __int128 support on the host compiler"
#endif
}

static void check_mul(const char *label, proven_u64 a, proven_u64 b) {
    proven_u128_parts_t expected = ref_mul(a, b);
    proven_u128_parts_t actual = proven_float_mul_u64_u64_to_u128(a, b);
    char msg[256];
    snprintf(msg, sizeof msg, "%s: hi/lo pair should match the reference product", label);
    require(actual.hi == expected.hi, msg);
    require(actual.lo == expected.lo, msg);
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_u128_mul",
        "Verify the shared 64x64 to 128-bit multiply helper returns exact high and low halves for representative operands.",
        "Inspect src/proven/float_decimal.c if the wide multiply helper stops matching the reference product."
    );

    PROVEN_TEST_SECTION(
        "basic vectors",
        "Confirm zero, one, powers of two, and all-ones products are exact.",
        "Inspect the shared multiply helper if a basic vector no longer matches the reference product.");
    check_mul("zero-times-zero", 0u, 0u);
    check_mul("zero-times-one", 0u, 1u);
    check_mul("one-times-one", 1u, 1u);
    check_mul("power-of-two carry", 1ull << 32, 1ull << 32);
    check_mul("all-ones square", 0xffffffffffffffffull, 0xffffffffffffffffull);

    PROVEN_TEST_SECTION(
        "hand-computed vectors",
        "Confirm a few asymmetric values also match the exact 128-bit reference product.",
        "Inspect the helper arithmetic if a hand-computed vector drifts.");
    check_mul("asymmetric one", 0x0123456789abcdefull, 0xfedcba9876543210ull);
    check_mul("asymmetric two", 0x00000000ffffffffull, 0x0000000100000001ull);
    check_mul("asymmetric three", 0x7fffffffffffffffull, 0x0000000200000003ull);

    PROVEN_TEST_PASS("Wide multiply checks passed.");
    return 0;
}
