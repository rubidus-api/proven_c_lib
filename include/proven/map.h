#ifndef PROVEN_MAP_H
#define PROVEN_MAP_H

#include <stdalign.h>
#include "proven/types.h"
#include "proven/error.h"
#include "proven/config.h"
#include "proven/allocator.h"
#include "proven/align.h"
#include "proven/u8str.h"

/**
 * @file map.h
 * @brief High-performance, Cache-friendly Open Addressing Hash Map.
 *        Supports zero-allocation U8 String Views or Generic Integer keys directly mapping dense arrays.
 */

typedef enum {
    PROVEN_KEY_TYPE_INT,
    PROVEN_KEY_TYPE_U8_BORROWED, // Borrowed view; caller keeps the bytes alive for the map lifetime
    PROVEN_KEY_TYPE_U8_OWNED     // Copied bytes; the map owns and frees the key storage
} proven_key_type_t;

typedef union {
    proven_size_t id;             // For Integer Keys
    proven_u8str_view_t str;      // For String Keys
} proven_map_key_t;

typedef struct {
    proven_allocator_t alloc;
    proven_mem_mut_t internal;    // Linear Open Addressing bucket array
    proven_size_t len;            // Number of occupied elements
    proven_size_t used;           // Occupied + Tombstones for true load factor calc
    proven_size_t cap;            // Capacity (always a power of 2)
    proven_size_t elem_size;      // Size of the mapped value structural footprint
    proven_size_t align;          // Alignment of the mapped value
    proven_size_t bucket_stride;  // Internal mathematically aligned bucket hopping metric
    proven_size_t payload_offset; // Cached offset to the start of the value payload
    proven_key_type_t key_type;   // Tracks configured map mode
} proven_map_t;

typedef struct {
    proven_err_t err;
    proven_map_t value;
} proven_result_map_t;

// -------------------------------------------------------------
// Type-Agnostic Core C API
// -------------------------------------------------------------

[[nodiscard]] proven_result_map_t proven_map_create(proven_allocator_t alloc, proven_size_t init_cap, proven_key_type_t key_type, proven_size_t elem_size, proven_size_t align);

/**
 * @brief Validates the structural integrity of the public map fields.
 */
[[nodiscard]] bool proven_map_is_valid(const proven_map_t *map);

/**
 * @brief Pre-allocates memory for the map to reach at least `new_cap` capacity.
 * Useful when working with arena allocators to prevent dead storage from reallocations.
 */
[[nodiscard]] proven_err_t proven_map_reserve(proven_map_t *map, proven_size_t new_cap);

/**
 * @brief Alias for proven_map_create that highlights the intentional capability allocation.
 */
#define proven_map_create_with_capacity(alloc, init_cap, key_type, elem_size, align) \
    proven_map_create(alloc, init_cap, key_type, elem_size, align)

/*
 * Sets a map value using a separate scratch allocator for temporary work buffers.
 *
 * Persistent map storage, including bucket arrays allocated during rehash,
 * still uses map->alloc. The scratch allocator is used only for temporary
 * buffers needed during this call, primarily to preserve an element that
 * aliases the map's current storage before a rehash.
 */
[[nodiscard]] proven_err_t proven_map_set_with_scratch(proven_map_t *map, proven_map_key_t key, const void *element, proven_allocator_t scratch);

[[nodiscard]] proven_err_t proven_map_set(proven_map_t *map, proven_map_key_t key, const void *element);

/**
 * @brief Inserts or replaces a value in an owned U8-string-key map.
 *
 * The map duplicates the key bytes into map-owned storage on insert.
 * The caller may release or reuse the source buffer after the call returns.
 */
[[nodiscard]] proven_err_t proven_map_set_u8_owned(proven_map_t *map, proven_u8str_view_t key, const void *element);

/**
 * @brief A pointer into the container's storage. It dies the next time the container grows.
 *
 * @warning The returned pointer is INVALIDATED by any operation that may reallocate -
 *          push, reserve, set, append, an insert that triggers a rehash. Using it
 *          afterwards is a use-after-free, and the sanitizers will say so. Hold the index
 *          or the key, not the pointer, across a mutation.
 */
[[nodiscard]] void* proven_map_get_mut(proven_map_t *map, proven_map_key_t key);
[[nodiscard]] const void* proven_map_get(const proven_map_t *map, proven_map_key_t key);

[[nodiscard]] proven_err_t proven_map_remove(proven_map_t *map, proven_map_key_t key);

void proven_map_destroy(proven_map_t *map);

// -------------------------------------------------------------
// Type-Safe Strict Macro Wrappers
// -------------------------------------------------------------

#define PROVEN_MAP_INIT_INT(alloc, type, init_cap) \
    proven_map_create((alloc), (init_cap), PROVEN_KEY_TYPE_INT, sizeof(type), alignof(type))

#define PROVEN_MAP_INIT_U8_BORROWED(alloc, type, init_cap) \
    proven_map_create((alloc), (init_cap), PROVEN_KEY_TYPE_U8_BORROWED, sizeof(type), alignof(type))

#define PROVEN_MAP_INIT_U8_OWNED(alloc, type, init_cap) \
    proven_map_create((alloc), (init_cap), PROVEN_KEY_TYPE_U8_OWNED, sizeof(type), alignof(type))

#define PROVEN_MAP_SET_INT(map_ptr, int_key, type, value) \
    proven_map_set((map_ptr), (proven_map_key_t){ .id = (proven_size_t)(int_key) }, (type[]){(value)})

#define PROVEN_MAP_SET_WITH_SCRATCH_INT(map_ptr, int_key, type, value, scratch) \
    proven_map_set_with_scratch( \
        (map_ptr), \
        (proven_map_key_t){ .id = (proven_size_t)(int_key) }, \
        (type[]){(value)}, \
        (scratch) \
    )

#define PROVEN_MAP_SET_U8_BORROWED(map_ptr, u8_view, type, value) \
    proven_map_set((map_ptr), (proven_map_key_t){ .str = (u8_view) }, (type[]){(value)})

#define PROVEN_MAP_SET_U8_OWNED(map_ptr, u8_view, type, value) \
    proven_map_set_u8_owned((map_ptr), (u8_view), (type[]){(value)})

#define PROVEN_MAP_SET_WITH_SCRATCH_U8_BORROWED(map_ptr, u8_view, type, value, scratch) \
    proven_map_set_with_scratch( \
        (map_ptr), \
        (proven_map_key_t){ .str = (u8_view) }, \
        (type[]){(value)}, \
        (scratch) \
    )

#define PROVEN_MAP_GET_INT(map_ptr, type, int_key) \
    ((const type*)proven_map_get((map_ptr), (proven_map_key_t){ .id = (proven_size_t)(int_key) }))

#define PROVEN_MAP_GET_U8_BORROWED(map_ptr, type, u8_view) \
    ((const type*)proven_map_get((map_ptr), (proven_map_key_t){ .str = (u8_view) }))

#define PROVEN_MAP_GET_U8_OWNED(map_ptr, type, u8_view) \
    ((const type*)proven_map_get((map_ptr), (proven_map_key_t){ .str = (u8_view) }))

#define PROVEN_MAP_GET_MUT_INT(map_ptr, type, int_key) \
    ((type*)proven_map_get_mut((map_ptr), (proven_map_key_t){ .id = (proven_size_t)(int_key) }))

#define PROVEN_MAP_GET_MUT_U8_BORROWED(map_ptr, type, u8_view) \
    ((type*)proven_map_get_mut((map_ptr), (proven_map_key_t){ .str = (u8_view) }))

#define PROVEN_MAP_GET_MUT_U8_OWNED(map_ptr, type, u8_view) \
    ((type*)proven_map_get_mut((map_ptr), (proven_map_key_t){ .str = (u8_view) }))

#define PROVEN_MAP_REMOVE_INT(map_ptr, int_key) \
    proven_map_remove((map_ptr), (proven_map_key_t){ .id = (proven_size_t)(int_key) })

#define PROVEN_MAP_REMOVE_U8_BORROWED(map_ptr, u8_view) \
    proven_map_remove((map_ptr), (proven_map_key_t){ .str = (u8_view) })

#define PROVEN_MAP_REMOVE_U8_OWNED(map_ptr, u8_view) \
    proven_map_remove((map_ptr), (proven_map_key_t){ .str = (u8_view) })

#define PROVEN_MAP_DESTROY(map_ptr) \
    proven_map_destroy(map_ptr)


#endif /* PROVEN_MAP_H */
