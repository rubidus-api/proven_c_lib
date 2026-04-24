#include "proven/job.h"
#include "proven/memory.h"
#include "../../platform/proven_sys_thread.h"
#include <stdatomic.h>

typedef struct {
    _Atomic(size_t) sequence;
    proven_job_t data;
} proven_job_cell_t;

typedef struct {
    size_t buffer_mask;
    proven_job_cell_t* buffer;
    _Atomic(size_t) enqueue_pos;
    _Atomic(size_t) dequeue_pos;
} proven_job_queue_t;

static proven_job_queue_t g_queue = {0};
static _Atomic(bool) g_shutdown_flag = false;
static proven_sys_thread_t* g_threads = NULL;
static size_t g_num_threads = 0;
static proven_allocator_t g_alloc = {0};

bool proven_job_execute_one(void) {
    proven_job_cell_t* cell;
    size_t pos = atomic_load_explicit(&g_queue.dequeue_pos, memory_order_relaxed);
    
    for (;;) {
        cell = &g_queue.buffer[pos & g_queue.buffer_mask];
        size_t seq = atomic_load_explicit(&cell->sequence, memory_order_acquire);
        proven_ptrdiff_t dif = (proven_ptrdiff_t)seq - (proven_ptrdiff_t)(pos + 1);
        
        if (dif == 0) {
            if (atomic_compare_exchange_weak_explicit(&g_queue.dequeue_pos, &pos, pos + 1, memory_order_relaxed, memory_order_relaxed)) {
                break;
            }
        } else if (dif < 0) {
            return false; // Queue is entirely empty
        } else {
            pos = atomic_load_explicit(&g_queue.dequeue_pos, memory_order_relaxed);
        }
    }
    
    proven_job_t task = cell->data;
    atomic_store_explicit(&cell->sequence, pos + g_queue.buffer_mask + 1, memory_order_release);
    
    if (task.routine) {
        task.routine(task.arg);
    }
    return true;
}

static void* worker_main(void* arg) {
    (void)arg;
    while (!atomic_load_explicit(&g_shutdown_flag, memory_order_relaxed)) {
        if (!proven_job_execute_one()) {
            proven_sys_thread_yield(); // Back-off to prevent 100% CPU starvation
        }
    }
    
    // Once shutdown is signaled, exhaust any remaining tasks before committing suicide
    while (proven_job_execute_one()) {}
    
    return NULL;
}

proven_err_t proven_job_system_init(proven_allocator_t alloc, proven_size_t num_workers, proven_size_t max_queue_capacity) {
    // Capacity implicitly required to be a power of 2 for fast wrapping via bitwise manipulation
    if (max_queue_capacity < 2 || (max_queue_capacity & (max_queue_capacity - 1)) != 0) {
        return PROVEN_ERR_INVALID_ARG;
    }
    
    proven_result_mem_mut_t q_res = alloc.alloc_fn(alloc.ctx, sizeof(proven_job_cell_t) * max_queue_capacity, 64);
    if (!proven_is_ok(q_res.err)) return q_res.err;
    
    proven_result_mem_mut_t t_res = alloc.alloc_fn(alloc.ctx, sizeof(proven_sys_thread_t) * num_workers, 8);
    if (!proven_is_ok(t_res.err)) {
        alloc.free_fn(alloc.ctx, q_res.value.ptr);
        return t_res.err;
    }

    g_alloc = alloc;
    g_queue.buffer_mask = max_queue_capacity - 1;
    g_queue.buffer = (proven_job_cell_t*)q_res.value.ptr;
    
    for (size_t i = 0; i < max_queue_capacity; ++i) {
        atomic_init(&g_queue.buffer[i].sequence, i);
    }
    
    atomic_init(&g_queue.enqueue_pos, 0);
    atomic_init(&g_queue.dequeue_pos, 0);
    atomic_init(&g_shutdown_flag, false);
    
    g_num_threads = num_workers;
    g_threads = (proven_sys_thread_t*)t_res.value.ptr;
    
    for (size_t i = 0; i < num_workers; ++i) {
        g_threads[i] = proven_sys_thread_create(worker_main, NULL);
    }
    
    return PROVEN_OK;
}

bool proven_job_submit(void (*routine)(void*), void* arg) {
    proven_job_cell_t* cell;
    size_t pos = atomic_load_explicit(&g_queue.enqueue_pos, memory_order_relaxed);
    
    for (;;) {
        cell = &g_queue.buffer[pos & g_queue.buffer_mask];
        size_t seq = atomic_load_explicit(&cell->sequence, memory_order_acquire);
        proven_ptrdiff_t dif = (proven_ptrdiff_t)seq - (proven_ptrdiff_t)pos;
        
        if (dif == 0) {
            if (atomic_compare_exchange_weak_explicit(&g_queue.enqueue_pos, &pos, pos + 1, memory_order_relaxed, memory_order_relaxed)) {
                break; // Claimed successfully
            }
        } else if (dif < 0) {
            return false; // Queue is entirely full
        } else {
            pos = atomic_load_explicit(&g_queue.enqueue_pos, memory_order_relaxed);
        }
    }
    
    cell->data.routine = routine;
    cell->data.arg = arg;
    atomic_store_explicit(&cell->sequence, pos + 1, memory_order_release); // Commit to workers
    return true;
}

void proven_job_system_shutdown(void) {
    atomic_store_explicit(&g_shutdown_flag, true, memory_order_relaxed);
    
    for (size_t i = 0; i < g_num_threads; ++i) {
        proven_sys_thread_join(g_threads[i]);
    }
    
    if (g_alloc.alloc_fn) {
        g_alloc.free_fn(g_alloc.ctx, g_queue.buffer);
        g_alloc.free_fn(g_alloc.ctx, g_threads);
    }
    
    g_num_threads = 0;
    g_threads = NULL;
}
