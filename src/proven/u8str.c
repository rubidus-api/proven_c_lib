#include "proven/u8str.h"
#include "proven/align.h"
#include "proven_internal_memrange.h"
#include "../../platform/proven_sys_mem.h"

proven_result_u8str_t proven_u8str_create(proven_allocator_t alloc, proven_size_t limit) {
    proven_result_u8str_t res = {0};
    proven_size_t cap;
    if (PROVEN_CKD_ADD(&cap, limit, 1)) {
        res.err = PROVEN_ERR_OVERFLOW;
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
    if (!proven_is_ok(res.err)) {
        proven_u8str_destroy(alloc, &res.value);
    }
    return res;
}

bool proven_u8str_is_valid(const proven_u8str_t *str) {
    if (!str) return false;
    if (str->internal.cap > 0) {
        if (!str->internal.ptr) return false;
        if (str->internal.len >= str->internal.cap) return false;
        if (str->internal.ptr[str->internal.len] != '\0') return false;
    } else {
        if (str->internal.len > 0) return false;
    }
    return true;
}

proven_err_t proven_u8str_append(proven_u8str_t *str, proven_u8str_view_t data) {
    if (!str || !str->internal.ptr) return PROVEN_ERR_INVALID_ARG;
    if (data.size == 0) return PROVEN_OK;
    if (!data.ptr) return PROVEN_ERR_INVALID_ARG;

    proven_size_t current_len = str->internal.len;
    proven_size_t cap = str->internal.cap;
    
    proven_size_t required;
    if (PROVEN_CKD_ADD(&required, current_len, data.size)) {
        return PROVEN_ERR_OVERFLOW;
    }
    if (PROVEN_CKD_ADD(&required, required, 1)) {
        return PROVEN_ERR_OVERFLOW;
    }
    
    if (required > cap) {
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }
    
    proven_sys_mem_move(str->internal.ptr + current_len, data.ptr, data.size);
    str->internal.len += data.size;
    str->internal.ptr[str->internal.len] = 0;
    
    return PROVEN_OK;
}

proven_result_size_t proven_u8str_append_partial(proven_u8str_t *str, proven_u8str_view_t data) {
    proven_result_size_t res = { .err = PROVEN_OK, .value = 0 };
    if (!str || !str->internal.ptr) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }
    if (data.size == 0) return res;
    if (!data.ptr) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }

    proven_size_t current_len = str->internal.len;
    proven_size_t cap = str->internal.cap;
    
    if (current_len >= cap) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }

    proven_size_t needed_for_term;
    if (PROVEN_CKD_ADD(&needed_for_term, current_len, 1)) {
        res.err = PROVEN_ERR_OVERFLOW;
        return res;
    }

    if (needed_for_term >= cap) {
        res.err = PROVEN_ERR_OUT_OF_BOUNDS;
        return res;
    }
    
    proven_size_t available = cap - current_len - 1;
    proven_size_t to_copy = data.size;
    if (to_copy > available) {
        to_copy = available;
        res.err = PROVEN_ERR_OUT_OF_BOUNDS;
    }
    
    proven_sys_mem_move(str->internal.ptr + current_len, data.ptr, to_copy);
    str->internal.len += to_copy;
    str->internal.ptr[str->internal.len] = 0;
    res.value = to_copy;
    
    return res;
}

proven_err_t proven_u8str_reserve(proven_allocator_t alloc, proven_u8str_t *str, proven_size_t new_cap) {
    if (!str) return PROVEN_ERR_INVALID_ARG;
    if (new_cap <= str->internal.cap) return PROVEN_OK;
    if (!proven_alloc_is_valid(alloc)) return PROVEN_ERR_INVALID_ARG;
    
    proven_result_mem_mut_t new_mem = alloc.realloc_fn(alloc.ctx, str->internal.ptr, str->internal.cap, new_cap, PROVEN_DEFAULT_ALIGNMENT);
    if (!proven_is_ok(new_mem.err)) return new_mem.err;
    
    str->internal.ptr = (proven_byte_t*)new_mem.value.ptr;
    str->internal.cap = new_cap;
    return PROVEN_OK;
}

proven_err_t proven_u8str_append_grow(proven_allocator_t alloc, proven_u8str_t *str, proven_u8str_view_t data) {
    if (!str) return PROVEN_ERR_INVALID_ARG;
    if (data.size > 0 && !data.ptr) return PROVEN_ERR_INVALID_ARG;
    
    proven_size_t required;
    if (PROVEN_CKD_ADD(&required, str->internal.len, data.size)) return PROVEN_ERR_OVERFLOW;
    if (PROVEN_CKD_ADD(&required, required, 1)) return PROVEN_ERR_OVERFLOW;

    if (required > str->internal.cap) {
        if (!proven_alloc_is_valid(alloc)) {
            return PROVEN_ERR_INVALID_ARG;
        }

        proven_size_t new_cap = str->internal.cap == 0 ? 16 : str->internal.cap;
        while (new_cap < required) {
            if (PROVEN_CKD_MUL(&new_cap, new_cap, 2)) {
                new_cap = required;
                break;
            }
        }
        
        proven_bufref_t alias_ref = proven_bufref_capture(str->internal.ptr, str->internal.cap, data.ptr, data.size);
        if (alias_ref.valid && data.size > str->internal.cap - alias_ref.offset) {
            return PROVEN_ERR_INVALID_ARG;
        }

        proven_result_mem_mut_t new_mem = alloc.realloc_fn(alloc.ctx, str->internal.ptr, str->internal.cap, new_cap, PROVEN_DEFAULT_ALIGNMENT);
        if (!proven_is_ok(new_mem.err)) {
            return new_mem.err;
        }
        
        str->internal.ptr = (proven_byte_t*)new_mem.value.ptr;
        str->internal.cap = new_cap;
        
        if (alias_ref.valid) {
            data.ptr = (const proven_byte_t*)proven_bufref_rebase_const(alias_ref, str->internal.ptr);
        }
    }

    return proven_u8str_append(str, data);
}

proven_err_t proven_u8str_append_byte(proven_allocator_t alloc, proven_u8str_t *str, proven_u8 b) {
    proven_u8str_view_t v = { (const proven_byte_t*)&b, 1 };
    return proven_u8str_append_grow(alloc, str, v);
}

proven_result_cstr_t proven_u8str_view_to_cstr(proven_u8str_view_t view, proven_allocator_t alloc) {
    proven_result_cstr_t res = {0};
    if (!proven_alloc_is_valid(alloc)) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }
    if (view.size > 0 && !view.ptr) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }
    
    // Check for interior NUL
    for (proven_size_t i = 0; i < view.size; ++i) {
        if (view.ptr[i] == 0) {
            res.err = PROVEN_ERR_INVALID_ARG;
            return res;
        }
    }
    
    // Allocate exact View length + 1 (for implicitly enforced '\0')
    proven_size_t cap;
    if (PROVEN_CKD_ADD(&cap, view.size, 1)) {
        res.err = PROVEN_ERR_OVERFLOW;
        return res;
    }

    proven_result_mem_mut_t mem = alloc.alloc_fn(alloc.ctx, cap, PROVEN_DEFAULT_ALIGNMENT);
    if (!proven_is_ok(mem.err)) {
        res.err = mem.err;
        return res;
    }
    
    // Copy the slice correctly
    if (view.size > 0) {
        proven_sys_mem_copy(mem.value.ptr, view.ptr, view.size);
    }
    mem.value.ptr[view.size] = 0; // Null-terminator sealing
    
    res.err = PROVEN_OK;
    res.value = (const char*)mem.value.ptr;
    
    return res;
}

proven_size_t proven_cstr_len(const char *s) {
    if (!s) return 0;
    const char *p = s;
    // Iterate until null terminator, limited by size_t range implicitly
    while (*p != '\0') p++;
    proven_ptrdiff_t diff = p - s;
    if (diff < 0) return 0;
    return (proven_size_t)diff;
}

int proven_u8str_view_eq(proven_u8str_view_t a, proven_u8str_view_t b) {
    if (a.size != b.size) return 0;
    if (a.size == 0) return 1;
    if (!a.ptr || !b.ptr) return 0;
    return proven_sys_mem_cmp(a.ptr, b.ptr, a.size) == 0;
}

proven_size_t proven_u8str_view_find(proven_u8str_view_t haystack, proven_size_t start_offset, proven_u8str_view_t needle) {
    if (start_offset > haystack.size) return PROVEN_INDEX_NOT_FOUND;
    if (needle.size == 0) return start_offset; // Empty needle always matches at offset
    if (haystack.size > 0 && !haystack.ptr) return PROVEN_INDEX_NOT_FOUND;
    if (needle.size > 0 && !needle.ptr) return PROVEN_INDEX_NOT_FOUND;
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
    if (prefix.size == 0) return 1;
    if (str.size < prefix.size) return 0;
    if (!str.ptr || !prefix.ptr) return 0;
    return proven_sys_mem_cmp(str.ptr, prefix.ptr, prefix.size) == 0;
}

int proven_u8str_view_ends_with(proven_u8str_view_t str, proven_u8str_view_t suffix) {
    if (suffix.size == 0) return 1;
    if (str.size < suffix.size) return 0;
    if (!str.ptr || !suffix.ptr) return 0;
    proven_size_t offset = str.size - suffix.size;
    return proven_sys_mem_cmp(str.ptr + offset, suffix.ptr, suffix.size) == 0;
}

proven_u8str_view_t proven_u8str_view_slice(proven_u8str_view_t str, proven_size_t index, proven_size_t len) {
    if ((str.size > 0 && !str.ptr) || index >= str.size || len == 0) {
        return (proven_u8str_view_t){ .ptr = (const proven_byte_t*)0, .size = 0 };
    }
    // Clamp length
    proven_size_t actual_len = len;
    proven_size_t end_idx;
    if (PROVEN_CKD_ADD(&end_idx, index, len) || end_idx > str.size) {
        actual_len = str.size - index;
    }
    return (proven_u8str_view_t){ .ptr = str.ptr + index, .size = actual_len };
}

proven_err_t proven_u8str_replace_at(proven_u8str_t *str, proven_size_t index, proven_size_t old_len, proven_u8str_view_t data) {
    if (!str || !str->internal.ptr) return PROVEN_ERR_INVALID_ARG;
    if (data.size > 0 && !data.ptr) return PROVEN_ERR_INVALID_ARG;
    if (index > str->internal.len) return PROVEN_ERR_OUT_OF_BOUNDS; // index must be <= len, allow append at the exact end.
    
    // Clamp old_len to the actual available size backwards from the string end
    proven_size_t actual_old = old_len;
    proven_size_t total_end;
    if (PROVEN_CKD_ADD(&total_end, index, old_len) || total_end > str->internal.len) {
        actual_old = str->internal.len - index;
    }
    
    // Check total capacity relative to what we can do
    proven_size_t new_total_len;
    if (PROVEN_CKD_SUB(&new_total_len, str->internal.len, actual_old)) {
        return PROVEN_ERR_OVERFLOW;
    }
    if (PROVEN_CKD_ADD(&new_total_len, new_total_len, data.size)) {
        return PROVEN_ERR_OVERFLOW;
    }

    proven_size_t required;
    if (PROVEN_CKD_ADD(&required, new_total_len, 1)) {
        return PROVEN_ERR_OVERFLOW;
    }
    if (required > str->internal.cap) {
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }
    
    proven_byte_t *base = str->internal.ptr;
    
    // Check for logical corruption: replacement source aliases this string buffer 
    // AND the size changes requiring tail shifting.
    proven_bufref_t alias_ref = proven_bufref_capture(base, str->internal.cap, data.ptr, data.size);
    if (alias_ref.valid && data.size != actual_old && index + actual_old < str->internal.len) {
        return PROVEN_ERR_INVALID_ARG;
    }
    
    // Do we need to move the tail?
    if (data.size != actual_old && index + actual_old < str->internal.len) {
        proven_size_t tail_len = str->internal.len - (index + actual_old);
        proven_sys_mem_move(base + index + data.size, base + index + actual_old, tail_len);
    }
    
    // Copy new data
    if (data.size > 0) {
        proven_sys_mem_move(base + index, data.ptr, data.size);
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
    if (!str) return;
    proven_buf_destroy(alloc, &str->internal);
}
