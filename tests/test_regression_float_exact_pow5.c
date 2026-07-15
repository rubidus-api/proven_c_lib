#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * The exact fallback was comparing against an approximate power of five.
 *
 * The float parser has three tiers: Clinger for the easy cases, Eisel-Lemire for most of
 * the rest, and - when neither can prove its answer - an exact big-integer comparison
 * that decides which way a decimal sitting on a rounding boundary must go. That last
 * tier is the one that makes the library's central promise true: correctly rounded,
 * ties-to-even, bit-identical to a correct strtod (scan.h says so; manual chapter 8 says
 * so).
 *
 * It built 5^q for 56 <= q <= 350 by taking the Eisel-Lemire table entry - a 128-bit
 * mantissa ROUNDED to 128 bits - and shifting it left. 5^q is odd, so that shift can
 * never be exact:
 *
 *     q=55   table ...078124        exact ...078125
 *     q=97   relative error -4.5e-39
 *
 * So the tier whose entire job was exactness compared against a corrupted number. Every
 * exact halfway value in that exponent window broke the tie in a fixed direction instead
 * of to even, and values within ~1e-38 of a boundary came out one ULP wrong - with
 * PROVEN_OK. A differential run against glibc found 2,923 of them.
 *
 * The three inputs below were verified with exact rational arithmetic, not just against
 * a host strtod, so this test says what is TRUE rather than what this machine happens to
 * agree with.
 */

static proven_u64 bits_of(double d) {
    proven_u64 b = 0;
    memcpy(&b, &d, sizeof b);
    return b;
}

static void expect_bits(const char *text, proven_u64 want, const char *what, const char *hint) {
    proven_u8str_view_t v = { .ptr = (const proven_u8 *)text, .size = strlen(text) };
    proven_scan_t s = proven_scan_init(v);
    proven_result_f64_t r = proven_scan_f64(&s);

    PROVEN_TEST_ASSERT(proven_is_ok(r.err), what, "the input must parse at all");
    PROVEN_TEST_ASSERT(bits_of(r.val) == want, what, hint);
}

int main(void) {
    PROVEN_TEST_SUITE("the exact float fallback uses an exact power of five",
        "Correctly rounded, ties-to-even, at every magnitude - including the 56..350 exponent window where the fallback used to compare against a rounded table entry.",
        "Inspect proven_float_bigint_build_pow5_cached in src/proven/float_decimal.c. Above the exact u128 table, 5^q must be MULTIPLIED, not looked up in the Eisel-Lemire table and shifted: that table is rounded, and 5^q is odd.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("an exact halfway value breaks to even",
        "This is the case the exact tier exists for, and the case the rounded table always got wrong.",
        "A tie must go to the even significand. With a corrupted 5^q it went the same way every time - up for a negative exponent, down for a positive one.");
    // ---------------------------------------------------------------
    {
        /* Exactly halfway between two doubles, exp10 = -97: inside the broken window. */
        expect_bits("1.1032184967161746799744147340198131380055836595066587335622898535802960"
                    "39581298828125e-13",
                    0x3d3f0d86ed37a97cull,
                    "an exact tie at exp10=-97 must round to even (…97c)",
                    "It used to give …97d: the tie was decided against a 5^97 that was off by 4.5e-39.");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a value just below a boundary rounds down; just above, up",
        "Not ties - ordinary values close enough to a rounding boundary that only an exact comparison can place them.",
        "Both were one ULP wrong. Neither reported an error: the answer was simply not the number the caller wrote.");
    // ---------------------------------------------------------------
    {
        expect_bits("0.0000000000000581831502950741840306488936917509682239632758105063459197"
                    "4812792614102363586425781249999999999999999999999999999999",
                    0x3d306089aed23bd4ull,
                    "a value below the midpoint at exp10=-130 must round down (…bd4)",
                    "It used to round up to …bd5.");

        expect_bits("34014179048958869370763083896833775022593e60",
                    0x54cf1a2b3c4d5e61ull,
                    "a value above the midpoint at exp10=+60 must round up (…e61)",
                    "It used to round down to …e60. Forty-one digits and an exponent of 60 is not an exotic input.");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the tiers that were already exact still are",
        "|exp10| <= 55 uses the exact u128 table and |exp10| > 350 multiplies. Both were correct before; they must stay correct.",
        "If one of these moved, the fix broke a path that was not broken.");
    // ---------------------------------------------------------------
    {
        /* Exact tie at exp10 = -55: the exact table. */
        expect_bits("0.3614139024208966877171889109376934356987476348876953125",
                    0x3fd72167c6cdeb4cull,
                    "an exact tie at exp10=-55 still rounds correctly",
                    "");

        /* The classic ones: they must be untouched. */
        expect_bits("0.1", 0x3fb999999999999aull, "0.1 still parses to the nearest double", "");
        expect_bits("1e308", 0x7fe1ccf385ebc8a0ull, "1e308 still parses", "");
        expect_bits("5e-324", 0x0000000000000001ull, "the smallest subnormal still parses", "");
    }

    PROVEN_TEST_PASS("the exact fallback is exact.");
    return 0;
}
