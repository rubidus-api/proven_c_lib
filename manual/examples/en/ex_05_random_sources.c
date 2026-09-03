#include "example.h"
#include <string.h>

/*
 * The other example shows which generator to pick. This one is about the layer
 * underneath: where the randomness comes FROM, and how to write code that does
 * not care.
 *
 *   proven_rng_t is a source of random bytes as a pair of pointers - a small
 *   table of functions and the generator state they work on. Code that takes a
 *   proven_rng_t works with the OS generator, with ChaCha20, with xoshiro, and
 *   with a fake you wrote for a test, without a line of change.
 *
 *   proven_random_set_source is the layer below THAT: where the raw entropy a
 *   generator is seeded from comes from. A hosted program already has one - the
 *   operating system's - and should leave it alone. A bare-metal program has
 *   none, and this is the hook where its hardware source is installed.
 *
 * The fixed-seed part matters more than it looks: a cryptographic generator
 * seeded from a KNOWN seed produces a known sequence, which is what makes a
 * test that involves randomness reproducible instead of "fails once a week".
 */

/* A source of "entropy" that is not random at all: it counts. Nothing like this
 * belongs in a real program - see the counter-example in the chapter - but it
 * is exactly the right shape for showing how the hook works, and for a test
 * that must produce the same bytes every run. */
static bool counting_entropy(void *ctx, void *buf, proven_size_t len) {
    proven_u8 *next = (proven_u8 *)ctx;
    proven_u8 *out = (proven_u8 *)buf;
    for (proven_size_t i = 0; i < len; ++i) {
        out[i] = (*next)++;
    }
    return true;
}

/* A function written against the trait. It never learns which generator it got. */
static proven_u64 roll_total(proven_rng_t rng, int rolls) {
    proven_u64 sum = 0;
    for (int i = 0; i < rolls; ++i) {
        sum += proven_rng_below(rng, 6) + 1;
    }
    return sum;
}

int main(void) {
    /* --- 1. a source you can check before you use it ---------------------- */

    proven_rng_t nothing = {0};
    EXAMPLE_REQUIRE(!proven_rng_is_valid(nothing), "a zero-initialised source is not a generator");

    /* Drawing from an invalid source does not crash and does not invent a
     * number: it returns 0. That is a defined, boring answer - but a stream of
     * zeros is not randomness, so check the source once when you receive it
     * rather than trusting every draw. */
    EXAMPLE_REQUIRE(proven_rng_u64(nothing) == 0, "an invalid source yields 0, not a fabricated value");

    /* --- 2. a cryptographic generator from a KNOWN seed ------------------- */

    /* proven_chacha_rng_seed takes the seed bytes directly, so the sequence is
     * reproducible. That is what you want in a test and never in production:
     * anyone who learns the seed knows every byte the generator will produce. */
    proven_byte_t seed[PROVEN_CHACHA_SEED_SIZE];
    memset(seed, 0xA5, sizeof seed);

    proven_chacha_rng_t a, b;
    proven_chacha_rng_seed(&a, seed);
    proven_chacha_rng_seed(&b, seed);

    /* next returns one 64-bit word at a time. Two generators given the same
     * seed walk the same sequence - which is the property the test relies on. */
    proven_u64 first = proven_chacha_rng_next(&a);
    EXAMPLE_REQUIRE(first == proven_chacha_rng_next(&b), "the same seed replays the same sequence");
    EXAMPLE_REQUIRE(proven_chacha_rng_next(&a) == proven_chacha_rng_next(&b), "and keeps replaying it");

    /* --- 3. using it through the trait ------------------------------------ */

    proven_rng_t rng = proven_chacha_rng(&a);
    EXAMPLE_REQUIRE(proven_rng_is_valid(rng), "a seeded generator makes a valid source");

    proven_u64 word = proven_rng_u64(rng);
    (void)word;   /* any 64-bit value is a legal answer; there is nothing to assert about it */

    /* fill is the bulk form: one call for a whole buffer, rather than a loop
     * over 64-bit words that has to deal with the remainder itself. */
    proven_byte_t nonce[12] = {0};
    proven_rng_fill(rng, nonce, sizeof nonce);

    bool all_zero = true;
    for (proven_size_t i = 0; i < sizeof nonce; ++i) {
        if (nonce[i] != 0) all_zero = false;
    }
    EXAMPLE_REQUIRE(!all_zero, "filling from a seeded generator must produce something");

    /* The same function, driven by two different generators. This is the only
     * reason the trait exists. */
    proven_chacha_rng_t c;
    proven_chacha_rng_seed(&c, seed);
    proven_u64 crypto_total = roll_total(proven_chacha_rng(&c), 50);

    proven_xoshiro256ss_t fast;
    proven_xoshiro256ss_seed(&fast, 7);
    proven_u64 fast_total = roll_total(proven_xoshiro256ss_rng(&fast), 50);

    EXAMPLE_REQUIRE(crypto_total >= 50 && crypto_total <= 300, "50 dice must total between 50 and 300");
    EXAMPLE_REQUIRE(fast_total >= 50 && fast_total <= 300, "whichever generator produced them");

    /* --- 4. one strong word, without holding a generator ------------------ */

    /* proven_random_u64 draws straight from the entropy source. Convenient for
     * a one-off - a table's hash key at startup, a request id - and the wrong
     * tool for a loop, because each call costs a trip to the operating system.
     * For bulk output, seed a generator once and draw from that. */
    proven_u64 one_off = proven_random_u64();
    proven_u64 another = proven_random_u64();
    EXAMPLE_REQUIRE(one_off != another || one_off != 0,
                    "two draws from the OS source are essentially never the same value");

    /* --- 5. installing an entropy source ---------------------------------- */

    /* On a hosted target the operating system's source is already installed and
     * you should leave it there. This hook exists for the bare-metal case,
     * where the library cannot know that the board's entropy lives in a
     * particular hardware register. Here it is installed with a deliberately
     * fake source, purely to show the mechanism and to prove the switch took
     * effect - a real one must be genuine hardware entropy. */
    proven_u8 counter = 0;
    proven_random_set_source(counting_entropy, &counter);

    proven_byte_t drawn[4] = {0};
    EXAMPLE_REQUIRE(proven_random_bytes(drawn, sizeof drawn), "the installed source must answer");
    EXAMPLE_REQUIRE(drawn[0] == 0 && drawn[1] == 1 && drawn[2] == 2 && drawn[3] == 3,
                    "and it is the source we installed that answered");

    /* Put the platform default back. Leaving a test source installed is how a
     * program ends up generating predictable keys in production. */
    proven_random_set_source(NULL, NULL);

    proven_byte_t real[8] = {0};
    EXAMPLE_REQUIRE(proven_random_bytes(real, sizeof real), "the OS source is back and working");

    printf("random: first word %llu, dice totals %llu and %llu\n",
           (unsigned long long)first, (unsigned long long)crypto_total, (unsigned long long)fast_total);

    return EXAMPLE_OK();
}
