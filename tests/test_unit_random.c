#include "proven.h"
#include "proven_test.h"
#include <string.h>
#include <stdatomic.h>

/*
 * Written from the contract in include/proven/random.h before the OS call was wired up
 * (TESTING.md section 5.1). You cannot write a known-answer test for randomness - the whole
 * point is that the answer is not known - so the contract is stated as the properties that
 * distinguish a real CSPRNG from the ways it is usually broken:
 *
 *   - it must actually SUCCEED on a hosted platform (a stub that returns false, or a
 *     forgotten link, fails here);
 *   - it must fill EVERY byte the caller asked for (a common bug leaves the tail zero);
 *   - two calls must differ (a fixed or unseeded generator repeats);
 *   - it must not be trivially structured (all-zero, all-equal, or a counter).
 *
 * None of these prove cryptographic strength - nothing a unit test does could - but each
 * one catches a real, shipped failure mode, and together they are the difference between
 * "the OS RNG" and "a buffer someone forgot to fill".
 */

/* S-004: two sources whose context says which source it belongs to. A draw that pairs one
 * source's function with the other's context is the torn read the old two globals allowed. */
static int ctx_a = 'a', ctx_b = 'b';
static atomic_int torn;
static atomic_int draws_left;
static atomic_int workers_done;
static bool source_a(void *ctx, void *buf, proven_size_t len) {
    if (ctx != &ctx_a) atomic_fetch_add(&torn, 1);
    memset(buf, 0xA5, len);
    return true;
}
static bool source_b(void *ctx, void *buf, proven_size_t len) {
    if (ctx != &ctx_b) atomic_fetch_add(&torn, 1);
    memset(buf, 0x5A, len);
    return true;
}
static bool source_fails(void *ctx, void *buf, proven_size_t len) {
    (void)ctx; (void)buf; (void)len;
    return false;
}
static void drawer(void *arg) {
    (void)arg;
    proven_byte_t b[8];
    while (atomic_fetch_sub(&draws_left, 1) > 0) (void)proven_random_bytes(b, sizeof b);
    atomic_fetch_add(&workers_done, 1);
}

int main(void) {
    PROVEN_TEST_SUITE("OS randomness",
        "Strong bytes from the OS CSPRNG: it succeeds, fills every byte, does not repeat, and is not trivially structured.",
        "Inspect platform/proven_sys_random.c. A failure here is a missing or wrong OS entropy call.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("it succeeds and fills every byte",
        "A hosted platform has a CSPRNG; the call must use it and report success.",
        "The tail of the buffer is where a length bug leaves zeros.");
    // ---------------------------------------------------------------
    {
        proven_byte_t buf[64];
        memset(buf, 0, sizeof buf);
        PROVEN_TEST_ASSERT(proven_random_bytes(buf, sizeof buf),
            "proven_random_bytes must succeed on a hosted platform",
            "If this returns false, the OS entropy source is not wired up.");

        /* Not proof of randomness, but a buffer left all-zero is the single most common
         * way this breaks - a stub, a wrong length, an ignored error. */
        bool all_zero = true;
        for (proven_size_t i = 0; i < sizeof buf; ++i) {
            if (buf[i] != 0) { all_zero = false; break; }
        }
        PROVEN_TEST_ASSERT(!all_zero, "64 random bytes must not all be zero", "");

        /* And the last bytes specifically must have been written. */
        bool tail_written = false;
        for (proven_size_t i = 48; i < 64; ++i) {
            if (buf[i] != 0) { tail_written = true; break; }
        }
        PROVEN_TEST_ASSERT(tail_written, "the tail of the buffer must be filled, not left zero", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("two calls differ, and the bytes are not trivially structured",
        "A fixed or unseeded generator repeats; a counter or a memset produces obvious structure.",
        "");
    // ---------------------------------------------------------------
    {
        proven_byte_t a[32], b[32];
        PROVEN_TEST_ASSERT(proven_random_bytes(a, sizeof a) && proven_random_bytes(b, sizeof b),
            "two draws must both succeed", "");
        PROVEN_TEST_ASSERT(memcmp(a, b, sizeof a) != 0,
            "two independent draws must not be identical",
            "If they are, the generator is fixed or unseeded - which is not random at all.");

        /* Not all 32 bytes equal (a memset), and not a simple ascending counter. */
        bool all_same = true, is_counter = true;
        for (proven_size_t i = 1; i < sizeof a; ++i) {
            if (a[i] != a[0]) all_same = false;
            if (a[i] != (proven_byte_t)(a[i - 1] + 1)) is_counter = false;
        }
        PROVEN_TEST_ASSERT(!all_same, "the bytes must not all be identical", "");
        PROVEN_TEST_ASSERT(!is_counter, "the bytes must not be a simple counter", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("len 0 is a successful no-op, and the u64 helper works",
        "Asking for nothing succeeds and touches nothing; the convenience helper draws a word.",
        "");
    // ---------------------------------------------------------------
    {
        PROVEN_TEST_ASSERT(proven_random_bytes(NULL, 0),
            "asking for zero bytes must succeed and do nothing", "");

        /* Two u64 draws differ (a repeated value would be astronomically unlikely from a
         * real RNG, and certain from a broken one). */
        proven_u64 x = proven_random_u64();
        proven_u64 y = proven_random_u64();
        PROVEN_TEST_ASSERT(x != y, "two random u64 draws must differ", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the checked u64 says when there is no entropy",
        "RFC-0009 S-002: proven_random_u64 returns 0 when the source fails - a predictable token. proven_random_u64_checked returns false instead.",
        "Inspect proven_random_u64_checked in src/proven/random.c.");
    // ---------------------------------------------------------------
    {
        proven_u64 v = 1;
        PROVEN_TEST_ASSERT(proven_random_u64_checked(&v), "with the platform source it succeeds", "");
        proven_u64 w = 1;
        PROVEN_TEST_ASSERT(proven_random_u64_checked(&w) && (v != w || v != 0), "and draws", "");
        PROVEN_TEST_ASSERT(!proven_random_u64_checked(NULL), "a NULL out is refused", "");
        proven_random_set_source(source_fails, NULL);
        v = 1;
        PROVEN_TEST_ASSERT(!proven_random_u64_checked(&v) && v == 0, "a failing source is false, and out is 0", "");
        PROVEN_TEST_ASSERT(proven_random_u64() == 0, "while the unchecked form silently returns 0", "");
        proven_random_set_source(NULL, NULL);
        PROVEN_TEST_ASSERT(proven_random_u64_checked(&v), "the platform default is back", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("installing a source while others draw is not a torn read",
        "RFC-0009 S-004: the hook was two plain globals, so a late install raced every draw and could pair one source's function with the other's context. Every draw now sees a matching pair; under TSan, no race.",
        "Inspect proven_random_set_source and entropy_source in src/proven/random.c: the pair is published under a sequence count.");
    // ---------------------------------------------------------------
    {
        proven_job_sys_t *sys = NULL;
        PROVEN_TEST_ASSERT(proven_is_ok(proven_job_system_init(proven_heap_allocator(), 3, 8, &sys)),
            "a three-worker job system starts", "");
        atomic_store(&draws_left, 300000);
        for (int i = 0; i < 3; ++i) {
            PROVEN_TEST_ASSERT(proven_job_submit(sys, drawer, NULL), "a drawer is submitted", "");
        }
        int installs = 0;
        while (atomic_load(&workers_done) < 3) {
            proven_random_set_source((installs & 1) ? source_b : source_a, (installs & 1) ? &ctx_b : &ctx_a);
            ++installs;
        }
        proven_job_system_destroy(sys);
        proven_random_set_source(NULL, NULL);
        PROVEN_TEST_INFO("{} installs raced 300000 draws", PROVEN_ARG(installs));
        PROVEN_TEST_ASSERT(atomic_load(&torn) == 0, "no draw saw one source's function with the other's context", "");
        proven_byte_t b[4];
        PROVEN_TEST_ASSERT(proven_random_bytes(b, sizeof b), "the platform default is back after installing NULL", "");
    }

    PROVEN_TEST_PASS("the OS randomness source behaves like one.");
    return 0;
}
