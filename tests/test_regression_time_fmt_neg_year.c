#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * proven_time_u8_fmt and proven_time_u16_fmt are documented as the same formatter differing
 * only in output encoding (include/proven/time.h). They diverged for negative years under a
 * zero-fill spec: {year:0>4} of year -44 rendered as "-044" through the u8 path (which
 * delegates to fmt.h, where the sign counts toward the field width, exactly as printf's
 * %04d) but as "-0044" through the u16 path, whose hand-rolled padding in
 * proven_sys_time_format_int_u16 zero-filled the digits to the full width and only then
 * prepended the sign - one column too wide.
 *
 * The two encodings must agree byte-for-byte (modulo width). This test formats the same
 * datetime with both and asserts equality; the negative-year rows are the ones that were red.
 */

static void decode_u16(const proven_u16str_t *s, char *out, proven_size_t out_cap) {
    const proven_u16 *p = proven_u16str_as_ptr(s);
    proven_size_t n = proven_u16str_len(s), i = 0;
    for (; i < n && i + 1 < out_cap; ++i) out[i] = (char)p[i];
    out[i] = 0;
}

int main(void) {
    PROVEN_TEST_SUITE("time formatting: u8 and u16 agree on zero-filled negative years",
        "The two time formatters are one formatter in two encodings; a zero-fill spec on a negative year must render identically, with the sign counted in the width like printf's %0Nd.",
        "Inspect proven_sys_time_format_int_u16 in platform/proven_sys_time.c: the sign must count toward pad width, matching the u8 path that delegates to fmt.h.");

    proven_allocator_t heap = proven_heap_allocator();

    struct { int year; const char *spec; const char *want; } cases[] = {
        { -44,  "{year:0>4}", "-044"  },   /* the reported divergence */
        { -7,   "{year:0>4}", "-007"  },
        { -123, "{year:0>4}", "-123"  },   /* already full width: no padding, no divergence */
        { -1,   "{year:0>6}", "-00001" },
        {  44,  "{year:0>4}", "0044"  },   /* positive years always agreed; must stay so */
        {  7,   "{year:0>2}", "07"    },
        {  2024,"{year:0>4}", "2024"  },
    };

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("both encodings produce the same string, and it matches the sign-in-width rule",
        "For each year and zero-fill spec, u8_fmt and u16_fmt must be equal, and equal to what printf's %0Nd would print.",
        "The negative rows are the regression; positive rows guard against a fix that breaks the case that already worked.");
    // ---------------------------------------------------------------
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        proven_datetime_t dt = {0};
        dt.year = cases[i].year; dt.month = 1; dt.day = 1;

        proven_result_u8str_t r8 = proven_u8str_create(heap, 32);
        PROVEN_TEST_ASSERT(proven_is_ok(r8.err), "setup u8 string", "");
        proven_u8str_t s8 = r8.value;
        proven_err_t e8 = proven_time_u8_fmt(heap, &s8, dt, NULL, cases[i].spec);
        PROVEN_TEST_ASSERT(proven_is_ok(e8), "u8 format must succeed", "");
        const char *g8 = proven_u8str_as_cstr(&s8);

        proven_result_u16str_t r16 = proven_u16str_create(heap, 32);
        PROVEN_TEST_ASSERT(proven_is_ok(r16.err), "setup u16 string", "");
        proven_u16str_t s16 = r16.value;
        proven_err_t e16 = proven_time_u16_fmt(heap, &s16, dt, NULL, cases[i].spec);
        PROVEN_TEST_ASSERT(proven_is_ok(e16), "u16 format must succeed", "");
        char g16[64];
        decode_u16(&s16, g16, sizeof g16);

        PROVEN_TEST_ASSERT(strcmp(g8, cases[i].want) == 0,
            "the u8 formatter must match the sign-in-width rule",
            "u8 delegates to fmt.h, which already counts the sign toward the field width.");
        PROVEN_TEST_ASSERT(strcmp(g16, cases[i].want) == 0,
            "the u16 formatter must render the same string as u8",
            "This is the regression: the u16 path padded to full width THEN added the sign, one column too wide.");
        PROVEN_TEST_ASSERT(strcmp(g8, g16) == 0,
            "u8 and u16 are one formatter in two encodings; they must never disagree", "");

        proven_u8str_destroy(heap, &s8);
        proven_u16str_destroy(heap, &s16);
    }

    PROVEN_TEST_PASS("negative-year zero-fill renders identically in both encodings, sign counted in the width.");
    return 0;
}
