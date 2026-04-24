#include "proven/buffer.h"
#include "proven/align.h"

proven_result_buf_t proven_buf_create(proven_allocator_t alloc, proven_size_t cap) {
    proven_result_buf_t res = {0};
    
    // Allocate the underlying capacity utilizing the allocator trait injected
    proven_result_mem_mut_t alloc_res = alloc.alloc_fn(alloc.ctx, cap, PROVEN_DEFAULT_ALIGNMENT);
    if (!proven_is_ok(alloc_res.err)) {
        res.err = alloc_res.err;
        return res;
    }
    
    res.err = PROVEN_OK;
    res.value.ptr = alloc_res.value.ptr;
    res.value.len = 0;
    res.value.cap = cap;
    
    return res;
}

proven_err_t proven_buf_append(proven_buf_t *buf, proven_mem_view_t data) {
    if (buf->len + data.size > buf->cap) {
        return PROVEN_ERR_OUT_OF_BOUNDS; 
    }
    
    // Copy data into the buffer
    // Memory overlaps check is not required for independent slices by design
    for (proven_size_t i = 0; i < data.size; ++i) {
        buf->ptr[buf->len + i] = data.ptr[i];
    }
    
    buf->len += data.size;
    return PROVEN_OK;
}
