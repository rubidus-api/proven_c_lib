#include "proven/array.h"

proven_result_array_t proven_array_create(proven_allocator_t alloc, proven_size_t init_cap, proven_size_t elem_size, proven_size_t align) {
    proven_result_array_t res = {0};
    
    if (elem_size == 0) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }

    proven_mem_mut_t block = { .ptr = (void*)0, .size = 0 };
    
    // Abstracted Pre-allocation
    if (init_cap > 0) {
        proven_size_t bytes_needed;
        if (PROVEN_CKD_MUL(&bytes_needed, init_cap, elem_size)) {
            res.err = PROVEN_ERR_NOMEM;
            return res;
        }
        proven_result_mem_mut_t alloc_res = alloc.alloc_fn(alloc.ctx, bytes_needed, align);
        if (!PROVEN_IS_OK(alloc_res.err)) {
            res.err = alloc_res.err;
            return res;
        }
        block = alloc_res.value;
    }

    proven_array_t arr = {
        .alloc = alloc,
        .internal = block,
        .len = 0,
        .elem_size = elem_size,
        .align = align
    };

    res.err = PROVEN_OK;
    res.value = arr;
    return res;
}

proven_err_t proven_array_push(proven_array_t *arr, const void *element) {
    if (!arr || !element) return PROVEN_ERR_INVALID_ARG;

    proven_size_t cap = arr->elem_size == 0 ? 0 : arr->internal.size / arr->elem_size;
    
    // Bounds limit reached, engage dynamic expansion!
    if (arr->len >= cap) {
        proven_size_t new_cap;
        if (cap == 0) {
            new_cap = 8;
        } else {
            if (PROVEN_CKD_MUL(&new_cap, cap, 2)) {
                return PROVEN_ERR_NOMEM;
            }
        }
        
        proven_size_t new_bytes;
        if (PROVEN_CKD_MUL(&new_bytes, new_cap, arr->elem_size)) {
            return PROVEN_ERR_NOMEM;
        }
        
        // Polymorphic zero-copy realloc execution natively hooking into arenas/heaps strictly safely!
        proven_result_mem_mut_t realloc_res = arr->alloc.realloc_fn(arr->alloc.ctx, arr->internal.ptr, arr->internal.size, new_bytes, arr->align);
        
        if (!PROVEN_IS_OK(realloc_res.err)) {
            return realloc_res.err;
        }

        // Commit pointer override atomically
        arr->internal = realloc_res.value;
    }

    // Embed bytes onto offset index matching array state
    proven_size_t offset;
    if (PROVEN_CKD_MUL(&offset, arr->len, arr->elem_size)) {
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }
    proven_byte_t *dst = arr->internal.ptr + offset;
    const proven_byte_t *src = (const proven_byte_t *)element;
    
    for (proven_size_t i = 0; i < arr->elem_size; ++i) {
        dst[i] = src[i];
    }
    
    arr->len++;
    return PROVEN_OK;
}

proven_err_t proven_array_pop(proven_array_t *arr, void *out_element) {
    if (!arr || arr->len == 0) return PROVEN_ERR_OUT_OF_BOUNDS;

    arr->len--;
    if (out_element) {
        proven_size_t offset;
        if (PROVEN_CKD_MUL(&offset, arr->len, arr->elem_size)) {
            return PROVEN_ERR_OUT_OF_BOUNDS;
        }
        const proven_byte_t *src = arr->internal.ptr + offset;
        proven_byte_t *dst = (proven_byte_t *)out_element;
        for (proven_size_t i = 0; i < arr->elem_size; ++i) {
            dst[i] = src[i];
        }
    }
    return PROVEN_OK;
}

void* proven_array_get(const proven_array_t *arr, proven_size_t index) {
    if (!arr || index >= arr->len) return (void*)0;
    
    proven_size_t offset;
    if (PROVEN_CKD_MUL(&offset, index, arr->elem_size)) {
        return (void*)0;
    }
    return arr->internal.ptr + offset;
}

void proven_array_destroy(proven_array_t *arr) {
    if (!arr) return;
    if (arr->internal.ptr) {
        arr->alloc.free_fn(arr->alloc.ctx, arr->internal.ptr); // Cleaned by trait mapping safely
    }
    arr->internal.ptr = (void*)0;
    arr->internal.size = 0;
    arr->len = 0;
}
