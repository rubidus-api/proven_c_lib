#include "proven.h"
#include "proven/numfmt.h"
#include "proven_test.h"
#include <stdint.h>
#include <string.h>

/*
 * numfmt.h (RFC-0058 D-19): numbers written and read with a place's conventions, as a value.
 * Every preset on known numbers; grouping of every length (India's 3 then 2); multi-byte
 * separators; refusal when the buffer is short; correctly rounded decimals; reading back what
 * was written, 100,000 random values per preset.
 */

static bool eq(const proven_byte_t *buf, proven_size_t n, const char *want) { return n == strlen(want) && memcmp(buf, want, n) == 0; }
static uint64_t g_seed = 0x9e3779b97f4a7c15u;
static uint64_t rnd(void) { g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17; return g_seed; }

int main(void) {
    PROVEN_TEST_SUITE("numfmt: numbers with a place's conventions, as a value",
        "RFC-0058 D-19: presets, grouping, multi-byte separators, refusal, decimals, and round trips.",
        "Inspect src/proven/numfmt.c: group_before for the grouping, localize, delocalize.");
    proven_byte_t buf[800];
    proven_size_t n = 0;
    const proven_mem_mut_t out = { buf, sizeof buf };

    PROVEN_TEST_SECTION("writing", "Every preset on 1234567.891 and on -1234567.", "Check group_before's walk over the sizes.");
    {
        bool ok = true;
        ok &= proven_numfmt_f64(&proven_numfmt_en, 1234567.891, 2, out, &n) == PROVEN_OK && eq(buf, n, "1,234,567.89");
        ok &= proven_numfmt_f64(&proven_numfmt_de, 1234567.891, 2, out, &n) == PROVEN_OK && eq(buf, n, "1.234.567,89");
        ok &= proven_numfmt_f64(&proven_numfmt_fr, 1234567.891, 2, out, &n) == PROVEN_OK && eq(buf, n, "1\xe2\x80\xaf" "234\xe2\x80\xaf" "567,89");
        ok &= proven_numfmt_f64(&proven_numfmt_ch, 1234567.891, 1, out, &n) == PROVEN_OK && eq(buf, n, "1\xe2\x80\x99" "234\xe2\x80\x99" "567.9");
        ok &= proven_numfmt_f64(&proven_numfmt_in, 1234567.891, 0, out, &n) == PROVEN_OK && eq(buf, n, "12,34,568");
        ok &= proven_numfmt_f64(&proven_numfmt_c, 1234567.891, 3, out, &n) == PROVEN_OK && eq(buf, n, "1234567.891");
        ok &= proven_numfmt_i64(&proven_numfmt_en, -1234567, out, &n) == PROVEN_OK && eq(buf, n, "-1,234,567");
        ok &= proven_numfmt_i64(&proven_numfmt_in, INT64_MIN, out, &n) == PROVEN_OK && eq(buf, n, "-92,23,37,20,36,85,47,75,808");
        ok &= proven_numfmt_u64(&proven_numfmt_en, UINT64_MAX, out, &n) == PROVEN_OK && eq(buf, n, "18,446,744,073,709,551,615");
        ok &= proven_numfmt_u64(&proven_numfmt_en, 999, out, &n) == PROVEN_OK && eq(buf, n, "999");
        ok &= proven_numfmt_u64(&proven_numfmt_en, 1000, out, &n) == PROVEN_OK && eq(buf, n, "1,000");
        ok &= proven_numfmt_u64(&proven_numfmt_en, 0, out, &n) == PROVEN_OK && eq(buf, n, "0");
        ok &= proven_numfmt_f64(&proven_numfmt_en, 0.125, 2, out, &n) == PROVEN_OK && eq(buf, n, "0.12");     /* a tie, to even */
        ok &= proven_numfmt_f64(&proven_numfmt_de, -0.5, 0, out, &n) == PROVEN_OK && eq(buf, n, "-0");
        ok &= proven_numfmt_f64(&proven_numfmt_en, 1.0 / 0.0, 2, out, &n) == PROVEN_OK && eq(buf, n, "inf");
        const proven_numfmt_t mine = { PROVEN_LIT("."), PROVEN_LIT(" "), { 4 }, PROVEN_LIT("\xe2\x88\x92") };     /* U+2212 minus */
        ok &= proven_numfmt_i64(&mine, -123456789, out, &n) == PROVEN_OK && eq(buf, n, "\xe2\x88\x92" "1 2345 6789");
        PROVEN_TEST_ASSERT(ok, "every preset, grouping of 3 and 3-then-2 and 4, a UTF-8 minus, ties, inf", "");
        proven_byte_t small[5];
        PROVEN_TEST_ASSERT(proven_numfmt_u64(&proven_numfmt_en, 123456, (proven_mem_mut_t){ small, 5 }, &n) == PROVEN_ERR_OUT_OF_BOUNDS
            && proven_numfmt_u64(&proven_numfmt_en, 12345, (proven_mem_mut_t){ small, 5 }, &n) == PROVEN_ERR_OUT_OF_BOUNDS
            && proven_numfmt_u64(&proven_numfmt_c, 12345, (proven_mem_mut_t){ small, 5 }, &n) == PROVEN_OK && n == 5
            && proven_numfmt_f64(&proven_numfmt_c, 1.0, 341, out, &n) == PROVEN_ERR_INVALID_ARG,
            "a short buffer is refused, an exact fit is not; decimals past 340 are refused", "");
    }

    PROVEN_TEST_SECTION("reading", "Known strings, refusals, and round trips of random values.", "Check delocalize: separators only between digits, one decimal mark.");
    {
        proven_i64 v = 0;
        double d = 0;
        proven_size_t used = 0;
        bool ok = true;
        ok &= proven_numfmt_parse_i64(&proven_numfmt_en, PROVEN_LIT("-1,234,567"), NULL, &v) == PROVEN_OK && v == -1234567;
        ok &= proven_numfmt_parse_i64(&proven_numfmt_in, PROVEN_LIT("12,34,567"), NULL, &v) == PROVEN_OK && v == 1234567;
        ok &= proven_numfmt_parse_f64(&proven_numfmt_de, PROVEN_LIT("1.234.567,89"), NULL, &d) == PROVEN_OK && d == (double)1234567.89;   /* the cast drops x87's excess precision (FLT_EVAL_METHOD 2) */
        ok &= proven_numfmt_parse_f64(&proven_numfmt_fr, PROVEN_LIT("1\xe2\x80\xaf" "234,5"), NULL, &d) == PROVEN_OK && d == 1234.5;
        ok &= proven_numfmt_parse_i64(&proven_numfmt_en, PROVEN_LIT("1,234 apples"), &used, &v) == PROVEN_OK && v == 1234 && used == 5;
        ok &= proven_numfmt_parse_i64(&proven_numfmt_en, PROVEN_LIT("1,234,"), &used, &v) == PROVEN_OK && used == 5;   /* a trailing separator is not taken */
        ok &= proven_numfmt_parse_i64(&proven_numfmt_en, PROVEN_LIT(",123"), NULL, &v) == PROVEN_ERR_INVALID_ARG;
        ok &= proven_numfmt_parse_i64(&proven_numfmt_en, PROVEN_LIT("1,234 apples"), NULL, &v) == PROVEN_ERR_INVALID_ARG;
        ok &= proven_numfmt_parse_i64(&proven_numfmt_en, PROVEN_LIT("9,223,372,036,854,775,808"), NULL, &v) == PROVEN_ERR_OVERFLOW;
        ok &= proven_numfmt_parse_f64(&proven_numfmt_de, PROVEN_LIT("3,"), &used, &d) == PROVEN_OK && d == 3.0 && used == 1;   /* a mark with no digit after it is not taken */
        PROVEN_TEST_ASSERT(ok, "group separators, decimal marks, partial reads and refusals", "");
        const proven_numfmt_t *presets[] = { &proven_numfmt_c, &proven_numfmt_en, &proven_numfmt_de, &proven_numfmt_fr, &proven_numfmt_ch, &proven_numfmt_in };
        int bad = 0;
        for (int p = 0; p < 6; p++)
            for (int i = 0; i < 100000; i++) {
                const proven_i64 x = (proven_i64)(rnd() >> (rnd() % 63));
                const proven_i64 s = (i & 1) ? -x : x;
                if (proven_numfmt_i64(presets[p], s, out, &n) != PROVEN_OK || proven_numfmt_parse_i64(presets[p], (proven_u8str_view_t){ buf, n }, NULL, &v) != PROVEN_OK || v != s) bad++;
                const double f = (double)(rnd() % 100000000) / 1000.0 * ((i & 2) ? -1 : 1);
                if (proven_numfmt_f64(presets[p], f, 3, out, &n) != PROVEN_OK || proven_numfmt_parse_f64(presets[p], (proven_u8str_view_t){ buf, n }, NULL, &d) != PROVEN_OK || d != f) bad++;
            }
        PROVEN_TEST_ASSERT(bad == 0, "100,000 integers and 100,000 decimals per preset read back as written", "");
    }
    PROVEN_TEST_PASS("numfmt checks passed.");
    return 0;
}
