#ifndef PROVEN_ALLOCATOR_H
#define PROVEN_ALLOCATOR_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/memory.h"

/**
 * @file allocator.h
 * @brief Polymorphic memory allocation interface (Trait).
 */

/**
 * @brief Function signature for a raw memory allocation trait.
 */
typedef proven_result_mem_mut_t (*proven_alloc_fn_t)(void *ctx, proven_size_t size, proven_size_t align);

/**
 * @brief Function signature for reallocating memory through the trait.
 */
typedef proven_result_mem_mut_t (*proven_realloc_fn_t)(void *ctx, void *old_ptr, proven_size_t old_size, proven_size_t new_size, proven_size_t align);

/**
 * @brief Function signature for deallocating memory through the trait.
 */
typedef void (*proven_free_fn_t)(void *ctx, void *ptr);

/**
 * @brief An interface wrapper allowing functions to be allocator-agnostic 
 * (Supports both Arena and Malloc-style Heap dynamically).
 */
typedef struct {
    void *ctx;
    proven_alloc_fn_t alloc_fn;
    /**
     * @brief Must leave old_ptr valid and unmodified on failure.
     */
    proven_realloc_fn_t realloc_fn;
    proven_free_fn_t  free_fn;
} proven_allocator_t;

/**
 * @brief Checks if all allocator functions are correctly provided.
 */
static inline bool proven_alloc_is_valid(proven_allocator_t alloc) {
    return alloc.alloc_fn && alloc.realloc_fn && alloc.free_fn;
}

#endif /* PROVEN_ALLOCATOR_H */
