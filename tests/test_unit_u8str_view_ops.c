#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * Written from docs/RFC-0003 §3.2, §3.3, §3.5 and tables §4.2, §4.3, §4.5, before the
 * implementations (docs/TESTING.md §5.1). Every empty result is {NULL, 0} by the RFC's single
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
        "RFC-0003 tables 4.2, 4.3 and 4.5, row for row: six-byte ASCII trim, prefix/suffix removal that leaves the view unchanged when absent, last-occurrence search that counts overlaps and returns size for an empty needle, and the one false case of is_well_formed.",
        "Inspect the view vocabulary at the end of src/proven/u8str.c. Each failing row is named.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("RFC-0003 table 4.2: trim and affix removal", "", "");
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
    PROVEN_TEST_SECTION("RFC-0003 table 4.3: find_last, and contains", "", "");
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

    PROVEN_TEST_PASS("view trim, affixes, reverse search and well-formedness");
    return 0;
}
