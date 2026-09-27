#include "example.h"
#include <string.h>

/*
 * The everyday text jobs, done on views: split a line into fields, trim them,
 * strip a known prefix or suffix, find the last dot, and sort what you got.
 * Nothing here allocates except the array the sort works on; every result
 * points into the text you started with.
 *
 * Two rules hold throughout. An empty result is {NULL, 0}, so test a view by
 * its size, never its pointer. And a split loop ends on the return value of
 * split_next - n separators always give n + 1 fields, empty ones included.
 */

static bool is(proven_u8str_view_t v, const char *want) {
    return v.size == strlen(want) && (v.size == 0 || memcmp(v.ptr, want, v.size) == 0);
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- split, then trim each field --------------------------------------- */

    /* A record with a trailing separator and an empty field: five separators,
     * so six fields - the last one empty. The loop a person writes first
     * ("while a separator is found") would give five and lose the tail. */
    proven_u8str_view_t record = PROVEN_LIT(" report.tar.gz , draft.txt,,notes.md ,\tREADME ,");
    proven_u8str_view_t fields[8];
    int n = 0;
    proven_u8str_view_split_t it = proven_u8str_view_split(record, PROVEN_LIT(","));
    proven_u8str_view_t f;
    while (n < 8 && proven_u8str_view_split_next(&it, &f)) {
        fields[n++] = proven_u8str_view_trim(f);   /* ' ', \t, \n, \v, \f, \r - nothing else */
    }
    EXAMPLE_REQUIRE(n == 6, "five separators give six fields");
    EXAMPLE_REQUIRE(is(fields[0], "report.tar.gz") && is(fields[3], "notes.md") && is(fields[4], "README"),
                    "each field is trimmed on both ends");
    EXAMPLE_REQUIRE(fields[2].size == 0 && fields[5].size == 0, "the empty fields are kept, and tested by size");

    /* One-sided trims, for text where one end is significant. */
    EXAMPLE_REQUIRE(is(proven_u8str_view_trim_start(PROVEN_LIT("  indented  ")), "indented  "), "trim_start");
    EXAMPLE_REQUIRE(is(proven_u8str_view_trim_end(PROVEN_LIT("  indented  ")), "  indented"), "trim_end");

    /* --- prefixes, suffixes, and the last dot ------------------------------- */

    proven_u8str_view_t name = fields[0];
    /* The extension is after the LAST dot: find_last, not find. */
    proven_size_t dot = proven_u8str_view_find_last(name, PROVEN_LIT("."));
    EXAMPLE_REQUIRE(dot == 10, "the last dot in report.tar.gz is at 10");
    EXAMPLE_REQUIRE(is(proven_u8str_view_slice(name, dot + 1, name.size), "gz"), "so the extension is gz");

    /* remove_suffix leaves the view unchanged when the suffix is absent - no
     * error, so ask starts_with/ends_with first when you need to know. */
    EXAMPLE_REQUIRE(is(proven_u8str_view_remove_suffix(name, PROVEN_LIT(".tar.gz")), "report"), "the double suffix goes");
    EXAMPLE_REQUIRE(is(proven_u8str_view_remove_suffix(name, PROVEN_LIT(".zip")), "report.tar.gz"),
                    "an absent suffix changes nothing");
    EXAMPLE_REQUIRE(is(proven_u8str_view_remove_prefix(PROVEN_LIT("# heading"), PROVEN_LIT("# ")), "heading"),
                    "a known prefix goes");
    EXAMPLE_REQUIRE(proven_u8str_view_contains(fields[1], PROVEN_LIT("draft")), "contains is find != NOT_FOUND");

    /* --- sort the non-empty fields ------------------------------------------ */

    proven_result_array_t ra = proven_array_create(alloc, 8, sizeof(proven_u8str_view_t), alignof(proven_u8str_view_t));
    EXAMPLE_REQUIRE(proven_is_ok(ra.err), "creating the array must succeed");
    if (!proven_is_ok(ra.err)) return EXAMPLE_OK();
    proven_array_t names = ra.value;
    for (int i = 0; i < n; ++i) {
        if (fields[i].size > 0) (void)proven_array_push(&names, &fields[i]);
    }
    /* cmp_ptr is cmp shaped for a sort: it receives pointers to the views. Bytes
     * compare unsigned, and a prefix sorts first; "README" sorts before
     * lowercase names because 'R' is below 'd' in ASCII. */
    proven_array_sort(&names, proven_u8str_view_cmp_ptr);
    const proven_u8str_view_t *first = proven_array_get(&names, 0);
    const proven_u8str_view_t *last = proven_array_get(&names, 3);
    EXAMPLE_REQUIRE(first && is(*first, "README") && last && is(*last, "report.tar.gz"), "sorted bytewise");
    EXAMPLE_REQUIRE(proven_u8str_view_cmp(PROVEN_LIT("app"), PROVEN_LIT("apple")) < 0,
                    "cmp answers by sign - never compare it with -1");

    /* --- well-formed is not "found" or "not empty" --------------------------- */

    /* A slice past the end is {NULL, 0}, which is well formed - so this
     * predicate cannot end a loop or tell "empty" from "past the end". It only
     * says the view is safe to read. */
    proven_u8str_view_t past = proven_u8str_view_slice(name, 100, 5);
    EXAMPLE_REQUIRE(proven_u8str_view_is_well_formed(past) && past.size == 0, "past the end: empty and well formed");
    EXAMPLE_REQUIRE(!proven_u8str_view_is_well_formed((proven_u8str_view_t){ NULL, 3 }), "only {NULL, n > 0} is not");

    printf("%d fields, %zu names sorted\n", n, (size_t)names.len);
    proven_array_destroy(&names);
    return EXAMPLE_OK();
}
