#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * The maths was right and the failure paths were not. An adversarial audit of the new
 * randomness module found three defects, and all three had the same shape: a generator that
 * was never usable handed back bytes that LOOKED usable.
 *
 *   1. proven_chacha_rng_next(NULL) declared an 8-byte scratch, called _fill (which returns
 *      immediately for a NULL generator, touching nothing), and then read the scratch anyway.
 *      It returned whatever was on the stack - plausible-looking "random" data, and possibly a
 *      stale secret. Every other entry point in the module guarded its NULL; this one did not.
 *
 *   2. When proven_chacha_rng_seed_from_entropy failed, it zeroed the state and the comment claimed
 *      a caller who ignored the `false` "gets zeros, not plausible garbage". That was true for
 *      exactly 64 bytes. ChaCha over an all-zero state produces an all-zero first block - but
 *      the block counter then advances, and block 1 of an all-zero-key ChaCha is a perfectly
 *      normal-looking, FIXED, publicly derivable keystream. A caller who ignored the failure
 *      and asked for a 32-byte key after any prior draw got bytes an attacker can compute.
 *
 *   3. A stack-declared proven_chacha_rng_t that was never seeded has used == 0, which the
 *      fill path read as "the block already holds 64 fresh keystream bytes" - so it copied its
 *      own uninitialised block[] out to the caller. A silent stack disclosure.
 *
 * 2 and 3 are one bug: `used` was the only state, and the zero-initialised struct - the shape
 * of "never seeded" - was indistinguishable from "freshly generated block ready to hand out".
 * The fix gives the generator an explicit seeded marker, so an unseeded or failed generator is
 * INERT: it yields an invalid proven_rng_t, its next() is 0, and its fill() writes zeros
 * rather than whatever the stack was holding.
 */

int main(void) {
    PROVEN_TEST_SUITE("an unseeded or failed generator is inert, not plausible",
        "A ChaCha generator that was never seeded - or whose seeding failed - must never hand back bytes that look random. It must be visibly unusable.",
        "Inspect the seeded marker in proven_chacha_rng_t and the guards in src/proven/random.c. `used` alone cannot encode this: a zero-initialised struct is the shape of 'never seeded'.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a NULL generator returns 0, not the stack",
        "proven_chacha_rng_next was the one entry point in the module without a NULL guard: it read an uninitialised local and returned it.",
        "");
    // ---------------------------------------------------------------
    {
        /* Prime this frame with a recognisable pattern, so a read of an uninitialised local
         * has something distinctive to pick up. */
        volatile proven_byte_t poison[256];
        for (int i = 0; i < 256; ++i) poison[i] = (proven_byte_t)(0xC0 + (i & 0x0F));
        (void)poison;

        PROVEN_TEST_ASSERT(proven_chacha_rng_next(NULL) == 0,
            "proven_chacha_rng_next(NULL) must be 0, like every other generator entry point",
            "A non-zero value here is whatever was on the stack, returned to the caller as randomness.");

        /* The sibling that always did guard, as the control. */
        PROVEN_TEST_ASSERT(proven_xoshiro256ss_next(NULL) == 0, "and xoshiro's NULL guard still holds", "");

        proven_byte_t buf[16];
        memset(buf, 0xAB, sizeof buf);
        proven_chacha_rng_fill(NULL, buf, sizeof buf);   /* must not crash */
        PROVEN_TEST_ASSERT(buf[0] == 0xAB, "a NULL generator fills nothing", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a generator that was never seeded hands back nothing",
        "A stack-declared proven_chacha_rng_t has used == 0, which the fill path used to read as 'a full block of fresh keystream is ready' - and copied its own uninitialised block out.",
        "This is the stack-disclosure case. Every byte must come back zero, and the trait must refuse.");
    // ---------------------------------------------------------------
    {
        /* Deliberately NOT zero-initialised: this is the shape of the bug. */
        proven_chacha_rng_t g;
        memset(&g, 0xEE, sizeof g);   /* stack garbage, as a fresh frame would hold */
        g.used = 0;                   /* the value a zeroed struct would have */

        proven_byte_t out[64];
        memset(out, 0, sizeof out);
        proven_chacha_rng_fill(&g, out, sizeof out);

        bool any_nonzero = false;
        for (proven_size_t i = 0; i < sizeof out; ++i) if (out[i] != 0) any_nonzero = true;
        PROVEN_TEST_ASSERT(!any_nonzero,
            "an unseeded generator must yield zeros, never its own uninitialised block",
            "Non-zero bytes here are the contents of this program's stack, handed out as random data.");

        PROVEN_TEST_ASSERT(proven_chacha_rng_next(&g) == 0,
            "and its next() is 0", "");

        proven_rng_t rng = proven_chacha_rng(&g);
        PROVEN_TEST_ASSERT(!proven_rng_is_valid(rng),
            "an unseeded generator must not present itself as a valid source",
            "If the trait is valid, every helper - below, shuffle, f64 - will happily draw from a generator with no key.");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a seeded generator IS valid, and its stream is not zeros",
        "The guard must not break the working case. This is the control for the two above.",
        "");
    // ---------------------------------------------------------------
    {
        proven_byte_t seed[PROVEN_CHACHA_SEED_SIZE];
        for (int i = 0; i < PROVEN_CHACHA_SEED_SIZE; ++i) seed[i] = (proven_byte_t)i;

        proven_chacha_rng_t g;
        memset(&g, 0xEE, sizeof g);          /* garbage before seeding... */
        proven_chacha_rng_seed(&g, seed);    /* ...and seeding must make it usable */

        proven_rng_t rng = proven_chacha_rng(&g);
        PROVEN_TEST_ASSERT(proven_rng_is_valid(rng), "a seeded generator is a valid source", "");

        proven_byte_t out[64];
        proven_chacha_rng_fill(&g, out, sizeof out);
        bool any_nonzero = false;
        for (proven_size_t i = 0; i < sizeof out; ++i) if (out[i] != 0) any_nonzero = true;
        PROVEN_TEST_ASSERT(any_nonzero, "a seeded generator produces a real keystream", "");

        /* And it is still the standard's keystream - the guard changed nothing about the maths. */
        static const proven_byte_t expect8[8] = { 0x39, 0xfd, 0x2b, 0x7d, 0xd9, 0xc5, 0x19, 0x6a };
        proven_chacha_rng_t h;
        proven_chacha_rng_seed(&h, seed);
        proven_byte_t first[8];
        proven_chacha_rng_fill(&h, first, sizeof first);
        PROVEN_TEST_ASSERT(memcmp(first, expect8, sizeof expect8) == 0,
            "and the keystream is unchanged: still ChaCha20, byte for byte", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a generator whose seeding FAILED stays unusable forever, not just for one block",
        "The failure path zeroed the state, and ChaCha over an all-zero state gives an all-zero FIRST block - so 'you get zeros' was true for 64 bytes and false after that: the counter advances, and block 1 is a normal-looking, fixed, publicly derivable keystream.",
        "There is no way to make the OS entropy call fail from here, so this simulates the state it leaves behind - a zeroed generator - and requires that it stay inert past the first block.");
    // ---------------------------------------------------------------
    {
        /* Exactly what a failed seed_from_os leaves behind: an all-zero generator. */
        proven_chacha_rng_t g;
        memset(&g, 0, sizeof g);

        /* Draw well past the first 64-byte block - this is where the old code started
         * producing attacker-derivable bytes that pass every smell test. */
        proven_byte_t out[256];
        memset(out, 0, sizeof out);
        proven_chacha_rng_fill(&g, out, sizeof out);

        proven_size_t nonzero = 0;
        for (proven_size_t i = 0; i < sizeof out; ++i) if (out[i] != 0) nonzero++;
        PROVEN_TEST_ASSERT(nonzero == 0,
            "a failed generator must yield zeros for EVERY byte, not only the first block",
            "Bytes past offset 64 used to be a fixed keystream anyone can compute - which is worse than obvious garbage, because nothing reports it.");

        proven_rng_t rng = proven_chacha_rng(&g);
        PROVEN_TEST_ASSERT(!proven_rng_is_valid(rng),
            "and it must not present itself as a valid source to the helpers", "");
    }

    PROVEN_TEST_PASS("an unseeded or failed generator is inert: no stack, no fixed keystream, no plausible bytes.");
    return 0;
}
