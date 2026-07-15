#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * Written from the contract in include/proven/random.h before the OS call was wired up
 * (docs/TESTING.md §5.1). You cannot write a known-answer test for randomness - the whole
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

    PROVEN_TEST_PASS("the OS randomness source behaves like one.");
    return 0;
}
