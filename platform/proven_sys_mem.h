#ifndef PROVEN_PLATFORM_SYS_MEM_H
#define PROVEN_PLATFORM_SYS_MEM_H

#include "proven/types.h"

/**
 * @file proven_sys_mem.h
 * @brief Platform Abstraction Layer (PAL) for system memory allocation APIs.
 * This is the ONLY boundary where system or OS <stdlib.h> headers are permitted.
 */

/**
 * @brief Allocates an aligned chunk of raw memory from the external system (e.g., OS Heap).
 */
[[nodiscard]]
void* proven_sys_mem_alloc(proven_size_t size, proven_size_t align);

/**
 * @brief Reallocates memory via generic system calls efficiently maintaining alignment properties perfectly.
 */
[[nodiscard]]
void* proven_sys_mem_realloc(void* ptr, proven_size_t old_size, proven_size_t new_size, proven_size_t align);

/**
 * @brief Frees memory allocated by proven_sys_mem_alloc.
 */
void proven_sys_mem_free(void* ptr);

#endif /* PROVEN_PLATFORM_SYS_MEM_H */
