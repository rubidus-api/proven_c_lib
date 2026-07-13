#include "proven.h"
#include "proven_test.h"
#include <string.h>
#include <stdio.h>

/*
 * Written from the contract in include/proven/time.h, which says both formatters support
 * "formatting specifiers matching fmt.h" (docs/TESTING.md §5.1). proven_time_u8_fmt honours
 * the whole {} grammar because it delegates each field to fmt.h; proven_time_u16_fmt hand-
 * rolled its own parser that recognised only zero-fill ":0>N" and SILENTLY DROPPED every
 * other fill/align/width spec - so {month:>4} came back as "3", not "   3", and {Weekday:>12}
 * ignored its width entirely. Silently discarding a spec and emitting the wrong-width string
 * is exactly the "quiet wrong answer" this library exists to refuse.
 *
 * The contract, stated as a property: for every datetime and every format string, the two
 * formatters must produce the SAME text (u16 being u8 widened to code units, since all time
 * output - digits, ASCII locale names, fill characters - is single-byte). This test drives a
 * broad matrix of specs through both and asserts byte-for-byte equality. The non-zero-fill
 * rows are the ones that land red against the hand-rolled parser.
 */

static void decode_u16(const proven_u16str_t *s, char *out, proven_size_t out_cap) {
    const proven_u16 *p = proven_u16str_as_ptr(s);
    proven_size_t n = proven_u16str_len(s), i = 0;
    for (; i < n && i + 1 < out_cap; ++i) out[i] = (char)p[i];
    out[i] = 0;
}

static void check(proven_allocator_t heap, proven_datetime_t dt, const char *fmt) {
    proven_result_u8str_t r8 = proven_u8str_create(heap, 64);
    proven_result_u16str_t r16 = proven_u16str_create(heap, 64);
    PROVEN_TEST_ASSERT(proven_is_ok(r8.err) && proven_is_ok(r16.err), "setup strings", "");
    proven_u8str_t s8 = r8.value;
    proven_u16str_t s16 = r16.value;

    proven_err_t e8 = proven_time_u8_fmt(heap, &s8, dt, &proven_time_locale_en, fmt);
    proven_err_t e16 = proven_time_u16_fmt(heap, &s16, dt, &proven_time_locale_en, fmt);

    PROVEN_TEST_ASSERT(e8 == e16,
        "the two formatters must agree on success or failure for the same input", "");

    if (proven_is_ok(e8)) {
        const char *g8 = proven_u8str_as_cstr(&s8);
        char g16[128];
        decode_u16(&s16, g16, sizeof g16);
        PROVEN_TEST_ASSERT(strcmp(g8, g16) == 0,
            "u16 output must equal u8 output widened to code units",
            "The u16 formatter dropped a fill/align/width spec the u8 formatter honoured.");
    }

    proven_u8str_destroy(heap, &s8);
    proven_u16str_destroy(heap, &s16);
}

int main(void) {
    PROVEN_TEST_SUITE("time formatting: u16 matches u8 across the whole fmt.h spec grammar",
        "The two time formatters are one formatter in two encodings; for every field and every fill/align/width spec, the u16 output must equal the u8 output widened to code units - not a wrong-width string from a spec silently dropped.",
        "Inspect proven_time_u16_fmt in src/proven/time.c: it must apply the same fmt.h spec grammar the u8 path does, for numeric AND named fields.");

    proven_allocator_t heap = proven_heap_allocator();

    proven_datetime_t dts[3];
    proven_datetime_t a = {0}; a.year = 2024; a.month = 3; a.day = 5; a.hour = 9; a.min = 7; a.sec = 2; a.ms = 42; a.weekday = 2; dts[0] = a;
    proven_datetime_t b = {0}; b.year = -44; b.month = 12; b.day = 31; b.hour = 23; b.min = 59; b.sec = 58; b.ms = 999; b.weekday = 0; dts[1] = b;
    proven_datetime_t c = {0}; c.year = 7; c.month = 1; c.day = 1; c.hour = 0; c.min = 0; c.sec = 0; c.ms = 0; c.weekday = 6; dts[2] = c;

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("numeric fields: every fill/align/width spec renders identically",
        "Zero-fill already worked; right-align, centre, left-align, and plain width are the ones the hand-rolled parser dropped.",
        "");
    // ---------------------------------------------------------------
    {
        const char *specs[] = {
            "{month}", "{month:0>2}", "{month:0>4}", "{month:>4}", "{month:<4}",
            "{month:^5}", "{month:*^5}", "{month:.<6}", "{year:0>6}", "{year:>8}",
            "{hour:0>2}:{min:0>2}:{sec:0>2}", "{ms:0>3}", "{day:_>4}",
        };
        for (size_t d = 0; d < 3; ++d)
            for (size_t i = 0; i < sizeof specs / sizeof specs[0]; ++i)
                check(heap, dts[d], specs[i]);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("named fields honour alignment and width too",
        "A month or weekday name is a rendered token like any other; {Weekday:>12} must pad it, not ignore the spec.",
        "The u16 path appended the name with no spec applied at all.");
    // ---------------------------------------------------------------
    {
        const char *specs[] = {
            "{Weekday}", "{Weekday:>12}", "{Weekday:<12}", "{Weekday:^13}", "{Weekday:*^13}",
            "{Month:>10}", "{Month:.<10}", "{mon} {wday}", "{mon:>5}", "{wday:_^7}",
        };
        for (size_t d = 0; d < 3; ++d)
            for (size_t i = 0; i < sizeof specs / sizeof specs[0]; ++i)
                check(heap, dts[d], specs[i]);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("literal text, escaping, and mixed layouts stay in lockstep",
        "The parity must hold for whole realistic format strings, not just isolated fields.",
        "");
    // ---------------------------------------------------------------
    {
        const char *specs[] = {
            "{year}-{month:0>2}-{day:0>2} {Weekday}",
            "[{hour:0>2}:{min:0>2}:{sec:0>2}.{ms:0>3}]",
            "{{literal braces}} and {Month:*^10}!",
            "no fields at all",
            "{unknown:0>4} stays literal",
            "{Weekday:>12} | {month:0>2}/{day:0>2}",
        };
        for (size_t d = 0; d < 3; ++d)
            for (size_t i = 0; i < sizeof specs / sizeof specs[0]; ++i)
                check(heap, dts[d], specs[i]);
    }

    PROVEN_TEST_PASS("u16 time formatting matches u8 across the whole spec grammar, for numeric and named fields alike.");
    return 0;
}
