#ifndef PROVEN_PLATFORM_SYS_THREAD_H
#define PROVEN_PLATFORM_SYS_THREAD_H

#include <stdbool.h>

/**
 * @file proven_sys_thread.h
 * @brief Platform Abstraction Layer for OS Threads and CPU Yielding.
 */

typedef struct {
    void* internal;
} proven_sys_thread_t;

/**
 * @brief Thread routine signature.
 */
typedef void* (*proven_sys_thread_fn)(void* arg);

/**
 * @brief Spawns a physical OS thread.
 */
[[nodiscard]]
proven_sys_thread_t proven_sys_thread_create(proven_sys_thread_fn routine, void* arg);

/**
 * @brief Block calling thread until the target thread terminates.
 */
void proven_sys_thread_join(proven_sys_thread_t thread);

/**
 * @brief Advise the OS to reschedule CPU time away from the current thread.
 */
void proven_sys_thread_yield(void);

#endif /* PROVEN_PLATFORM_SYS_THREAD_H */
