#include "proven_test.h"
#include "proven/job.h"
#include "proven/heap.h"
#include "proven_sys_thread.h"
#include <stdint.h>
#include <stdatomic.h>

#define JOB_STRESS_WORKERS 4u
#define JOB_STRESS_PRODUCERS 4u
#define JOB_STRESS_PER_PRODUCER 1024u
#define JOB_STRESS_TOTAL (JOB_STRESS_PRODUCERS * JOB_STRESS_PER_PRODUCER)
#define JOB_STRESS_QUEUE_CAP 32u

static _Atomic unsigned int g_hits[JOB_STRESS_TOTAL];
static _Atomic unsigned int g_total;

typedef struct {
    proven_job_sys_t *sys;
    unsigned int base;
    unsigned int count;
} job_stress_producer_ctx_t;

static void job_stress_worker(void *arg) {
    unsigned int idx = (unsigned int)(uintptr_t)arg;
    atomic_fetch_add_explicit(&g_hits[idx], 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_total, 1u, memory_order_relaxed);
}

static void *job_stress_producer(void *arg) {
    job_stress_producer_ctx_t *ctx = (job_stress_producer_ctx_t *)arg;
    for (unsigned int i = 0; i < ctx->count; ++i) {
        unsigned int idx = ctx->base + i;
        while (!proven_job_submit(ctx->sys, job_stress_worker, (void *)(uintptr_t)idx)) {
            proven_sys_thread_yield();
        }
        if ((i & 7u) == 0u) {
            proven_sys_thread_yield();
        }
    }
    return NULL;
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_job_stress_tsan",
        "Stress the hosted job system with concurrent producers so the queue and shutdown path see a denser interleaving profile under TSAN.",
        "Inspect the queue admission state, sequence counters, and worker wake/shutdown ordering if a duplicate or missing job appears."
    );

    proven_job_sys_t *sys = NULL;
    proven_err_t err = proven_job_system_init(proven_heap_allocator(), JOB_STRESS_WORKERS, JOB_STRESS_QUEUE_CAP, &sys);
    PROVEN_TEST_ASSERT(err == PROVEN_OK && sys != NULL, "job system init should succeed for the stress harness", "Inspect job system initialization or the heap allocator if the stress harness cannot start.");

    for (unsigned int i = 0; i < JOB_STRESS_TOTAL; ++i) {
        atomic_store_explicit(&g_hits[i], 0u, memory_order_relaxed);
    }
    atomic_store_explicit(&g_total, 0u, memory_order_relaxed);

    PROVEN_TEST_SECTION(
        "concurrent producer pressure",
        "Launch multiple producers that keep submitting until the bounded queue accepts every job.",
        "Inspect proven_job_submit and queue admission if producers stall or a job is dropped."
    );

    job_stress_producer_ctx_t ctx[JOB_STRESS_PRODUCERS];
    proven_sys_thread_t threads[JOB_STRESS_PRODUCERS];
    for (unsigned int producer = 0; producer < JOB_STRESS_PRODUCERS; ++producer) {
        ctx[producer].sys = sys;
        ctx[producer].base = producer * JOB_STRESS_PER_PRODUCER;
        ctx[producer].count = JOB_STRESS_PER_PRODUCER;
        threads[producer] = proven_sys_thread_create(job_stress_producer, &ctx[producer]);
        PROVEN_TEST_ASSERT(threads[producer].internal != NULL, "producer thread creation should succeed", "Inspect the thread PAL if producer thread creation fails.");
    }

    for (unsigned int producer = 0; producer < JOB_STRESS_PRODUCERS; ++producer) {
        proven_sys_thread_join(threads[producer]);
    }

    proven_job_system_close(sys);
    proven_job_system_destroy(sys);

    PROVEN_TEST_SECTION(
        "exactly-once execution",
        "Verify every submitted job ran once and only once after the queue drained and the workers shut down.",
        "Inspect job submission, queue claiming, or shutdown synchronization if a count drifts away from one."
    );

    PROVEN_TEST_ASSERT(atomic_load_explicit(&g_total, memory_order_relaxed) == JOB_STRESS_TOTAL, "all stressed jobs should run exactly once", "Inspect the queue or worker wakeup path if the total execution count does not match the submission count.");
    for (unsigned int i = 0; i < JOB_STRESS_TOTAL; ++i) {
        unsigned int hits = atomic_load_explicit(&g_hits[i], memory_order_relaxed);
        PROVEN_TEST_ASSERT(hits == 1u, "each stressed job should run once", "Inspect the queue, worker execution, or duplicate submission handling if a slot count drifts away from one.");
    }

    PROVEN_TEST_PASS("job stress harness completed.");
    return 0;
}
