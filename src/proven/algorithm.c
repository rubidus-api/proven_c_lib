#include "proven/algorithm.h"

// -------------------------------------------------------------
// Internal Quicksort Implementation
// -------------------------------------------------------------

static void swap_elements(proven_byte_t *a, proven_byte_t *b, proven_size_t size) {
    if (a == b) return;
    for (proven_size_t i = 0; i < size; ++i) {
        proven_byte_t tmp = a[i];
        a[i] = b[i];
        b[i] = tmp;
    }
}

static void quicksort_recursive(proven_byte_t *base, proven_size_t left, proven_size_t right, proven_size_t size, proven_compare_fn_t cmp) {
    if (left >= right) return;

    // Median-of-three or simple middle pivot for simplicity in Phase 12
    proven_size_t pivot_idx = left + (right - left) / 2;
    proven_byte_t *pivot_ptr = base + (pivot_idx * size);
    
    // We move the pivot to the end for partitioning
    swap_elements(pivot_ptr, base + (right * size), size);
    
    proven_size_t store_idx = left;
    for (proven_size_t i = left; i < right; ++i) {
        if (cmp(base + (i * size), base + (right * size)) < 0) {
            swap_elements(base + (i * size), base + (store_idx * size), size);
            store_idx++;
        }
    }
    
    swap_elements(base + (store_idx * size), base + (right * size), size);

    if (store_idx > 0) quicksort_recursive(base, left, store_idx - 1, size, cmp);
    quicksort_recursive(base, store_idx + 1, right, size, cmp);
}

// -------------------------------------------------------------
// Public C API
// -------------------------------------------------------------

void proven_array_sort(proven_array_t *arr, proven_compare_fn_t cmp) {
    if (!arr || arr->len < 2 || !cmp) return;
    quicksort_recursive((proven_byte_t*)arr->internal.ptr, 0, arr->len - 1, arr->elem_size, cmp);
}

void* proven_array_binary_search(const proven_array_t *arr, const void *key, proven_compare_fn_t cmp) {
    if (!arr || arr->len == 0 || !cmp || !key) return (void*)0;

    proven_ptrdiff_t low = 0;
    proven_ptrdiff_t high = (proven_ptrdiff_t)arr->len - 1;
    proven_byte_t *base = (proven_byte_t*)arr->internal.ptr;

    while (low <= high) {
        proven_ptrdiff_t mid = low + (high - low) / 2;
        void *mid_ptr = base + (mid * arr->elem_size);
        int res = cmp(key, mid_ptr);

        if (res == 0) return mid_ptr;
        if (res < 0) high = mid - 1;
        else low = mid + 1;
    }

    return (void*)0;
}

void* proven_array_linear_search(const proven_array_t *arr, const void *key, proven_compare_fn_t cmp) {
    if (!arr || !key || !cmp) return (void*)0;
    
    proven_byte_t *base = (proven_byte_t*)arr->internal.ptr;
    for (proven_size_t i = 0; i < arr->len; ++i) {
        void *current = base + (i * arr->elem_size);
        if (cmp(key, current) == 0) return current;
    }
    
    return (void*)0;
}
