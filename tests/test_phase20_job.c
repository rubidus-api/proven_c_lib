#include "proven.h"
#include "proven_test.h"
#include "proven/sysio.h"
#include "proven/job.h"
#include "proven/heap.h"
#include "../platform/proven_sys_thread.h"
#include <stdatomic.h>


static _Atomic(int) test_counter = 0;
static _Atomic(int) collision_tracker[1000] = {0};

static void sample_job_worker(void* arg) {
    int idx = (int)(intptr_t)arg;
    
    // Increment to capture execution execution
    atomic_fetch_add_explicit(&test_counter, 1, memory_order_relaxed);
    
    // Detect race conditions via strictly indexed validation
    atomic_fetch_add_explicit(&collision_tracker[idx], 1, memory_order_relaxed);
}

int main(void) {
    PROVEN_TEST_INFO("Running Phase 20: Multicore Job Pool Scheduler...");

    proven_allocator_t heap = proven_heap_allocator();
    
    // Spawn 4 Physical Worker Threads with a Ring Buffer matching a 1024 power-of-2 structure
    PROVEN_TEST_INFO("Initializing job system (4 workers, 1024 depth)...");
    proven_job_sys_t *sys = NULL;
    proven_err_t err = proven_job_system_init(heap, 4, 1024, &sys);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(err), "Testing condition: PROVEN_IS_OK(err)", "Review logic surrounding PROVEN_IS_OK(err)");
    PROVEN_TEST_ASSERT(sys != NULL, "Testing condition: sys != NULL", "Review logic surrounding sys != NULL");

    // Heavy multithreaded simulation inserting 1000 Tasks rapidly 
    PROVEN_TEST_INFO("  [job] Dispatching 1000 concurrent simulation tasks.");
    for (int i = 0; i < 1000; i++) {
        while (!proven_job_submit(sys, sample_job_worker, (void*)(intptr_t)i)) {
            // Unlikely to stall with 1024 cap and fast queues, but validates bounds checks.
            proven_sys_thread_yield();
        }
    }

    // Gracefully instruct hardware threads to shut down and flush 
    PROVEN_TEST_INFO("  [job] Committing shutdown synchronization barriers...");
    proven_job_system_close(sys);
    proven_job_system_destroy(sys); 

    PROVEN_TEST_ASSERT(atomic_load(&test_counter) == 1000, "Testing condition: atomic_load(&test_counter) == 1000", "Review logic surrounding atomic_load(&test_counter) == 1000");
    
    for (int i = 0; i < 1000; ++i) {
        // Evaluate memory collisions (every index must solely be executed perfectly 1 time without data corruption).
        PROVEN_TEST_ASSERT(atomic_load(&collision_tracker[i]) == 1, "Testing condition: atomic_load(&collision_tracker[i]) == 1", "Review logic surrounding atomic_load(&collision_tracker[i]) == 1");
    }

    PROVEN_TEST_PASS("All Phase 20 Job Processor Tests Passed Successfully!");
    return 0;
}
