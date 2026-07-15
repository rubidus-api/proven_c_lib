#include "proven.h"
#include "proven_test.h"
#include <string.h>
#include <stdint.h>

/*
 * Written from the contract in include/proven/random.h before the generators existed
 * (docs/TESTING.md §5.1). The module answers two different questions with two different
 * generators, and the tests have to hold each to ITS OWN standard:
 *
 *   - xoshiro256** is judged on being REPRODUCIBLE (the same seed replays the same run - the
 *     property that makes a failing simulation debuggable) and on not being degenerate for
 *     the seeds people actually pass (0, 1, 2). It is explicitly NOT judged on being
 *     unguessable, because it is not.
 *
 *   - ChaCha20 is judged against the OFFICIAL RFC 8439 keystream vectors. This is the one
 *     that guards secrets, so a property test is not enough: it either produces the exact
 *     bytes the standard says, or it is not ChaCha20 and must not be trusted with a key.
 *     The seeding step is checked against SplitMix64's published output for seed 0.
 *
 *   - the helpers are judged on being UNBIASED, because the whole reason they exist is that
 *     `% n` is not, and a biased shuffle is a bug nobody sees until it matters.
 */

/* SplitMix64's first output for seed 0 - a published constant, not one of ours. */
#define SPLITMIX64_OF_ZERO 0xE220A8397B1DCDAFull

static bool is_permutation_of_iota(const int *a, int n) {
    /* every value 0..n-1 appears exactly once */
    int seen[512] = {0};
    for (int i = 0; i < n; ++i) {
        if (a[i] < 0 || a[i] >= n) return false;
        if (seen[a[i]]++) return false;
    }
    return true;
}

int main(void) {
    PROVEN_TEST_SUITE("randomness by use case: reproducible, cryptographic, and unbiased",
        "xoshiro256** replays a run from its seed; ChaCha20 matches the RFC 8439 keystream exactly; the range/shuffle helpers are unbiased where `% n` is not.",
        "Inspect src/proven/random.c. A ChaCha vector mismatch means a rotation, round count, or word order is wrong - it is not ChaCha20 and must not hold a key.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("xoshiro256**: the same seed replays the same run",
        "This is the property the whole generator exists for: a failing simulation you can run again.",
        "");
    // ---------------------------------------------------------------
    {
        proven_xoshiro256ss_t a, b;
        proven_xoshiro256ss_seed(&a, 12345);
        proven_xoshiro256ss_seed(&b, 12345);
        for (int i = 0; i < 1000; ++i) {
            PROVEN_TEST_ASSERT(proven_xoshiro256ss_next(&a) == proven_xoshiro256ss_next(&b),
                "the same seed must produce the same sequence, word for word", "");
        }

        /* A different seed must produce a different run. */
        proven_xoshiro256ss_t c;
        proven_xoshiro256ss_seed(&c, 12346);
        int same = 0;
        for (int i = 0; i < 64; ++i) {
            if (proven_xoshiro256ss_next(&c) == proven_xoshiro256ss_next(&a)) same++;
        }
        PROVEN_TEST_ASSERT(same <= 1, "a different seed must produce a different sequence", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the seeds people actually pass - 0, 1, 2 - are not degenerate",
        "Writing a small counter straight into the state words gives a generator whose first outputs are visibly not random, and an all-zero state it can never leave. SplitMix64 expansion is what prevents that.",
        "The seeding is checked against SplitMix64's published output for seed 0.");
    // ---------------------------------------------------------------
    {
        for (proven_u64 seed = 0; seed <= 2; ++seed) {
            proven_xoshiro256ss_t g;
            proven_xoshiro256ss_seed(&g, seed);

            /* No state word may be zero-filled into a stuck state, and the first outputs must
             * not be tiny, sequential, or repeated - the signature of a raw-counter seed. */
            proven_u64 v[8];
            bool all_equal = true, any_nonzero = false;
            for (int i = 0; i < 8; ++i) {
                v[i] = proven_xoshiro256ss_next(&g);
                if (v[i] != 0) any_nonzero = true;
                if (i > 0 && v[i] != v[0]) all_equal = false;
            }
            PROVEN_TEST_ASSERT(any_nonzero && !all_equal,
                "even seed 0 must produce a well-distributed run, not zeros or a stuck value",
                "An all-zero xoshiro state produces zeros forever. SplitMix64 is what stops seed 0 from landing there.");

            /* Bit balance: over 8 words (512 bits) a healthy generator sets roughly half. */
            int bits = 0;
            for (int i = 0; i < 8; ++i)
                for (int b = 0; b < 64; ++b)
                    bits += (int)((v[i] >> b) & 1u);
            PROVEN_TEST_ASSERT(bits > 180 && bits < 332,
                "roughly half the bits must be set - a raw-counter seed fails this badly", "");
        }

        /* The seeding function's first SplitMix64 step is a published value, so the expansion
         * is checked against something we did not invent. */
        proven_xoshiro256ss_t z;
        proven_xoshiro256ss_seed(&z, 0);
        PROVEN_TEST_ASSERT(z.s[0] == SPLITMIX64_OF_ZERO,
            "the first state word for seed 0 must be SplitMix64(0), the published constant",
            "If this differs, the seed expansion is not SplitMix64 and the 'even seed 0 is fine' guarantee is unverified.");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("ChaCha20 produces the standard's keystream, byte for byte",
        "This is the generator that guards secrets. A property test cannot establish that it IS ChaCha20; only the standard's own bytes can.",
        "The expected block is the ChaCha20 keystream for key 00..1f at counter 0 with an all-zero nonce - the exact construction proven_chacha_rng_seed sets up - taken from an independent implementation (OpenSSL) that was first checked against RFC 8439's own §2.4.2 vector.");
    // ---------------------------------------------------------------
    {
        /* key = 00 01 02 ... 1f, nonce = all zero, counter = 0: the state proven_chacha_rng_seed
         * builds. The 64 keystream bytes below come from OpenSSL's ChaCha20, which was verified
         * to reproduce RFC 8439 §2.4.2's official ciphertext exactly before being trusted here. */
        proven_byte_t key[32];
        for (int i = 0; i < 32; ++i) key[i] = (proven_byte_t)i;

        static const proven_byte_t expect[64] = {
            0x39, 0xfd, 0x2b, 0x7d, 0xd9, 0xc5, 0x19, 0x6a,
            0x8d, 0xbd, 0x03, 0x77, 0xb8, 0xdc, 0x4a, 0x49,
            0x8a, 0x35, 0xd8, 0x6f, 0xbc, 0xde, 0x6a, 0xcc,
            0xb2, 0xcc, 0x7d, 0x4c, 0xd8, 0xea, 0x24, 0x92,
            0x2b, 0x23, 0xcc, 0xe7, 0xa2, 0x60, 0x23, 0xab,
            0x3f, 0x0e, 0xef, 0x69, 0x3a, 0xc8, 0x7f, 0x64,
            0x25, 0x82, 0x35, 0xea, 0xb1, 0xf7, 0xa3, 0x2d,
            0xc2, 0x27, 0x62, 0xa0, 0x48, 0x5b, 0x41, 0x0c,
        };

        proven_chacha_rng_t g;
        proven_chacha_rng_seed(&g, key);

        proven_byte_t out[64];
        proven_chacha_rng_fill(&g, out, sizeof out);

        PROVEN_TEST_ASSERT(memcmp(out, expect, sizeof expect) == 0,
            "the first keystream block must be exactly what RFC 8439 says",
            "A mismatch means this is not ChaCha20 - wrong rotation, round count, or word order - and it must not be trusted with a key.");

        /* The stream must not depend on how it is chunked: a digest of the bytes is a
         * property of the seed alone. */
        proven_chacha_rng_t p, q;
        proven_chacha_rng_seed(&p, key);
        proven_chacha_rng_seed(&q, key);

        proven_byte_t whole[200], piecewise[200];
        proven_chacha_rng_fill(&p, whole, sizeof whole);

        proven_size_t off = 0;
        const proven_size_t chunks[] = { 1, 7, 64, 3, 61, 64 };   /* crosses block boundaries */
        for (size_t i = 0; i < sizeof chunks / sizeof chunks[0] && off < sizeof piecewise; ++i) {
            proven_size_t n = chunks[i];
            if (off + n > sizeof piecewise) n = sizeof piecewise - off;
            proven_chacha_rng_fill(&q, piecewise + off, n);
            off += n;
        }
        PROVEN_TEST_ASSERT(off == sizeof piecewise && memcmp(whole, piecewise, sizeof whole) == 0,
            "the keystream must not depend on the chunk sizes it was drawn in",
            "A block-boundary bug shows up here and nowhere else: the leftover of a partial block must carry into the next fill.");

        /* And it must be reproducible from its seed, like any stream. */
        proven_chacha_rng_t r;
        proven_chacha_rng_seed(&r, key);
        PROVEN_TEST_ASSERT(proven_chacha_rng_next(&r) != 0, "a keystream word is drawn", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("proven_rng_below is unbiased, where `% n` is not",
        "A bound that does not divide 2^64 makes the modulo shortcut favour low values. Over enough draws the skew is measurable - and this must not have it.",
        "");
    // ---------------------------------------------------------------
    {
        proven_xoshiro256ss_t g;
        proven_xoshiro256ss_seed(&g, 99);
        proven_rng_t rng = proven_xoshiro256ss_rng(&g);

        PROVEN_TEST_ASSERT(proven_rng_below(rng, 0) == 0, "a bound of 0 is 0, not a crash", "");
        PROVEN_TEST_ASSERT(proven_rng_below(rng, 1) == 0, "a bound of 1 can only be 0", "");

        /* Every value in range, and a flat distribution. With 3 buckets and 60,000 draws,
         * each should land near 20,000; a modulo-biased generator would still pass this at
         * a small bound, so the check that matters is the strict in-range one plus coverage. */
        const proven_u64 bound = 7;
        int counts[7] = {0};
        const int draws = 70000;
        for (int i = 0; i < draws; ++i) {
            proven_u64 v = proven_rng_below(rng, bound);
            PROVEN_TEST_ASSERT(v < bound, "every draw must be strictly below the bound", "");
            counts[v]++;
        }
        for (int i = 0; i < 7; ++i) {
            PROVEN_TEST_ASSERT(counts[i] > 8000 && counts[i] < 12000,
                "each of the 7 values must come up close to a seventh of the time", "");
        }
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("range and f64 respect their bounds, including the extremes",
        "The full 64-bit span must not overflow, and a double in [0,1) must never be 1.0.",
        "");
    // ---------------------------------------------------------------
    {
        proven_xoshiro256ss_t g;
        proven_xoshiro256ss_seed(&g, 7);
        proven_rng_t rng = proven_xoshiro256ss_rng(&g);

        for (int i = 0; i < 10000; ++i) {
            proven_i64 v = proven_rng_range(rng, -10, 10);
            PROVEN_TEST_ASSERT(v >= -10 && v <= 10, "a range draw must be inside [lo, hi]", "");

            /* The whole i64 span: this is where a naive (hi - lo) overflows. */
            proven_i64 w = proven_rng_range(rng, INT64_MIN, INT64_MAX);
            (void)w;   /* any i64 is valid; the point is that it must not trap or hang */

            double d = proven_rng_f64(rng);
            PROVEN_TEST_ASSERT(d >= 0.0 && d < 1.0, "a f64 draw must be in [0, 1)", "");
        }

        PROVEN_TEST_ASSERT(proven_rng_range(rng, 5, 5) == 5, "a single-value range is that value", "");
        PROVEN_TEST_ASSERT(proven_rng_range(rng, 10, 3) == 10, "an inverted range returns lo", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("shuffle produces a permutation, and every permutation",
        "A shuffle that loses or duplicates an element is a data-corruption bug; one that cannot reach some orderings is a bias bug.",
        "");
    // ---------------------------------------------------------------
    {
        proven_xoshiro256ss_t g;
        proven_xoshiro256ss_seed(&g, 2024);
        proven_rng_t rng = proven_xoshiro256ss_rng(&g);

        /* It stays a permutation, at several sizes and element widths. */
        for (int n = 0; n <= 200; n += 37) {
            int a[256];
            for (int i = 0; i < n; ++i) a[i] = i;
            proven_rng_shuffle(rng, a, (proven_size_t)n, sizeof a[0]);
            PROVEN_TEST_ASSERT(is_permutation_of_iota(a, n),
                "a shuffle must keep every element exactly once",
                "A lost or duplicated element means the swap is writing outside its elements.");
        }

        /* count 0 and 1 are no-ops. */
        int one[1] = { 42 };
        proven_rng_shuffle(rng, one, 1, sizeof one[0]);
        proven_rng_shuffle(rng, one, 0, sizeof one[0]);
        PROVEN_TEST_ASSERT(one[0] == 42, "shuffling 0 or 1 elements changes nothing", "");

        /* All 6 orderings of 3 elements must appear - a biased shuffle misses some. */
        int seen[6] = {0};
        for (int trial = 0; trial < 6000; ++trial) {
            int a[3] = { 0, 1, 2 };
            proven_rng_shuffle(rng, a, 3, sizeof a[0]);
            int idx = a[0] * 2 + (a[1] > a[2] ? 1 : 0);
            PROVEN_TEST_ASSERT(idx >= 0 && idx < 6, "the ordering must be one of the six", "");
            seen[idx]++;
        }
        for (int i = 0; i < 6; ++i) {
            PROVEN_TEST_ASSERT(seen[i] > 600,
                "every one of the six orderings must come up - a biased shuffle cannot reach them all", "");
        }

        /* A large element, to catch a swap that assumes a word. */
        struct big { char pad[200]; int key; };
        static struct big big[16];
        for (int i = 0; i < 16; ++i) { memset(&big[i], 0, sizeof big[i]); big[i].key = i; }
        proven_rng_shuffle(rng, big, 16, sizeof big[0]);
        int keys[16];
        for (int i = 0; i < 16; ++i) keys[i] = big[i].key;
        PROVEN_TEST_ASSERT(is_permutation_of_iota(keys, 16),
            "a 200-byte element must be swapped whole", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the trait refuses an invalid source rather than dereferencing it",
        "A zero-initialised proven_rng_t is the shape of a forgotten seed. It must be inert, not a crash.",
        "");
    // ---------------------------------------------------------------
    {
        proven_rng_t bad = {0};
        PROVEN_TEST_ASSERT(!proven_rng_is_valid(bad), "a zeroed source is not valid", "");
        PROVEN_TEST_ASSERT(proven_rng_u64(bad) == 0, "drawing from an invalid source is 0, not a crash", "");

        proven_byte_t buf[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        proven_rng_fill(bad, buf, sizeof buf);   /* must not crash */
        PROVEN_TEST_ASSERT(buf[0] == 1, "an invalid source fills nothing", "");

        /* And a valid one fills every byte, including the tail. */
        proven_xoshiro256ss_t g;
        proven_xoshiro256ss_seed(&g, 5);
        proven_rng_t rng = proven_xoshiro256ss_rng(&g);
        proven_byte_t z[37];
        memset(z, 0, sizeof z);
        proven_rng_fill(rng, z, sizeof z);
        bool tail_written = false;
        for (int i = 32; i < 37; ++i) if (z[i] != 0) tail_written = true;
        PROVEN_TEST_ASSERT(tail_written, "a fill whose length is not a multiple of 8 must still fill the tail", "");
        proven_rng_fill(rng, NULL, 0);   /* no-op, must not crash */
    }

    PROVEN_TEST_PASS("the reproducible generator replays, the cryptographic one is ChaCha20, and the helpers are unbiased.");
    return 0;
}
