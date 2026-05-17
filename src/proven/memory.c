#include "proven/memory.h"
#include "../../platform/proven_sys_mem.h"

/**
 * @file memory.c
 * @brief Implementation of memory-related non-inline functions.
 * Currently, core slicing is inlined in headers for performance,
 * but this file serves as the library's memory core object.
 */

int proven_memcmp(const void *s1, const void *s2, proven_size_t size) {
    return proven_sys_mem_cmp(s1, s2, size); // forward to sys
}
