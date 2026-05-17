#include "proven_sys_thread.h"

#if defined(_WIN32) || defined(_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct {
    proven_sys_thread_fn routine;
    void *arg;
} proven_win_thread_start_t;

static DWORD WINAPI proven_win_thread_trampoline(LPVOID p) {
    proven_win_thread_start_t *start = (proven_win_thread_start_t *)p;
    proven_sys_thread_fn routine = start->routine;
    void *arg = start->arg;
    HeapFree(GetProcessHeap(), 0, start);
    routine(arg);
    return 0;
}

proven_sys_thread_t proven_sys_thread_create(proven_sys_thread_fn routine, void* arg) {
    proven_win_thread_start_t *start = HeapAlloc(GetProcessHeap(), 0, sizeof(proven_win_thread_start_t));
    if (!start) return (proven_sys_thread_t){ .internal = NULL };
    
    start->routine = routine;
    start->arg = arg;

    HANDLE hThread = CreateThread(NULL, 0, proven_win_thread_trampoline, start, 0, NULL);
    if (!hThread) {
        HeapFree(GetProcessHeap(), 0, start);
        return (proven_sys_thread_t){ .internal = NULL };
    }
    
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
#include <stdlib.h>

typedef struct {
    pthread_t th;
} proven_posix_thread_box_t;

proven_sys_thread_t proven_sys_thread_create(proven_sys_thread_fn routine, void* arg) {
    proven_posix_thread_box_t *box = (proven_posix_thread_box_t *)malloc(sizeof(proven_posix_thread_box_t));
    if (!box) return (proven_sys_thread_t){ .internal = NULL };

    if (pthread_create(&box->th, NULL, routine, arg) != 0) {
        free(box);
        return (proven_sys_thread_t){ .internal = NULL };
    }
    return (proven_sys_thread_t){ .internal = (void*)box };
}

void proven_sys_thread_join(proven_sys_thread_t thread) {
    if (thread.internal) {
        proven_posix_thread_box_t *box = (proven_posix_thread_box_t *)thread.internal;
        pthread_join(box->th, NULL);
        free(box);
    }
}

void proven_sys_thread_yield(void) {
    sched_yield();
}

#endif
