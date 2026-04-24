#include "proven/ring.h"

proven_result_ring_t proven_ring_create(proven_allocator_t alloc, proven_size_t cap, proven_size_t elem_size, proven_size_t align) {
    proven_result_ring_t res = {0};
    
    // Bounds prevention enforcing non-zero dimension limitations
    if (cap == 0 || elem_size == 0) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }

    proven_size_t bytes_needed = cap * elem_size;
    proven_result_mem_mut_t alloc_res = alloc.alloc_fn(alloc.ctx, bytes_needed, align);
    
    if (!PROVEN_IS_OK(alloc_res.err)) {
        res.err = alloc_res.err;
        return res;
    }

    proven_ring_t ring = {
        .alloc = alloc,
        .internal = alloc_res.value,
        .head = 0,
        .tail = 0,
        .len = 0,
        .cap = cap,
        .elem_size = elem_size,
        .align = align
    };

    res.err = PROVEN_OK;
    res.value = ring;
    return res;
}

proven_err_t proven_ring_push(proven_ring_t *ring, const void *element) {
    if (!ring || !element) return PROVEN_ERR_INVALID_ARG;
    
    // Hardcap logic guaranteeing absolute containment avoiding re-allocations
    if (ring->len >= ring->cap) return PROVEN_ERR_OUT_OF_BOUNDS;

    proven_byte_t *dst = ring->internal.ptr + (ring->tail * ring->elem_size);
    const proven_byte_t *src = (const proven_byte_t *)element;

    for (proven_size_t i = 0; i < ring->elem_size; ++i) {
        dst[i] = src[i];
    }

    // Mathematical logical wrap-around
    ring->tail = (ring->tail + 1) % ring->cap;
    ring->len++;
    
    return PROVEN_OK;
}

proven_err_t proven_ring_pop(proven_ring_t *ring, void *out_element) {
    if (!ring || ring->len == 0) return PROVEN_ERR_OUT_OF_BOUNDS; // Read starvation threshold

    if (out_element) {
        const proven_byte_t *src = ring->internal.ptr + (ring->head * ring->elem_size);
        proven_byte_t *dst = (proven_byte_t *)out_element;
        for (proven_size_t i = 0; i < ring->elem_size; ++i) {
            dst[i] = src[i];
        }
    }

    // Mathematical logical wrap-around
    ring->head = (ring->head + 1) % ring->cap;
    ring->len--;
    
    return PROVEN_OK;
}

void proven_ring_destroy(proven_ring_t *ring) {
    if (!ring) return;
    if (ring->internal.ptr) {
        ring->alloc.free_fn(ring->alloc.ctx, ring->internal.ptr);
    }
    ring->internal.ptr = (void*)0;
    ring->internal.size = 0;
    ring->head = 0;
    ring->tail = 0;
    ring->len = 0;
    ring->cap = 0;
}
