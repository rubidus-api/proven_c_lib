#include "proven/algorithm.h"
#include "proven/array.h"
#include "proven/heap.h"
#include "proven_test.h"
#include <stdbool.h>

/*
 * proven_array_sort used a Lomuto partition with a strict `cmp(x, pivot) < 0`
 * test, so every element EQUAL to the pivot went into the right partition. On
 * low-cardinality keys - a status column, an enum, a bucket id, anything a
 * caller actually sorts by - the split collapsed to 1/(n-1) and the sort went
 * quadratic: 100,000 identical int32 keys took 10.6 seconds. A caller sorting
 * data an attacker can shape had a denial of service.
 *
 * This test counts comparisons rather than timing anything: a wall-clock
 * threshold is a flaky test on a shared machine, while the comparison count is
 * exactly the thing that blew up. Quadratic behaviour on n = 20,000 all-equal
 * elements means ~2x10^8 comparisons; O(n log n) means a few hundred thousand.
 * The bound below sits far from both, so it cannot fire spuriously and cannot
 * miss a regression.
 */

static long g_cmps = 0;

static int cmp_i32(const void *a, const void *b) {
    g_cmps++;
    int x = *(const int *)a;
    int y = *(const int *)b;
    return (x > y) - (x < y);
}

typedef struct { int key; char payload[44]; } wide_t;

static int cmp_wide(const void *a, const void *b) {
    g_cmps++;
    int x = ((const wide_t *)a)->key;
    int y = ((const wide_t *)b)->key;
    return (x > y) - (x < y);
}

static unsigned long long g_seed = 0x9E3779B97F4A7C15ull;
static unsigned long long next_rand(void) {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 7;
    g_seed ^= g_seed << 17;
    return g_seed;
}

static bool is_sorted_i32(const proven_array_t *arr) {
    for (proven_size_t i = 1; i < arr->len; ++i) {
        const int *prev = PROVEN_ARRAY_GET(arr, int, i - 1);
        const int *cur  = PROVEN_ARRAY_GET(arr, int, i);
        if (*prev > *cur) return false;
    }
    return true;
}

int main(void) {
    PROVEN_TEST_SUITE("sort on duplicate keys",
        "Sorting low-cardinality keys must stay O(n log n); equal elements must not all land on one side of the pivot.",
        "Inspect the partition in src/proven/algorithm.c. A two-way partition that sends equal elements right is quadratic on duplicates.");

    proven_allocator_t heap = proven_heap_allocator();

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("all-equal keys do not go quadratic",
        "Every element equal to the pivot must be recognised as equal, not pushed into a partition.",
        "If the comparison count explodes, the partition is two-way again: equal elements are landing on one side and the split has collapsed to 1/(n-1).");
    // ---------------------------------------------------------------
    {
        const proven_size_t n = 20000;
        proven_result_array_t ar = PROVEN_ARRAY_INIT(heap, int, n);
        PROVEN_TEST_ASSERT(proven_is_ok(ar.err), "array init failed", "");
        proven_array_t arr = ar.value;
        for (proven_size_t i = 0; i < n; ++i) {
            int v = 7;
            PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, int, v)), "push failed", "");
        }

        g_cmps = 0;
        proven_array_sort(&arr, cmp_i32);

        /* Quadratic would be ~2x10^8 here. A three-way partition needs O(n).
         * 40n leaves enormous headroom for any reasonable O(n log n) scheme. */
        PROVEN_TEST_ASSERT(g_cmps < 40L * (long)n,
            "sorting all-equal keys took a quadratic number of comparisons",
            "the partition must place elements equal to the pivot in the middle and never recurse into them");
        PROVEN_TEST_ASSERT(is_sorted_i32(&arr), "all-equal array came back unsorted", "");
        PROVEN_ARRAY_DESTROY(&arr);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("few distinct values stay near-linear",
        "The realistic case: a key with a handful of possible values.",
        "Same partition question as above; this is the shape real data takes.");
    // ---------------------------------------------------------------
    {
        const proven_size_t n = 20000;
        proven_result_array_t ar = PROVEN_ARRAY_INIT(heap, int, n);
        PROVEN_TEST_ASSERT(proven_is_ok(ar.err), "array init failed", "");
        proven_array_t arr = ar.value;
        for (proven_size_t i = 0; i < n; ++i) {
            int v = (int)(next_rand() % 8u);
            PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, int, v)), "push failed", "");
        }

        g_cmps = 0;
        proven_array_sort(&arr, cmp_i32);

        PROVEN_TEST_ASSERT(g_cmps < 60L * (long)n,
            "sorting 8 distinct values took far too many comparisons",
            "duplicates must collapse into an equal run rather than being partitioned repeatedly");
        PROVEN_TEST_ASSERT(is_sorted_i32(&arr), "8-distinct array came back unsorted", "");
        PROVEN_ARRAY_DESTROY(&arr);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("adversarial and degenerate orderings are bounded",
        "Sorted, reverse-sorted, and organ-pipe inputs are the classic quicksort killers.",
        "A blown bound here means the pivot choice or the depth fallback is gone; introsort must escape to heapsort rather than degrade.");
    // ---------------------------------------------------------------
    {
        const proven_size_t n = 20000;
        for (int shape = 0; shape < 3; ++shape) {
            proven_result_array_t ar = PROVEN_ARRAY_INIT(heap, int, n);
            PROVEN_TEST_ASSERT(proven_is_ok(ar.err), "array init failed", "");
            proven_array_t arr = ar.value;
            for (proven_size_t i = 0; i < n; ++i) {
                int v;
                if (shape == 0) v = (int)i;
                else if (shape == 1) v = (int)(n - i);
                else v = (int)((i < n / 2) ? i : (n - i));
                PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, int, v)), "push failed", "");
            }

            g_cmps = 0;
            proven_array_sort(&arr, cmp_i32);

            PROVEN_TEST_ASSERT(g_cmps < 60L * (long)n,
                "a degenerate ordering took far too many comparisons",
                "median-of-three plus a heapsort depth fallback must bound this");
            PROVEN_TEST_ASSERT(is_sorted_i32(&arr), "a degenerate ordering came back unsorted", "");
            PROVEN_ARRAY_DESTROY(&arr);
        }
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("wide elements sort correctly",
        "The swap path takes a bulk-copy branch for larger elements; it must still move whole elements.",
        "A corrupted payload means swap_elements is copying the wrong span - inspect the scratch-buffer branch.");
    // ---------------------------------------------------------------
    {
        const proven_size_t n = 3000;
        proven_result_array_t ar = PROVEN_ARRAY_INIT(heap, wide_t, n);
        PROVEN_TEST_ASSERT(proven_is_ok(ar.err), "array init failed", "");
        proven_array_t arr = ar.value;
        for (proven_size_t i = 0; i < n; ++i) {
            wide_t w = {0};
            w.key = (int)(next_rand() % 50u);
            /* Tie the payload to the key so a torn swap is detectable. */
            for (int k = 0; k < 44; ++k) w.payload[k] = (char)(w.key & 0x7F);
            PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, wide_t, w)), "push failed", "");
        }

        proven_array_sort(&arr, cmp_wide);

        bool ok = true;
        for (proven_size_t i = 0; i < n; ++i) {
            const wide_t *w = PROVEN_ARRAY_GET(&arr, wide_t, i);
            for (int k = 0; k < 44; ++k) {
                if (w->payload[k] != (char)(w->key & 0x7F)) ok = false;
            }
            if (i > 0) {
                const wide_t *p = PROVEN_ARRAY_GET(&arr, wide_t, i - 1);
                if (p->key > w->key) ok = false;
            }
        }
        PROVEN_TEST_ASSERT(ok,
            "wide elements came back unsorted or with a torn payload",
            "swap_elements must move the whole element; check the bulk-copy branch and its size bound");
        PROVEN_ARRAY_DESTROY(&arr);
    }

    PROVEN_TEST_PASS("sort stays sub-quadratic on duplicate and degenerate input.");
    return 0;
}
