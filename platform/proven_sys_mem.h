#ifndef PROVEN_PLATFORM_SYS_MEM_H
#define PROVEN_PLATFORM_SYS_MEM_H

#include "proven/types.h"

/**
 * @file proven_sys_mem.h
 * @brief Platform Abstraction Layer (PAL) for system memory allocation APIs.
 * This is the ONLY boundary where system or OS <stdlib.h> headers are permitted.
 *
 * NOTE: For all memory manipulation functions (copy, move, zero, set), if 
 * size > 0 and any required pointer is NULL, the operation is skipped.
 * Callers should ensure pointers are valid if failure to operate is a logic error.
 */

#ifdef PROVEN_FREESTANDING

static inline void* proven_sys_mem_alloc(proven_size_t size, proven_size_t align) {
    (void)size; (void)align;
    return (void*)0;
}

static inline void* proven_sys_mem_realloc(void* ptr, proven_size_t old_size, proven_size_t new_size, proven_size_t align) {
    (void)ptr; (void)old_size; (void)new_size; (void)align;
    return (void*)0;
}

static inline void proven_sys_mem_free(void* ptr) {
    (void)ptr;
}

static inline void proven_sys_mem_copy(void* dst, const void* src, proven_size_t size) {
    if (size > 0 && dst && src) {
        char *d = (char *)dst;
        const char *s = (const char *)src;
        for (proven_size_t i = 0; i < size; ++i) d[i] = s[i];
    }
}

static inline void proven_sys_mem_move(void* dst, const void* src, proven_size_t size) {
    if (size > 0 && dst && src && dst != src) {
        char *d = (char *)dst;
        const char *s = (const char *)src;
        if (d < s) {
            for (proven_size_t i = 0; i < size; ++i) d[i] = s[i];
        } else {
            for (proven_size_t i = size; i > 0; --i) d[i - 1] = s[i - 1];
        }
    }
}

static inline void proven_sys_mem_zero(void* dst, proven_size_t size) {
    if (size > 0 && dst) {
        char *d = (char *)dst;
        for (proven_size_t i = 0; i < size; ++i) d[i] = 0;
    }
}

static inline void proven_sys_mem_set(void* dst, int value, proven_size_t size) {
    if (size > 0 && dst) {
        char *d = (char *)dst;
        char v = (char)value;
        for (proven_size_t i = 0; i < size; ++i) d[i] = v;
    }
}

static inline int proven_sys_mem_cmp(const void* s1, const void* s2, proven_size_t size) {
    if (size == 0) return 0;
    if (!s1 && !s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;
    for (proven_size_t i = 0; i < size; ++i) {
        if (p1[i] != p2[i]) return (int)p1[i] - (int)p2[i];
    }
    return 0;
}

#else

/**
 * @brief Allocates an aligned chunk of raw memory from the external system (e.g., OS Heap).
 */
[[nodiscard]]
void* proven_sys_mem_alloc(proven_size_t size, proven_size_t align);

/**
 * @brief Reallocates memory via generic system calls while maintaining alignment.
 */
[[nodiscard]]
void* proven_sys_mem_realloc(void* ptr, proven_size_t old_size, proven_size_t new_size, proven_size_t align);

/**
 * @brief Frees memory allocated by proven_sys_mem_alloc.
 */
void proven_sys_mem_free(void* ptr);

/**
 * @brief Copies memory from source to destination using the system routine.
 * Behavior is undefined if source and destination regions overlap.
 */
void proven_sys_mem_copy(void* dst, const void* src, proven_size_t size);

/**
 * @brief Moves memory from source to destination safely allowing for overlapping regions.
 */
void proven_sys_mem_move(void* dst, const void* src, proven_size_t size);

/**
 * @brief Zeroes out a region of memory.
 */
void proven_sys_mem_zero(void* dst, proven_size_t size);

/**
 * @brief Sets a region of memory to a specific byte value.
 */
void proven_sys_mem_set(void* dst, int value, proven_size_t size);

/**
 * @brief Compares memory between two regions.
 */
int proven_sys_mem_cmp(const void* s1, const void* s2, proven_size_t size);

#endif /* PROVEN_FREESTANDING */

#endif /* PROVEN_PLATFORM_SYS_MEM_H */
