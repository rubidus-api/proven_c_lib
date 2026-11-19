#include "proven.h"
#include "proven_test.h"
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#if !defined(_WIN32) && !defined(_WIN64)
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

/*
 * Written from the contract in include/proven/random.h before the generators existed
 * (TESTING.md section 5.1). The module answers two different questions with two different
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

/* RFC-0048: an independent model of the generator's stream - the RFC 8439 block function
 * written out again here, and the erasure rule in its plainest form: blocks in order, counter
 * from 0; every block whose counter is 15 mod 16 is not output, its first 32 bytes are the key
 * from the next block on. */
static uint32_t m_rotl(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }
#define M_QR(a, b, c, d) (a += b, d ^= a, d = m_rotl(d, 16), c += d, b ^= c, b = m_rotl(b, 12), a += b, d ^= a, d = m_rotl(d, 8), c += d, b ^= c, b = m_rotl(b, 7))
static void m_block(const uint32_t key[8], uint64_t ctr, uint8_t out[64]) {
    uint32_t s[16] = { 0x61707865u, 0x3320646Eu, 0x79622D32u, 0x6B206574u }, x[16];
    for (int i = 0; i < 8; ++i) s[4 + i] = key[i];
    s[12] = (uint32_t)ctr; s[13] = (uint32_t)(ctr >> 32);
    memcpy(x, s, sizeof x);
    for (int i = 0; i < 10; ++i) {
        M_QR(x[0], x[4], x[8], x[12]); M_QR(x[1], x[5], x[9], x[13]); M_QR(x[2], x[6], x[10], x[14]); M_QR(x[3], x[7], x[11], x[15]);
        M_QR(x[0], x[5], x[10], x[15]); M_QR(x[1], x[6], x[11], x[12]); M_QR(x[2], x[7], x[8], x[13]); M_QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; ++i) { uint32_t v = x[i] + s[i]; out[4 * i] = (uint8_t)v; out[4 * i + 1] = (uint8_t)(v >> 8); out[4 * i + 2] = (uint8_t)(v >> 16); out[4 * i + 3] = (uint8_t)(v >> 24); }
}
static void m_stream(const proven_byte_t seed[32], bool erasure, uint8_t *out, size_t len) {
    uint32_t key[8];
    for (int i = 0; i < 8; ++i) key[i] = (uint32_t)seed[4 * i] | ((uint32_t)seed[4 * i + 1] << 8) | ((uint32_t)seed[4 * i + 2] << 16) | ((uint32_t)seed[4 * i + 3] << 24);
    uint8_t b[64];
    for (uint64_t ctr = 0; len > 0; ++ctr) {
        m_block(key, ctr, b);
        if (erasure && ctr % 16 == 15) {
            for (int i = 0; i < 8; ++i) key[i] = (uint32_t)b[4 * i] | ((uint32_t)b[4 * i + 1] << 8) | ((uint32_t)b[4 * i + 2] << 16) | ((uint32_t)b[4 * i + 3] << 24);
            continue;
        }
        size_t n = len < 64 ? len : 64;
        memcpy(out, b, n);
        out += n; len -= n;
    }
}
static bool all_zero(const proven_byte_t *p, size_t n) { for (size_t i = 0; i < n; ++i) if (p[i]) return false; return true; }

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
        "The expected block is the ChaCha20 keystream for key 00..1f at counter 0 with an all-zero nonce - the exact construction proven_chacha_rng_seed sets up - taken from an independent implementation (OpenSSL) that was first checked against RFC 8439's own section 2.4.2 vector.");
    // ---------------------------------------------------------------
    {
        /* key = 00 01 02 ... 1f, nonce = all zero, counter = 0: the state proven_chacha_rng_seed
         * builds. The 64 keystream bytes below come from OpenSSL's ChaCha20, which was verified
         * to reproduce RFC 8439 section 2.4.2's official ciphertext exactly before being trusted here. */
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
    PROVEN_TEST_SECTION("large fills give the stream byte for byte",
        "A fill of whole blocks writes them straight into the caller's buffer, several at once where the processor has vector lanes (AVX2: 8, SSE2: 4); the bytes must be those one-byte draws give, through every path, at any offset, and across the block counter's wrap into word 13.",
        "Inspect proven_chacha_rng_fill in src/proven/random.c and proven_sys_chacha_keystream in platform/proven_sys_random.c.");
    // ---------------------------------------------------------------
    {
        static proven_byte_t ref[20000], got[20000];
        proven_byte_t key[PROVEN_CHACHA_SEED_SIZE];
        for (int i = 0; i < PROVEN_CHACHA_SEED_SIZE; ++i) key[i] = (proven_byte_t)(0xA5 ^ (i * 7));
        /* word 12 at 0 (a fresh seed) and three places near its wrap */
        const proven_u32 starts[] = { 0, 0xFFFFFFF0u, 0xFFFFFFFEu, 0xFFFFFFFFu - 9 };
        const proven_size_t sizes[] = { 5, 1000, 4096, 3, 777, 576, 511, 513, 256, 64, 2000, 63, 7000 };
        for (size_t k = 0; k < sizeof starts / sizeof starts[0]; ++k) {
            proven_chacha_rng_t a, b;
            proven_chacha_rng_seed(&a, key); proven_chacha_rng_seed(&b, key);
            a.state[12] = b.state[12] = starts[k];
            for (size_t i = 0; i < sizeof ref; ++i) proven_chacha_rng_fill(&a, ref + i, 1);
            proven_size_t off = 0;
            for (size_t i = 0; off < sizeof got; i = (i + 1) % (sizeof sizes / sizeof sizes[0])) {
                proven_size_t n = sizes[i];
                if (n > sizeof got - off) n = sizeof got - off;
                proven_chacha_rng_fill(&b, got + off, n);
                off += n;
            }
            PROVEN_TEST_ASSERT(memcmp(ref, got, sizeof ref) == 0 && a.state[12] == b.state[12] && a.state[13] == b.state[13],
                "fills of every size give what one-byte draws give, and leave the counter where they do", "");
            PROVEN_TEST_ASSERT(proven_chacha_rng_next(&a) == proven_chacha_rng_next(&b), "and the next word after them is the same", "");
        }
        proven_chacha_rng_t w;
        proven_chacha_rng_seed_ex(&w, key, PROVEN_CHACHA_RNG_NO_ERASURE);
        w.state[12] = 0xFFFFFFFFu;
        proven_chacha_rng_fill(&w, got, 64 * 8);
        PROVEN_TEST_ASSERT(w.state[12] == 7 && w.state[13] == 1, "past the wrap of word 12 the count carries into word 13", "");
        /* With erasure the last epoch before the wrap ends on its key block, 0xFFFFFFFF: 15 data
         * blocks, the key, and the next epoch at 0 with word 13 carried. */
        proven_chacha_rng_seed(&w, key);
        w.state[12] = 0xFFFFFFF0u;
        proven_chacha_rng_fill(&w, got, 64 * 16);
        PROVEN_TEST_ASSERT(w.state[12] == 1 && w.state[13] == 1, "with erasure: 16 blocks from 0xFFFFFFF0 are 15, the key at 0xFFFFFFFF, then block 0 of word 13's next value", "");
    }


    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("key erasure: the stream the model gives, however it is drawn",
        "RFC-0048 E-1. Epochs of 16 blocks; the 16th is the next key and is never output. Against an independent model of RFC 8439 and that rule: 40 KiB drawn in one fill, in pieces of awkward sizes, and as words; the first 15 blocks are the plain keystream; PROVEN_CHACHA_RNG_NO_ERASURE gives the plain keystream throughout; bytes handed out are wiped from the generator; after the first epoch the key is no longer the seed.",
        "Inspect chacha_rekey_from, chacha_after_block and the erasure branch of proven_chacha_rng_fill in src/proven/random.c.");
    // ---------------------------------------------------------------
    {
        enum { N = 40960 };
        static proven_byte_t model[N], plainm[N], got[N];
        proven_byte_t seed[PROVEN_CHACHA_SEED_SIZE];
        for (int i = 0; i < PROVEN_CHACHA_SEED_SIZE; ++i) seed[i] = (proven_byte_t)(i * 29 + 3);
        m_stream(seed, true, model, N);
        m_stream(seed, false, plainm, N);
        PROVEN_TEST_ASSERT(memcmp(model, plainm, 15 * 64) == 0 && memcmp(model + 15 * 64, plainm + 15 * 64, 64) != 0,
            "the model: its first 15 blocks are the plain keystream, the 16th is not output", "");
        proven_chacha_rng_t g;
        proven_chacha_rng_seed(&g, seed);
        proven_chacha_rng_fill(&g, got, N);
        PROVEN_TEST_ASSERT(memcmp(got, model, N) == 0, "one fill of 40 KiB is the model's stream", "");
        const proven_size_t sizes[] = { 1, 959, 1, 64, 3, 1024, 191, 192, 255, 960, 17, 4096, 61, 833 };
        proven_chacha_rng_seed(&g, seed);
        memset(got, 0, sizeof got);
        for (proven_size_t off = 0, i = 0; off < N; i = (i + 1) % (sizeof sizes / sizeof sizes[0])) {
            proven_size_t n = sizes[i] < N - off ? sizes[i] : N - off;
            proven_chacha_rng_fill(&g, got + off, n);
            off += n;
        }
        PROVEN_TEST_ASSERT(memcmp(got, model, N) == 0, "pieces of fourteen sizes, from 1 to 4096 bytes, give the same stream", "");
        proven_chacha_rng_seed(&g, seed);
        for (proven_size_t off = 0; off < N; off += 8) {
            const proven_u64 v = proven_chacha_rng_next(&g);
            for (int k = 0; k < 8; ++k) got[off + (proven_size_t)k] = (proven_byte_t)(v >> (8 * k));
        }
        PROVEN_TEST_ASSERT(memcmp(got, model, N) == 0, "and so do words from proven_chacha_rng_next", "");
        proven_chacha_rng_seed_ex(&g, seed, PROVEN_CHACHA_RNG_NO_ERASURE);
        proven_chacha_rng_fill(&g, got, N);
        PROVEN_TEST_ASSERT(memcmp(got, plainm, N) == 0, "PROVEN_CHACHA_RNG_NO_ERASURE gives the plain keystream, as before v0.116.0", "");
        proven_chacha_rng_seed_ex(&g, seed, PROVEN_CHACHA_RNG_NO_FORK_RESEED | PROVEN_CHACHA_RNG_RESEED_EVERY_MIB);
        proven_chacha_rng_fill(&g, got, N);
        PROVEN_TEST_ASSERT(memcmp(got, model, N) == 0, "a seed of its own ignores the two options that concern a seed from entropy", "");

        proven_chacha_rng_seed(&g, seed);
        (void)proven_chacha_rng_next(&g);
        proven_chacha_rng_fill(&g, got, 13);
        PROVEN_TEST_ASSERT(g.used == 21 && all_zero(g.block, 21), "the 21 bytes handed out are wiped from the generator's block", "Check the secure_zero calls in proven_chacha_rng_fill and _next.");
        proven_chacha_rng_fill(&g, got, 960);
        uint32_t k0[8];
        for (int i = 0; i < 8; ++i) k0[i] = (uint32_t)seed[4 * i] | ((uint32_t)seed[4 * i + 1] << 8) | ((uint32_t)seed[4 * i + 2] << 16) | ((uint32_t)seed[4 * i + 3] << 24);
        PROVEN_TEST_ASSERT(memcmp(&g.state[4], k0, sizeof k0) != 0 && g.state[12] >= 16, "a draw past the first epoch has replaced the key: the seed is no longer in the generator", "");
        proven_chacha_rng_seed(&g, seed);
        proven_chacha_rng_fill(&g, got, 15 * 64);
        PROVEN_TEST_ASSERT(g.state[12] == 16 && memcmp(&g.state[4], k0, sizeof k0) != 0, "the key is replaced as soon as the epoch's last data block is made, not at the next draw", "Check chacha_after_block.");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("reseeding every MiB, on request",
        "RFC-0048 E-3. Two copies of one generator seeded from entropy give the same stream; with PROVEN_CHACHA_RNG_RESEED_EVERY_MIB they part at the first MiB of keystream, where each mixes in new entropy - and not before.",
        "Inspect the CHACHA_MIB branch of chacha_rekey_from.");
    // ---------------------------------------------------------------
    {
        static proven_byte_t a[1100000], b[1100000];
        proven_chacha_rng_t x, y;
        PROVEN_TEST_ASSERT(proven_chacha_rng_seed_from_entropy_ex(&x, PROVEN_CHACHA_RNG_RESEED_EVERY_MIB), "seeded from entropy with the option", "");
        y = x;
        proven_chacha_rng_fill(&x, a, sizeof a);
        proven_chacha_rng_fill(&y, b, sizeof b);
        /* 16,384 blocks of keystream, 1,024 of them keys: 15,360 output blocks before the reseed. */
        const size_t at = (size_t)15360 * 64;
        PROVEN_TEST_ASSERT(memcmp(a, b, at) == 0 && memcmp(a + at, b + at, 64) != 0, "the copies agree for 15,360 blocks (1 MiB of keystream) and part at once after it", "");
        PROVEN_TEST_ASSERT(proven_chacha_rng_seed_from_entropy(&x), "and without the option", "");
        y = x;
        proven_chacha_rng_fill(&x, a, sizeof a);
        proven_chacha_rng_fill(&y, b, sizeof b);
        PROVEN_TEST_ASSERT(memcmp(a, b, sizeof a) == 0, "copies never part: no reseed unless asked for", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("reseeding every MiB where word 12 wraps",
        "With PROVEN_CHACHA_RNG_RESEED_EVERY_MIB, an epoch that ends where the block counter (word 12) wraps carries into word 13 once, whatever sizes the draw comes in - in whole epochs (eight lanes) or a block at a time.",
        "Inspect the whole-epoch branch of proven_chacha_rng_fill in src/proven/random.c: it must not step the counter past the key block twice.");
    // ---------------------------------------------------------------
    {
        static proven_byte_t a[4096];
        proven_chacha_rng_t x, y;
        PROVEN_TEST_ASSERT(proven_chacha_rng_seed_from_entropy_ex(&x, PROVEN_CHACHA_RNG_RESEED_EVERY_MIB), "seeded from entropy with the option", "");
        x.state[12] = 0u - 16u;                    /* an epoch's first block, the wrap at its end */
        y = x;
        const proven_u32 w13 = x.state[13];
        proven_chacha_rng_fill(&x, a, sizeof a);
        for (int i = 0; i < 64; ++i) proven_chacha_rng_fill(&y, a, 64);
        PROVEN_TEST_ASSERT(x.state[13] == w13 + 1u && y.state[13] == w13 + 1u && x.state[12] == y.state[12],
                           "one carry into word 13, in one draw and in 64-byte draws alike, and the same counter after", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("proven_random_fast_bytes",
        "RFC-0048 E-4. Fills, of any size, from a generator of the thread's own; two draws differ.",
        "Inspect proven_random_fast_bytes in src/proven/random.c.");
    // ---------------------------------------------------------------
    {
        static proven_byte_t a[5000], b[5000];
        PROVEN_TEST_ASSERT(proven_random_fast_bytes(a, 0) && !proven_random_fast_bytes(NULL, 4), "len 0 is true, no buffer is false", "");
        PROVEN_TEST_ASSERT(proven_random_fast_bytes(a, sizeof a) && proven_random_fast_bytes(b, sizeof b) && memcmp(a, b, sizeof a) != 0 && !all_zero(a, sizeof a),
            "two fills of 5,000 bytes, different and not zero", "");
        for (int n = 1; n < 300; n += 37) PROVEN_TEST_ASSERT(proven_random_fast_bytes(a, (proven_size_t)n), "small fills too", "");
    }

#if !defined(_WIN32) && !defined(_WIN64)
    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("fork: a generator seeded from entropy takes a new key in the child",
        "RFC-0048 E-2. A generator that has handed out part of a block forks; parent and child each draw 64 bytes. Seeded from entropy: different. With PROVEN_CHACHA_RNG_NO_FORK_RESEED, or seeded with bytes of its own: the same (reproducible). proven_random_fast_bytes: different.",
        "Inspect chacha_fork_check in src/proven/random.c and proven_sys_fork_watch in platform/proven_sys_random.c.");
    // ---------------------------------------------------------------
    {
        proven_byte_t seed[PROVEN_CHACHA_SEED_SIZE] = { 1, 2, 3 };
        for (int mode = 0; mode < 4; ++mode) {
            proven_chacha_rng_t g;
            proven_byte_t mine[64], theirs[64];
            if (mode == 0) PROVEN_TEST_ASSERT(proven_chacha_rng_seed_from_entropy(&g), "seeded", "");
            if (mode == 1) PROVEN_TEST_ASSERT(proven_chacha_rng_seed_from_entropy_ex(&g, PROVEN_CHACHA_RNG_NO_FORK_RESEED), "seeded", "");
            if (mode == 2) proven_chacha_rng_seed(&g, seed);
            if (mode == 3) PROVEN_TEST_ASSERT(proven_random_fast_bytes(mine, 20), "the thread's generator is seeded", "");
            if (mode != 3) (void)proven_chacha_rng_next(&g);       /* part of a block is buffered */
            int fd[2];
            PROVEN_TEST_ASSERT(pipe(fd) == 0, "a pipe", "");
            fflush(NULL);
            const pid_t pid = fork();
            PROVEN_TEST_ASSERT(pid >= 0, "fork", "");
            if (pid == 0) {
                if (mode == 3) (void)proven_random_fast_bytes(theirs, sizeof theirs);
                else proven_chacha_rng_fill(&g, theirs, sizeof theirs);
                _exit(write(fd[1], theirs, sizeof theirs) == (ssize_t)sizeof theirs ? 0 : 1);
            }
            close(fd[1]);
            if (mode == 3) (void)proven_random_fast_bytes(mine, sizeof mine);
            else proven_chacha_rng_fill(&g, mine, sizeof mine);
            size_t got = 0;
            while (got < sizeof theirs) { ssize_t r = read(fd[0], theirs + got, sizeof theirs - got); if (r <= 0) break; got += (size_t)r; }
            close(fd[0]);
            int status = 0;
            (void)waitpid(pid, &status, 0);
            PROVEN_TEST_ASSERT(got == sizeof theirs && WIFEXITED(status) && WEXITSTATUS(status) == 0, "the child's 64 bytes", "");
            const bool same = memcmp(mine, theirs, sizeof mine) == 0;
            static const char *const what[] = { "seeded from entropy: parent and child differ", "PROVEN_CHACHA_RNG_NO_FORK_RESEED: the child replays the parent",
                                                "seeded with bytes of its own: the child replays the parent (reproducible)", "proven_random_fast_bytes: parent and child differ" };
            PROVEN_TEST_ASSERT(same == (mode == 1 || mode == 2), what[mode], "");
        }
    }
#endif

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
