#ifndef PROVEN_ALGORITHM_H
#define PROVEN_ALGORITHM_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/array.h"

/**
 * @file algorithm.h
 * @brief Generic sorting and searching algorithms for proven collections.
 */

/**
 * @brief Comparison function signature.
 * Should return:
 *  - negative if a < b
 *  - zero if a == b
 *  - positive if a > b
 */
typedef int (*proven_compare_fn_t)(const void *a, const void *b);

// -------------------------------------------------------------
// Array Algorithms
// -------------------------------------------------------------

/**
 * @brief Sorts a proven_array_t in-place using a robust quicksort implementation.
 * @param arr Pointer to the array.
 * @param cmp Comparison function.
 */
void proven_array_sort(proven_array_t *arr, proven_compare_fn_t cmp);

/**
 * @brief Performs a binary search on a sorted proven_array_t.
 * @param arr Pointer to the sorted array.
 * @param key Pointer to the element to search for.
 * @param cmp Comparison function.
 * @return Pointer to the found element in the array, or NULL if not found.
 */
void* proven_array_binary_search(const proven_array_t *arr, const void *key, proven_compare_fn_t cmp);

/**
 * @brief Performs a linear search on a proven_array_t.
 * @param arr Pointer to the array.
 * @param key Pointer to the element to search for.
 * @param cmp Comparison function.
 * @return Pointer to the found element in the array, or NULL if not found.
 */
void* proven_array_linear_search(const proven_array_t *arr, const void *key, proven_compare_fn_t cmp);

#endif /* PROVEN_ALGORITHM_H */
