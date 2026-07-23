#include "proven.h"
#include "proven_test.h"
#include "proven/sysio.h"
#include "proven/job.h"
#include "proven/heap.h"
#include "proven/time.h"
#include "../platform/proven_sys_thread.h"
#include <stdatomic.h>


static _Atomic(int) test_counter = 0;
static _Atomic(int) collision_tracker[1000] = {0};
static _Atomic(bool) stale_blocker_started = false;
static _Atomic(bool) stale_blocker_release = false;
static _Atomic(unsigned int) stale_jobs_executed = 0;

static void sample_job_worker(void* arg) {
    int idx = (int)(intptr_t)arg;
    
    // Increment to capture execution execution
    atomic_fetch_add_explicit(&test_counter, 1, memory_order_relaxed);
    
    // Detect race conditions via strictly indexed validation
    atomic_fetch_add_explicit(&collision_tracker[idx], 1, memory_order_relaxed);
}

static void stale_permit_blocker(void *arg) {
    (void)arg;
    atomic_store_explicit(&stale_blocker_started, true, memory_order_release);
    while (!atomic_load_explicit(&stale_blocker_release,
                                 memory_order_acquire)) {
        proven_sys_thread_yield();
    }
}

static void stale_permit_job(void *arg) {
    (void)arg;
    atomic_fetch_add_explicit(&stale_jobs_executed, 1u, memory_order_relaxed);
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

    /*
     * Let every worker reach the empty-queue path before publishing work. A
     * parked implementation must retain the later wake rather than losing it
     * between the empty observation and the wait.
     */
    PROVEN_TEST_INFO("  [job] Letting workers become idle before the first submission.");
    proven_time_sleep(25);

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

    /*
     * Closing an entirely idle system must wake every parked worker. Without
     * the close permits, destroy would wait forever in join.
     */
    PROVEN_TEST_INFO("  [job] Closing a system whose workers are all parked.");
    sys = NULL;
    err = proven_job_system_init(heap, 4, 8, &sys);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(err) && sys != NULL,
                       "idle job system init should succeed",
                       "Inspect semaphore and worker initialization.");
    proven_time_sleep(25);
    proven_job_system_close(sys);
    proven_job_system_destroy(sys);

    /*
     * A caller of execute_one can claim work before a worker consumes the
     * corresponding permit. Those retained but now stale permits must only
     * cause an empty recheck; they must not duplicate work or prevent close.
     */
    PROVEN_TEST_INFO("  [job] Draining work externally before workers consume its permits.");
    atomic_store_explicit(&stale_blocker_started, false, memory_order_relaxed);
    atomic_store_explicit(&stale_blocker_release, false, memory_order_relaxed);
    atomic_store_explicit(&stale_jobs_executed, 0u, memory_order_relaxed);
    sys = NULL;
    err = proven_job_system_init(heap, 1, 8, &sys);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(err) && sys != NULL,
                       "stale-permit job system init should succeed",
                       "Inspect semaphore and worker initialization.");
    PROVEN_TEST_ASSERT(proven_job_submit(sys, stale_permit_blocker, NULL),
                       "blocking job submission should succeed",
                       "Inspect queue publication and worker wakeup.");
    while (!atomic_load_explicit(&stale_blocker_started,
                                 memory_order_acquire)) {
        proven_sys_thread_yield();
    }
    for (unsigned int i = 0; i < 4u; ++i) {
        PROVEN_TEST_ASSERT(proven_job_submit(sys, stale_permit_job, NULL),
                           "stale-permit job submission should succeed",
                           "Inspect queue capacity and publication.");
    }
    for (unsigned int i = 0; i < 4u; ++i) {
        PROVEN_TEST_ASSERT(proven_job_execute_one(sys),
                           "the calling thread should drain each queued job",
                           "Inspect the external consumer queue path.");
    }
    proven_job_system_close(sys);
    atomic_store_explicit(&stale_blocker_release, true, memory_order_release);
    proven_job_system_destroy(sys);
    PROVEN_TEST_ASSERT(
        atomic_load_explicit(&stale_jobs_executed, memory_order_relaxed) == 4u,
        "externally drained jobs should execute exactly once",
        "Inspect stale semaphore permit handling and queue claims.");

    PROVEN_TEST_PASS("All Phase 20 Job Processor Tests Passed Successfully!");
    return 0;
}
