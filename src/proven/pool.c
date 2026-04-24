#include "proven/pool.h"

proven_err_t proven_pool_init(proven_pool_t *pool, proven_allocator_t base_alloc, proven_size_t item_size, proven_size_t item_align, proven_size_t bin_cap) {
    if (!pool) {
        return PROVEN_ERR_INVALID_ARG;
    }

    pool->base_alloc = base_alloc;
    pool->item_size = item_size;
    pool->item_align = item_align;
    pool->bin_cap = bin_cap;
    pool->bin_len = 0;
    pool->bin = 0;

    if (bin_cap > 0) {
        proven_size_t bin_size;
        if (PROVEN_CKD_MUL(&bin_size, bin_cap, sizeof(void*))) {
            return PROVEN_ERR_NOMEM;
        }

        proven_result_mem_mut_t res = base_alloc.alloc_fn(base_alloc.ctx, bin_size, alignof(void*));
        if (!PROVEN_IS_OK(res.err)) {
            return res.err;
        }

        pool->bin = (void **)res.value.ptr;
    }

    return PROVEN_OK;
}

static proven_result_mem_mut_t proven_pool_alloc_trait(void *ctx, proven_size_t size, proven_size_t align) {
    proven_result_mem_mut_t res = {0};
    proven_pool_t *pool = (proven_pool_t *)ctx;

    if (size != pool->item_size || align > pool->item_align) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }

    if (pool->bin_len > 0) {
        pool->bin_len--;
        res.value.ptr = pool->bin[pool->bin_len];
        res.value.size = pool->item_size;
        res.err = PROVEN_OK;
        return res;
    }

    return pool->base_alloc.alloc_fn(pool->base_alloc.ctx, size, pool->item_align);
}

static proven_result_mem_mut_t proven_pool_realloc_trait(void *ctx, void *old_ptr, proven_size_t old_size, proven_size_t new_size, proven_size_t align) {
    (void)ctx;
    (void)old_ptr;
    (void)old_size;
    (void)new_size;
    (void)align;
    proven_result_mem_mut_t res = {0};
    // The pool allocator strictly manages fixed-size blocks. Reallocation is not supported.
    res.err = PROVEN_ERR_INVALID_ARG;
    return res;
}

static void proven_pool_free_trait(void *ctx, void *ptr) {
    if (!ptr) {
        return;
    }

    proven_pool_t *pool = (proven_pool_t *)ctx;

    if (pool->bin_len < pool->bin_cap) {
        pool->bin[pool->bin_len] = ptr;
        pool->bin_len++;
    } else {
        if (pool->base_alloc.free_fn) {
            pool->base_alloc.free_fn(pool->base_alloc.ctx, ptr);
        }
    }
}

proven_allocator_t proven_pool_as_allocator(proven_pool_t *pool) {
    proven_allocator_t alloc = {0};
    alloc.ctx = pool;
    alloc.alloc_fn = proven_pool_alloc_trait;
    alloc.realloc_fn = proven_pool_realloc_trait;
    alloc.free_fn = proven_pool_free_trait;
    return alloc;
}
