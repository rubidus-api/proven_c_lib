#include "proven/arena.h"

proven_result_mem_mut_t proven_arena_alloc_aligned(proven_arena_t *arena, proven_size_t size, proven_size_t align) {
    proven_result_mem_mut_t res = {0};

    // Calculate the current unaligned address relative to the pointer
    proven_size_t current_addr = (proven_size_t)(arena->backing.ptr + arena->offset);
    
    // Apply zero-overhead alignment math
    proven_size_t aligned_addr = proven_mem_align_up(current_addr, align);
    if (aligned_addr == 0 && current_addr > 0) {
        res.err = PROVEN_ERR_NOMEM;
        return res;
    }
    proven_ptrdiff_t padding = (proven_ptrdiff_t)(aligned_addr - current_addr);

    // Strict bounds check preventing integer overflow
    proven_size_t required;
    if (PROVEN_CKD_ADD(&required, arena->offset, (proven_size_t)padding)) {
        res.err = PROVEN_ERR_NOMEM;
        return res;
    }
    if (PROVEN_CKD_ADD(&required, required, size)) {
        res.err = PROVEN_ERR_NOMEM;
        return res;
    }

    if (required > arena->backing.size) {
        res.err = PROVEN_ERR_NOMEM;
        return res;
    }

    // Commit the allocation
    res.err = PROVEN_OK;
    res.value.ptr = arena->backing.ptr + required - size;
    res.value.size = size;

    // Advance the offset state
    arena->offset = required;

    return res;
}

proven_result_mem_mut_t proven_arena_realloc_aligned(proven_arena_t *arena, void *old_ptr, proven_size_t old_size, proven_size_t new_size, proven_size_t align) {
    proven_result_mem_mut_t res = {0};

    if (!arena) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }

    if (!old_ptr) {
        return proven_arena_alloc_aligned(arena, new_size, align);
    }

    proven_byte_t *old_byte_ptr = (proven_byte_t *)old_ptr;
    proven_byte_t *current_tail = arena->backing.ptr + arena->offset;

    // MAGICAL LOGIC: Is this allocation located absolutely directly at the current physical tail?
    if (old_byte_ptr + old_size == current_tail) {
        // Yes! ZERO-COPY in-place extension.
        proven_size_t new_offset;
        // Re-calculations bounding the diff gracefully handling mathematical shrinkage securely!
        proven_ptrdiff_t diff = (proven_ptrdiff_t)new_size - (proven_ptrdiff_t)old_size;
        
        if (PROVEN_CKD_ADD(&new_offset, arena->offset, (proven_size_t)diff)) {
             res.err = PROVEN_ERR_NOMEM;
             return res;
        }
        
        if (new_offset > arena->backing.size) {
            res.err = PROVEN_ERR_NOMEM;
            return res;
        }

        // Extends or Shrinks physically modifying internal tracker indexes rapidly
        arena->offset = new_offset;
        
        res.err = PROVEN_OK;
        res.value.ptr = old_byte_ptr;
        res.value.size = new_size;
        return res;
    }

    // Mathematical logical fallback matching disconnected reallocation requests explicitly
    proven_result_mem_mut_t new_alloc = proven_arena_alloc_aligned(arena, new_size, align);
    if (PROVEN_IS_OK(new_alloc.err)) {
        // Manual memcpy avoiding `<string.h>` breaking isolation inside Core!
        proven_byte_t *dst = new_alloc.value.ptr;
        proven_size_t copy_amount = old_size < new_size ? old_size : new_size;
        for (proven_size_t i = 0; i < copy_amount; ++i) dst[i] = old_byte_ptr[i];
    }
    
    return new_alloc;
}
