#include "example.h"

/*
 * Randomness, by use case. There is no single "random": there are two jobs that look
 * identical and are not, and picking the wrong one is the whole danger.
 *
 *   A key, a token, a nonce - anything an attacker must not guess - needs a CRYPTOGRAPHIC
 *   source. A simulation, a test, a game needs a REPRODUCIBLE one, because a failing run you
 *   cannot replay is a failing run you cannot debug. The two requirements are in direct
 *   opposition: reproducible means predictable, and predictable is exactly what a token must
 *   not be. So the library gives them different names, and the choice is visible here at the
 *   call site rather than buried in how something was seeded.
 */

int main(void) {
    /* ---- Job 1: a secret. The OS CSPRNG - and the one place randomness can fail. ---- */
    proven_byte_t key[32];
    EXAMPLE_REQUIRE(proven_random_bytes(key, sizeof key),
                    "the OS must give us strong bytes on a hosted platform");

    /* ---- Job 2: lots of cryptographic bytes, or any at all on a board with no OS.
     * ChaCha20 is pure arithmetic: seed it once from real entropy and it needs nothing from
     * the operating system afterwards - no syscall per draw, and it works on bare metal.
     * Seeding is the ONLY step that can fail, so it is the only one you have to check. ---- */
    proven_chacha_rng_t crypto;
    EXAMPLE_REQUIRE(proven_chacha_rng_seed_from_os(&crypto), "seed the CSPRNG from the OS, once");

    proven_byte_t token[16];
    proven_chacha_rng_fill(&crypto, token, sizeof token);   /* cannot fail: it is seeded */

    /* ---- Job 3: a REPRODUCIBLE run. xoshiro256** is fast and replays exactly from its seed,
     * which is what makes a failing simulation debuggable. It is NOT secret-grade: a few of
     * its outputs reveal its whole state. Never hand it a token to generate. ---- */
    proven_xoshiro256ss_t sim;
    proven_xoshiro256ss_seed(&sim, 12345);

    proven_xoshiro256ss_t replay;
    proven_xoshiro256ss_seed(&replay, 12345);
    EXAMPLE_REQUIRE(proven_xoshiro256ss_next(&sim) == proven_xoshiro256ss_next(&replay),
                    "the same seed replays the same run - that is the whole point");

    /* ---- The helpers work over ANY source, through the proven_rng_t trait. ---- */
    proven_rng_t rng = proven_xoshiro256ss_rng(&sim);

    /* A number in a range. `rng_u64() % 6` is what everyone writes, and it is BIASED unless
     * the bound divides 2^64 - the low values come up more often. This one is not. */
    for (int i = 0; i < 100; ++i) {
        proven_u64 die = proven_rng_below(rng, 6) + 1;
        EXAMPLE_REQUIRE(die >= 1 && die <= 6, "a die roll is 1..6, uniformly");
    }

    proven_i64 temperature = proven_rng_range(rng, -40, 85);
    EXAMPLE_REQUIRE(temperature >= -40 && temperature <= 85, "an inclusive range, both ends");

    double p = proven_rng_f64(rng);
    EXAMPLE_REQUIRE(p >= 0.0 && p < 1.0, "a double in [0, 1) - never 1.0");

    /* An unbiased shuffle: Fisher-Yates over the unbiased index above. The `% n` version of
     * this loop measurably favours some orderings. */
    int deck[10];
    for (int i = 0; i < 10; ++i) deck[i] = i;
    proven_rng_shuffle(rng, deck, 10, sizeof deck[0]);

    int sum = 0;
    for (int i = 0; i < 10; ++i) sum += deck[i];
    EXAMPLE_REQUIRE(sum == 45, "a shuffle is a permutation: every card is still there, once");

    /* The cryptographic generator satisfies the same trait, so the same helpers work over it
     * when the choice must be unguessable rather than merely uniform. */
    proven_rng_t secure = proven_chacha_rng(&crypto);
    proven_u64 unguessable_index = proven_rng_below(secure, 1000);
    EXAMPLE_REQUIRE(unguessable_index < 1000, "the helpers do not care which source they draw from");

    (void)token;
    (void)key;
    return EXAMPLE_OK();
}
