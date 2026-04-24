#ifndef PROVEN_U8STR_H
#define PROVEN_U8STR_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/memory.h"
#include "proven/buffer.h"
#include "proven/allocator.h"

/**
 * @file u8str.h
 * @brief String wrappers enforcing Unsigned 8-bit (UTF-8/ASCII) logic and null-termination guarantees.
 */

/**
 * @brief Represents a read-only u8-string view. 
 */
typedef struct {
    const proven_byte_t *ptr;
    proven_size_t        size;
} proven_u8str_view_t;

/**
 * @brief Represents a mutable u8-string view.
 */
typedef struct {
    proven_byte_t *ptr;
    proven_size_t  size;
} proven_u8str_mut_t;

/**
 * @brief A string buffer tracking U8 bytes.
 */
typedef struct {
    proven_buf_t internal;
} proven_u8str_t;

/**
 * @brief Result wrapper for a string creation.
 */
typedef struct {
    proven_err_t err;
    proven_u8str_t value;
} proven_result_u8str_t;

/**
 * @brief Result wrapper for explicitly allocating a safe C-String.
 */
typedef struct {
    proven_err_t err;
    const char *value;
} proven_result_cstr_t;

/**
 * @brief Macro to create a u8str view strictly from a C string literal at compile time.
 */
#define PROVEN_LIT(s) ((proven_u8str_view_t){ .ptr = (const proven_byte_t *)("" s), .size = sizeof("" s) - 1 })

#include "proven/align.h"

#define PROVEN_INDEX_NOT_FOUND ((proven_size_t)-1)

[[nodiscard]] proven_result_u8str_t proven_u8str_create(proven_allocator_t alloc, proven_size_t limit);
[[nodiscard]] proven_result_u8str_t proven_u8str_create_from_view(proven_allocator_t alloc, proven_u8str_view_t view);

[[nodiscard]] proven_err_t proven_u8str_append(proven_u8str_t *str, proven_u8str_view_t data);
[[nodiscard]] proven_err_t proven_u8str_append_byte(proven_allocator_t alloc, proven_u8str_t *str, proven_u8 byte);
[[nodiscard]] proven_err_t proven_u8str_append_view(proven_allocator_t alloc, proven_u8str_t *str, proven_u8str_view_t data);

[[nodiscard]] proven_err_t proven_u8str_replace_at(proven_u8str_t *str, proven_size_t index, proven_size_t old_len, proven_u8str_view_t data);
[[nodiscard]] proven_err_t proven_u8str_insert(proven_u8str_t *str, proven_size_t index, proven_u8str_view_t data);
[[nodiscard]] proven_err_t proven_u8str_remove(proven_u8str_t *str, proven_size_t index, proven_size_t len);
[[nodiscard]] proven_err_t proven_u8str_replace_first(proven_u8str_t *str, proven_size_t start_offset, proven_u8str_view_t target, proven_u8str_view_t replacement);

[[nodiscard]] proven_size_t proven_u8str_view_find(proven_u8str_view_t haystack, proven_size_t start_offset, proven_u8str_view_t needle);
[[nodiscard]] int proven_u8str_view_starts_with(proven_u8str_view_t str, proven_u8str_view_t prefix);
[[nodiscard]] int proven_u8str_view_ends_with(proven_u8str_view_t str, proven_u8str_view_t suffix);
[[nodiscard]] proven_u8str_view_t proven_u8str_view_slice(proven_u8str_view_t str, proven_size_t index, proven_size_t len);

/**
 * @brief Zero-cost extraction of a standard C string pointer inherently guaranteed by internal structure.
 */
static inline const char* proven_u8str_as_cstr(const proven_u8str_t *str) {
    return (const char*)str->internal.ptr;
}

/**
 * @brief Converts a read-only u8-string view to a generic read-only memory view.
 */
static inline proven_mem_view_t proven_mem_view_from_u8(proven_u8str_view_t view) {
    return (proven_mem_view_t){ .ptr = view.ptr, .size = view.size };
}

/**
 * @brief Allocates and creates a null-terminated C-string from an arbitrary u8str view slicing block.
 * Requires an explicit allocator (Arena or Heap).
 */
[[nodiscard]] proven_result_cstr_t proven_u8str_view_to_cstr(proven_u8str_view_t view, proven_allocator_t alloc);

[[nodiscard]] proven_size_t proven_cstr_len(const char *s);

[[nodiscard]] static inline proven_u8str_view_t proven_u8str_view_from_cstr(const char *s) {
    if (!s) return (proven_u8str_view_t){ .ptr = (const proven_byte_t*)0, .size = 0 };
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = proven_cstr_len(s) };
}

[[nodiscard]] int proven_u8str_view_eq(proven_u8str_view_t a, proven_u8str_view_t b);

void proven_u8str_destroy(proven_allocator_t alloc, proven_u8str_t *str);

/**
 * @brief Zero-cost downgrade of a mutable string to a read-only view.
 */
static inline proven_u8str_view_t proven_u8str_as_view(const proven_u8str_t *str) {
    if (!str) return (proven_u8str_view_t){ .ptr = (const proven_byte_t*)0, .size = 0 };
    return (proven_u8str_view_t){ .ptr = str->internal.ptr, .size = str->internal.len };
}

#endif /* PROVEN_U8STR_H */
