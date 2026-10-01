#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * Written from RFC-0005 section 3.2, section 3.3, section 3.5 and tables section 4.2, section 4.3, section 4.5, before the
 * implementations (TESTING.md section 5.1). Every empty result is {NULL, 0} by the RFC's single
 * spelling of empty, so these tests compare sizes and contents, never pointers.
 */

static proven_u8str_view_t b(const char *s, proven_size_t n) {
    return (proven_u8str_view_t){ (const proven_byte_t *)s, n };
}
static proven_u8str_view_t z(const char *s) { return b(s, strlen(s)); }

static bool is(proven_u8str_view_t got, const char *want) {
    size_t n = strlen(want);
    return got.size == n && (n == 0 || (got.ptr && memcmp(got.ptr, want, n) == 0));
}

/* An empty result is {NULL, 0}: size 0, and no pointer to read. */
static bool empty(proven_u8str_view_t v) { return v.size == 0; }

int main(void) {
    PROVEN_TEST_SUITE("view trim, affixes, reverse search and well-formedness",
        "RFC-0005 tables 4.2, 4.3 and 4.5, row for row: six-byte ASCII trim, prefix/suffix removal that leaves the view unchanged when absent, last-occurrence search that counts overlaps and returns size for an empty needle, and the one false case of is_well_formed.",
        "Inspect the view vocabulary at the end of src/proven/u8str.c. Each failing row is named.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("RFC-0005 table 4.2: trim and affix removal", "", "");
    // ---------------------------------------------------------------
    PROVEN_TEST_ASSERT(is(proven_u8str_view_trim(z("  a  ")), "a"), "trim both ends", "");
    PROVEN_TEST_ASSERT(is(proven_u8str_view_trim(z("a")), "a"), "nothing to trim", "");
    PROVEN_TEST_ASSERT(empty(proven_u8str_view_trim(z("   "))), "all whitespace trims to empty", "");
    PROVEN_TEST_ASSERT(empty(proven_u8str_view_trim(z(""))), "empty stays empty", "");
    PROVEN_TEST_ASSERT(is(proven_u8str_view_trim(z("\t\n\v\f\r a \r\f\v\n\t")), "a"), "all six bytes, both ends", "");
    PROVEN_TEST_ASSERT(is(proven_u8str_view_trim_start(z("  a  ")), "a  "), "trim_start leaves the end", "");
    PROVEN_TEST_ASSERT(is(proven_u8str_view_trim_end(z("  a  ")), "  a"), "trim_end leaves the start", "");
    PROVEN_TEST_ASSERT(is(proven_u8str_view_trim(z("a b")), "a b"), "interior whitespace untouched", "");
    PROVEN_TEST_ASSERT(empty(proven_u8str_view_trim(b(NULL, 5))), "ill-formed trims to empty", "");
    PROVEN_TEST_ASSERT(is(proven_u8str_view_trim(z("\xC2\xA0" "a")), "\xC2\xA0" "a"),
        "a UTF-8 no-break space is not one of the six bytes", "");
    PROVEN_TEST_ASSERT(empty(proven_u8str_view_trim_start(b(NULL, 3))) && empty(proven_u8str_view_trim_end(b(NULL, 3))),
        "both one-sided trims treat ill-formed as empty", "");

    PROVEN_TEST_ASSERT(is(proven_u8str_view_remove_prefix(z("foobar"), z("foo")), "bar"), "prefix removed", "");
    PROVEN_TEST_ASSERT(is(proven_u8str_view_remove_prefix(z("foobar"), z("xyz")), "foobar"), "absent prefix: unchanged", "");
    PROVEN_TEST_ASSERT(empty(proven_u8str_view_remove_prefix(z("foo"), z("foo"))), "whole string removed: empty", "");
    PROVEN_TEST_ASSERT(is(proven_u8str_view_remove_prefix(z("foo"), z("foobar")), "foo"), "longer prefix: unchanged", "");
    PROVEN_TEST_ASSERT(is(proven_u8str_view_remove_prefix(z("foo"), z("")), "foo"), "empty prefix: unchanged", "");
    PROVEN_TEST_ASSERT(is(proven_u8str_view_remove_suffix(z("foobar"), z("bar")), "foo"), "suffix removed", "");
    PROVEN_TEST_ASSERT(is(proven_u8str_view_remove_suffix(z("foobar"), z("xyz")), "foobar"), "absent suffix: unchanged", "");
    PROVEN_TEST_ASSERT(empty(proven_u8str_view_remove_suffix(z("foo"), z("foo"))), "whole string removed: empty", "");
    PROVEN_TEST_ASSERT(empty(proven_u8str_view_remove_prefix(b(NULL, 4), z("a"))), "ill-formed input: empty", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("RFC-0005 table 4.3: find_last, and contains", "", "");
    // ---------------------------------------------------------------
    {
        const proven_size_t NF = PROVEN_INDEX_NOT_FOUND;
        struct { proven_u8str_view_t h, n; proven_size_t want; } rows[] = {
            { z("abcabc"), z("abc"), 3 },
            { z("abcabc"), z("z"), NF },
            { z("aaa"), z("aa"), 1 },
            { z("abc"), z("abc"), 0 },
            { z("abc"), z("abcd"), NF },
            { z("a/b/c"), z("/"), 3 },
            { z("abc"), z(""), 3 },
            { z(""), z(""), 0 },
            { b(NULL, 0), b(NULL, 0), 0 },
            { z(""), z("a"), NF },
            { b(NULL, 0), z("a"), NF },
            { b(NULL, 5), z("a"), NF },
        };
        for (size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
            proven_size_t got = proven_u8str_view_find_last(rows[i].h, rows[i].n);
            if (got != rows[i].want) {
                PROVEN_TEST_INFO("row {} got {}", PROVEN_ARG((int)i), PROVEN_ARG((unsigned long long)got));
                PROVEN_TEST_ASSERT(false, "find_last must match table 4.3", "");
            }
            bool c = proven_u8str_view_contains(rows[i].h, rows[i].n);
            PROVEN_TEST_ASSERT(c == (proven_u8str_view_find(rows[i].h, 0, rows[i].n) != NF),
                "contains is exactly find != NOT_FOUND", "");
        }
        PROVEN_TEST_ASSERT(proven_u8str_view_contains(z("hello"), z("ll")) && !proven_u8str_view_contains(z("hello"), z("lo!")),
            "contains, plainly", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("RFC-0005 table 4.5: is_well_formed",
        "True for every view except {NULL, n > 0} - including {NULL, 0}, which is why it cannot end a loop.", "");
    // ---------------------------------------------------------------
    PROVEN_TEST_ASSERT(proven_u8str_view_is_well_formed(z("abc")), "a real view", "");
    PROVEN_TEST_ASSERT(proven_u8str_view_is_well_formed(b(NULL, 0)), "the empty view is well formed", "");
    PROVEN_TEST_ASSERT(proven_u8str_view_is_well_formed(z("")), "so is a zero-length view with a pointer", "");
    PROVEN_TEST_ASSERT(!proven_u8str_view_is_well_formed(b(NULL, 5)), "{NULL, 5} is the only false case", "");
    PROVEN_TEST_ASSERT(proven_u8str_view_is_well_formed(proven_u8str_view_slice(z("abc"), 10, 2)),
        "an out-of-range slice is well formed too - so this cannot tell 'empty' from 'past the end'", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the one spelling of empty holds for a non-NULL empty input",
        "An empty result is {NULL, 0} even when the input was {p, 0} with p != NULL - the header promises one spelling. Found by code review: remove_prefix/_suffix and split passed {p, 0} through.",
        "view_or_empty must map every empty view, not only the ill-formed ones, to {NULL, 0}.");
    // ---------------------------------------------------------------
    {
        proven_u8str_view_t pe = { (const proven_byte_t *)"xyz", 0 };   /* empty, pointer set */
        proven_u8str_view_t r1 = proven_u8str_view_remove_prefix(pe, z("a"));
        proven_u8str_view_t r2 = proven_u8str_view_remove_suffix(pe, z("a"));
        proven_u8str_view_t r3 = proven_u8str_view_trim(pe);
        PROVEN_TEST_ASSERT(r1.size == 0 && r1.ptr == NULL, "remove_prefix of {p, 0} is {NULL, 0}", "");
        PROVEN_TEST_ASSERT(r2.size == 0 && r2.ptr == NULL, "remove_suffix of {p, 0} is {NULL, 0}", "");
        PROVEN_TEST_ASSERT(r3.size == 0 && r3.ptr == NULL, "trim of {p, 0} is {NULL, 0}", "");
        proven_u8str_view_split_t it = proven_u8str_view_split(pe, z(","));
        proven_u8str_view_t f = { (const proven_byte_t *)"q", 1 };
        PROVEN_TEST_ASSERT(proven_u8str_view_split_next(&it, &f) && f.size == 0 && f.ptr == NULL,
            "splitting {p, 0} yields one field, {NULL, 0}", "");
        it = proven_u8str_view_split(z("abc"), pe);
        PROVEN_TEST_ASSERT(proven_u8str_view_split_next(&it, &f) && f.size == 3, "an empty separator {p, 0} yields the input", "");
    }

    PROVEN_TEST_PASS("view trim, affixes, reverse search and well-formedness");
    return 0;
}
