#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * The comparator must never be handed a misaligned element.
 *
 * insertion_sort held the element being moved in a `proven_byte_t tmp[128]` - alignment
 * 1 - and passed its address to the CALLER'S comparator, which of course reads it as the
 * element type. For any element more strictly aligned than the compiler happened to place
 * that byte array, that is a misaligned typed access: undefined behaviour, and a fault on
 * a target that does not tolerate it. UBSan reported it at every optimisation level for a
 * 32-, 64- or 128-byte-aligned struct.
 *
 * It never crashed on x86, where a misaligned double is merely slow, which is why it sat
 * there. A struct holding an __m256 or an __m512, or the same code on an alignment-strict
 * target, does not get that mercy.
 *
 * This test sorts over-aligned elements and asserts, from inside the comparator, that
 * every pointer it is given is correctly aligned. Under `./nob ubsan` the sanitizer says
 * it too - but the check below fails on ANY build, which is the point: a contract that
 * only one build mode enforces is a contract that breaks in release.
 */

#define AL 64

typedef struct {
    alignas(AL) double key;
    char pad[AL - sizeof(double)];
} wide_t;

static int g_misaligned = 0;

static int cmp_wide(const void *a, const void *b) {
    if (((proven_uintptr_t)a % AL) != 0 || ((proven_uintptr_t)b % AL) != 0) {
        g_misaligned++;
        return 0;
    }
    const wide_t *x = (const wide_t *)a;
    const wide_t *y = (const wide_t *)b;
    if (x->key < y->key) return -1;
    if (x->key > y->key) return 1;
    return 0;
}

int main(void) {
    PROVEN_TEST_SUITE("the sort never hands the comparator a misaligned element",
        "The scratch an insertion sort holds the moving element in is passed straight to the caller's comparator, so it must satisfy the element's alignment.",
        "Inspect insertion_sort in src/proven/algorithm.c. Its scratch is over-aligned to PROVEN_SORT_SCRATCH_ALIGN, and elements more strictly aligned than that take the swap path, which only ever shows the comparator real array elements.");

    proven_allocator_t heap = proven_heap_allocator();

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("64-byte-aligned elements sort, and every comparison sees an aligned pointer",
        "Small n takes the insertion-sort path, which is the one that used the byte-array scratch.",
        "A non-zero misaligned count means the scratch reached the comparator with alignment the element does not have.");
    // ---------------------------------------------------------------
    {
        proven_result_array_t ar = PROVEN_ARRAY_INIT(heap, wide_t, 64);
        PROVEN_TEST_ASSERT(proven_is_ok(ar.err), "setup: an array of over-aligned elements", "");
        proven_array_t arr = ar.value;

        /* n below the insertion-sort cutoff, and n above it (so the partition runs and
         * then hands its small ranges to insertion sort). */
        for (int n = 2; n <= 200; n += 43) {
            while (arr.len > 0) {
                wide_t drop;
                proven_err_t d = proven_array_pop(&arr, &drop);
                PROVEN_TEST_ASSERT(proven_is_ok(d), "setup: drain", "");
            }
            for (int i = 0; i < n; ++i) {
                wide_t w = {0};
                w.key = (double)((n - i) * 7 % 101);
                proven_err_t e = proven_array_push(&arr, &w);
                PROVEN_TEST_ASSERT(proven_is_ok(e), "setup: push", "");
            }

            g_misaligned = 0;
            proven_array_sort(&arr, cmp_wide);

            PROVEN_TEST_ASSERT(g_misaligned == 0,
                "every pointer the comparator sees must be aligned for the element type",
                "insertion_sort's scratch used to be a plain byte array (alignment 1) and it was passed straight to the comparator.");

            bool sorted = true;
            for (proven_size_t i = 1; i < arr.len; ++i) {
                const wide_t *p = (const wide_t *)proven_array_get(&arr, i - 1);
                const wide_t *q = (const wide_t *)proven_array_get(&arr, i);
                if (p->key > q->key) { sorted = false; break; }
            }
            PROVEN_TEST_ASSERT(sorted, "and the result must actually be sorted", "");
        }

        proven_array_destroy(&arr);
    }

    PROVEN_TEST_PASS("over-aligned elements sort without a misaligned comparison.");
    return 0;
}
