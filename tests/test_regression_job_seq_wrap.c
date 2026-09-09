/*
 * RFC-0006 H-004: the job queue compared two positions by casting both to a signed type
 * and subtracting.
 *
 *     proven_ptrdiff_t dif = (proven_ptrdiff_t)seq - (proven_ptrdiff_t)pos;
 *
 * The queue's counters run forward for ever and wrap; only the DISTANCE between them is
 * ever small. At the sign boundary those two casts can produce PTRDIFF_MAX and PTRDIFF_MIN,
 * and their difference does not exist in the type - even though the distance being asked
 * about is -1. That is signed overflow: undefined behaviour, not a wrong-but-predictable
 * number, and a legitimately full queue reaches it with no concurrency involved. UBSan
 * reports it from a queue of capacity two seeded at that boundary.
 *
 * The comparison now happens in the unsigned counter type - where wrapping is defined and
 * is the modular arithmetic the algorithm actually wants - and is classified by the high
 * bit of the distance.
 *
 * This test does not seed a live queue: doing that means compiling a second copy of the
 * implementation into the test, and one implementation compiled twice is a thing that can
 * disagree with itself. It checks the shared classifier directly, at every boundary the
 * defect lived at, and pins that the queue paths use it.
 */

#include "proven.h"
#include "proven_test.h"
#include "../src/proven/proven_internal_jobseq.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

static char *read_text_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static _Atomic int executed;

static void counting_job(void *arg) {
    (void)arg;
    atomic_fetch_add_explicit(&executed, 1, memory_order_relaxed);
}

int main(void) {
    PROVEN_TEST_SUITE("queue sequence comparison at the sign boundary (RFC-0006 H-004)",
        "The queue's positions wrap. Only their distance is small, so the comparison has to be modular - a signed subtraction of two wrapped counters is undefined at the boundary they wrap around.",
        "A failure here means the classifier disagrees with the modular distance, or the queue paths went back to computing a signed difference.");

    const proven_size_t M = PROVEN_SIZE_MAX;
    const proven_size_t SIGN = PROVEN_JOB_SEQ_SIGN_BIT;   /* PTRDIFF_MAX + 1, as an unsigned value */

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the classifier answers the modular distance, everywhere",
        "READY means the cell is exactly at this position, BEHIND means it is a lap behind - a full producer queue or an empty consumer queue - and AHEAD means someone else moved first.",
        "The distances checked are small: it is the POSITIONS that are extreme. That is the whole point - a distance of -1 must read as -1 no matter where in the counter range the two positions sit.");
    // ---------------------------------------------------------------
    {
        /* The exact state the RFC's reproduction seeds: capacity two, enqueue position at
         * PTRDIFF_MAX + 1, a cell one behind it. The old expression cast these to
         * PTRDIFF_MIN and PTRDIFF_MAX and subtracted. */
        PROVEN_TEST_ASSERT(proven_job_cell_state(SIGN - 1u, SIGN) == PROVEN_JOB_CELL_BEHIND,
            "a cell one lap behind, straddling the sign boundary, is BEHIND",
            "This is the RFC's reproduction. The queue is full and submit must refuse, without touching the cell.");
        PROVEN_TEST_ASSERT(proven_job_cell_state(SIGN, SIGN) == PROVEN_JOB_CELL_READY,
            "a cell exactly at the boundary position is READY", "");
        PROVEN_TEST_ASSERT(proven_job_cell_state(SIGN + 1u, SIGN) == PROVEN_JOB_CELL_AHEAD,
            "a cell one ahead of the boundary position is AHEAD", "");

        /* Every other place the counters can be. */
        const proven_size_t positions[] = { 0u, 1u, 2u, SIGN - 2u, SIGN - 1u, SIGN, SIGN + 1u, M - 1u, M };
        for (size_t i = 0; i < sizeof positions / sizeof positions[0]; ++i) {
            proven_size_t p = positions[i];
            PROVEN_TEST_ASSERT(proven_job_cell_state(p, p) == PROVEN_JOB_CELL_READY,
                "a cell at the position is READY, wherever the position is", "");
            PROVEN_TEST_ASSERT(proven_job_cell_state(p - 1u, p) == PROVEN_JOB_CELL_BEHIND,
                "one behind is BEHIND, including across the wrap to zero and across the sign boundary",
                "p - 1 wraps for p == 0. Unsigned wrapping is defined and is exactly the modular distance wanted here.");
            PROVEN_TEST_ASSERT(proven_job_cell_state(p + 1u, p) == PROVEN_JOB_CELL_AHEAD,
                "and one ahead is AHEAD, on the same terms", "");
        }

        /* The far edges of the classification, which is where an off-by-one in the sign-bit
         * test would show. A distance of exactly half the range is the first BEHIND. */
        PROVEN_TEST_ASSERT(proven_job_cell_state(SIGN - 1u, 0u) == PROVEN_JOB_CELL_AHEAD,
            "the largest AHEAD distance is one below half the range", "");
        PROVEN_TEST_ASSERT(proven_job_cell_state(SIGN, 0u) == PROVEN_JOB_CELL_BEHIND,
            "and half the range itself already reads as BEHIND",
            "This is why the capacity has to stay strictly below half the range: past it, ahead and behind stop being distinguishable.");
        PROVEN_TEST_ASSERT(proven_job_cell_state(M, 0u) == PROVEN_JOB_CELL_BEHIND,
            "the furthest BEHIND distance is -1", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a capacity that would make the comparison ambiguous is refused before anything is allocated",
        "The modular comparison only tells ahead from behind while a live distance stays strictly inside half the counter range.",
        "An impossible capacity used to reach the allocator and be refused there, for the wrong reason. It has to be refused for this reason, here.");
    // ---------------------------------------------------------------
    {
        proven_job_sys_t *sys = NULL;
        PROVEN_TEST_ASSERT(proven_job_system_init(proven_heap_allocator(), 1, PROVEN_JOB_MAX_CAPACITY * 2u, &sys) == PROVEN_ERR_INVALID_ARG,
            "a capacity at half the counter range is INVALID_ARG", "");
        PROVEN_TEST_ASSERT(sys == NULL, "and nothing is handed back", "");
        PROVEN_TEST_ASSERT(proven_job_system_init(proven_heap_allocator(), 1, 3, &sys) == PROVEN_ERR_INVALID_ARG,
            "a capacity that is not a power of two is still INVALID_ARG", "The existing guards must not have been disturbed.");
        PROVEN_TEST_ASSERT(proven_job_system_init(proven_heap_allocator(), 1, 1, &sys) == PROVEN_ERR_INVALID_ARG,
            "and so is a capacity below two", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the queue paths compute no signed difference of positions",
        "The classifier is only worth having if both call sites use it. Two copies of a comparison this easy to get wrong are two chances to get it wrong differently.",
        "If this fails, someone reintroduced a (proven_ptrdiff_t) cast in job.c. The distance must be taken in the unsigned counter type.");
    // ---------------------------------------------------------------
    {
        char *src = read_text_file("src/proven/job.c");
        PROVEN_TEST_ASSERT(src != NULL, "src/proven/job.c must be readable from the build's working directory", "");
        if (src) {
            PROVEN_TEST_ASSERT(strstr(src, "proven_ptrdiff_t") == NULL,
                "job.c must not cast a queue position to a signed type",
                "That cast is the defect: two positions straddling the sign boundary become PTRDIFF_MAX and PTRDIFF_MIN, and subtracting them is undefined.");
            PROVEN_TEST_ASSERT(strstr(src, "proven_job_cell_state(seq, pos)") != NULL &&
                               strstr(src, "proven_job_cell_state(seq, pos + 1)") != NULL,
                "both the producer and the consumer classify through the shared helper", "");
            free(src);
        }
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("an ordinary queue still submits, drains, and reports full",
        "A comparison rewritten for a boundary nobody reaches in practice must still be right in the middle of the range, where every real caller lives.",
        "Inspect proven_job_submit and proven_job_execute_one in src/proven/job.c.");
    // ---------------------------------------------------------------
    {
        proven_job_sys_t *sys = NULL;
        PROVEN_TEST_ASSERT(proven_is_ok(proven_job_system_init(proven_heap_allocator(), 1, 2, &sys)) && sys != NULL,
            "a capacity-two queue is created", "");

        /* One worker thread is already draining, so this cannot assert an exact full/reject
         * boundary. What it can assert is that submission and execution keep working and
         * that shutdown completes - the shape a real caller sees. */
        int accepted = 0;
        for (int i = 0; i < 64; ++i) {
            if (proven_job_submit(sys, counting_job, NULL)) accepted++;
        }
        PROVEN_TEST_ASSERT(accepted > 0, "a queue with room accepts work", "");
        proven_job_system_destroy(sys);
        int ran = atomic_load_explicit(&executed, memory_order_relaxed);
        PROVEN_TEST_ASSERT(ran == accepted,
            "every accepted job ran exactly once by the time destroy returns",
            "A job accepted and never executed means a claimed cell was never committed, or the drain missed it.");
        PROVEN_TEST_INFO("accepted and ran {} of 64 submissions into a capacity-two queue with one worker", PROVEN_ARG(accepted));
    }

    PROVEN_TEST_PASS("the queue's position comparison is modular, unambiguous at every boundary, and used by both sides.");
    return 0;
}
