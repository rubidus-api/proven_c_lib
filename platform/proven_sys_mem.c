#include "proven_sys_mem.h"
#include "proven/align.h"

#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdalign.h>

#if !defined(_MSC_VER) && !defined(__FreeBSD__) && !defined(__NetBSD__) && !defined(__OpenBSD__) && !defined(_WIN32)
// C11 standard declaration - explicitly provided to bypass header inclusion/macro guards.
extern void *aligned_alloc(size_t alignment, size_t size);
#endif

// THIS IS THE ONLY FILE PERMITTED TO INCLUDE THIS HEADER IN THE PROJECT

#if defined(_WIN32)
#include <malloc.h> // for _aligned_malloc and _aligned_free
#endif

void* proven_sys_mem_alloc(proven_size_t size, proven_size_t align) {
    if (size == 0) return NULL;
    if (align < alignof(max_align_t)) align = alignof(max_align_t);
    if (!proven_is_pow2(align)) return NULL;
    
    size_t padded;
    if (PROVEN_CKD_ADD(&padded, size, align - 1)) {
        return NULL;
    }
    size_t alloc_size = padded & ~((size_t)align - 1);
    
#if defined(_WIN32)
    return _aligned_malloc(alloc_size, (size_t)align);
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(_POSIX_VERSION) || defined(_POSIX_C_SOURCE)
    void *p = NULL;
    if (posix_memalign(&p, (size_t)align, alloc_size) != 0) return NULL;
    return p;
#else
    return aligned_alloc((size_t)align, alloc_size);
#endif
}

void* proven_sys_mem_realloc(void* ptr, proven_size_t old_size, proven_size_t new_size, proven_size_t align) {
    if (new_size == 0) {
        proven_sys_mem_free(ptr);
        return NULL;
    }
    if (!ptr) {
        return proven_sys_mem_alloc(new_size, align);
    }
    
    // Explicit manual block movement protecting boundary logic enforcing aligned spaces.
    void *new_ptr = proven_sys_mem_alloc(new_size, align);
    if (new_ptr) {
        proven_size_t copy_size = old_size < new_size ? old_size : new_size;
        proven_sys_mem_copy(new_ptr, ptr, copy_size);
        proven_sys_mem_free(ptr);
    }
    return new_ptr;
}

void proven_sys_mem_free(void* ptr) {
    if (!ptr) return;
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

void proven_sys_mem_copy(void* dst, const void* src, proven_size_t size) {
    if (size > 0 && dst && src) {
        memcpy(dst, src, size);
    }
}

void proven_sys_mem_move(void* dst, const void* src, proven_size_t size) {
    if (size > 0 && dst && src) {
        memmove(dst, src, size);
    }
}

void proven_sys_mem_zero(void* dst, proven_size_t size) {
    if (size > 0 && dst) {
        memset(dst, 0, size);
    }
}

void proven_sys_mem_set(void* dst, int value, proven_size_t size) {
    if (size > 0 && dst) {
        memset(dst, value, size);
    }
}

int proven_sys_mem_cmp(const void* s1, const void* s2, proven_size_t size) {
    if (size == 0) return 0;
    if (!s1 && !s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;

    return memcmp(s1, s2, size);
}

const void* proven_sys_mem_chr(const void* s, int c, proven_size_t size) {
    if (!s || size == 0) return (const void*)0;
    return memchr(s, c, size);
}
