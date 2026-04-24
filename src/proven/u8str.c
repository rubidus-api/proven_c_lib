#include "proven/u8str.h"
#include "proven/align.h"

proven_result_u8str_t proven_u8str_create(proven_allocator_t alloc, proven_size_t limit) {
    proven_result_u8str_t res = {0};
    proven_size_t cap;
    if (PROVEN_CKD_ADD(&cap, limit, 1)) {
        res.err = PROVEN_ERR_NOMEM;
        return res;
    }
    
    proven_result_buf_t buf_res = proven_buf_create(alloc, cap);
    
    if (!proven_is_ok(buf_res.err)) {
        res.err = buf_res.err;
        return res;
    }
    
    buf_res.value.ptr[0] = 0;
    res.err = PROVEN_OK;
    res.value.internal = buf_res.value;
    
    return res;
}

proven_result_u8str_t proven_u8str_create_from_view(proven_allocator_t alloc, proven_u8str_view_t view) {
    proven_result_u8str_t res = proven_u8str_create(alloc, view.size);
    if (!proven_is_ok(res.err)) return res;
    
    res.err = proven_u8str_append(&res.value, view);
    return res;
}

proven_err_t proven_u8str_append(proven_u8str_t *str, proven_u8str_view_t data) {
    proven_size_t required;
    if (PROVEN_CKD_ADD(&required, str->internal.len, data.size)) {
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }
    if (PROVEN_CKD_ADD(&required, required, 1)) {
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }

    if (required > str->internal.cap) {
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }

    for (proven_size_t i = 0; i < data.size; ++i) {
        str->internal.ptr[str->internal.len + i] = data.ptr[i];
    }
    
    str->internal.len += data.size;
    str->internal.ptr[str->internal.len] = 0;
    
    return PROVEN_OK;
}

[[nodiscard]]
proven_err_t proven_u8str_append_byte(proven_allocator_t alloc, proven_u8str_t *str, proven_u8 byte) {
    proven_u8str_view_t view = { .ptr = &byte, .size = 1 };
    return proven_u8str_append_view(alloc, str, view);
}

[[nodiscard]]
proven_err_t proven_u8str_append_view(proven_allocator_t alloc, proven_u8str_t *str, proven_u8str_view_t data) {
    proven_size_t required;
    if (PROVEN_CKD_ADD(&required, str->internal.len, data.size)) return PROVEN_ERR_OUT_OF_BOUNDS;
    if (PROVEN_CKD_ADD(&required, required, 1)) return PROVEN_ERR_OUT_OF_BOUNDS;

    if (required > str->internal.cap) {
        proven_size_t new_cap = str->internal.cap == 0 ? 16 : str->internal.cap;
        while (new_cap < required) {
            if (PROVEN_CKD_MUL(&new_cap, new_cap, 2)) {
                new_cap = required;
                break;
            }
        }
        
        proven_result_mem_mut_t new_mem = alloc.realloc_fn(alloc.ctx, str->internal.ptr, str->internal.cap, new_cap, PROVEN_DEFAULT_ALIGNMENT);
        if (!proven_is_ok(new_mem.err)) return new_mem.err;
        
        str->internal.ptr = new_mem.value.ptr;
        str->internal.cap = new_cap;
    }

    // Now we have space, use regular append
    return proven_u8str_append(str, data);
}

proven_result_cstr_t proven_u8str_view_to_cstr(proven_u8str_view_t view, proven_allocator_t alloc) {
    proven_result_cstr_t res = {0};
    
    // Allocate exact View length + 1 (for implicitly enforced '\0')
    proven_size_t cap;
    if (PROVEN_CKD_ADD(&cap, view.size, 1)) {
        res.err = PROVEN_ERR_NOMEM;
        return res;
    }

    proven_result_mem_mut_t mem = alloc.alloc_fn(alloc.ctx, cap, PROVEN_DEFAULT_ALIGNMENT);
    if (!proven_is_ok(mem.err)) {
        res.err = mem.err;
        return res;
    }
    
    // Copy the slice correctly
    for (proven_size_t i = 0; i < view.size; ++i) {
        mem.value.ptr[i] = view.ptr[i];
    }
    mem.value.ptr[view.size] = 0; // Null-terminator sealing
    
    res.err = PROVEN_OK;
    res.value = (const char*)mem.value.ptr;
    
    return res;
}

proven_size_t proven_cstr_len(const char *s) {
    if (!s) return 0;
    const char *p = s;
    while (*p != '\0') p++;
    return (proven_size_t)(p - s);
}

int proven_u8str_view_eq(proven_u8str_view_t a, proven_u8str_view_t b) {
    if (a.size != b.size) return 0; 
    for (proven_size_t i = 0; i < a.size; ++i) {
        if (a.ptr[i] != b.ptr[i]) return 0;
    }
    return 1;
}

proven_size_t proven_u8str_view_find(proven_u8str_view_t haystack, proven_size_t start_offset, proven_u8str_view_t needle) {
    if (start_offset > haystack.size) return PROVEN_INDEX_NOT_FOUND;
    if (needle.size == 0) return start_offset; // Empty needle always matches at offset
    if (needle.size > haystack.size - start_offset) return PROVEN_INDEX_NOT_FOUND;
    
    for (proven_size_t i = start_offset; i <= haystack.size - needle.size; ++i) {
        int match = 1;
        for (proven_size_t j = 0; j < needle.size; ++j) {
            if (haystack.ptr[i + j] != needle.ptr[j]) {
                match = 0;
                break;
            }
        }
        if (match) return i;
    }
    return PROVEN_INDEX_NOT_FOUND;
}

int proven_u8str_view_starts_with(proven_u8str_view_t str, proven_u8str_view_t prefix) {
    if (str.size < prefix.size) return 0;
    for (proven_size_t i = 0; i < prefix.size; ++i) {
        if (str.ptr[i] != prefix.ptr[i]) return 0;
    }
    return 1;
}

int proven_u8str_view_ends_with(proven_u8str_view_t str, proven_u8str_view_t suffix) {
    if (str.size < suffix.size) return 0;
    proven_size_t offset = str.size - suffix.size;
    for (proven_size_t i = 0; i < suffix.size; ++i) {
        if (str.ptr[offset + i] != suffix.ptr[i]) return 0;
    }
    return 1;
}

proven_u8str_view_t proven_u8str_view_slice(proven_u8str_view_t str, proven_size_t index, proven_size_t len) {
    if (index >= str.size || len == 0) return (proven_u8str_view_t){ .ptr = (const proven_byte_t*)0, .size = 0 };
    // Clamp length
    proven_size_t actual_len = len;
    if (index + len > str.size) {
        actual_len = str.size - index;
    }
    return (proven_u8str_view_t){ .ptr = str.ptr + index, .size = actual_len };
}

proven_err_t proven_u8str_replace_at(proven_u8str_t *str, proven_size_t index, proven_size_t old_len, proven_u8str_view_t data) {
    if (index > str->internal.len) return PROVEN_ERR_OUT_OF_BOUNDS; // index must be <= len, allow append at the exact end.
    
    // Clamp old_len to the actual available size backwards from the string end
    proven_size_t actual_old = old_len;
    if (index + old_len > str->internal.len) {
        actual_old = str->internal.len - index;
    }
    
    // Check total capacity
    proven_size_t new_total_len;
    if (PROVEN_CKD_SUB(&new_total_len, str->internal.len, actual_old)) {
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }
    if (PROVEN_CKD_ADD(&new_total_len, new_total_len, data.size)) {
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }

    proven_size_t required;
    if (PROVEN_CKD_ADD(&required, new_total_len, 1)) {
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }

    if (required > str->internal.cap) {
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }
    
    proven_byte_t *base = str->internal.ptr;
    
    // Do we need to move the tail?
    if (data.size != actual_old && index + actual_old < str->internal.len) {
        proven_size_t tail_len = str->internal.len - (index + actual_old);
        if (data.size > actual_old) {
            // Shift right (backwards loop)
            for (proven_size_t i = 0; i < tail_len; ++i) {
                base[index + data.size + tail_len - 1 - i] = base[index + actual_old + tail_len - 1 - i];
            }
        } else {
            // Shift left (forwards loop)
            for (proven_size_t i = 0; i < tail_len; ++i) {
                base[index + data.size + i] = base[index + actual_old + i];
            }
        }
    }
    
    // Copy new data
    for (proven_size_t i = 0; i < data.size; ++i) {
        base[index + i] = data.ptr[i];
    }
    
    str->internal.len = new_total_len;
    base[new_total_len] = 0; // Null-terminator sealing guarantee
    
    return PROVEN_OK;
}

proven_err_t proven_u8str_insert(proven_u8str_t *str, proven_size_t index, proven_u8str_view_t data) {
    return proven_u8str_replace_at(str, index, 0, data);
}

proven_err_t proven_u8str_remove(proven_u8str_t *str, proven_size_t index, proven_size_t len) {
    return proven_u8str_replace_at(str, index, len, (proven_u8str_view_t){ .ptr = (const proven_byte_t*)0, .size = 0 });
}

proven_err_t proven_u8str_replace_first(proven_u8str_t *str, proven_size_t start_offset, proven_u8str_view_t target, proven_u8str_view_t replacement) {
    if (target.size == 0) return PROVEN_ERR_INVALID_ARG;
    proven_size_t idx = proven_u8str_view_find(proven_u8str_as_view(str), start_offset, target);
    if (idx == PROVEN_INDEX_NOT_FOUND) return PROVEN_OK; // Or PROVEN_INDEX_NOT_FOUND error? PROVEN_OK matches meaning "done/nothing to do"
    
    return proven_u8str_replace_at(str, idx, target.size, replacement);
}

void proven_u8str_destroy(proven_allocator_t alloc, proven_u8str_t *str) {
    if (!str || !str->internal.ptr) return;
    alloc.free_fn(alloc.ctx, str->internal.ptr);
    str->internal.ptr = (void*)0;
    str->internal.len = 0;
    str->internal.cap = 0;
}
