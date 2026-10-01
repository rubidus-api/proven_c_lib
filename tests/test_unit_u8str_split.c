#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * Written from RFC-0005 section 3.1 and table section 4.1 before the implementation (TESTING.md
 * section 5.1). The contract is permanent once callers exist: n separators yield n + 1 fields. Every
 * empty field is {NULL, 0}, including a LEADING empty field that a test author would guess
 * points at offset 0 - so fields are compared by size and content, never by pointer.
 */

static proven_u8str_view_t b(const char *s, proven_size_t n) {
    return (proven_u8str_view_t){ (const proven_byte_t *)s, n };
}
static proven_u8str_view_t z(const char *s) { return b(s, strlen(s)); }

#define MAX_FIELDS 64

/* Split, bounded: a non-terminating iterator stops at MAX_FIELDS instead of hanging. */
static int collect(proven_u8str_view_t src, proven_u8str_view_t sep, proven_u8str_view_t *out) {
    proven_u8str_view_split_t it = proven_u8str_view_split(src, sep);
    int n = 0;
    proven_u8str_view_t f;
    while (n < MAX_FIELDS && proven_u8str_view_split_next(&it, &f)) out[n++] = f;
    return n;
}

static bool fields_are(proven_u8str_view_t src, proven_u8str_view_t sep, int want_n, const char *const *want) {
    proven_u8str_view_t got[MAX_FIELDS];
    int n = collect(src, sep, got);
    if (n != want_n) return false;
    for (int i = 0; i < n; ++i) {
        size_t len = strlen(want[i]);
        if (got[i].size != len) return false;
        if (len && memcmp(got[i].ptr, want[i], len) != 0) return false;
    }
    return true;
}

/* Non-overlapping occurrences, left to right: the walk the iterator performs. */
static int count_occurrences(const unsigned char *s, size_t n, const unsigned char *sep, size_t m) {
    int c = 0;
    for (size_t i = 0; m > 0 && i + m <= n;) {
        if (memcmp(s + i, sep, m) == 0) { ++c; i += m; } else ++i;
    }
    return c;
}

static unsigned long long g_state = 0xD1B54A32D192ED03ull;
static unsigned rnd(unsigned bound) {
    g_state ^= g_state << 13; g_state ^= g_state >> 7; g_state ^= g_state << 17;
    return (unsigned)(g_state % bound);
}

int main(void) {
    PROVEN_TEST_SUITE("splitting a view",
        "RFC-0005 table 4.1 row for row, the field-count property (n separators, n + 1 fields) over random inputs, every field a sub-range of its source, and a copied iterator continuing on its own.",
        "Inspect proven_u8str_view_split_next in src/proven/u8str.c, whose four steps must be in the RFC's order. A count one short is the dropped tail; a count at the cap is a non-terminating iterator.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("RFC-0005 table 4.1", "", "");
    // ---------------------------------------------------------------
    {
        const char *r1[] = { "a", "b", "c" };
        PROVEN_TEST_ASSERT(fields_are(z("a,b,c"), z(","), 3, r1), "\"a,b,c\" is three fields", "the natural loop drops the tail here");
        const char *r2[] = { "a" };
        PROVEN_TEST_ASSERT(fields_are(z("a"), z(","), 1, r2), "no separator is one field", "");
        const char *r3[] = { "a", "" };
        PROVEN_TEST_ASSERT(fields_are(z("a,"), z(","), 2, r3), "a trailing separator yields a trailing empty field", "");
        const char *r4[] = { "", "a" };
        PROVEN_TEST_ASSERT(fields_are(z(",a"), z(","), 2, r4), "and a leading one", "");
        const char *r5[] = { "a", "", "b" };
        PROVEN_TEST_ASSERT(fields_are(z("a,,b"), z(","), 3, r5), "empty fields are kept, unlike strtok", "");
        const char *r6[] = { "" };
        PROVEN_TEST_ASSERT(fields_are(z(""), z(","), 1, r6), "\"\" is one empty field, not zero", "");
        PROVEN_TEST_ASSERT(fields_are(b(NULL, 0), z(","), 1, r6), "a null view is an empty string", "");
        const char *r8[] = { "a", "b" };
        PROVEN_TEST_ASSERT(fields_are(z("aXXb"), z("XX"), 2, r8), "a multi-byte separator", "");
        const char *r9[] = { "a", "Xb" };
        PROVEN_TEST_ASSERT(fields_are(z("aXXXb"), z("XX"), 2, r9), "leftmost, non-overlapping: one occurrence, two fields", "");
        const char *r10[] = { "abc" };
        PROVEN_TEST_ASSERT(fields_are(z("abc"), z(""), 1, r10), "an empty separator yields one field and ends", "");
        PROVEN_TEST_ASSERT(fields_are(z("abc"), b(NULL, 0), 1, r10), "a null separator likewise", "");
        PROVEN_TEST_ASSERT(fields_are(b(NULL, 5), z(","), 1, r6), "an ill-formed source is one empty field, never a NULL field of size 5", "");

        proven_u8str_view_split_t it = proven_u8str_view_split(z("a,b"), z(","));
        proven_u8str_view_t f;
        PROVEN_TEST_ASSERT(!proven_u8str_view_split_next(NULL, &f), "a NULL iterator yields nothing", "");
        PROVEN_TEST_ASSERT(!proven_u8str_view_split_next(&it, NULL), "a NULL output yields nothing", "");
        PROVEN_TEST_ASSERT(proven_u8str_view_split_next(&it, &f) && f.size == 1 && f.ptr[0] == 'a',
            "and did not consume a field", "");
        proven_u8str_view_split_t kept = proven_u8str_view_split(z("x;y"), z(";"));
        PROVEN_TEST_ASSERT(kept.sep.size == 1 && kept.sep.ptr[0] == ';', "a well-formed separator reads back as passed", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("properties over random inputs",
        "For a non-empty separator, fields = non-overlapping occurrences + 1; every field lies inside the source (or is empty); a copy made part-way continues exactly as the original.",
        "The property is scoped to sep.size > 0: an empty separator occurs at every position and yields one field by definition.");
    // ---------------------------------------------------------------
    {
        static unsigned char src[200];
        unsigned char sep[4];
        unsigned long cases = 0, bad = 0, forks = 0, fields_total = 0;
        for (int iter = 0; iter < 50000; ++iter) {
            size_t n = rnd(40);
            for (size_t i = 0; i < n; ++i) src[i] = (unsigned char)("ab,"[rnd(3)]);
            size_t m = 1 + rnd(3);
            for (size_t i = 0; i < m; ++i) sep[i] = (unsigned char)("ab,"[rnd(3)]);
            proven_u8str_view_t sv = { n ? src : NULL, n };
            proven_u8str_view_t pv = { sep, m };

            proven_u8str_view_t got[MAX_FIELDS];
            int count = collect(sv, pv, got);
            ++cases;
            fields_total += (unsigned long)count;
            if (count != count_occurrences(src, n, sep, m) + 1) ++bad;
            for (int i = 0; i < count; ++i) {
                if (got[i].size == 0) continue;
                if (got[i].ptr < src || got[i].ptr + got[i].size > src + n) ++bad;
            }

            /* Fork after a random number of fields; both must yield the same remainder. */
            proven_u8str_view_split_t a = proven_u8str_view_split(sv, pv);
            proven_u8str_view_t fa, fb;
            int skip = (int)rnd((unsigned)count + 1);
            for (int i = 0; i < skip; ++i) (void)proven_u8str_view_split_next(&a, &fa);
            proven_u8str_view_split_t c = a;
            ++forks;
            for (int guard = 0; guard < MAX_FIELDS; ++guard) {
                bool ra = proven_u8str_view_split_next(&a, &fa);
                bool rc = proven_u8str_view_split_next(&c, &fb);
                if (ra != rc) { ++bad; break; }
                if (!ra) break;
                if (fa.size != fb.size || (fa.size && fa.ptr != fb.ptr)) { ++bad; break; }
            }
        }
        PROVEN_TEST_INFO("{} cases, {} fields, {} forks", PROVEN_ARG(cases), PROVEN_ARG(fields_total), PROVEN_ARG(forks));
        PROVEN_TEST_ASSERT(bad == 0, "every property must hold on every case", "");
        PROVEN_TEST_ASSERT(fields_total > cases * 2, "the corpus must contain separators, or the property proves little", "");
    }

    PROVEN_TEST_PASS("splitting a view");
    return 0;
}
