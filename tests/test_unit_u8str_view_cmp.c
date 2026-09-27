#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * Written from docs/RFC-0003 §3.4 and §4.4 before the implementation (docs/TESTING.md §5.1).
 * The table is the RFC's, row for row. Two rows carry more weight than they look: "\xFF" after
 * "a" pins unsigned comparison, and "a\0b" before "a\0c" says that a NUL inside a view is data -
 * the property that separates these strings from C strings.
 */

static proven_u8str_view_t b(const char *s, proven_size_t n) {
    return (proven_u8str_view_t){ (const proven_byte_t *)s, n };
}

static int sign(int v) { return (v > 0) - (v < 0); }

int main(void) {
    PROVEN_TEST_SUITE("view ordering",
        "proven_u8str_view_cmp is bytewise, unsigned, prefix-first, treats ill-formed views as empty, and is a total order; cmp_ptr sorts an array of views.",
        "Inspect proven_u8str_view_cmp in src/proven/u8str.c. A wrong sign on the \\xFF row is signed comparison; on the prefix rows, the length tie-break.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("RFC-0003 table 4.4", "Each row's sign, and its mirror.", "");
    // ---------------------------------------------------------------
    struct { proven_u8str_view_t a, b; int want; } rows[] = {
        { b("a", 1),      b("b", 1),      -1 },
        { b("b", 1),      b("a", 1),       1 },
        { b("a", 1),      b("a", 1),       0 },
        { b("a", 1),      b("ab", 2),     -1 },
        { b("ab", 2),     b("a", 1),       1 },
        { b("", 0),       b("", 0),        0 },
        { b("", 0),       b("a", 1),      -1 },
        { b(NULL, 0),     b("", 0),        0 },
        { b(NULL, 5),     b("", 0),        0 },
        { b("\xFF", 1),   b("a", 1),       1 },
        { b("a\0b", 3),   b("a\0c", 3),   -1 },
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
        int got = proven_u8str_view_cmp(rows[i].a, rows[i].b);
        int mirror = proven_u8str_view_cmp(rows[i].b, rows[i].a);
        if (sign(got) != rows[i].want || sign(mirror) != -rows[i].want) {
            PROVEN_TEST_INFO("row {} got {} mirror {} want {}", PROVEN_ARG((int)i), PROVEN_ARG(got), PROVEN_ARG(mirror), PROVEN_ARG(rows[i].want));
            PROVEN_TEST_ASSERT(false, "every row must have the RFC's sign, and its mirror the opposite", "");
        }
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a total order over a small universe",
        "Every triple of short strings over {0x00, 'a', 0xFF}: antisymmetric, transitive, and zero exactly when proven_u8str_view_eq says equal.",
        "");
    // ---------------------------------------------------------------
    {
        static const unsigned char alpha[] = { 0x00, 'a', 0xFF };
        unsigned char store[40][3];
        proven_u8str_view_t u[40];
        size_t n = 0;
        for (size_t len = 0; len <= 3; ++len) {
            size_t count = 1;
            for (size_t k = 0; k < len; ++k) count *= 3;
            for (size_t c = 0; c < count && n < 40; ++c) {
                size_t x = c;
                for (size_t k = 0; k < len; ++k) { store[n][k] = alpha[x % 3]; x /= 3; }
                u[n] = (proven_u8str_view_t){ len ? store[n] : NULL, len };
                ++n;
            }
        }
        unsigned long bad = 0;
        for (size_t i = 0; i < n; ++i)
            for (size_t j = 0; j < n; ++j) {
                int ij = sign(proven_u8str_view_cmp(u[i], u[j]));
                if (ij != -sign(proven_u8str_view_cmp(u[j], u[i]))) ++bad;
                if ((ij == 0) != (proven_u8str_view_eq(u[i], u[j]) != 0)) ++bad;
                for (size_t k = 0; k < n; ++k) {
                    int jk = sign(proven_u8str_view_cmp(u[j], u[k]));
                    int ik = sign(proven_u8str_view_cmp(u[i], u[k]));
                    if (ij <= 0 && jk <= 0 && ik > 0) ++bad;
                }
            }
        PROVEN_TEST_ASSERT(n == 40 && bad == 0, "cmp must be a total order consistent with eq", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("cmp_ptr sorts an array of views",
        "proven_array_sort with proven_u8str_view_cmp_ptr puts views in cmp order.", "");
    // ---------------------------------------------------------------
    {
        proven_allocator_t heap = proven_heap_allocator();
        proven_result_array_t r = proven_array_create(heap, 8, sizeof(proven_u8str_view_t), alignof(proven_u8str_view_t));
        PROVEN_TEST_ASSERT(proven_is_ok(r.err), "array", "");
        proven_array_t arr = r.value;
        const char *words[] = { "pear", "apple", "\xFF", "app", "", "banana" };
        for (size_t i = 0; i < 6; ++i) {
            proven_u8str_view_t w = b(words[i], strlen(words[i]));
            PROVEN_TEST_ASSERT(proven_is_ok(proven_array_push(&arr, &w)), "push", "");
        }
        proven_array_sort(&arr, proven_u8str_view_cmp_ptr);
        const char *want[] = { "", "app", "apple", "banana", "pear", "\xFF" };
        for (size_t i = 0; i < 6; ++i) {
            const proven_u8str_view_t *got = proven_array_get(&arr, i);
            PROVEN_TEST_ASSERT(got && got->size == strlen(want[i]) && memcmp(got->ptr, want[i], got->size) == 0,
                "sorted order must be the cmp order", "");
        }
        proven_array_destroy(&arr);
    }

    PROVEN_TEST_PASS("view ordering");
    return 0;
}
