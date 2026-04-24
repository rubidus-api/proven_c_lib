#ifndef PROVEN_MAP_H
#define PROVEN_MAP_H

#include <stdalign.h>
#include "proven/types.h"
#include "proven/error.h"
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
    PROVEN_KEY_TYPE_U8
} proven_key_type_t;

typedef union {
    proven_size_t id;             // For Integer Keys
    proven_u8str_view_t str;      // For String Keys
} proven_map_key_t;

typedef struct {
    proven_allocator_t alloc;
    proven_mem_mut_t internal;    // Linear Open Addressing bucket array
    proven_size_t len;            // Number of occupied elements
    proven_size_t cap;            // Capacity (always a power of 2)
    proven_size_t elem_size;      // Size of the mapped value structural footprint
    proven_size_t align;          // Alignment of the mapped value
    proven_size_t bucket_stride;  // Internal mathematically aligned bucket hopping metric
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

[[nodiscard]] proven_err_t proven_map_set(proven_map_t *map, proven_map_key_t key, const void *element);

[[nodiscard]] void* proven_map_get(const proven_map_t *map, proven_map_key_t key);

[[nodiscard]] proven_err_t proven_map_remove(proven_map_t *map, proven_map_key_t key);

void proven_map_destroy(proven_map_t *map);

// -------------------------------------------------------------
// Type-Safe Strict Macro Wrappers
// -------------------------------------------------------------

#define PROVEN_MAP_INIT_INT(alloc, type, init_cap) \
    proven_map_create((alloc), (init_cap), PROVEN_KEY_TYPE_INT, sizeof(type), alignof(type))

#define PROVEN_MAP_INIT_U8(alloc, type, init_cap) \
    proven_map_create((alloc), (init_cap), PROVEN_KEY_TYPE_U8, sizeof(type), alignof(type))

#define PROVEN_MAP_SET_INT(map_ptr, int_key, type, value) \
    proven_map_set((map_ptr), (proven_map_key_t){ .id = (proven_size_t)(int_key) }, (type[]){(value)})

#define PROVEN_MAP_SET_U8(map_ptr, u8_view, type, value) \
    proven_map_set((map_ptr), (proven_map_key_t){ .str = (u8_view) }, (type[]){(value)})

#define PROVEN_MAP_GET_INT(map_ptr, type, int_key) \
    ((type*)proven_map_get((map_ptr), (proven_map_key_t){ .id = (proven_size_t)(int_key) }))

#define PROVEN_MAP_GET_U8(map_ptr, type, u8_view) \
    ((type*)proven_map_get((map_ptr), (proven_map_key_t){ .str = (u8_view) }))

#define PROVEN_MAP_REMOVE_INT(map_ptr, int_key) \
    proven_map_remove((map_ptr), (proven_map_key_t){ .id = (proven_size_t)(int_key) })

#define PROVEN_MAP_REMOVE_U8(map_ptr, u8_view) \
    proven_map_remove((map_ptr), (proven_map_key_t){ .str = (u8_view) })

#define PROVEN_MAP_DESTROY(map_ptr) \
    proven_map_destroy(map_ptr)


#endif /* PROVEN_MAP_H */
