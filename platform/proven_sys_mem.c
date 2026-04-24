#include "proven_sys_mem.h"

#include <stdlib.h>
#include <string.h>

#if !defined(_MSC_VER) && !defined(__FreeBSD__) && !defined(__NetBSD__) && !defined(__OpenBSD__) && !defined(_WIN32)
// C11 standard declaration - explicitly provided to bypass header inclusion/macro guards.
extern void *aligned_alloc(size_t alignment, size_t size);
#endif

// THIS IS THE ONLY FILE PERMITTED TO INCLUDE THIS HEADER IN THE PROJECT

#if defined(_WIN32)
#include <malloc.h> // for _aligned_malloc and _aligned_free
#endif

void* proven_sys_mem_alloc(proven_size_t size, proven_size_t align) {
    if (align == 0) align = 1;
    size_t alloc_size = (size + align - 1) & ~((size_t)align - 1);
    
#if defined(_WIN32)
    return _aligned_malloc(alloc_size, (size_t)align);
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
        memcpy(new_ptr, ptr, copy_size);
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
