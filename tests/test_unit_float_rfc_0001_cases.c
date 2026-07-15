#include "proven/float_parse.h"
#include "proven/float_config.h"
#include "proven_test.h"
#include <float.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint64_t double_bits(double value) {
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static void expect_ascii_bits_host(const char *label, const char *text) {
    proven_parse_double_result_t parsed = proven_parse_double_ascii(proven_u8str_view_from_cstr(text));
    char *end = NULL;
    double host = strtod(text, &end);

    PROVEN_TEST_ASSERT(parsed.err == PROVEN_OK, label, "Inspect the locale-free parser if a valid RFC decimal case stops parsing.");
    PROVEN_TEST_ASSERT(parsed.consumed == strlen(text), label, "Inspect consumed-length bookkeeping if the ASCII parser stops before the full RFC token.");
    PROVEN_TEST_ASSERT(end != NULL && *end == '\0', label, "Inspect the RFC test corpus if the host oracle leaves trailing characters.");
    PROVEN_TEST_ASSERT(double_bits(parsed.val) == double_bits(host), label, "Inspect decimal-to-binary64 rounding if the RFC parse case drifts from the host oracle.");
}

/* Only the >= 768-digit midpoint corpus uses this exact-bits helper, so it is
 * compiled out together with those cases on reduced-cap embedded builds. */
#if PROVEN_FLOAT_MAX_SIGNIFICAND_DIGITS >= 768u
static void expect_ascii_bits_exact(const char *label, const char *text, double expected) {
    proven_parse_double_result_t parsed = proven_parse_double_ascii(proven_u8str_view_from_cstr(text));

    PROVEN_TEST_ASSERT(parsed.err == PROVEN_OK, label, "Inspect the locale-free parser if an exact RFC decimal case stops parsing.");
    PROVEN_TEST_ASSERT(parsed.consumed == strlen(text), label, "Inspect consumed-length bookkeeping if the ASCII parser stops before the full exact RFC token.");
    PROVEN_TEST_ASSERT(double_bits(parsed.val) == double_bits(expected), label, "Inspect midpoint handling if the exact RFC boundary case rounds to the wrong binary64 value.");
}
#endif

static void expect_strtod_bits_host(const char *label, const char *text, size_t consumed) {
    char *end = NULL;
    char *host_end = NULL;
    double value = proven_strtod(text, &end);
    double host = strtod(text, &host_end);

    PROVEN_TEST_ASSERT(host_end != NULL, label, "Inspect the RFC wrapper corpus if the host oracle does not produce an end pointer.");
    PROVEN_TEST_ASSERT((size_t)(end - text) == consumed, label, "Inspect wrapper endptr bookkeeping if the RFC wrapper stops at the wrong byte.");
    PROVEN_TEST_ASSERT((size_t)(host_end - text) == consumed, label, "Inspect the RFC wrapper corpus if the host oracle stops at a different byte than expected.");
    PROVEN_TEST_ASSERT(double_bits(value) == double_bits(host), label, "Inspect wrapper range or tokenization behavior if the RFC wrapper diverges from the host oracle.");
}

static void expect_ascii_invalid(const char *label, const char *text) {
    proven_parse_double_result_t parsed = proven_parse_double_ascii(proven_u8str_view_from_cstr(text));

    PROVEN_TEST_ASSERT(parsed.err == PROVEN_ERR_INVALID_ARG, label, "Inspect malformed-token rejection if the ASCII parser starts accepting invalid RFC cases.");
    PROVEN_TEST_ASSERT(parsed.consumed == 0u, label, "Inspect malformed-token rollback if the ASCII parser reports a partial conversion on invalid input.");
}

int main(void) {
    static const char midpoint_below[] =
        "24703282292062327208828439643411068618252990130716238221279284125033775363510437593264991818081799618989828234772285886546332835517796989819938739800539093906315035659515570226392290858392449105184435931802849936536152500319370457678249219365623669863658480757001585769269903706311928279558551332927834338409351978015531246597263579574622766465272827220056374006485499977096599470454020828166226237857393450736339007967761930577506740176324673600968951340535537458516661134223766678604162159680461914467291840300530057530849048765391711386591646239524912623653881879636239373280423891018672348497668235089863388587925628302755995657524455507255189313690836254779186948667994968324049705821028513185451396213837722826145437693412532098591327667236328124e-1075";
    static const char midpoint_exact[] =
        "24703282292062327208828439643411068618252990130716238221279284125033775363510437593264991818081799618989828234772285886546332835517796989819938739800539093906315035659515570226392290858392449105184435931802849936536152500319370457678249219365623669863658480757001585769269903706311928279558551332927834338409351978015531246597263579574622766465272827220056374006485499977096599470454020828166226237857393450736339007967761930577506740176324673600968951340535537458516661134223766678604162159680461914467291840300530057530849048765391711386591646239524912623653881879636239373280423891018672348497668235089863388587925628302755995657524455507255189313690836254779186948667994968324049705821028513185451396213837722826145437693412532098591327667236328125e-1075";
    static const char midpoint_above[] =
        "24703282292062327208828439643411068618252990130716238221279284125033775363510437593264991818081799618989828234772285886546332835517796989819938739800539093906315035659515570226392290858392449105184435931802849936536152500319370457678249219365623669863658480757001585769269903706311928279558551332927834338409351978015531246597263579574622766465272827220056374006485499977096599470454020828166226237857393450736339007967761930577506740176324673600968951340535537458516661134223766678604162159680461914467291840300530057530849048765391711386591646239524912623653881879636239373280423891018672348497668235089863388587925628302755995657524455507255189313690836254779186948667994968324049705821028513185451396213837722826145437693412532098591327667236328126e-1075";
    static const char long_sig_110[] =
        "11111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111e-109";

    PROVEN_TEST_SUITE(
        "test_unit_float_rfc_0001_cases",
        "Verify the decimal-to-binary64 rewrite satisfies the explicit RFC-0001 parse, boundary, special-value, and wrapper requirements.",
        "Inspect include/proven/float_parse.h, src/proven/float_parse.c, src/proven/float_decimal.c, and the RFC audit document if a named RFC case fails."
    );

    PROVEN_TEST_SECTION(
        "basic finite values",
        "Confirm the public ASCII parser accepts the RFC basic corpus and matches the host oracle bit-for-bit.",
        "Inspect token parsing or core decimal conversion if a basic finite RFC case drifts."
    );
    expect_ascii_bits_host("zero", "0");
    expect_ascii_bits_host("plus zero", "+0");
    expect_ascii_bits_host("minus zero", "-0");
    expect_ascii_bits_host("one", "1");
    expect_ascii_bits_host("minus one", "-1");
    expect_ascii_bits_host("pi-ish", "3.14");
    expect_ascii_bits_host("positive power of ten", "1e10");
    expect_ascii_bits_host("negative power of ten", "1e-10");

    PROVEN_TEST_SECTION(
        "special values",
        "Confirm the public ASCII parser accepts the RFC special spellings and the wrapper handles signed infinity and NaN spellings.",
        "Inspect special-token matching or sign handling if a special RFC case stops parsing."
    );
    expect_ascii_bits_host("inf", "inf");
    expect_ascii_bits_host("plus inf", "+inf");
    expect_ascii_bits_host("minus inf", "-inf");
    expect_ascii_bits_host("infinity", "infinity");
    expect_ascii_bits_host("nan", "nan");
    expect_ascii_bits_host("signed nan payload", "-nan(payload)");

    PROVEN_TEST_SECTION(
        "integer boundaries",
        "Confirm the parser still matches the RFC 2^53 boundary corpus and ties-to-even integer midpoint.",
        "Inspect midpoint validation or exact fallback if the 2^53 boundary cases drift."
    );
    expect_ascii_bits_host("2^53 minus one", "9007199254740991");
    expect_ascii_bits_host("2^53", "9007199254740992");
    expect_ascii_bits_host("2^53 midpoint ties-even", "9007199254740993");

    PROVEN_TEST_SECTION(
        "binary64 boundaries",
        "Confirm DBL_MAX, DBL_MIN, the normal/subnormal boundary, and the smallest subnormal all match the host oracle.",
        "Inspect overflow, subnormal packing, or exact midpoint comparison if a binary64 boundary case drifts."
    );
    expect_ascii_bits_host("db1 max", "1.7976931348623157e308");
    expect_ascii_bits_host("dbl min normal", "2.2250738585072014e-308");
    expect_ascii_bits_host("largest subnormal", "2.2250738585072009e-308");
    expect_ascii_bits_host("normal boundary plus one ulp", "2.2250738585072015e-308");
    expect_ascii_bits_host("smallest subnormal", "4.9406564584124654e-324");

    PROVEN_TEST_SECTION(
        "midpoint boundaries",
        "Confirm values just below, exactly at, and just above the true-min midpoint obey round-to-nearest, ties-to-even.",
        "Inspect exact midpoint comparison if the half-way true-min threshold rounds incorrectly."
    );
    /*
     * The true-min midpoint expansions are ~750 significant digits. They are
     * exact only when the significand cap can hold a full binary64 rounding
     * boundary (>= 768 digits). Smaller embedded caps stay within one ULP but
     * are not exact for boundaries longer than the cap, so skip them there.
     */
#if PROVEN_FLOAT_MAX_SIGNIFICAND_DIGITS >= 768u
    expect_ascii_bits_exact("true-min midpoint below", midpoint_below, 0.0);
    expect_ascii_bits_exact("true-min midpoint exact ties-even", midpoint_exact, 0.0);
    expect_ascii_bits_exact("true-min midpoint above", midpoint_above, DBL_TRUE_MIN);
#else
    (void)midpoint_below;
    (void)midpoint_exact;
    (void)midpoint_above;
#endif

    PROVEN_TEST_SECTION(
        "long significands and huge exponents",
        "Confirm very long decimal inputs scan safely and still produce the RFC-directed hosted results for finite, overflow, and underflow cases.",
        "Inspect decimal accumulation bounds or early range checks if a very long RFC input misbehaves."
    );
    expect_ascii_bits_host("110-digit significand finite", long_sig_110);
    expect_strtod_bits_host("huge positive exponent overflow", "1e100000tail", 8u);
    expect_strtod_bits_host("huge negative exponent underflow", "-1e-100000tail", 10u);

    PROVEN_TEST_SECTION(
        "wrapper and malformed inputs",
        "Confirm the wrapper and ASCII parser follow the RFC endptr and malformed-input expectations.",
        "Inspect wrapper tokenization or malformed-input rollback if an RFC edge case regresses."
    );
    expect_strtod_bits_host("wrapper trailing junk", "123abc", 3u);
    expect_strtod_bits_host("wrapper whitespace", " \t42xyz", 4u);
    expect_ascii_invalid("bare decimal point", ".");
    expect_ascii_invalid("bare exponent", "e10");
    {
        /*
         * A dangling exponent marker is not part of the number. Match strtod:
         * keep the mantissa and stop at the `e` rather than rejecting the token.
         */
        proven_parse_double_result_t p1 = proven_parse_double_ascii(proven_u8str_view_from_cstr("1e"));
        PROVEN_TEST_ASSERT(p1.err == PROVEN_OK && p1.consumed == 1u && p1.val == 1.0, "dangling exponent keeps mantissa", "Inspect incomplete-exponent backtracking in the ASCII token scanner.");
        proven_parse_double_result_t p2 = proven_parse_double_ascii(proven_u8str_view_from_cstr("1e+"));
        PROVEN_TEST_ASSERT(p2.err == PROVEN_OK && p2.consumed == 1u && p2.val == 1.0, "dangling exponent sign keeps mantissa", "Inspect incomplete-exponent backtracking in the ASCII token scanner.");
    }

    PROVEN_TEST_PASS("RFC-0001 parse audit corpus passed.");
    return 0;
}
