#include "proven.h"
#include "proven_test.h"
#include "../src/proven/proven_internal_crypto.h"
#include "test_unit_crypto_vectors.h"
#include "../platform/proven_sys_aes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The primitives under the TLS unit: ChaCha20-Poly1305, AES-GCM (the bitsliced code and the
 * processor's instructions), the multi-precision arithmetic, P-256 and P-384, X25519, Ed25519
 * and RSA signature verification. They are internal, so this test reaches them by path.
 *
 * The expected values are in test_unit_crypto_vectors.h and come from another implementation
 * (Python `cryptography`, OpenSSL underneath), made by a private generator. The first case of
 * several sets is a published one that the generator reproduced before writing it: RFC 8439
 * section 2.8.2, FIPS 197 appendix C, RFC 7748 section 6.1, RFC 8032 section 7.1 test 1, and
 * RFC 6979 appendix A.2.5.
 */

#define MAX_FIELDS 12
static char g_line[20000];
static proven_byte_t g_bytes[MAX_FIELDS][2100];
static proven_size_t g_len[MAX_FIELDS];
static char *g_field[MAX_FIELDS];

/* Split one case into fields, and decode each as hex where it is hex ("-" is empty). */
static int split(const char *line) {
    int n = 0;
    snprintf(g_line, sizeof g_line, "%s", line);
    for (char *tok = strtok(g_line, " "); tok && n < MAX_FIELDS; tok = strtok(NULL, " ")) {
        g_field[n] = tok;
        g_len[n] = 0;
        proven_size_t len = strlen(tok);
        if (tok[0] != '-' && len % 2 == 0 && len / 2 <= sizeof g_bytes[0]) {
            for (proven_size_t i = 0; i < len / 2; ++i) {
                char c[3] = { tok[2 * i], tok[2 * i + 1], 0 };
                g_bytes[n][i] = (proven_byte_t)strtoul(c, NULL, 16);
            }
            g_len[n] = len / 2;
        }
        n++;
    }
    return n;
}

#define V(i) ((proven_mem_view_t){ .ptr = g_bytes[i], .size = g_len[i] })
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static proven_byte_t g_out[2100], g_out2[2100];

/* AES-GCM over every case, on whichever implementation contexts get now. */
static proven_size_t run_gcm(void) {
    proven_size_t good = 0;
    for (proven_size_t i = 0; i < COUNT(CRV_GCM); ++i) {
        split(CRV_GCM[i]);
        proven_crypto_aes_gcm_t ctx;
        if (!proven_crypto_aes_gcm_init(&ctx, g_bytes[1], g_len[1])) continue;
        if (g_field[0][0] == 'b') {
            proven_crypto_aes_encrypt_block(&ctx, g_bytes[2], g_out);
            if (memcmp(g_out, g_bytes[3], 16) == 0) good++;
            continue;
        }
        proven_byte_t tag[16];
        proven_crypto_aes_gcm_seal(&ctx, g_bytes[2], V(3), V(4), g_out, tag);
        bool ok = memcmp(g_out, g_bytes[5], g_len[4]) == 0 && memcmp(tag, g_bytes[6], 16) == 0;
        ok = ok && proven_crypto_aes_gcm_open(&ctx, g_bytes[2], V(3), V(5), g_bytes[6], g_out2) && memcmp(g_out2, g_bytes[4], g_len[4]) == 0;
        /* In place, and then a forgery: one bit of the tag, of the text, of the associated data. */
        memcpy(g_out2, g_bytes[4], g_len[4]);
        proven_crypto_aes_gcm_seal(&ctx, g_bytes[2], V(3), (proven_mem_view_t){ g_out2, g_len[4] }, g_out2, tag);
        ok = ok && memcmp(g_out2, g_bytes[5], g_len[4]) == 0;
        tag[5] ^= 0x10;
        memset(g_out2, 0xee, sizeof g_out2);
        ok = ok && !proven_crypto_aes_gcm_open(&ctx, g_bytes[2], V(3), V(5), tag, g_out2);
        for (proven_size_t k = 0; k < g_len[5]; ++k) ok = ok && g_out2[k] == 0;
        if (g_len[5] > 0) { g_bytes[5][0] ^= 1; ok = ok && !proven_crypto_aes_gcm_open(&ctx, g_bytes[2], V(3), V(5), g_bytes[6], g_out2); }
        else if (g_len[3] > 0) { g_bytes[3][0] ^= 1; ok = ok && !proven_crypto_aes_gcm_open(&ctx, g_bytes[2], V(3), V(5), g_bytes[6], g_out2); }
        if (ok) good++;
    }
    return good;
}

int main(void) {
    PROVEN_TEST_SUITE("the cryptographic primitives under TLS",
        "Each primitive against values from another implementation, and against the forgeries and malformed inputs it must refuse.",
        "Inspect the src/proven/crypto_*.c file the failing section names. A mismatch is a disagreement with the standard.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("ChaCha20-Poly1305",
        "RFC 8439's example and lengths on each side of every block boundary; a changed tag, text or header is refused.",
        "Check src/proven/crypto_chacha.c.");
    {
        proven_size_t good = 0;
        for (proven_size_t i = 0; i < COUNT(CRV_CHACHA); ++i) {
            split(CRV_CHACHA[i]);
            proven_byte_t tag[16];
            proven_crypto_chacha20poly1305_seal(g_bytes[0], g_bytes[1], V(2), V(3), g_out, tag);
            bool ok = memcmp(g_out, g_bytes[4], g_len[3]) == 0 && memcmp(tag, g_bytes[5], 16) == 0;
            ok = ok && proven_crypto_chacha20poly1305_open(g_bytes[0], g_bytes[1], V(2), V(4), g_bytes[5], g_out2) && memcmp(g_out2, g_bytes[3], g_len[3]) == 0;
            tag[0] ^= 1;
            memset(g_out2, 0xee, sizeof g_out2);
            ok = ok && !proven_crypto_chacha20poly1305_open(g_bytes[0], g_bytes[1], V(2), V(4), tag, g_out2);
            for (proven_size_t k = 0; k < g_len[4]; ++k) ok = ok && g_out2[k] == 0;
            if (g_len[2] > 0) { g_bytes[2][g_len[2] - 1] ^= 0x80; ok = ok && !proven_crypto_chacha20poly1305_open(g_bytes[0], g_bytes[1], V(2), V(4), g_bytes[5], g_out2); }
            if (ok) good++;
        }
        PROVEN_TEST_ASSERT(good == COUNT(CRV_CHACHA) && good >= 16, "every case seals to the expected bytes, opens again, and refuses a forgery leaving zeros", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("AES-GCM, both implementations",
        "FIPS 197's blocks and GCM at many lengths, on the bitsliced code and on the processor's instructions, which must agree.",
        "Check src/proven/crypto_aes.c (portable) and platform/proven_sys_aes.c (hardware).");
    {
        proven_crypto_aes_force_portable(true);
        PROVEN_TEST_ASSERT(!proven_crypto_aes_hw_available(), "the test hook forces the portable code", "");
        PROVEN_TEST_ASSERT(run_gcm() == COUNT(CRV_GCM) && COUNT(CRV_GCM) >= 50, "bitsliced AES and bit-serial GHASH: every case", "");
        proven_crypto_aes_force_portable(false);
        if (proven_crypto_aes_hw_available()) {
            PROVEN_TEST_ASSERT(run_gcm() == COUNT(CRV_GCM), "the processor's AES and carry-less multiply: every case", "");
            /* The two against each other on inputs neither was written for. */
            proven_crypto_aes_gcm_t hw, sw, sw16;
            proven_byte_t key[32], nonce[12], tag_hw[16], tag_sw[16];
            static proven_byte_t text[777], a[777], b[777];
            bool same = true;
            for (int round = 0; round < 24; ++round) {
                for (proven_size_t i = 0; i < sizeof key; ++i) key[i] = (proven_byte_t)(i * 31 + round * 7 + 1);
                for (proven_size_t i = 0; i < sizeof nonce; ++i) nonce[i] = (proven_byte_t)(i + round * 13);
                for (proven_size_t i = 0; i < sizeof text; ++i) text[i] = (proven_byte_t)(i * 5 + round);
                proven_size_t len = (proven_size_t)(round * 33 % 777), alen = (proven_size_t)(round * 17 % 90);
                PROVEN_TEST_ASSERT(proven_crypto_aes_gcm_init(&hw, key, round % 2 ? 32 : 16), "init", "");
                proven_crypto_aes_force_portable(true);
                PROVEN_TEST_ASSERT(proven_crypto_aes_gcm_init(&sw, key, round % 2 ? 32 : 16), "init", "");
                proven_crypto_aes_force_portable(false);
                proven_crypto_aes_gcm_seal(&hw, nonce, (proven_mem_view_t){ text, alen }, (proven_mem_view_t){ text, len }, a, tag_hw);
                proven_crypto_aes_gcm_seal(&sw, nonce, (proven_mem_view_t){ text, alen }, (proven_mem_view_t){ text, len }, b, tag_sw);
                same = same && hw.hw && !sw.hw && memcmp(a, b, len) == 0 && memcmp(tag_hw, tag_sw, 16) == 0;
            }
            PROVEN_TEST_ASSERT(same, "the two implementations agree on 24 further messages", "");

            /* Every length: the hardware path takes eight blocks at a time and folds eight
             * into one reduction, so each count of whole blocks and each length of tail is a
             * different way through it - for the text, and for the associated data, which
             * goes through the same hash. Sealed out of place, in place, and opened. */
            static proven_byte_t big[1200], c1[1200], c2[1200], back[1200];
            for (proven_size_t i = 0; i < sizeof big; ++i) big[i] = (proven_byte_t)(i * 73 + i / 7 + 11);
            proven_size_t differing = 0;
            for (proven_size_t len = 0; len <= 1100; ++len) {
                const proven_size_t klen = len % 3 == 0 ? 32 : 16, alen = (len * 7) % 401, at = len % 16;
                for (proven_size_t i = 0; i < sizeof key; ++i) key[i] = (proven_byte_t)(i * 29 + len);
                for (proven_size_t i = 0; i < sizeof nonce; ++i) nonce[i] = (proven_byte_t)(i * 3 + len / 5);
                (void)proven_crypto_aes_gcm_init(&hw, key, klen);
                proven_crypto_aes_force_portable(true);
                (void)proven_crypto_aes_gcm_init(&sw, key, klen);
                proven_crypto_aes_force_portable(false);
                const proven_mem_view_t ad = { big + 700 + at, alen }, plain = { big + at, len };
                proven_crypto_aes_gcm_seal(&hw, nonce, ad, plain, c1, tag_hw);
                proven_crypto_aes_gcm_seal(&sw, nonce, ad, plain, c2, tag_sw);
                bool row = memcmp(c1, c2, len) == 0 && memcmp(tag_hw, tag_sw, 16) == 0;
                memcpy(back + at, big + at, len);
                proven_crypto_aes_gcm_seal(&hw, nonce, ad, (proven_mem_view_t){ back + at, len }, back + at, tag_sw);
                row = row && memcmp(back + at, c1, len) == 0 && memcmp(tag_hw, tag_sw, 16) == 0;
                row = row && proven_crypto_aes_gcm_open(&hw, nonce, ad, (proven_mem_view_t){ back + at, len }, tag_hw, back + at) && memcmp(back + at, big + at, len) == 0;
                row = row && proven_crypto_aes_gcm_open(&sw, nonce, ad, (proven_mem_view_t){ c1, len }, tag_hw, c2) && memcmp(c2, big + at, len) == 0;
                if (!row) { if (differing == 0) PROVEN_TEST_INFO("first difference at length {} (associated data {})", PROVEN_ARG((int)len), PROVEN_ARG((int)alen)); differing++; }
            }
            /* And long enough for the counter's low byte to turn over inside a message, which
             * 1,100 bytes are not: a TLS record of the largest size is 1,025 blocks. */
            static proven_byte_t rec[16401], r1[16401], r2[16401];
            for (proven_size_t i = 0; i < sizeof rec; ++i) rec[i] = (proven_byte_t)(i * 151 + i / 255);
            for (proven_size_t len = 16401 - 40; len <= 16401; len += 13) {
                proven_crypto_aes_gcm_seal(&hw, nonce, (proven_mem_view_t){ rec, 5 }, (proven_mem_view_t){ rec, len }, r1, tag_hw);
                proven_crypto_aes_gcm_seal(&sw, nonce, (proven_mem_view_t){ rec, 5 }, (proven_mem_view_t){ rec, len }, r2, tag_sw);
                if (memcmp(r1, r2, len) != 0 || memcmp(tag_hw, tag_sw, 16) != 0) differing++;
            }
            PROVEN_TEST_ASSERT(differing == 0, "and on every length from 0 to 1,100 bytes, with associated data of 0 to 400 - sealed, sealed in place, and each opened by the other - and on four records of the largest size",
                "Inspect proven_sys_aes_ctr and proven_sys_aes_ghash in platform/proven_sys_aes.c: the eight-block loops, what is left after them, and the one-block tails.");

            /* The counter, across the end of its thirty-two bits. GCM as TLS uses it never
             * gets there, but the function says what it does and so it is held to it: the
             * keystream is the cipher on nonce | counter, block by block. */
            bool wraps = true;
            (void)proven_crypto_aes_gcm_init(&hw, key, 16);
            proven_crypto_aes_force_portable(true);
            (void)proven_crypto_aes_gcm_init(&sw16, key, 16);       /* the bitsliced cipher: another implementation altogether */
            proven_crypto_aes_force_portable(false);
            static const proven_u32 carries[4] = { 0x100u, 0x10000u, 0x1000000u, 0u };     /* where a byte of the counter turns over; 0 is 2^32 */
            for (int edge = 0; edge < 4; ++edge) for (proven_u32 start = carries[edge] - 24; start != carries[edge] + 4; ++start) {
                memset(c1, 0, 400);
                proven_sys_aes_ctr(hw.rk, hw.rounds, nonce, start, c1, c1, 400);
                for (proven_u32 k = 0; k < 25; ++k) {
                    proven_byte_t block[16], want[16];
                    const proven_u32 ctr = start + k;
                    memcpy(block, nonce, 12);
                    block[12] = (proven_byte_t)(ctr >> 24); block[13] = (proven_byte_t)(ctr >> 16); block[14] = (proven_byte_t)(ctr >> 8); block[15] = (proven_byte_t)ctr;
                    proven_crypto_aes_encrypt_block(&sw16, block, want);
                    wraps = wraps && memcmp(c1 + 16 * k, want, 16) == 0;
                }
            }
            PROVEN_TEST_ASSERT(wraps, "the counter carries into each of its bytes, and from 2^32 - 1 to 0, inside a run of eight blocks - 28 starting points at each of the four - as it does one block at a time", "");
        } else {
            PROVEN_TEST_INFO("this processor or build has no hardware AES path; only the portable code ran");
        }
        proven_crypto_aes_gcm_t ctx;
        proven_byte_t key[32] = { 0 };
        PROVEN_TEST_ASSERT(!proven_crypto_aes_gcm_init(&ctx, key, 24) && !proven_crypto_aes_gcm_init(&ctx, key, 0) &&
                           !proven_crypto_aes_gcm_init(NULL, key, 16) && !proven_crypto_aes_gcm_init(&ctx, NULL, 16),
            "a key that is not 16 or 32 bytes, and a null argument, are refused", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("multi-precision arithmetic",
        "Multiplication, addition, subtraction, exponentiation, inversion and reduction modulo primes and odd composites of many sizes.",
        "Check src/proven/crypto_mp.c.");
    {
        proven_size_t good = 0;
        proven_size_t secret_inits = 0;
        static proven_crypto_mp_mod_t m, m2;
        static proven_crypto_limb_t a[PROVEN_CRYPTO_MP_MAX], b[PROVEN_CRYPTO_MP_MAX], e[PROVEN_CRYPTO_MP_MAX], am[PROVEN_CRYPTO_MP_MAX], bm[PROVEN_CRYPTO_MP_MAX], r[PROVEN_CRYPTO_MP_MAX];
        for (proven_size_t i = 0; i < COUNT(CRV_MP); ++i) {
            /* n a b e wide | a*b a+b a-b a^e a^-1 wide mod n */
            split(CRV_MP[i]);
            proven_size_t nl = g_len[0];
            if (!proven_crypto_mp_mod_init(&m, g_bytes[0], nl)) continue;
            proven_size_t s = m.limbs;
            if (!proven_crypto_mp_load_be(a, s, g_bytes[1], g_len[1]) || !proven_crypto_mp_load_be(b, s, g_bytes[2], g_len[2]) ||
                !proven_crypto_mp_load_be(e, s, g_bytes[3], g_len[3])) continue;
            bool ok = true;
            /* What the modulus was prepared with: n * n0inv is -1 in a limb, and a modulus read as
             * a secret - no leading zeros looked for, a longer walk to R^2 - is prepared the same. */
            ok = ok && (proven_crypto_limb_t)(m.n[0] * m.n0inv) == (proven_crypto_limb_t)0 - 1u;
            if (nl >= 8 && g_bytes[0][0] != 0) {
                ok = ok && proven_crypto_mp_mod_init_secret(&m2, g_bytes[0], nl) && m2.limbs == s && m2.n0inv == m.n0inv &&
                     proven_crypto_mp_eq(m2.rr, m.rr, s) != 0 && proven_crypto_mp_eq(m2.n, m.n, s) != 0;
                secret_inits++;
            }
            proven_crypto_mp_to_mont(&m, am, a); proven_crypto_mp_to_mont(&m, bm, b);
            proven_crypto_mp_montmul(&m, r, am, bm); proven_crypto_mp_from_mont(&m, r, r); proven_crypto_mp_store_be(g_out, nl, r, s);
            ok = ok && memcmp(g_out, g_bytes[5], nl) == 0;
            proven_crypto_mp_add(&m, r, a, b); proven_crypto_mp_store_be(g_out, nl, r, s); ok = ok && memcmp(g_out, g_bytes[6], nl) == 0;
            proven_crypto_mp_sub(&m, r, a, b); proven_crypto_mp_store_be(g_out, nl, r, s); ok = ok && memcmp(g_out, g_bytes[7], nl) == 0;
            proven_crypto_mp_pow(&m, r, am, e, s); proven_crypto_mp_from_mont(&m, r, r); proven_crypto_mp_store_be(g_out, nl, r, s);
            ok = ok && memcmp(g_out, g_bytes[8], nl) == 0;
            if (g_field[9][0] != 'x') {
                proven_crypto_mp_inv_prime(&m, r, am); proven_crypto_mp_from_mont(&m, r, r); proven_crypto_mp_store_be(g_out, nl, r, s);
                ok = ok && memcmp(g_out, g_bytes[9], nl) == 0;
            }
            proven_crypto_mp_reduce_be(&m, r, g_bytes[4], g_len[4]); proven_crypto_mp_store_be(g_out, nl, r, s);
            ok = ok && memcmp(g_out, g_bytes[10], nl) == 0;
            ok = ok && (proven_crypto_mp_lt(a, b, s) != 0) == (memcmp(g_bytes[1], g_bytes[2], nl) < 0) && proven_crypto_mp_eq(a, a, s) != 0 &&
                 (proven_crypto_mp_eq(a, b, s) != 0) == (memcmp(g_bytes[1], g_bytes[2], nl) == 0);
            if (ok) good++;
        }
        PROVEN_TEST_ASSERT(good == COUNT(CRV_MP) && good >= 60, "every case, including 0, 1 and n-1 as operands", "");
        PROVEN_TEST_ASSERT(secret_inits >= 40, "and the modulus prepared as a secret one agrees, for those of eight bytes and more", "");
        static const proven_byte_t even[] = { 0x01, 0x00 }, one[] = { 0x01 }, zero[] = { 0x00, 0x00 };
        PROVEN_TEST_ASSERT(!proven_crypto_mp_mod_init(&m, even, 2) && !proven_crypto_mp_mod_init(&m, one, 1) && !proven_crypto_mp_mod_init(&m, zero, 2) &&
                           !proven_crypto_mp_mod_init(&m, one, 0),
            "an even modulus, 1, 0 and nothing are not moduli", "");
        /* Moduli with every limb but the lowest all ones, and operands of all ones and of n - 1:
         * where a product's top word overflows and a sum's carry leaves the top limb. No oracle
         * is needed for what the arithmetic must satisfy by itself. */
        {
            static const proven_size_t sizes[] = { 1, 2, 3, 4, 5, 7, 8, 9, 12, 15, 16, 17, 24, 31, 32, 33, 48, 64, 65, 128, 256, 264, 512, 1024 };
            static const proven_byte_t lows[] = { 0xff, 0xfd, 0x01, 0x61 };
            static proven_byte_t nb[PROVEN_CRYPTO_MP_MAX_BYTES], xb[PROVEN_CRYPTO_MP_MAX_BYTES];
            static proven_crypto_limb_t one[PROVEN_CRYPTO_MP_MAX], neg[PROVEN_CRYPTO_MP_MAX], x[PROVEN_CRYPTO_MP_MAX], y[PROVEN_CRYPTO_MP_MAX], u[PROVEN_CRYPTO_MP_MAX], v[PROVEN_CRYPTO_MP_MAX];
            static proven_crypto_limb_t pr[2 * PROVEN_CRYPTO_MP_MAX];             /* a plain product: twice the limbs */
            proven_size_t held = 0, tried = 0;
            for (proven_size_t si = 0; si < COUNT(sizes); ++si) {
                for (proven_size_t li = 0; li < COUNT(lows); ++li) {
                    const proven_size_t len = sizes[si];
                    if (len == 1 && lows[li] == 0x01) continue;                   /* 1 is not a modulus */
                    memset(nb, 0xff, len); nb[len - 1] = lows[li];
                    if (len > 8 && li == 2) nb[len / 2] = 0x00;                   /* and one with a hole in it */
                    tried++;
                    if (!proven_crypto_mp_mod_init(&m, nb, len)) continue;
                    const proven_size_t s = m.limbs;
                    bool ok = (proven_crypto_limb_t)(m.n[0] * m.n0inv) == (proven_crypto_limb_t)0 - 1u;
                    for (proven_size_t i = 0; i < s; ++i) { one[i] = 0; u[i] = 0; }
                    one[0] = 1;
                    proven_crypto_mp_sub(&m, neg, u, one);                        /* n - 1 */
                    proven_crypto_mp_add(&m, v, neg, one);                        /* and back to zero */
                    ok = ok && proven_crypto_mp_is_zero(v, s) != 0 && proven_crypto_mp_lt(neg, m.n, s) != 0 && proven_crypto_mp_lt(m.n, neg, s) == 0;
                    /* x: bytes of all ones with the top one cleared - below n, every low limb full. */
                    memset(xb, 0xff, len); xb[0] = len == 1 ? 0x03 : 0x7f;
                    proven_crypto_mp_reduce_be(&m, x, xb, len);
                    proven_crypto_mp_to_mont(&m, u, neg); proven_crypto_mp_to_mont(&m, v, x);
                    /* (n-1)(n-1) = 1 */
                    proven_crypto_mp_montmul(&m, y, u, u); proven_crypto_mp_from_mont(&m, y, y);
                    ok = ok && proven_crypto_mp_eq(y, one, s) != 0;
                    /* (n-1) x = -x, so adding x gives zero */
                    proven_crypto_mp_montmul(&m, y, u, v); proven_crypto_mp_from_mont(&m, y, y);
                    proven_crypto_mp_add(&m, y, y, x);
                    ok = ok && proven_crypto_mp_is_zero(y, s) != 0;
                    /* x (x + (n-1)) = x x - x, and the product does not depend on the order */
                    proven_crypto_mp_add(&m, y, v, u);
                    proven_crypto_mp_montmul(&m, y, v, y);
                    proven_crypto_mp_montmul(&m, a, v, v); proven_crypto_mp_sub(&m, a, a, v);
                    ok = ok && proven_crypto_mp_eq(y, a, s) != 0;
                    proven_crypto_mp_montmul(&m, y, u, v); proven_crypto_mp_montmul(&m, a, v, u);
                    ok = ok && proven_crypto_mp_eq(y, a, s) != 0;
                    /* into and out of Montgomery form, and x^3 two ways */
                    proven_crypto_mp_from_mont(&m, y, v);
                    ok = ok && proven_crypto_mp_eq(y, x, s) != 0;
                    for (proven_size_t i = 0; i < s; ++i) e[i] = 0;
                    e[0] = 3;
                    proven_crypto_mp_pow(&m, y, v, e, s);
                    proven_crypto_mp_montmul(&m, a, v, v); proven_crypto_mp_montmul(&m, a, a, v);
                    ok = ok && proven_crypto_mp_eq(y, a, s) != 0;
                    /* the plain product against the modular one: x * x reduced is x x */
                    if (2 * len <= sizeof g_out) {
                        proven_crypto_mp_mul(pr, x, s, x, s);
                        proven_crypto_mp_store_be(g_out, 2 * len, pr, 2 * s);
                        bool whole = true;                                         /* nothing of it above 2 len bytes */
                        for (proven_size_t i = PROVEN_CRYPTO_MP_LIMBS(2 * len); i < 2 * s; ++i) whole = whole && pr[i] == 0;
                        proven_crypto_mp_reduce_be(&m, y, g_out, 2 * len);
                        proven_crypto_mp_montmul(&m, a, v, v); proven_crypto_mp_from_mont(&m, a, a);
                        ok = ok && whole && proven_crypto_mp_eq(y, a, s) != 0;
                    }
                    /* a small divisor: the remainder the long way, bit by bit with 65521 as a modulus (not
                     * 65537, modulo which 2^32 is 1 and the two halves of a limb could change places unseen) */
                    static const proven_byte_t f4[] = { 0xff, 0xf1 };
                    proven_crypto_mp_store_be(xb, len, x, s);
                    ok = ok && proven_crypto_mp_mod_init(&m2, f4, sizeof f4);
                    proven_crypto_mp_reduce_be(&m2, y, xb, len);
                    ok = ok && proven_crypto_mp_mod_small(x, s, 65521) == (proven_u32)y[0];
                    if (ok) held++;
                }
            }
            PROVEN_TEST_ASSERT(held == tried && tried >= 90, "what the arithmetic must satisfy by itself holds at moduli and operands of all ones, from one byte to the largest", "");
        }
        /* (1 + k a) / e, where it is exact: a = e q - 1 and k = 1 give q back. */
        {
            proven_crypto_limb_t q[5] = { (proven_crypto_limb_t)0 - 1u, (proven_crypto_limb_t)0 - 7u, 0x1234567u, (proven_crypto_limb_t)0 - 1u, 0 }, prod[6], got[7], ev[1] = { 65537 };
            proven_crypto_mp_mul(prod, q, 5, ev, 1);
            bool exact = prod[0] != 0;
            prod[0] -= 1;                                                         /* e q - 1: the low limb of e q is not zero */
            proven_crypto_mp_inverse_from(got, prod, 6, 1, 65537);
            for (int i = 0; i < 7; ++i) exact = exact && got[i] == (i < 5 ? q[i] : 0);
            exact = exact && proven_crypto_mp_mod_small(prod, 6, 65537) == 65536;
            PROVEN_TEST_ASSERT(exact, "the exact division by a small number gives the quotient it was made from", "");
        }
        /* One byte more than a limb holds, and the same value in two limbs, read back as bytes. */
        proven_byte_t wide[PROVEN_CRYPTO_MP_LIMB_BYTES + 1] = { 0x01 }, back[2 * PROVEN_CRYPTO_MP_LIMB_BYTES + 3];
        bool fits = !proven_crypto_mp_load_be(a, 1, wide, sizeof wide) && proven_crypto_mp_load_be(a, 2, wide, sizeof wide) && a[1] == 1 && a[0] == 0;
        proven_crypto_mp_store_be(back, sizeof back, a, 2);
        for (proven_size_t i = 0; i < sizeof back; ++i) fits = fits && back[i] == (i == sizeof back - sizeof wide ? 1 : 0);
        wide[0] = 0;                                       /* a leading zero byte is not a value that does not fit */
        fits = fits && proven_crypto_mp_load_be(a, 1, wide, sizeof wide) && a[0] == 0;
        PROVEN_TEST_ASSERT(fits && proven_crypto_mp_is_zero(b, 0) != 0,
            "a value that does not fit the limbs is refused by the loader", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("P-256 and P-384",
        "Public keys, ECDH, deterministic ECDSA (RFC 6979's own example first) and verification; points off the curve and scalars out of range are refused.",
        "Check src/proven/crypto_ec.c.");
    {
        proven_size_t good = 0;
        for (proven_size_t i = 0; i < COUNT(CRV_EC); ++i) {
            split(CRV_EC[i]);
            proven_crypto_ec_curve_t c = strcmp(g_field[1], "p256") == 0 ? PROVEN_CRYPTO_EC_P256 : PROVEN_CRYPTO_EC_P384;
            proven_size_t sz = proven_crypto_ec_size(c);
            bool ok = true;
            if (strcmp(g_field[0], "dh") == 0) {
                /* dh curve private public peer shared */
                ok = ok && proven_crypto_ec_public(c, g_bytes[2], g_out) && memcmp(g_out, g_bytes[3], 1 + 2 * sz) == 0;
                ok = ok && proven_crypto_ecdh(c, g_bytes[2], V(4), g_out) && memcmp(g_out, g_bytes[5], sz) == 0;
                g_bytes[4][7] ^= 1;
                ok = ok && !proven_crypto_ecdh(c, g_bytes[2], V(4), g_out);
                g_bytes[4][7] ^= 1; g_bytes[4][0] = 0x02;
                ok = ok && !proven_crypto_ecdh(c, g_bytes[2], V(4), g_out);
            } else {
                /* sig curve hash private public digest r|s */
                int hash = atoi(g_field[2]);
                proven_mem_view_t pub = V(4), dig = V(5), r = { g_bytes[6], sz }, s = { g_bytes[6] + sz, sz };
                ok = ok && proven_crypto_ecdsa_sign(c, hash, g_bytes[3], dig, g_out) && memcmp(g_out, g_bytes[6], 2 * sz) == 0;
                ok = ok && proven_crypto_ecdsa_verify(c, pub, dig, r, s);
                /* The DER form, built here. */
                proven_byte_t der[2 + 2 * (3 + 48)];
                proven_size_t at = 2;
                for (int part = 0; part < 2; ++part) {
                    const proven_byte_t *v = g_bytes[6] + (proven_size_t)part * sz;
                    proven_size_t n = sz;
                    while (n > 1 && v[0] == 0) { v++; n--; }
                    der[at++] = 0x02; der[at++] = (proven_byte_t)(n + (v[0] >> 7));
                    if (v[0] & 0x80) der[at++] = 0;
                    memcpy(der + at, v, n); at += n;
                }
                der[0] = 0x30; der[1] = (proven_byte_t)(at - 2);
                ok = ok && proven_crypto_ecdsa_verify_der(c, pub, dig, (proven_mem_view_t){ der, at });
                ok = ok && !proven_crypto_ecdsa_verify_der(c, pub, dig, (proven_mem_view_t){ der, at + 1 }) &&
                     !proven_crypto_ecdsa_verify_der(c, pub, dig, (proven_mem_view_t){ der, at - 1 });
                g_bytes[5][0] ^= 1;
                ok = ok && !proven_crypto_ecdsa_verify(c, pub, dig, r, s);
                g_bytes[5][0] ^= 1; g_bytes[6][sz + 2] ^= 4;
                ok = ok && !proven_crypto_ecdsa_verify(c, pub, dig, r, s);
            }
            if (ok) good++;
        }
        PROVEN_TEST_ASSERT(good == COUNT(CRV_EC) && good >= 48, "every case on both curves, over SHA-256, SHA-384 and SHA-512", "");

        proven_byte_t zero[48] = { 0 }, order[32], pub[97], sh[48];
        static const char n_hex[] = "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551";
        for (int i = 0; i < 32; ++i) { char c[3] = { n_hex[2 * i], n_hex[2 * i + 1], 0 }; order[i] = (proven_byte_t)strtoul(c, NULL, 16); }
        PROVEN_TEST_ASSERT(!proven_crypto_ec_public(PROVEN_CRYPTO_EC_P256, zero, pub) && !proven_crypto_ec_public(PROVEN_CRYPTO_EC_P256, order, pub) &&
                           !proven_crypto_ec_public((proven_crypto_ec_curve_t)9, order, pub) && proven_crypto_ec_size((proven_crypto_ec_curve_t)9) == 0,
            "a private scalar of 0, or equal to the group order, is not a key; an unknown curve has no size", "");
        order[31]--;
        PROVEN_TEST_ASSERT(proven_crypto_ec_public(PROVEN_CRYPTO_EC_P256, order, pub) && proven_crypto_ecdh(PROVEN_CRYPTO_EC_P256, order, (proven_mem_view_t){ pub, 65 }, sh),
            "the largest scalar, n - 1, is one", "");
        proven_byte_t r0[32] = { 0 }, digest[32] = { 1 };
        PROVEN_TEST_ASSERT(!proven_crypto_ecdsa_verify(PROVEN_CRYPTO_EC_P256, (proven_mem_view_t){ pub, 65 }, (proven_mem_view_t){ digest, 32 }, (proven_mem_view_t){ r0, 32 }, (proven_mem_view_t){ order, 32 }) &&
                           !proven_crypto_ecdsa_verify(PROVEN_CRYPTO_EC_P256, (proven_mem_view_t){ pub, 65 }, (proven_mem_view_t){ digest, 32 }, (proven_mem_view_t){ order, 32 }, (proven_mem_view_t){ r0, 32 }),
            "a signature with r or s of zero is refused", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("X25519 and Ed25519",
        "RFC 7748's and RFC 8032's own examples first, then more; a small-order point and every changed part of a signature are refused.",
        "Check src/proven/crypto_25519.c.");
    {
        proven_size_t good = 0;
        for (proven_size_t i = 0; i < COUNT(CRV_X25519); ++i) {
            split(CRV_X25519[i]);
            bool ok = true;
            proven_byte_t o[64];
            if (g_field[0][0] == 'x') {
                /* x scalar public peer shared */
                proven_crypto_x25519_public(o, g_bytes[1]);
                ok = ok && memcmp(o, g_bytes[2], 32) == 0;
                ok = ok && proven_crypto_x25519(o, g_bytes[1], g_bytes[3]) && memcmp(o, g_bytes[4], 32) == 0;
                proven_byte_t small[32] = { 0 };
                ok = ok && !proven_crypto_x25519(o, g_bytes[1], small);
                small[0] = 1;
                ok = ok && !proven_crypto_x25519(o, g_bytes[1], small);
            } else {
                /* e seed public message signature */
                proven_crypto_ed25519_public(o, g_bytes[1]);
                ok = ok && memcmp(o, g_bytes[2], 32) == 0;
                proven_crypto_ed25519_sign(o, g_bytes[1], g_bytes[2], V(3));
                ok = ok && memcmp(o, g_bytes[4], 64) == 0 && proven_crypto_ed25519_verify(g_bytes[2], V(3), g_bytes[4]);
                g_bytes[4][40] ^= 1; ok = ok && !proven_crypto_ed25519_verify(g_bytes[2], V(3), g_bytes[4]); g_bytes[4][40] ^= 1;
                g_bytes[4][3] ^= 1; ok = ok && !proven_crypto_ed25519_verify(g_bytes[2], V(3), g_bytes[4]); g_bytes[4][3] ^= 1;
                g_bytes[2][9] ^= 1; ok = ok && !proven_crypto_ed25519_verify(g_bytes[2], V(3), g_bytes[4]); g_bytes[2][9] ^= 1;
                g_bytes[3][g_len[3]] = 0x55;
                ok = ok && !proven_crypto_ed25519_verify(g_bytes[2], (proven_mem_view_t){ g_bytes[3], g_len[3] + 1 }, g_bytes[4]);
                /* S + L is the same signature to arithmetic and must not be one here. */
                static const proven_byte_t l_le[32] = { 0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                                                        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10 };
                unsigned carry = 0;
                proven_byte_t twin[64];
                memcpy(twin, g_bytes[4], 64);
                for (int k = 0; k < 32; ++k) { unsigned v = (unsigned)twin[32 + k] + l_le[k] + carry; twin[32 + k] = (proven_byte_t)v; carry = v >> 8; }
                ok = ok && !proven_crypto_ed25519_verify(g_bytes[2], V(3), twin);
            }
            if (ok) good++;
        }
        PROVEN_TEST_ASSERT(good == COUNT(CRV_X25519) && good >= 42, "every case: keys, shared secrets, signatures, and the refusals", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("RSA signature verification",
        "PKCS #1 v1.5 and PSS over three hashes and several modulus sizes; a key below 2048 bits, another hash, another salt length and any changed bit are refused.",
        "Check src/proven/crypto_rsa.c.");
    {
        proven_size_t good = 0;
        for (proven_size_t i = 0; i < COUNT(CRV_RSA); ++i) {
            /* kind bits hash salt n e digest signature */
            split(CRV_RSA[i]);
            int bits = atoi(g_field[1]), hash = atoi(g_field[2]);
            proven_size_t salt = (proven_size_t)atoi(g_field[3]);
            bool pss = g_field[0][0] == 's', want = bits >= 2048, ok = true;
            proven_mem_view_t n = V(4), e = V(5), d = V(6), s = V(7);
            bool got = pss ? proven_crypto_rsa_verify_pss(n, e, hash, salt, d, s) : proven_crypto_rsa_verify_pkcs1(n, e, hash, d, s);
            ok = ok && got == want;
            if (want) {
                g_bytes[6][1] ^= 1;
                ok = ok && !(pss ? proven_crypto_rsa_verify_pss(n, e, hash, salt, d, s) : proven_crypto_rsa_verify_pkcs1(n, e, hash, d, s));
                g_bytes[6][1] ^= 1; g_bytes[7][g_len[7] - 1] ^= 1;
                ok = ok && !(pss ? proven_crypto_rsa_verify_pss(n, e, hash, salt, d, s) : proven_crypto_rsa_verify_pkcs1(n, e, hash, d, s));
                g_bytes[7][g_len[7] - 1] ^= 1;
                if (pss) ok = ok && !proven_crypto_rsa_verify_pss(n, e, hash, salt + 1, d, s) && !proven_crypto_rsa_verify_pkcs1(n, e, hash, d, s);
                else ok = ok && !proven_crypto_rsa_verify_pss(n, e, hash, 32, d, s);
                /* A signature one byte short, and one equal to the modulus. */
                ok = ok && !(pss ? proven_crypto_rsa_verify_pss(n, e, hash, salt, d, (proven_mem_view_t){ g_bytes[7], g_len[7] - 1 })
                                 : proven_crypto_rsa_verify_pkcs1(n, e, hash, d, (proven_mem_view_t){ g_bytes[7], g_len[7] - 1 }));
                ok = ok && !(pss ? proven_crypto_rsa_verify_pss(n, e, hash, salt, d, n) : proven_crypto_rsa_verify_pkcs1(n, e, hash, d, n));
            }
            if (ok) good++;
        }
        PROVEN_TEST_ASSERT(good == COUNT(CRV_RSA) && good >= 60, "every case", "");
    }

    PROVEN_TEST_PASS("the primitives agree with another implementation and refuse what they must.");
    return 0;
}
