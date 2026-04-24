#include "proven/map.h"
#include <stdalign.h>

#define BUCKET_EMPTY 0
#define BUCKET_OCCUPIED 1
#define BUCKET_TOMBSTONE 2

typedef struct {
    proven_byte_t state;
    // Padding logic handled correctly via proven_mem_align_up mathematics on payload extraction
    proven_map_key_t key;
} proven_map_bucket_header_t;

// Standard FNV-1a extremely efficient U8 string hashing
static proven_size_t hash_u8(proven_u8str_view_t view) {
    proven_size_t hash = 14695981039346656037ULL;
    for (proven_size_t i = 0; i < view.size; ++i) {
        hash ^= (proven_size_t)view.ptr[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

// SplitMix64 rapid avalanche integer mixer
static proven_size_t hash_int(proven_size_t key) {
    proven_size_t z = (key ^ (key >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static proven_size_t get_hash(proven_key_type_t type, proven_map_key_t key) {
    if (type == PROVEN_KEY_TYPE_INT) return hash_int(key.id);
    return hash_u8(key.str);
}

static int keys_equal(proven_key_type_t type, proven_map_key_t a, proven_map_key_t b) {
    if (type == PROVEN_KEY_TYPE_INT) {
        return a.id == b.id;
    } else {
        return proven_u8str_view_eq(a.str, b.str);
    }
}

// Ensure capacity is a valid mathematical power of 2 for rapid modulo wrapping
static proven_size_t next_pow2(proven_size_t v) {
    if (v < 8) return 8; // Optimal starting floor
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    v++;
    return v;
}

static proven_result_mem_mut_t alloc_buckets(proven_allocator_t alloc, proven_size_t cap, proven_size_t bucket_stride, proven_size_t align) {
    // Determine maximum boundary limit unifying payload alignments overriding structural metadata limitations
    proven_size_t req_align = align > alignof(proven_map_bucket_header_t) ? align : alignof(proven_map_bucket_header_t);
    
    proven_size_t total_size;
    if (PROVEN_CKD_MUL(&total_size, cap, bucket_stride)) {
        return (proven_result_mem_mut_t){ .err = PROVEN_ERR_NOMEM };
    }
    
    proven_result_mem_mut_t res = alloc.alloc_fn(alloc.ctx, total_size, req_align);
    
    if (PROVEN_IS_OK(res.err)) {
        // Zero-initialization enforcing Empty states implicitly across fresh blocks
        for (proven_size_t i = 0; i < total_size; ++i) {
            res.value.ptr[i] = 0;
        }
    }
    return res;
}

proven_result_map_t proven_map_create(proven_allocator_t alloc, proven_size_t init_cap, proven_key_type_t key_type, proven_size_t elem_size, proven_size_t align) {
    proven_result_map_t res = {0};
    
    if (elem_size == 0) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }

    proven_size_t actual_cap = next_pow2(init_cap);
    
    // Calculate mathematically exact physical bounding jumps preventing hardware faults
    proven_size_t payload_offset = proven_mem_align_up(sizeof(proven_map_bucket_header_t), align);
    if (payload_offset == 0) {
        res.err = PROVEN_ERR_NOMEM;
        return res;
    }
    
    proven_size_t stride_base;
    if (PROVEN_CKD_ADD(&stride_base, payload_offset, elem_size)) {
        res.err = PROVEN_ERR_NOMEM;
        return res;
    }
    
    proven_size_t bucket_stride = proven_mem_align_up(stride_base, alignof(proven_map_bucket_header_t));
    if (bucket_stride == 0) {
        res.err = PROVEN_ERR_NOMEM;
        return res;
    }

    proven_mem_mut_t block = {0};
    
    if (actual_cap > 0) {
        proven_result_mem_mut_t am = alloc_buckets(alloc, actual_cap, bucket_stride, align);
        if (!PROVEN_IS_OK(am.err)) {
            res.err = am.err;
            return res;
        }
        block = am.value;
    }

    proven_map_t map = {
        .alloc = alloc,
        .internal = block,
        .len = 0,
        .cap = actual_cap,
        .elem_size = elem_size,
        .align = align,
        .bucket_stride = bucket_stride,
        .key_type = key_type
    };

    res.err = PROVEN_OK;
    res.value = map;
    return res;
}

static proven_err_t map_rehash(proven_map_t *map) {
    proven_size_t new_cap;
    if (PROVEN_CKD_MUL(&new_cap, map->cap, 2)) {
        return PROVEN_ERR_NOMEM;
    }
    proven_result_mem_mut_t new_block_res = alloc_buckets(map->alloc, new_cap, map->bucket_stride, map->align);
    
    if (!PROVEN_IS_OK(new_block_res.err)) return new_block_res.err;
    
    proven_byte_t *old_ptr = map->internal.ptr;
    proven_size_t old_cap = map->cap;
    
    // Mount the new target block immediately
    map->internal = new_block_res.value;
    map->cap = new_cap;
    map->len = 0; // Length will rebuild natively on setting
    
    // Migrate populated elements (re-hashing logic executing safely on stable boundaries)
    proven_size_t payload_offset = proven_mem_align_up(sizeof(proven_map_bucket_header_t), map->align);
    for (proven_size_t i = 0; i < old_cap; ++i) {
        proven_size_t offset;
        if (PROVEN_CKD_MUL(&offset, i, map->bucket_stride)) break;
        proven_map_bucket_header_t *old_hdr = (proven_map_bucket_header_t *)(old_ptr + offset);
        if (old_hdr->state == BUCKET_OCCUPIED) {
            void *old_payload = (void *)((proven_byte_t *)old_hdr + payload_offset);
            (void)proven_map_set(map, old_hdr->key, old_payload); // Re-inserts organically!
        }
    }
    
    // Destroy obsolete block smoothly
    if (old_ptr) {
        map->alloc.free_fn(map->alloc.ctx, old_ptr);
    }
    
    return PROVEN_OK;
}

proven_err_t proven_map_set(proven_map_t *map, proven_map_key_t key, const void *element) {
    if (!map || !element || map->cap == 0) return PROVEN_ERR_INVALID_ARG;
    
    // 75% Load Factor Growth execution preventing dense collision chaining slowdowns
    proven_size_t threshold;
    if (PROVEN_CKD_MUL(&threshold, map->cap, 3)) threshold = map->cap; // fallback
    threshold /= 4;
    
    if (map->len >= threshold) {
        proven_err_t grow_err = map_rehash(map);
        if (!PROVEN_IS_OK(grow_err)) return grow_err;
    }

    proven_size_t hash = get_hash(map->key_type, key);
    proven_size_t idx = hash & (map->cap - 1); // Fast power-of-2 modulo
    
    proven_size_t payload_offset = proven_mem_align_up(sizeof(proven_map_bucket_header_t), map->align);
    proven_map_bucket_header_t *first_tombstone = (void*)0;

    for (proven_size_t i = 0; i < map->cap; ++i) {
        proven_size_t offset;
        if (PROVEN_CKD_MUL(&offset, idx, map->bucket_stride)) return PROVEN_ERR_OUT_OF_BOUNDS;
        proven_map_bucket_header_t *hdr = (proven_map_bucket_header_t *)(map->internal.ptr + offset);
        
        if (hdr->state == BUCKET_EMPTY) {
            // Drop target into the best resolution path
            proven_map_bucket_header_t *target = first_tombstone ? first_tombstone : hdr;
            target->state = BUCKET_OCCUPIED;
            target->key = key;
            proven_byte_t *dst = (proven_byte_t *)target + payload_offset;
            const proven_byte_t *src = (const proven_byte_t *)element;
            for(proven_size_t b = 0; b < map->elem_size; ++b) dst[b] = src[b];
            map->len++;
            return PROVEN_OK;
        } else if (hdr->state == BUCKET_OCCUPIED) {
            if (keys_equal(map->key_type, hdr->key, key)) {
                // Key matches exactly - overwrite value payload (don't increment len)
                proven_byte_t *dst = (proven_byte_t *)hdr + payload_offset;
                const proven_byte_t *src = (const proven_byte_t *)element;
                for(proven_size_t b = 0; b < map->elem_size; ++b) dst[b] = src[b];
                return PROVEN_OK;
            }
        } else if (hdr->state == BUCKET_TOMBSTONE) {
            if (!first_tombstone) first_tombstone = hdr; // Save cache efficiency shortcut
        }
        
        idx = (idx + 1) & (map->cap - 1); // Linear Probing wrapping mechanism seamlessly
    }
    
    return PROVEN_ERR_OUT_OF_BOUNDS; // Theoretically unreachable via 75% Load sizing logic
}

void* proven_map_get(const proven_map_t *map, proven_map_key_t key) {
    if (!map || map->cap == 0) return (void*)0;
    
    proven_size_t hash = get_hash(map->key_type, key);
    proven_size_t idx = hash & (map->cap - 1);
    proven_size_t payload_offset = proven_mem_align_up(sizeof(proven_map_bucket_header_t), map->align);
    
    for (proven_size_t i = 0; i < map->cap; ++i) {
        proven_size_t offset;
        if (PROVEN_CKD_MUL(&offset, idx, map->bucket_stride)) return (void*)0;
        proven_map_bucket_header_t *hdr = (proven_map_bucket_header_t *)(map->internal.ptr + offset);
        
        if (hdr->state == BUCKET_EMPTY) {
            return (void*)0; // Cannot exist if sequence is broken natively by Empty states
        } else if (hdr->state == BUCKET_OCCUPIED && keys_equal(map->key_type, hdr->key, key)) {
            return (void*)((proven_byte_t *)hdr + payload_offset);
        }
        
        idx = (idx + 1) & (map->cap - 1);
    }
    
    return (void*)0;
}

proven_err_t proven_map_remove(proven_map_t *map, proven_map_key_t key) {
    if (!map || map->cap == 0) return PROVEN_ERR_OUT_OF_BOUNDS;
    
    proven_size_t hash = get_hash(map->key_type, key);
    proven_size_t idx = hash & (map->cap - 1);
    
    for (proven_size_t i = 0; i < map->cap; ++i) {
        proven_size_t offset;
        if (PROVEN_CKD_MUL(&offset, idx, map->bucket_stride)) return PROVEN_ERR_OUT_OF_BOUNDS;
        proven_map_bucket_header_t *hdr = (proven_map_bucket_header_t *)(map->internal.ptr + offset);
        
        if (hdr->state == BUCKET_EMPTY) {
            return PROVEN_ERR_OUT_OF_BOUNDS;
        } else if (hdr->state == BUCKET_OCCUPIED && keys_equal(map->key_type, hdr->key, key)) {
            hdr->state = BUCKET_TOMBSTONE; // Drop Tombstone correctly maintaining ongoing open chaining algorithms mathematically!
            map->len--;
            return PROVEN_OK;
        }
        
        idx = (idx + 1) & (map->cap - 1);
    }
    return PROVEN_ERR_OUT_OF_BOUNDS;
}

void proven_map_destroy(proven_map_t *map) {
    if (!map) return;
    if (map->internal.ptr) {
        map->alloc.free_fn(map->alloc.ctx, map->internal.ptr); // Transparently utilizes heap vs arena trait overrides flawlessly!
    }
    map->internal.ptr = (void*)0;
    map->internal.size = 0;
    map->cap = 0;
    map->len = 0;
}
