#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * What a project replacing snprintf and printf-style diagnostics needs of the formatter
 * (v0.109.0): `{:.N}` on a string, the count of arguments a format names and what a wrong
 * count does, and proven_fmt_to - snprintf's shape - with output cut only between UTF-8
 * characters.
 */

static bool same(const char *got, const char *want) { return strcmp(got, want) == 0; }

int main(void) {
    PROVEN_TEST_SUITE("fmt: string precision, argument counts, proven_fmt_to",
        "Verify {:.N} on C strings and views keeps at most N bytes and no partial UTF-8 character, with width and alignment on what is kept; proven_fmt_arg_count gives the count a call must pass; proven_fmt_to always ends its text with a NUL, cuts between characters and reports what was needed.",
        "Inspect fmt_utf8_whole and the CSTR/STR_VIEW cases of render_arg, proven_fmt_arg_count, and proven_fmt_to_internal in src/proven/fmt.c.");
    char b[64];
    proven_fmt_result_t r;

    PROVEN_TEST_SECTION("string precision", "printf's %.Ns, cut back to the start of a character it would split.", "Check fmt_utf8_whole and the CSTR and STR_VIEW cases.");
    r = proven_fmt_to(b, sizeof b, "[{:.2}]", PROVEN_ARG("abc"));
    PROVEN_TEST_ASSERT(r.err == PROVEN_OK && same(b, "[ab]"), "{:.2} on \"abc\" is \"ab\"", "");
    r = proven_fmt_to(b, sizeof b, "[{:.5}]", PROVEN_ARG("abc"));
    PROVEN_TEST_ASSERT(r.err == PROVEN_OK && same(b, "[abc]"), "a precision past the end keeps it all", "");
    r = proven_fmt_to(b, sizeof b, "[{:.0}]", PROVEN_ARG("abc"));
    PROVEN_TEST_ASSERT(r.err == PROVEN_OK && same(b, "[]"), "{:.0} keeps nothing", "");
    r = proven_fmt_to(b, sizeof b, "[{:6.2}][{:<6.2}][{:*^6.2}]", PROVEN_ARG("abc"), PROVEN_ARG("abc"), PROVEN_ARG("abc"));
    PROVEN_TEST_ASSERT(r.err == PROVEN_OK && same(b, "[    ab][ab    ][**ab**]"), "width and alignment apply to what is kept, as %6.2s and %-6.2s", "");
    /* "h" "\xc3\xa9" (e acute, 2 bytes) "\xe2\x82\xac" (euro, 3 bytes) "\xf0\x9f\x98\x80" (4 bytes) "z" */
    static const char u[] = "h\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80z";
    static const struct { int n; const char *want; } cut[] = {
        { 1, "h" }, { 2, "h" }, { 3, "h\xc3\xa9" }, { 4, "h\xc3\xa9" }, { 5, "h\xc3\xa9" }, { 6, "h\xc3\xa9\xe2\x82\xac" },
        { 7, "h\xc3\xa9\xe2\x82\xac" }, { 9, "h\xc3\xa9\xe2\x82\xac" }, { 10, "h\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80" }, { 11, u }, { 40, u },
    };
    for (size_t i = 0; i < sizeof cut / sizeof *cut; ++i) {
        char f[16];
        (void)proven_fmt_to(f, sizeof f, "{{:.{}}}", PROVEN_ARG(cut[i].n));
        PROVEN_TEST_ASSERT(proven_fmt_to(b, sizeof b, f, PROVEN_ARG(u)).err == PROVEN_OK && same(b, cut[i].want), f, "A C string cut at N bytes, back to a whole character.");
        proven_u8str_view_t v = { .ptr = (const proven_byte_t *)u, .size = sizeof u - 1 };
        PROVEN_TEST_ASSERT(proven_fmt_to(b, sizeof b, f, PROVEN_ARG(v)).err == PROVEN_OK && same(b, cut[i].want), f, "And a view the same.");
    }
    {
        /* No byte past the N-th is read: a text with no NUL at all. */
        const char raw[3] = { 'x', 'y', 'z' };
        r = proven_fmt_to(b, sizeof b, "{:.3}", PROVEN_ARG(raw));
        PROVEN_TEST_ASSERT(r.err == PROVEN_OK && same(b, "xyz"), "a C string with no NUL inside N is read to N and no further", "");
    }
    {
        /* A text that starts just past the output's end is not the output's own (v0.112.3: gcc
         * 16 laid `raw` above right after `b`, and the self-alias check refused it); one that
         * starts inside it still is. */
        char block[17] = "0123456789abcdef";
        r = proven_fmt_to(block, 8, "{:.3}", PROVEN_ARG((const char *)block + 8));
        PROVEN_TEST_ASSERT(r.err == PROVEN_OK && same(block, "89a"), "a C string starting at the buffer's end is taken", "");
        r = proven_fmt_to(block, 8, "{:.3}", PROVEN_ARG((const char *)block + 7));
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_INVALID_ARG, "one starting at its last byte is refused as the output's own", "");
    }
    r = proven_fmt_to(b, sizeof b, "{:.2}", PROVEN_ARG("a\x80\x80\x80"));
    PROVEN_TEST_ASSERT(r.err == PROVEN_OK && same(b, "a\x80"), "bytes that are not UTF-8 are cut where N says", "");
    r = proven_fmt_to(b, sizeof b, "[{:.3}]", PROVEN_ARG((const char *)0));
    PROVEN_TEST_ASSERT(r.err == PROVEN_OK && same(b, "[]"), "a NULL C string is empty, with a precision too", "");
    r = proven_fmt_to(b, sizeof b, "{:.2}", PROVEN_ARG(42));
    PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_INVALID_FORMAT && same(b, ""), "a precision on an integer is still refused", "");
    r = proven_fmt_to(b, sizeof b, "{:.2}", PROVEN_ARG('c'));
    PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_INVALID_FORMAT, "and on a char", "");
    r = proven_fmt_to(b, sizeof b, "{:.61}", PROVEN_ARG(1.5));
    PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_OUT_OF_BOUNDS, "a float's precision is still at most 60", "");
    static char longs[400];
    memset(longs, 'q', sizeof longs - 1);
    char wide[400];
    r = proven_fmt_to(wide, sizeof wide, "{:.300}", PROVEN_ARG(longs));
    PROVEN_TEST_ASSERT(r.err == PROVEN_OK && r.written == 300, "a string's precision may be larger (up to 10000)", "");

    PROVEN_TEST_SECTION("how many arguments a format names", "The rule a script can check, and the function that applies it.", "Check proven_fmt_arg_count and fmt_run's count_only path.");
    static const struct { const char *f; proven_size_t n; } counts[] = {
        { "", 0 }, { "plain", 0 }, { "{{}}", 0 }, { "{}", 1 }, { "{} {}", 2 }, { "{:>8} {:x}", 2 }, { "{1}", 2 },
        { "{1} {0}", 2 }, { "{} {0} {}", 2 }, { "{2:08}", 3 }, { "{{{}}}", 1 }, { "{:.3}", 1 },
    };
    for (size_t i = 0; i < sizeof counts / sizeof *counts; ++i) {
        proven_result_size_t c = proven_fmt_arg_count(counts[i].f);
        PROVEN_TEST_ASSERT(c.err == PROVEN_OK && c.value == counts[i].n, counts[i].f, "Each {} takes the next number; {N} names N.");
    }
    PROVEN_TEST_ASSERT(proven_fmt_arg_count("{").err == PROVEN_ERR_INVALID_FORMAT && proven_fmt_arg_count("}").err == PROVEN_ERR_INVALID_FORMAT &&
                       proven_fmt_arg_count("{:q}").err == PROVEN_ERR_INVALID_FORMAT && proven_fmt_arg_count(NULL).err == PROVEN_ERR_INVALID_ARG,
        "what is not a format is said so", "");
    r = proven_fmt_to(b, sizeof b, "{} {}", PROVEN_ARG(1));
    PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_INVALID_ARG && same(b, ""), "one argument too few: PROVEN_ERR_INVALID_ARG and nothing written", "");
    r = proven_fmt_to(b, sizeof b, "{}", PROVEN_ARG(1), PROVEN_ARG(2));
    PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_INVALID_ARG && same(b, ""), "one too many: the same", "");
    r = proven_fmt_to(b, sizeof b, "{1}", PROVEN_ARG(1), PROVEN_ARG(2));
    PROVEN_TEST_ASSERT(r.err == PROVEN_OK && same(b, "2"), "an argument not shown but below the highest named is allowed", "");

    PROVEN_TEST_SECTION("proven_fmt_to", "snprintf's shape: a buffer and its size.", "Check proven_fmt_to_internal and the truncating path of proven_u8str_fmt_internal.");
    r = proven_fmt_to(b, sizeof b, "{:02X}-{}-{:>4}", PROVEN_ARG(0xbu), PROVEN_ARG("x"), PROVEN_ARG(7));
    PROVEN_TEST_ASSERT(r.err == PROVEN_OK && same(b, "0B-x-   7") && r.written == 9 && r.required == 9, "it formats, and says what it wrote", "");
    {
        char s6[6];
        r = proven_fmt_to(s6, sizeof s6, "{}", PROVEN_ARG("abc\xc3\xa9xyz"));
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_OUT_OF_BOUNDS && same(s6, "abc\xc3\xa9") && r.written == 5 && r.required == 8, "cut at cap - 1, whole characters, required the whole", "");
        r = proven_fmt_to(s6, 5, "{}", PROVEN_ARG("abc\xc3\xa9xyz"));
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_OUT_OF_BOUNDS && same(s6, "abc") && r.written == 3, "a cut that would split the character leaves it out", "");
        r = proven_fmt_to(s6, 1, "{}", PROVEN_ARG("abc"));
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_OUT_OF_BOUNDS && s6[0] == '\0' && r.written == 0 && r.required == 3, "a buffer of one byte holds the NUL", "");
        memcpy(s6, "zzzzz", 6);
        r = proven_fmt_to(s6, sizeof s6, "{:q}", PROVEN_ARG(1));
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_INVALID_FORMAT && s6[0] == '\0', "after an error the text is empty and ends", "");
    }
    r = proven_fmt_to(NULL, 0, "{:>8}", PROVEN_ARG(5));
    PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_OUT_OF_BOUNDS && r.written == 0 && r.required == 8, "a size of 0 measures, with no buffer", "");
    r = proven_fmt_to(NULL, 4, "{}", PROVEN_ARG(5));
    PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_INVALID_ARG, "no buffer with a size is refused", "");
    {
        proven_byte_t mem[8];
        proven_u8str_t s = proven_u8str_borrow(mem, sizeof mem);
        r = proven_u8str_append_fmt_trunc(&s, "{}", PROVEN_ARG("ab\xe2\x82\xac\xe2\x82\xac"));
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_OUT_OF_BOUNDS && proven_u8str_as_view(&s).size == 5 && same(proven_u8str_as_cstr(&s), "ab\xe2\x82\xac"),
            "proven_u8str_append_fmt_trunc cuts between characters too", "");
    }

    PROVEN_TEST_PASS("string precision, argument counts and proven_fmt_to behave as documented.");
    return 0;
}
