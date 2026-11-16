#include "proven.h"
#include "proven_test.h"
#include "../crypto/src/proven_internal_crypto.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if !defined(_WIN32) && !defined(_WIN64)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/*
 * proven_aes_rng_t (RFC-0049): AES-256 in counter mode under the ChaCha generator's rules.
 * Judged against OpenSSL's AES-256-CTR for the first epoch, and against an independent model
 * built on proven_aes_encrypt_block (one block at a time, not the counter-mode path) for the
 * erasure rule: blocks 0..61 of an epoch out, 62 and 63 the next key, the counter from 0 again.
 */

/* `openssl enc -aes-256-ctr -K 000102..1f -iv 00..00` over zeros: bytes 0..63 and 976..991. */
static const proven_byte_t ossl_first64[64] = {
    0xf2, 0x90, 0x00, 0xb6, 0x2a, 0x49, 0x9f, 0xd0, 0xa9, 0xf3, 0x9a, 0x6a, 0xdd, 0x2e, 0x77, 0x80,
    0xf0, 0x5d, 0x76, 0xae, 0x4a, 0xb9, 0x9f, 0xe5, 0xa6, 0xf6, 0x9b, 0x31, 0x48, 0xc2, 0x36, 0x3d,
    0x0e, 0xbc, 0xb5, 0xde, 0xb5, 0x2c, 0x83, 0xbd, 0x08, 0xa8, 0xa9, 0x35, 0x18, 0x2c, 0x91, 0x99,
    0xd2, 0x43, 0x56, 0x53, 0x28, 0x81, 0x60, 0x2f, 0x80, 0x9e, 0xb3, 0x83, 0xc5, 0xff, 0x5d, 0x56,
};
static const proven_byte_t ossl_976[16] = { 0x1a, 0xcd, 0xdd, 0x70, 0xf3, 0x90, 0x12, 0xf1, 0xc6, 0xd9, 0x9d, 0x4b, 0x38, 0x10, 0xbe, 0x38 };

/* Block j of the keystream under `key`: AES(key, j as a 128-bit big-endian number). */
static void m_block(const proven_byte_t key[32], uint64_t j, proven_byte_t out[16]) {
    proven_aes_t a;
    PROVEN_TEST_ASSERT(proven_aes_init(&a, (proven_mem_view_t){ key, 32 }) == PROVEN_OK, "an AES-256 key", "");
    proven_byte_t in[16] = { 0 };
    for (int i = 0; i < 8; ++i) in[15 - i] = (proven_byte_t)(j >> (8 * i));
    proven_aes_encrypt_block(&a, in, out);
}
static void m_stream(const proven_byte_t seed[32], bool erasure, proven_byte_t *out, size_t len) {
    proven_byte_t key[32], b[16];
    memcpy(key, seed, 32);
    for (uint64_t j = 0; len > 0; ++j) {
        if (erasure && j == 62) {
            proven_byte_t k[32];
            m_block(key, 62, k); m_block(key, 63, k + 16);
            memcpy(key, k, 32);
            j = (uint64_t)-1;                                          /* the next epoch's block 0 */
            continue;
        }
        m_block(key, j, b);
        size_t n = len < 16 ? len : 16;
        memcpy(out, b, n);
        out += n; len -= n;
    }
}
static bool all_zero(const proven_byte_t *p, size_t n) { for (size_t i = 0; i < n; ++i) if (p[i]) return false; return true; }

int main(void) {
    PROVEN_TEST_SUITE("an AES-256-CTR random generator",
        "proven_aes_rng_t: the first epoch is OpenSSL's AES-256-CTR, the erasure stream is an independent model's however it is drawn, the plain stream crosses the 32-bit counter's wrap, the processor's instructions and the bitsliced cipher give the same bytes, what is handed out is wiped, the reseed every MiB parts two copies where it should, and a forked child takes a new key.",
        "Inspect the proven_aes_rng_ functions at the end of crypto/src/aes.c, and proven_crypto_aes_ctr in crypto/src/crypto_aes.c.");

    proven_byte_t seed[32];
    for (int i = 0; i < 32; ++i) seed[i] = (proven_byte_t)i;
    enum { N = 40960 };
    static proven_byte_t model[N], plainm[N], got[N];

    PROVEN_TEST_SECTION("the standard's keystream, and the model", "The first epoch is AES-256-CTR with a zero IV as OpenSSL computes it; the model agrees.", "");
    {
        m_stream(seed, true, model, N);
        m_stream(seed, false, plainm, N);
        PROVEN_TEST_ASSERT(memcmp(plainm, ossl_first64, 64) == 0 && memcmp(plainm + 976, ossl_976, 16) == 0,
            "the model's plain stream is OpenSSL's AES-256-CTR (bytes 0..63 and 976..991)", "proven_aes_encrypt_block disagrees with OpenSSL: check the AES key schedule.");
        PROVEN_TEST_ASSERT(memcmp(model, plainm, 992) == 0 && memcmp(model + 992, plainm + 992, 16) != 0,
            "and the erasure model's first 992 bytes are the same, the next not", "");
        proven_aes_rng_t g;
        proven_aes_rng_seed(&g, seed);
        proven_aes_rng_fill(&g, got, 64);
        PROVEN_TEST_ASSERT(memcmp(got, ossl_first64, 64) == 0, "the generator's first 64 bytes are OpenSSL's", "");
    }

    PROVEN_TEST_SECTION("key erasure: the model's stream, however it is drawn", "40 KiB in one fill, in pieces of awkward sizes, and as words; NO_ERASURE gives plain AES-256-CTR.", "Inspect aes_rng_blocks and aes_rng_rekey.");
    {
        proven_aes_rng_t g;
        proven_aes_rng_seed(&g, seed);
        proven_aes_rng_fill(&g, got, N);
        PROVEN_TEST_ASSERT(memcmp(got, model, N) == 0, "one fill of 40 KiB", "");
        const size_t sizes[] = { 1, 991, 1, 64, 3, 1024, 15, 16, 17, 992, 33, 4096, 7, 833 };
        proven_aes_rng_seed(&g, seed);
        memset(got, 0, sizeof got);
        for (size_t off = 0, i = 0; off < N; i = (i + 1) % (sizeof sizes / sizeof sizes[0])) {
            size_t n = sizes[i] < N - off ? sizes[i] : N - off;
            proven_aes_rng_fill(&g, got + off, n);
            off += n;
        }
        PROVEN_TEST_ASSERT(memcmp(got, model, N) == 0, "pieces of fourteen sizes from 1 to 4096 bytes", "");
        proven_aes_rng_seed(&g, seed);
        for (size_t off = 0; off < N; off += 8) {
            const proven_u64 v = proven_aes_rng_next(&g);
            for (int k = 0; k < 8; ++k) got[off + (size_t)k] = (proven_byte_t)(v >> (8 * k));
        }
        PROVEN_TEST_ASSERT(memcmp(got, model, N) == 0, "words from proven_aes_rng_next", "");
        proven_aes_rng_seed_ex(&g, seed, PROVEN_AES_RNG_NO_ERASURE);
        proven_aes_rng_fill(&g, got, N);
        PROVEN_TEST_ASSERT(memcmp(got, plainm, N) == 0, "PROVEN_AES_RNG_NO_ERASURE: plain AES-256-CTR", "");
        proven_aes_rng_seed_ex(&g, seed, PROVEN_AES_RNG_NO_FORK_RESEED | PROVEN_AES_RNG_RESEED_EVERY_MIB);
        proven_aes_rng_fill(&g, got, N);
        PROVEN_TEST_ASSERT(memcmp(got, model, N) == 0, "a seed of one's own ignores the options for a seed from entropy", "");
        proven_aes_rng_seed(&g, seed);
        (void)proven_aes_rng_next(&g);
        proven_aes_rng_fill(&g, got, 5);
        PROVEN_TEST_ASSERT(g.used == 13 && all_zero(g.block, 13), "the 13 bytes handed out are wiped from the generator's block", "");
    }

    PROVEN_TEST_SECTION("the plain stream across the 32-bit counter's wrap", "Blocks 2^32 - 2 to 2^32 + 5: the counter's high bits carry into the nonce.", "Inspect aes_rng_keystream.");
    {
        proven_aes_rng_t g;
        proven_aes_rng_seed_ex(&g, seed, PROVEN_AES_RNG_NO_ERASURE);
        g.counter = 0xFFFFFFFEull;
        proven_aes_rng_fill(&g, got, 16 * 8);
        bool same = true;
        for (uint64_t j = 0; j < 8; ++j) { proven_byte_t b[16]; m_block(seed, 0xFFFFFFFEull + j, b); same = same && memcmp(b, got + 16 * j, 16) == 0; }
        PROVEN_TEST_ASSERT(same && g.counter == 0x100000006ull, "eight blocks across the wrap are the model's, and the counter is past it", "");
    }

    PROVEN_TEST_SECTION("the processor's instructions and the bitsliced cipher give the same bytes", "The same seed with the hardware path forced off.", "Inspect proven_crypto_aes_ctr's two paths in crypto/src/crypto_aes.c.");
    {
        static proven_byte_t hw[10000], sw[10000];
        proven_aes_rng_t g;
        proven_aes_rng_seed(&g, seed);
        proven_aes_rng_fill(&g, hw, sizeof hw);
        proven_crypto_aes_force_portable(true);
        proven_aes_rng_seed(&g, seed);
        proven_aes_rng_fill(&g, sw, sizeof sw);
        proven_crypto_aes_force_portable(false);
        PROVEN_TEST_ASSERT(memcmp(hw, sw, sizeof hw) == 0 && memcmp(hw, model, sizeof hw) == 0, "10,000 bytes, the same either way, and the model's", "");
        fprintf(stderr, "[PROVEN][TEST][INFO] the processor's AES instructions are %s here\n", proven_aes_hw_available() ? "used" : "not available");
    }

    PROVEN_TEST_SECTION("seeding from entropy, the reseed every MiB, an unseeded generator, the trait", "", "");
    {
        static proven_byte_t a[1100000], b[1100000];
        proven_aes_rng_t x, y;
        PROVEN_TEST_ASSERT(proven_aes_rng_seed_from_entropy_ex(&x, PROVEN_AES_RNG_RESEED_EVERY_MIB), "seeded from entropy with the option", "");
        y = x;
        proven_aes_rng_fill(&x, a, sizeof a);
        proven_aes_rng_fill(&y, b, sizeof b);
        const size_t at = (size_t)1024 * 992;                          /* 1,024 epochs of 992 bytes */
        PROVEN_TEST_ASSERT(memcmp(a, b, at) == 0 && memcmp(a + at, b + at, 16) != 0, "two copies agree for 1,024 epochs and part at once after", "");
        PROVEN_TEST_ASSERT(proven_aes_rng_seed_from_entropy(&x), "and without the option", "");
        y = x;
        proven_aes_rng_fill(&x, a, sizeof a);
        proven_aes_rng_fill(&y, b, sizeof b);
        PROVEN_TEST_ASSERT(memcmp(a, b, sizeof a) == 0 && !all_zero(a, 64), "copies never part, and the bytes are not zero", "");
        proven_aes_rng_t z;
        memset(&z, 0, sizeof z);
        proven_byte_t t[32];
        memset(t, 0xAA, sizeof t);
        proven_aes_rng_fill(&z, t, sizeof t);
        PROVEN_TEST_ASSERT(all_zero(t, sizeof t) && proven_aes_rng_next(&z) == 0 && !proven_rng_is_valid(proven_aes_rng(&z)), "a zeroed generator is inert: zeros, 0 and an invalid trait", "");
        proven_rng_t r = proven_aes_rng(&x);
        bool in_range = proven_rng_is_valid(r);
        for (int i = 0; i < 1000 && in_range; ++i) { proven_i64 v = proven_rng_range(r, 1, 6); in_range = v >= 1 && v <= 6; }
        PROVEN_TEST_ASSERT(in_range, "through the trait, proven_rng_range(1, 6) stays in range", "");
        PROVEN_TEST_ASSERT(proven_random_fork_generation() == proven_random_fork_generation(), "the fork generation is steady in one process", "");
    }

#if !defined(_WIN32) && !defined(_WIN64)
    PROVEN_TEST_SECTION("fork: a generator seeded from entropy takes a new key in the child",
        "After a word, a fork; parent and child draw 64 bytes. Seeded from entropy: different. With NO_FORK_RESEED, or a seed of one's own: the same.", "Inspect aes_rng_fork_check.");
    {
        for (int mode = 0; mode < 3; ++mode) {
            proven_aes_rng_t g;
            proven_byte_t mine[64], theirs[64];
            if (mode == 0) PROVEN_TEST_ASSERT(proven_aes_rng_seed_from_entropy(&g), "seeded", "");
            if (mode == 1) PROVEN_TEST_ASSERT(proven_aes_rng_seed_from_entropy_ex(&g, PROVEN_AES_RNG_NO_FORK_RESEED), "seeded", "");
            if (mode == 2) proven_aes_rng_seed(&g, seed);
            (void)proven_aes_rng_next(&g);
            int fd[2];
            PROVEN_TEST_ASSERT(pipe(fd) == 0, "a pipe", "");
            fflush(NULL);
            const pid_t pid = fork();
            PROVEN_TEST_ASSERT(pid >= 0, "fork", "");
            if (pid == 0) {
                proven_aes_rng_fill(&g, theirs, sizeof theirs);
                _exit(write(fd[1], theirs, sizeof theirs) == (ssize_t)sizeof theirs ? 0 : 1);
            }
            close(fd[1]);
            proven_aes_rng_fill(&g, mine, sizeof mine);
            size_t n = 0;
            while (n < sizeof theirs) { ssize_t r = read(fd[0], theirs + n, sizeof theirs - n); if (r <= 0) break; n += (size_t)r; }
            close(fd[0]);
            int status = 0;
            (void)waitpid(pid, &status, 0);
            PROVEN_TEST_ASSERT(n == sizeof theirs && WIFEXITED(status) && WEXITSTATUS(status) == 0, "the child's 64 bytes", "");
            static const char *const what[] = { "seeded from entropy: parent and child differ", "NO_FORK_RESEED: the child replays the parent", "a seed of one's own: the child replays the parent" };
            PROVEN_TEST_ASSERT((memcmp(mine, theirs, sizeof mine) == 0) == (mode != 0), what[mode], "");
        }
    }
#endif

    PROVEN_TEST_PASS("the AES generator is AES-256-CTR, erases its key at fixed positions, and behaves as the ChaCha generator does.");
    return 0;
}
