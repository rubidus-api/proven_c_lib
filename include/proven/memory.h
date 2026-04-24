#ifndef PROVEN_MEMORY_H
#define PROVEN_MEMORY_H

#include "types.h"
#include "error.h"

/**
 * @file memory.h
 * @brief Core memory block definitions.
 */

/**
 * @brief Represents an owned memory block.
 */
typedef struct {
    proven_byte_t *ptr;
    proven_size_t  size;
} proven_mem_t;

/**
 * @brief Represents a non-owning read-only view of memory.
 */
typedef struct {
    const proven_byte_t *ptr;
    proven_size_t        size;
} proven_mem_view_t;

/**
 * @brief Represents a non-owning read-write slice of memory.
 */
typedef struct {
    proven_byte_t *ptr;
    proven_size_t  size;
} proven_mem_mut_t;

/**
 * @brief Result wrapper for a proven_mem_mut_t.
 * Replaces out-pointers for error handling.
 */
typedef struct {
    proven_err_t     err;
    proven_mem_mut_t value;
} proven_result_mem_mut_t;

/* --- Creation Helpers --- */

/**
 * @brief Creates a read-only view from an owned memory block.
 */
static inline proven_mem_view_t proven_mem_view_from_owned(proven_mem_t mem) {
    return (proven_mem_view_t){ .ptr = mem.ptr, .size = mem.size };
}

/**
 * @brief Creates a read-write slice from an owned memory block.
 */
static inline proven_mem_mut_t proven_mem_mut_from_owned(proven_mem_t mem) {
    return (proven_mem_mut_t){ .ptr = mem.ptr, .size = mem.size };
}

/* --- Slicing Utilities --- */

/**
 * @brief Slices a view into a sub-view.
 * @note This is unsafe (no bounds checking in Phase 2) as per low-level focus.
 */
static inline proven_mem_view_t proven_mem_view_slice(proven_mem_view_t view, proven_size_t offset, proven_size_t size) {
    return (proven_mem_view_t){ .ptr = view.ptr + offset, .size = size };
}

/**
 * @brief Slices a mut-slice into a smaller mut-slice.
 */
static inline proven_mem_mut_t proven_mem_mut_slice(proven_mem_mut_t mut, proven_size_t offset, proven_size_t size) {
    return (proven_mem_mut_t){ .ptr = mut.ptr + offset, .size = size };
}

#endif /* PROVEN_MEMORY_H */
