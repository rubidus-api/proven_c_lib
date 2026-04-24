#ifndef PROVEN_BUFFER_H
#define PROVEN_BUFFER_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/memory.h"
#include "proven/arena.h"

/**
 * @file buffer.h
 * @brief Dynamic byte buffer allocated from an arena.
 */

/**
 * @brief Raw growing buffer. 
 * Assumes no specific encoding.
 */
typedef struct {
    proven_byte_t *ptr;
    proven_size_t  len;
    proven_size_t  cap;
} proven_buf_t;

/**
 * @brief Result wrapper for buffer creation.
 */
typedef struct {
    proven_err_t err;
    proven_buf_t value;
} proven_result_buf_t;

/**
 * @brief Creates a buffer with a specific capacity from the given polymorphic allocator trait.
 */
[[nodiscard]]
proven_result_buf_t proven_buf_create(proven_allocator_t alloc, proven_size_t cap);

/**
 * @brief Appends raw data to the buffer.
 */
[[nodiscard]]
proven_err_t proven_buf_append(proven_buf_t *buf, proven_mem_view_t data);

#endif /* PROVEN_BUFFER_H */
