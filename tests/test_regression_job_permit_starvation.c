#include "proven_test.h"
#include "proven/job.h"
#include "proven/heap.h"
#include "proven/time.h"
#include "proven_sys_thread.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * The job system deadlocked, and the shape of it is worth keeping.
 *
 * A permit on the workers' semaphore used to mean "take exactly one job": a worker woke, ran one
 * job, and went back to waiting. That is sound only if a woken worker can always find the job its
 * permit was posted for - and it cannot. The queue hands out slots in order, so a producer that
 * has claimed slot n and not yet published it hides slot n+1 from every consumer. The worker woken
 * by the permit for n+1 reads an empty queue, spends the permit, and parks. A moment later slot n
 * is published and the queue is no longer empty, but the permit that would have announced the work
 * is gone.
 *
 * Lose enough of those and the queue stops draining: work sits in it, no worker is awake to take
 * it, and because the queue never empties no worker reaches its exit test either - so close and
 * destroy wait for threads that will never finish. Under load the stress harness hung in 18 runs
 * out of 40.
 *
 * The fix is that a permit now means "there may be work", and a woken worker drains the queue
 * rather than taking one job from it, so a spent permit cannot strand the jobs behind it.
 *
 * This test reproduces the pressure that provoked it: many more producers than cores, a queue far
 * too small for them, and jobs small enough that the window between claiming a slot and publishing
 * it is a large share of the work. A deadlock here does not fail an assertion - it stops - so a
 * watchdog thread turns the hang into a reported failure with a bounded runtime.
 */

enum {
    JOB_STARVE_WORKERS = 8,
    JOB_STARVE_PRODUCERS = 24,
    JOB_STARVE_PER_PRODUCER = 256,
    JOB_STARVE_QUEUE_CAP = 4,          /* deliberately tiny: maximum contention per slot */
    JOB_STARVE_ROUNDS = 6,
    JOB_STARVE_WATCHDOG_SECONDS = 60
};

#define JOB_STARVE_TOTAL (JOB_STARVE_PRODUCERS * JOB_STARVE_PER_PRODUCER)

static _Atomic unsigned int g_ran;
static _Atomic unsigned int g_round;
static _Atomic bool g_finished;

typedef struct {
    proven_job_sys_t *sys;
    unsigned int count;
} job_starve_ctx_t;

static void job_starve_task(void *arg) {
    (void)arg;
    atomic_fetch_add_explicit(&g_ran, 1u, memory_order_relaxed);
}

static void *job_starve_producer(void *arg) {
    job_starve_ctx_t *ctx = (job_starve_ctx_t *)arg;
    for (unsigned int i = 0; i < ctx->count; ++i) {
        while (!proven_job_submit(ctx->sys, job_starve_task, NULL)) {
            proven_sys_thread_yield();
        }
    }
    return NULL;
}

/*
 * The watchdog is the only way this failure can be reported at all: a deadlocked run produces no
 * wrong answer to assert on, it produces no answer. Waiting a bounded time and then saying so is
 * what turns "the suite hangs" into "this test failed, here is the round it stopped in".
 */
static void *job_starve_watchdog(void *arg) {
    (void)arg;
    for (int i = 0; i < JOB_STARVE_WATCHDOG_SECONDS * 10; ++i) {
        if (atomic_load_explicit(&g_finished, memory_order_acquire)) return NULL;
        proven_time_sleep(100u);
    }
    printf("[PROVEN][TEST][FAIL] job system deadlocked: round %u never completed\n",
           atomic_load_explicit(&g_round, memory_order_relaxed));
    printf("[PROVEN][TEST][FAIL_HINT] A worker is parked on the work semaphore with jobs still in "
           "the queue. Check that a woken worker DRAINS the queue rather than taking one job per "
           "permit - a permit spent on an empty read must not strand the work behind it.\n");
    fflush(stdout);
    _Exit(1);
    return NULL;
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_regression_job_permit_starvation",
        "A permit spent on an empty queue must not strand the jobs behind it. Treating one permit as one job let a producer that had claimed a slot but not published it hide the next slot from every consumer, so the queue stopped draining and close and destroy waited for ever.",
        "If this hangs or the watchdog fires, a worker is parked while the queue still holds work: check that a woken worker drains the queue instead of taking a single job per permit."
    );

    atomic_store_explicit(&g_finished, false, memory_order_relaxed);
    proven_sys_thread_t watchdog = proven_sys_thread_create(job_starve_watchdog, NULL);
    PROVEN_TEST_ASSERT(watchdog.internal != NULL, "the watchdog thread must start",
                       "Without it a deadlock hangs the suite instead of failing this test.");

    PROVEN_TEST_SECTION(
        "a tiny queue under heavy producer pressure still drains and shuts down",
        "More producers than cores and a four-slot queue, so the window between claiming a slot and publishing it is hit constantly.",
        "Inspect the worker wake path: one permit must be able to clear the whole queue."
    );

    for (unsigned int round = 0; round < JOB_STARVE_ROUNDS; ++round) {
        atomic_store_explicit(&g_round, round, memory_order_relaxed);
        atomic_store_explicit(&g_ran, 0u, memory_order_relaxed);

        proven_job_sys_t *sys = NULL;
        proven_err_t err = proven_job_system_init(proven_heap_allocator(), JOB_STARVE_WORKERS,
                                                  JOB_STARVE_QUEUE_CAP, &sys);
        PROVEN_TEST_ASSERT(err == PROVEN_OK && sys != NULL,
                           "the job system must start for each round",
                           "Inspect job system initialization or the heap allocator.");

        job_starve_ctx_t ctx = { .sys = sys, .count = JOB_STARVE_PER_PRODUCER };
        proven_sys_thread_t producers[JOB_STARVE_PRODUCERS];
        for (unsigned int i = 0; i < JOB_STARVE_PRODUCERS; ++i) {
            producers[i] = proven_sys_thread_create(job_starve_producer, &ctx);
            PROVEN_TEST_ASSERT(producers[i].internal != NULL, "producer threads must start",
                               "Inspect the thread PAL if producer creation fails.");
        }
        for (unsigned int i = 0; i < JOB_STARVE_PRODUCERS; ++i) {
            proven_sys_thread_join(producers[i]);
        }

        /* Both of these blocked for ever when a permit had been spent on an empty read. */
        proven_job_system_close(sys);
        proven_job_system_destroy(sys);

        unsigned int ran = atomic_load_explicit(&g_ran, memory_order_relaxed);
        PROVEN_TEST_ASSERT(ran == (unsigned int)JOB_STARVE_TOTAL,
                           "every accepted job must run before destroy returns",
                           "A job left in the queue at destroy means the drain path missed it.");
    }

    atomic_store_explicit(&g_finished, true, memory_order_release);
    proven_sys_thread_join(watchdog);

    PROVEN_TEST_PASS("the queue drains and the workers shut down under permit starvation.");
    return 0;
}
