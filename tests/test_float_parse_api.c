#include "proven/float_parse.h"
#include "proven_test.h"
#include "../src/proven/float_decimal.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#if !defined(PROVEN_FREESTANDING)
#include <errno.h>
#endif

static uint64_t double_bits(double value) {
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static void expect_parse_ok_bits(const char *label, const char *text, proven_size_t consumed, double expected) {
    proven_parse_double_result_t res = proven_parse_double_ascii(proven_u8str_view_from_cstr(text));
    PROVEN_TEST_ASSERT(res.err == PROVEN_OK, label, "Inspect the locale-free ASCII parser if a valid token stops parsing.");
    PROVEN_TEST_ASSERT(res.consumed == consumed, label, "Inspect consumed-length bookkeeping if the parser stops at the wrong byte.");
    PROVEN_TEST_ASSERT(double_bits(res.val) == double_bits(expected), label, "Inspect decimal-to-binary64 rounding if the parsed value drifts.");
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_float_parse_api",
        "Verify the public ASCII float parser and strtod-like wrapper expose consumed-length, endptr, and range-error behavior over the shared exact backend.",
        "Inspect include/proven/float_parse.h, src/proven/float_parse.c, and src/proven/float_decimal.c if the RFC parser seam or wrapper contract drifts."
    );

    PROVEN_TEST_SECTION(
        "ascii parser",
        "Confirm the locale-free parser reports consumed length, special values, and malformed input without skipping leading whitespace.",
        "Inspect proven_parse_double_ascii if consumed length or no-whitespace behavior regresses."
    );
    expect_parse_ok_bits("simple decimal", "3.14", 4u, strtod("3.14", NULL));
    expect_parse_ok_bits("trailing junk", "123abc", 3u, 123.0);
    expect_parse_ok_bits("inf", "inf", 3u, strtod("inf", NULL));
    expect_parse_ok_bits("nan payload", "-nan(payload)", 13u, -strtod("nan", NULL));
    /*
     * A dangling exponent marker is not part of the number. Match strtod:
     * accept the mantissa and stop at the `e`, rather than rejecting the token.
     */
    expect_parse_ok_bits("dangling exponent sign", "1e+", 1u, strtod("1e+", NULL));
    expect_parse_ok_bits("dangling exponent", "1e", 1u, strtod("1e", NULL));
    expect_parse_ok_bits("dangling exponent minus", "3e-", 1u, strtod("3e-", NULL));
    expect_parse_ok_bits("exponent letter junk", "1.5eZ", 3u, strtod("1.5eZ", NULL));
    {
        proven_parse_double_result_t res = proven_parse_double_ascii(proven_u8str_view_from_cstr(" 1"));
        PROVEN_TEST_ASSERT(res.err == PROVEN_ERR_INVALID_ARG, "leading whitespace rejected", "Inspect the ASCII parser if it starts skipping whitespace instead of leaving that policy to the wrapper.");
        PROVEN_TEST_ASSERT(res.consumed == 0u, "leading whitespace consumed", "Inspect failure-path consumed bookkeeping if invalid input advances.");
    }

    PROVEN_TEST_SECTION(
        "strtod wrapper",
        "Confirm the wrapper skips leading whitespace, updates endptr, and reports overflow and underflow in the hosted errno path.",
        "Inspect proven_strtod if whitespace handling, endptr placement, or range signaling drifts."
    );
    {
        const char *text = " \t-12.5xyz";
        char *end = NULL;
        double value = proven_strtod(text, &end);
        PROVEN_TEST_ASSERT(double_bits(value) == double_bits(-12.5), "wrapper whitespace parse", "Inspect wrapper whitespace skipping or sign handling if the parsed value drifts.");
        PROVEN_TEST_ASSERT(end == text + 7, "wrapper endptr", "Inspect endptr bookkeeping if the wrapper stops at the wrong byte.");
    }
    {
        const char *text = "e10";
        char *end = (char *)1;
        double value = proven_strtod(text, &end);
        PROVEN_TEST_ASSERT(double_bits(value) == double_bits(0.0), "wrapper no conversion value", "Inspect no-conversion behavior if the wrapper returns a non-zero value.");
        PROVEN_TEST_ASSERT(end == text, "wrapper no conversion endptr", "Inspect no-conversion behavior if endptr advances.");
    }
#if !defined(PROVEN_FREESTANDING)
    {
        const char *text = "1e309tail";
        char *end = NULL;
        errno = 0;
        double value = proven_strtod(text, &end);
        PROVEN_TEST_ASSERT(double_bits(value) == double_bits(strtod("inf", NULL)), "wrapper overflow value", "Inspect overflow handling if the wrapper stops returning signed infinity.");
        PROVEN_TEST_ASSERT(end == text + 5, "wrapper overflow endptr", "Inspect overflow endptr bookkeeping if the wrapper stops at the wrong byte.");
        PROVEN_TEST_ASSERT(errno == ERANGE, "wrapper overflow errno", "Inspect hosted range signaling if overflow stops setting ERANGE.");
    }
    {
        const char *text = "-1e-9999";
        char *end = NULL;
        errno = 0;
        double value = proven_strtod(text, &end);
        PROVEN_TEST_ASSERT(double_bits(value) == double_bits(-0.0), "wrapper underflow value", "Inspect underflow handling if signed zero is not preserved.");
        PROVEN_TEST_ASSERT(end == text + 8, "wrapper underflow endptr", "Inspect underflow endptr bookkeeping if the wrapper stops at the wrong byte.");
        PROVEN_TEST_ASSERT(errno == ERANGE, "wrapper underflow errno", "Inspect hosted range signaling if underflow stops setting ERANGE.");
    }
#endif

    PROVEN_TEST_SECTION(
        "backend path metrics",
        "Confirm representative inputs can be observed hitting the small Clinger path, the Eisel-Lemire layer, and the exact bigint fallback.",
        "Inspect src/proven/float_decimal.c if the decimal conversion counters stop distinguishing fast paths from the exact fallback."
    );
    {
        proven_float_decimal_stats_t stats = {0};

        proven_float_decimal_reset_stats();
        (void)proven_parse_double_ascii(proven_u8str_view_from_cstr("3.14"));
        proven_float_decimal_get_stats(&stats);
        PROVEN_TEST_ASSERT(stats.total_conversions == 1u, "metrics total after clinger", "Inspect conversion entry counting if a valid decimal stops incrementing the total counter.");
        PROVEN_TEST_ASSERT(stats.clinger_fast_path_hits == 1u, "metrics clinger hit", "Inspect the small exact-range fast path if a representative short decimal stops reaching it.");
        PROVEN_TEST_ASSERT(stats.eisel_lemire_fast_path_hits == 0u, "metrics no eisel-lemire on clinger", "Inspect fast-path accounting if the Eisel-Lemire layer is counted for a simple decimal.");
        PROVEN_TEST_ASSERT(stats.exact_fallback_hits == 0u, "metrics no fallback on clinger", "Inspect fallback accounting if a simple decimal stops staying on the first fast path.");

        proven_float_decimal_reset_stats();
        (void)proven_parse_double_ascii(proven_u8str_view_from_cstr("1844674407370955161e27"));
        proven_float_decimal_get_stats(&stats);
        PROVEN_TEST_ASSERT(stats.total_conversions == 1u, "metrics total after eisel-lemire", "Inspect conversion entry counting if an Eisel-Lemire candidate stops incrementing the total counter.");
        PROVEN_TEST_ASSERT(stats.clinger_fast_path_hits == 0u, "metrics no clinger on eisel-lemire", "Inspect Clinger gating if a large positive-exponent integer starts being misclassified as a small exact-range decimal.");
        PROVEN_TEST_ASSERT(stats.eisel_lemire_fast_path_hits == 1u, "metrics eisel-lemire hit", "Inspect the Eisel-Lemire layer if a representative large decimal integer stops reaching it.");
        PROVEN_TEST_ASSERT(stats.exact_fallback_hits == 0u, "metrics no fallback on eisel-lemire", "Inspect fast-path certainty checks if a representative Eisel-Lemire case starts falling through.");

        proven_float_decimal_reset_stats();
        expect_parse_ok_bits("positive exponent generated u128 subset", "1e40", 4u, strtod("1e40", NULL));
        proven_float_decimal_get_stats(&stats);
        PROVEN_TEST_ASSERT(stats.total_conversions == 1u, "metrics total after positive-exponent u128 subset", "Inspect conversion entry counting if a generated-u128 positive exponent candidate stops incrementing the total counter.");
        PROVEN_TEST_ASSERT(stats.clinger_fast_path_hits == 0u, "metrics no clinger on positive-exponent u128 subset", "Inspect Clinger exponent limits if a wide positive exponent starts being misclassified as a small exact-range decimal.");
        PROVEN_TEST_ASSERT(stats.eisel_lemire_fast_path_hits == 1u, "metrics positive-exponent u128 eisel-lemire hit", "Inspect the generated-u128 Eisel-Lemire path if a wide positive exponent stops reaching it.");
        PROVEN_TEST_ASSERT(stats.eisel_lemire_product_plan_hits == 1u, "metrics positive-exponent product-plan hit", "Inspect cached-power product-plan routing if a representative positive exponent stops finishing through the staged product path.");
        PROVEN_TEST_ASSERT(stats.exact_fallback_hits == 0u, "metrics no fallback on positive-exponent u128 subset", "Inspect the widened positive-exponent Eisel-Lemire path if a generated-u128 case starts falling through.");

        proven_float_decimal_reset_stats();
        expect_parse_ok_bits("negative exponent eisel-lemire subset", "11920928955078125e-23", 21u, strtod("11920928955078125e-23", NULL));
        proven_float_decimal_get_stats(&stats);
        PROVEN_TEST_ASSERT(stats.total_conversions == 1u, "metrics total after negative-exponent eisel-lemire", "Inspect conversion entry counting if an exact negative-exponent subset stops incrementing the total counter.");
        PROVEN_TEST_ASSERT(stats.clinger_fast_path_hits == 0u, "metrics no clinger on negative-exponent eisel-lemire", "Inspect Clinger range limits if a large negative exponent starts being misclassified as a small exact-range decimal.");
        PROVEN_TEST_ASSERT(stats.eisel_lemire_fast_path_hits == 1u, "metrics negative-exponent eisel-lemire hit", "Inspect the Eisel-Lemire layer if the exact negative-exponent subset stops reaching it.");
        PROVEN_TEST_ASSERT(stats.eisel_lemire_product_plan_hits == 1u, "metrics negative-exponent product-plan hit", "Inspect reciprocal cached-power routing if a representative negative exponent stops finishing through the staged product family.");
        PROVEN_TEST_ASSERT(stats.exact_fallback_hits == 0u, "metrics no fallback on negative-exponent eisel-lemire", "Inspect negative-exponent certainty handling if an exact power-of-two subset starts falling through.");

        proven_float_decimal_reset_stats();
        expect_parse_ok_bits("negative exponent rounded ratio subset", "9007199254740993e-1", 19u, strtod("9007199254740993e-1", NULL));
        proven_float_decimal_get_stats(&stats);
        PROVEN_TEST_ASSERT(stats.total_conversions == 1u, "metrics total after negative-exponent rounded ratio", "Inspect conversion entry counting if a rounded negative-exponent ratio stops incrementing the total counter.");
        PROVEN_TEST_ASSERT(stats.clinger_fast_path_hits == 0u, "metrics no clinger on negative-exponent rounded ratio", "Inspect Clinger range limits if a large negative exponent ratio starts being misclassified as a small exact-range decimal.");
        PROVEN_TEST_ASSERT(stats.eisel_lemire_fast_path_hits == 1u, "metrics negative-exponent rounded ratio eisel-lemire hit", "Inspect the Eisel-Lemire layer if the rounded negative-exponent ratio subset stops reaching it.");
        PROVEN_TEST_ASSERT(stats.exact_fallback_hits == 0u, "metrics no fallback on negative-exponent rounded ratio", "Inspect negative-exponent ratio certainty handling if a rounded subset case starts falling through.");

        proven_float_decimal_reset_stats();
        expect_parse_ok_bits("negative exponent wide-shift ratio subset", "1e-27", 5u, strtod("1e-27", NULL));
        proven_float_decimal_get_stats(&stats);
        PROVEN_TEST_ASSERT(stats.total_conversions == 1u, "metrics total after negative-exponent wide shift", "Inspect conversion entry counting if a wide-shift negative exponent ratio stops incrementing the total counter.");
        PROVEN_TEST_ASSERT(stats.clinger_fast_path_hits == 0u, "metrics no clinger on negative-exponent wide shift", "Inspect Clinger range limits if a wide-shift negative exponent starts being misclassified as a small exact-range decimal.");
        PROVEN_TEST_ASSERT(stats.eisel_lemire_fast_path_hits == 1u, "metrics negative-exponent wide shift eisel-lemire hit", "Inspect the Eisel-Lemire layer if a wide-shift negative exponent ratio stops reaching it.");
        PROVEN_TEST_ASSERT(stats.exact_fallback_hits == 0u, "metrics no fallback on negative-exponent wide shift", "Inspect the __uint128_t negative-ratio path if a wide-shift normal-range case starts falling through.");

        proven_float_decimal_reset_stats();
        expect_parse_ok_bits("negative exponent generated u128 ratio subset", "1e-30", 5u, strtod("1e-30", NULL));
        proven_float_decimal_get_stats(&stats);
        PROVEN_TEST_ASSERT(stats.total_conversions == 1u, "metrics total after negative-exponent u128 ratio", "Inspect conversion entry counting if a generated-u128 negative exponent candidate stops incrementing the total counter.");
        PROVEN_TEST_ASSERT(stats.clinger_fast_path_hits == 0u, "metrics no clinger on negative-exponent u128 ratio", "Inspect Clinger exponent limits if a wide negative exponent starts being misclassified as a small exact-range decimal.");
        PROVEN_TEST_ASSERT(stats.eisel_lemire_fast_path_hits == 1u, "metrics negative-exponent u128 ratio eisel-lemire hit", "Inspect the generated-u128 negative-exponent Eisel-Lemire path if a wide ratio case stops reaching it.");
        PROVEN_TEST_ASSERT(stats.exact_fallback_hits == 0u, "metrics no fallback on negative-exponent u128 ratio", "Inspect the widened negative-exponent ratio path if a generated-u128 case starts falling through.");

        proven_float_decimal_reset_stats();
        expect_parse_ok_bits("negative exponent deep wide-shift ratio subset", "1e-40", 5u, strtod("1e-40", NULL));
        proven_float_decimal_get_stats(&stats);
        PROVEN_TEST_ASSERT(stats.total_conversions == 1u, "metrics total after negative-exponent deep wide shift", "Inspect conversion entry counting if a deep wide-shift negative exponent ratio stops incrementing the total counter.");
        PROVEN_TEST_ASSERT(stats.clinger_fast_path_hits == 0u, "metrics no clinger on negative-exponent deep wide shift", "Inspect Clinger exponent limits if a deep wide-shift negative exponent starts being misclassified as a small exact-range decimal.");
        PROVEN_TEST_ASSERT(stats.eisel_lemire_fast_path_hits == 1u, "metrics negative-exponent deep wide shift eisel-lemire hit", "Inspect the u256-backed Eisel-Lemire ratio path if a deep wide-shift negative exponent stops reaching it.");
        PROVEN_TEST_ASSERT(stats.exact_fallback_hits == 0u, "metrics no fallback on negative-exponent deep wide shift", "Inspect the u256-backed negative ratio path if a deep wide-shift case starts falling through.");

        proven_float_decimal_reset_stats();
        expect_parse_ok_bits("negative exponent reciprocal cache subset", "1e-100", 6u, strtod("1e-100", NULL));
        proven_float_decimal_get_stats(&stats);
        PROVEN_TEST_ASSERT(stats.total_conversions == 1u, "metrics total after reciprocal cache subset", "Inspect conversion entry counting if a wider reciprocal-cache negative exponent candidate stops incrementing the total counter.");
        PROVEN_TEST_ASSERT(stats.clinger_fast_path_hits == 0u, "metrics no clinger on reciprocal cache subset", "Inspect Clinger exponent limits if a wide reciprocal-cache negative exponent starts being misclassified as a small exact-range decimal.");
        PROVEN_TEST_ASSERT(stats.eisel_lemire_fast_path_hits == 1u, "metrics reciprocal cache eisel-lemire hit", "Inspect the generated reciprocal-cache Eisel-Lemire path if a wider negative exponent stops reaching it.");
        PROVEN_TEST_ASSERT(stats.exact_fallback_hits == 0u, "metrics no fallback on reciprocal cache subset", "Inspect the generated reciprocal-cache negative exponent path if a wider case starts falling through.");

        proven_float_decimal_reset_stats();
        expect_parse_ok_bits("zero exponent eisel-lemire integer", "9007199254740993", 16u, strtod("9007199254740993", NULL));
        proven_float_decimal_get_stats(&stats);
        PROVEN_TEST_ASSERT(stats.total_conversions == 1u, "metrics total after zero-exponent eisel-lemire", "Inspect conversion entry counting if a zero-exponent integer candidate stops incrementing the total counter.");
        PROVEN_TEST_ASSERT(stats.clinger_fast_path_hits == 0u, "metrics no clinger on zero-exponent eisel-lemire", "Inspect Clinger exactness guards if a tie-to-even boundary integer starts slipping through the small fast path.");
        PROVEN_TEST_ASSERT(stats.eisel_lemire_fast_path_hits == 1u, "metrics zero-exponent eisel-lemire hit", "Inspect the Eisel-Lemire candidate validator if a zero-exponent integer stops reaching the staged fast path.");
        PROVEN_TEST_ASSERT(stats.exact_fallback_hits == 0u, "metrics no fallback on zero-exponent eisel-lemire", "Inspect the Eisel-Lemire midpoint validator if a tie-to-even boundary integer starts falling through.");

        proven_float_decimal_reset_stats();
        expect_parse_ok_bits("positive exponent scaled cache subset", "1e100", 5u, strtod("1e100", NULL));
        proven_float_decimal_get_stats(&stats);
        PROVEN_TEST_ASSERT(stats.total_conversions == 1u, "metrics total after positive scaled cache subset", "Inspect conversion entry counting if a scaled-cache positive exponent candidate stops incrementing the total counter.");
        PROVEN_TEST_ASSERT(stats.clinger_fast_path_hits == 0u, "metrics no clinger on positive scaled cache subset", "Inspect Clinger range limits if a wide positive exponent starts being misclassified as a small exact-range decimal.");
        PROVEN_TEST_ASSERT(stats.eisel_lemire_fast_path_hits == 1u, "metrics positive scaled cache eisel-lemire hit", "Inspect the generated scaled-cache Eisel-Lemire path if a wider positive exponent stops reaching it.");
        PROVEN_TEST_ASSERT(stats.exact_fallback_hits == 0u, "metrics no fallback on positive scaled cache subset", "Inspect the generated scaled-cache positive exponent path if a wider case starts falling through.");

        proven_float_decimal_reset_stats();
        expect_parse_ok_bits("subnormal staged eisel-lemire", "5e-324", 6u, strtod("5e-324", NULL));
        proven_float_decimal_get_stats(&stats);
        PROVEN_TEST_ASSERT(stats.total_conversions == 1u, "metrics total after staged subnormal", "Inspect conversion entry counting if a staged subnormal candidate stops incrementing the total counter.");
        PROVEN_TEST_ASSERT(stats.clinger_fast_path_hits == 0u, "metrics no clinger on staged subnormal", "Inspect Clinger range limits if the true-min subnormal starts being misclassified as a small exact-range decimal.");
        PROVEN_TEST_ASSERT(stats.eisel_lemire_fast_path_hits == 1u, "metrics staged subnormal eisel-lemire hit", "Inspect cached-power candidate packing if the staged layer stops accepting the smallest subnormal.");
        PROVEN_TEST_ASSERT(stats.eisel_lemire_product_plan_hits == 1u, "metrics staged subnormal product-plan hit", "Inspect cached-power product-plan routing if the staged true-min subnormal stops finishing through the shared product family.");
        PROVEN_TEST_ASSERT(stats.exact_fallback_hits == 0u, "metrics no fallback on staged subnormal", "Inspect staged subnormal packing or midpoint validation if the smallest subnormal starts falling through.");

        proven_float_decimal_reset_stats();
        expect_parse_ok_bits("below-half true-min fallback", "2.4703282292062327e-324", 23u, strtod("2.4703282292062327e-324", NULL));
        proven_float_decimal_get_stats(&stats);
        PROVEN_TEST_ASSERT(stats.total_conversions == 1u, "metrics total after below-half true-min fallback", "Inspect conversion entry counting if a below-half true-min case stops incrementing the total counter.");
        PROVEN_TEST_ASSERT(stats.clinger_fast_path_hits == 0u, "metrics no clinger on below-half true-min fallback", "Inspect Clinger range limits if a below-half true-min case starts being misclassified as a small exact-range decimal.");
        PROVEN_TEST_ASSERT(stats.eisel_lemire_fast_path_hits == 0u, "metrics no eisel-lemire on below-half true-min fallback", "Inspect staged subnormal certainty rules if a below-half true-min case stops deferring to exact fallback.");
        PROVEN_TEST_ASSERT(stats.exact_fallback_hits == 1u, "metrics fallback on below-half true-min", "Inspect exact fallback accounting if a below-half true-min case stops reaching the bigint path.");

        proven_float_decimal_reset_stats();
        expect_parse_ok_bits("fallback long significand", "123456789012345678901e40", 24u, strtod("123456789012345678901e40", NULL));
        proven_float_decimal_get_stats(&stats);
        PROVEN_TEST_ASSERT(stats.total_conversions == 1u, "metrics total after fallback", "Inspect conversion entry counting if a long-significand decimal stops incrementing the total counter.");
        PROVEN_TEST_ASSERT(stats.clinger_fast_path_hits == 0u, "metrics no clinger on fallback", "Inspect Clinger range limits if a long-significand decimal starts being misclassified as a small exact-range decimal.");
        PROVEN_TEST_ASSERT(stats.eisel_lemire_fast_path_hits == 0u, "metrics no eisel-lemire on fallback", "Inspect Eisel-Lemire gating if a long-significand decimal stops falling through.");
        PROVEN_TEST_ASSERT(stats.eisel_lemire_product_plan_hits == 0u, "metrics no product-plan hit on fallback", "Inspect staged success accounting if a long-significand fallback case is still being counted as a product-plan success.");
        PROVEN_TEST_ASSERT(stats.exact_fallback_hits == 1u, "metrics fallback hit", "Inspect exact fallback accounting if a long-significand decimal stops reaching the bigint path.");
    }

    PROVEN_TEST_PASS("Float parse API checks passed.");
    return 0;
}
