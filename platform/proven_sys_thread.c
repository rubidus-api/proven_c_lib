#include "proven_sys_thread.h"

#if defined(_WIN32) || defined(_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

proven_sys_thread_t proven_sys_thread_create(proven_sys_thread_fn routine, void* arg) {
    HANDLE hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)routine, arg, 0, NULL);
    return (proven_sys_thread_t){ .internal = (void*)hThread };
}

void proven_sys_thread_join(proven_sys_thread_t thread) {
    if (thread.internal) {
        WaitForSingleObject((HANDLE)thread.internal, INFINITE);
        CloseHandle((HANDLE)thread.internal);
    }
}

void proven_sys_thread_yield(void) {
    SwitchToThread();
}

#else
#include <pthread.h>
#include <sched.h>
#include <stdint.h>

proven_sys_thread_t proven_sys_thread_create(proven_sys_thread_fn routine, void* arg) {
    pthread_t th;
    if (pthread_create(&th, NULL, routine, arg) != 0) {
        return (proven_sys_thread_t){ .internal = NULL };
    }
    return (proven_sys_thread_t){ .internal = (void*)(intptr_t)th };
}

void proven_sys_thread_join(proven_sys_thread_t thread) {
    if (thread.internal) {
        pthread_join((pthread_t)(intptr_t)thread.internal, NULL);
    }
}

void proven_sys_thread_yield(void) {
    sched_yield();
}

#endif
