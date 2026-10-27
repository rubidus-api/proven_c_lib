#include "proven.h"
#include "proven_test.h"
#include "../src/proven/proven_internal_crypto.h"
#include "test_unit_crypto_vectors.h"
#include "../platform/proven_sys_aes.h"
#include "../platform/proven_sys_chacha.h"
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

/* ChaCha20-Poly1305, ChaCha20 alone and Poly1305 alone over every case, on whichever ChaCha20
 * path is in force: each case seals to the expected bytes, opens again, and refuses a forgery
 * leaving zeros. Returns the number of cases that failed. */
static proven_size_t run_chacha(void) {
    proven_size_t bad = 0;
    for (proven_size_t i = 0; i < COUNT(CRV_CHACHA); ++i) {
        split(CRV_CHACHA[i]);
        proven_byte_t tag[16];
        proven_crypto_chacha20poly1305_seal(g_bytes[0], g_bytes[1], V(2), V(3), g_out, tag);
        bool ok = memcmp(g_out, g_bytes[4], g_len[3]) == 0 && memcmp(tag, g_bytes[5], 16) == 0;
        ok = ok && proven_crypto_chacha20poly1305_open(g_bytes[0], g_bytes[1], V(2), V(4), g_bytes[5], g_out2) && memcmp(g_out2, g_bytes[3], g_len[3]) == 0;
        /* In place. */
        memcpy(g_out2, g_bytes[3], g_len[3]);
        proven_crypto_chacha20poly1305_seal(g_bytes[0], g_bytes[1], V(2), (proven_mem_view_t){ g_out2, g_len[3] }, g_out2, tag);
        ok = ok && memcmp(g_out2, g_bytes[4], g_len[3]) == 0;
        tag[0] ^= 1;
        memset(g_out2, 0xee, sizeof g_out2);
        ok = ok && !proven_crypto_chacha20poly1305_open(g_bytes[0], g_bytes[1], V(2), V(4), tag, g_out2);
        for (proven_size_t k = 0; k < g_len[4]; ++k) ok = ok && g_out2[k] == 0;
        if (g_len[2] > 0) { g_bytes[2][g_len[2] - 1] ^= 0x80; ok = ok && !proven_crypto_chacha20poly1305_open(g_bytes[0], g_bytes[1], V(2), V(4), g_bytes[5], g_out2); }
        if (!ok) bad++;
    }
    for (proven_size_t i = 0; i < COUNT(CRV_CHACHA20); ++i) {
        split(CRV_CHACHA20[i]);
        proven_u32 counter = (proven_u32)strtoul(g_field[1], NULL, 16);
        proven_crypto_chacha20(g_bytes[0], counter, g_bytes[2], g_bytes[3], g_out, g_len[3]);
        if (memcmp(g_out, g_bytes[4], g_len[4]) != 0) bad++;
    }
    for (proven_size_t i = 0; i < COUNT(CRV_POLY1305); ++i) {
        split(CRV_POLY1305[i]);
        proven_byte_t tag[16];
        proven_crypto_poly1305_t st;
        proven_crypto_poly1305_init(&st, g_bytes[0]);
        /* In uneven pieces, so that the buffered partial block is crossed too. */
        for (proven_size_t at = 0, step = 1; at < g_len[1]; at += step, step = step * 3 % 37 + 1)
            proven_crypto_poly1305_update(&st, g_bytes[1] + at, step < g_len[1] - at ? step : g_len[1] - at);
        proven_crypto_poly1305_final(&st, tag);
        if (memcmp(tag, g_bytes[2], 16) != 0) bad++;
        /* And whole, so that long runs of blocks take the vector path where there is one; then
         * one block first, so that they start from a state that is not zero. */
        proven_crypto_poly1305_init(&st, g_bytes[0]);
        proven_crypto_poly1305_update(&st, g_bytes[1], g_len[1]);
        proven_crypto_poly1305_final(&st, tag);
        if (memcmp(tag, g_bytes[2], 16) != 0) bad++;
        if (g_len[1] > 17) {
            proven_crypto_poly1305_init(&st, g_bytes[0]);
            proven_crypto_poly1305_update(&st, g_bytes[1], 16);
            proven_crypto_poly1305_update(&st, g_bytes[1] + 16, g_len[1] - 16);
            proven_crypto_poly1305_final(&st, tag);
            if (memcmp(tag, g_bytes[2], 16) != 0) bad++;
        }
    }
    return bad;
}

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
        "RFC 8439's example and lengths on each side of every block boundary and of the vector path's groups, on the portable code and each vector width; a changed tag, text or header is refused.",
        "Check src/proven/crypto_chacha.c (portable, Poly1305) and platform/proven_sys_chacha.c (vector).");
    {
        PROVEN_TEST_ASSERT(COUNT(CRV_CHACHA) >= 31 && COUNT(CRV_CHACHA20) >= 10 && COUNT(CRV_POLY1305) >= 25, "the vectors are all there", "");
        PROVEN_TEST_ASSERT(proven_crypto_chacha_force_width(0) == 0, "the test hook forces the portable code", "");
        PROVEN_TEST_ASSERT(run_chacha() == 0, "the portable code: every case of the AEAD, ChaCha20 alone (counters up to 2^32) and Poly1305 alone (RFC 8439 A.3)", "");
        int widest = proven_sys_chacha_width();
        for (int w = 4; w <= widest; w *= 2) {
            PROVEN_TEST_ASSERT(proven_crypto_chacha_force_width(w) == w, "the hook selects the vector width", "");
            proven_size_t bad = run_chacha();
            char what[96];
            snprintf(what, sizeof what, "%d blocks a call: every case (%zu failed)", w, (size_t)bad);
            PROVEN_TEST_ASSERT(bad == 0, what, "platform/proven_sys_chacha.c");
            /* Against the portable code on every length to 1,100 bytes, with counters that wrap past 2^32. */
            static proven_byte_t key[32], nonce[12], text[1100], a[1100], b[1100];
            for (int k = 0; k < 32; ++k) key[k] = (proven_byte_t)(k * 7 + w);
            for (int k = 0; k < 12; ++k) nonce[k] = (proven_byte_t)(k * 13 + 1);
            for (int k = 0; k < 1100; ++k) text[k] = (proven_byte_t)(k * 31 + 5);
            proven_size_t differ = 0;
            for (proven_size_t len = 0; len <= sizeof text; len += len < 600 ? 1 : 37) {
                proven_u32 counter = (proven_u32)(0xfffffff0u + len % 23);
                proven_crypto_chacha_force_width(w);
                proven_crypto_chacha20(key, counter, nonce, text, a, len);
                proven_crypto_chacha_force_width(0);
                proven_crypto_chacha20(key, counter, nonce, text, b, len);
                if (memcmp(a, b, len) != 0) differ++;
            }
            PROVEN_TEST_ASSERT(differ == 0, "the vector path and the portable code give the same keystream at every length", "");
        }
        if (widest == 0) PROVEN_TEST_INFO("no vector path on this processor or build; the portable code alone was checked");
        PROVEN_TEST_ASSERT(proven_crypto_chacha_force_width(-1) == widest, "the hook restores the processor's widest", "");
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

            /* The key schedule itself (RFC-0025 step 3: AESKEYGENASSIST where the processor has
             * it): the round keys, byte for byte, against the portable schedule, for 200 keys of
             * each length. */
            bool keys_same = true;
            for (int round = 0; round < 400; ++round) {
                const proven_size_t klen = round % 2 ? 32 : 16;
                for (proven_size_t i = 0; i < sizeof key; ++i) key[i] = (proven_byte_t)(i * 97 + round * 211 + (round >> 3));
                PROVEN_TEST_ASSERT(proven_crypto_aes_gcm_init(&hw, key, klen), "init", "");
                proven_crypto_aes_force_portable(true);
                PROVEN_TEST_ASSERT(proven_crypto_aes_gcm_init(&sw, key, klen), "init", "");
                proven_crypto_aes_force_portable(false);
                keys_same = keys_same && hw.rounds == sw.rounds && memcmp(hw.rk, sw.rk, (proven_size_t)(16 * (hw.rounds + 1))) == 0 && memcmp(hw.h, sw.h, 16) == 0;
            }
            PROVEN_TEST_ASSERT(keys_same, "the processor's key schedule makes the portable one's round keys for 200 keys of 16 bytes and 200 of 32",
                "Inspect proven_sys_aes_expand in platform/proven_sys_aes.c: the round constants, and AES-256's odd round keys.");

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

            /* The one-pass code (RFC-0023): x86 seals and opens with the counter mode and GHASH
             * in the same loop, the powers of H kept with the key. Every length to 2,048 bytes
             * (each count of groups and each tail), associated data of every length to 64 (QUIC
             * long headers), in place and not, against the portable code. */
#if (defined(__x86_64__) || defined(__i386__)) && !defined(PROVEN_FREESTANDING) && !defined(PROVEN_SYS_AES_DISABLE)
            PROVEN_TEST_ASSERT(hw.one_pass && !sw16.one_pass, "on x86 a hardware context seals and opens in one pass; a portable one does not", "");
#endif
            static proven_byte_t msg[2048 + 64], p1[2048 + 64], p2[2048 + 64];
            for (proven_size_t i = 0; i < sizeof msg; ++i) msg[i] = (proven_byte_t)(i * 199 + i / 13 + 5);
            proven_size_t wrong = 0, cleared = 0, opened = 0;
            for (proven_size_t n = 0; n <= 2048 + 65 * 11; ++n) {
                /* The first 2,049 rounds walk the text's length; the rest hold a few lengths and walk the associated data's. */
                static const proven_size_t few[11] = { 0, 1, 15, 16, 17, 127, 128, 129, 255, 256, 1425 };
                const proven_size_t len = n <= 2048 ? n : few[(n - 2049) % 11];
                const proven_size_t alen = n <= 2048 ? (n * 11) % 65 : (n - 2049) / 11;
                const proven_size_t klen = n % 2 ? 32 : 16, at = n % 7;
                for (proven_size_t i = 0; i < sizeof key; ++i) key[i] = (proven_byte_t)(i * 13 + n);
                for (proven_size_t i = 0; i < sizeof nonce; ++i) nonce[i] = (proven_byte_t)(i * 7 + n / 3);
                (void)proven_crypto_aes_gcm_init(&hw, key, klen);
                proven_crypto_aes_force_portable(true);
                (void)proven_crypto_aes_gcm_init(&sw, key, klen);
                proven_crypto_aes_force_portable(false);
                const proven_mem_view_t ad = { msg + 2048 - alen, alen }, plain = { msg + at, len };
                proven_crypto_aes_gcm_seal(&sw, nonce, ad, plain, p2, tag_sw);
                proven_crypto_aes_gcm_seal(&hw, nonce, ad, plain, p1, tag_hw);
                bool row = memcmp(p1, p2, len) == 0 && memcmp(tag_hw, tag_sw, 16) == 0;
                /* In place: sealed over the text, then opened over the ciphertext. */
                memcpy(p1 + at, msg + at, len);
                proven_crypto_aes_gcm_seal(&hw, nonce, ad, (proven_mem_view_t){ p1 + at, len }, p1 + at, tag_hw);
                row = row && memcmp(p1 + at, p2, len) == 0 && memcmp(tag_hw, tag_sw, 16) == 0;
                row = row && proven_crypto_aes_gcm_open(&hw, nonce, ad, (proven_mem_view_t){ p1 + at, len }, tag_sw, p1 + at) && memcmp(p1 + at, msg + at, len) == 0;
                /* Out of place, with the portable side opening what the hardware sealed. */
                row = row && proven_crypto_aes_gcm_open(&sw, nonce, ad, (proven_mem_view_t){ p2, len }, tag_hw, p1) && memcmp(p1, msg + at, len) == 0;
                if (!row) { if (wrong == 0) PROVEN_TEST_INFO("first difference at length {} (associated data {})", PROVEN_ARG((int)len), PROVEN_ARG((int)alen)); wrong++; }
                /* A wrong tag, or a changed byte: false, and nothing of the plaintext left where it was asked for. */
                tag_hw[n % 16] ^= (proven_byte_t)(1u << (n % 8));
                memcpy(p1, p2, len);
                memset(p1 + len, 0xa5, 16);
                const bool bad_tag = proven_crypto_aes_gcm_open(&hw, nonce, ad, (proven_mem_view_t){ p1, len }, tag_hw, p1);
                bool zero = true;
                for (proven_size_t i = 0; i < len; ++i) zero = zero && p1[i] == 0;
                if (!bad_tag && zero && p1[len] == 0xa5) cleared++;
                tag_hw[n % 16] ^= (proven_byte_t)(1u << (n % 8));
                if (len > 0) {
                    p2[n % len] ^= 0x10;
                    if (!proven_crypto_aes_gcm_open(&hw, nonce, ad, (proven_mem_view_t){ p2, len }, tag_hw, p1)) opened++;
                } else opened++;
            }
            PROVEN_TEST_ASSERT(wrong == 0, "one pass: every length from 0 to 2,048 bytes and associated data of 0 to 64, sealed, sealed in place, opened in place, and opened by the portable code, as the portable code does",
                "Inspect proven_sys_aes_gcm in platform/proven_sys_aes.c: the stitched loop, the group it hashes, what is left after it, and the lengths block.");
            PROVEN_TEST_ASSERT(cleared == 2049 + 65 * 11, "one pass: a wrong tag is refused, and the plaintext written during the pass is cleared - every byte asked for and none beyond", "");
            PROVEN_TEST_ASSERT(opened == 2049 + 65 * 11, "one pass: a changed byte of ciphertext anywhere is refused", "");
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
        "Multiplication, squaring, addition, subtraction, exponentiation, inversion and reduction modulo primes and odd composites of many sizes.",
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
            /* The squaring against the product with itself, and in place. */
            static proven_crypto_limb_t sq[PROVEN_CRYPTO_MP_MAX], pq[PROVEN_CRYPTO_MP_MAX];
            for (int k = 0; k < 2; ++k) {
                const proven_crypto_limb_t *xm = k ? bm : am;
                proven_crypto_mp_montsqr(&m, sq, xm); proven_crypto_mp_montmul(&m, pq, xm, xm);
                ok = ok && proven_crypto_mp_eq(sq, pq, s) != 0;
                for (proven_size_t j = 0; j < s; ++j) sq[j] = xm[j];
                proven_crypto_mp_montsqr(&m, sq, sq);
                ok = ok && proven_crypto_mp_eq(sq, pq, s) != 0;
            }
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
                    /* (n-1)(n-1) = 1, by product and by squaring */
                    proven_crypto_mp_montmul(&m, y, u, u); proven_crypto_mp_from_mont(&m, y, y);
                    ok = ok && proven_crypto_mp_eq(y, one, s) != 0;
                    proven_crypto_mp_montsqr(&m, y, u); proven_crypto_mp_from_mont(&m, y, y);
                    ok = ok && proven_crypto_mp_eq(y, one, s) != 0;
                    /* x x, every low limb full: squaring and product agree */
                    proven_crypto_mp_montsqr(&m, y, v); proven_crypto_mp_montmul(&m, a, v, v);
                    ok = ok && proven_crypto_mp_eq(y, a, s) != 0;
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
                    /* a small divisor: the remainder by the general reduction, with 65521 as a modulus (not
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

        /* The generator's table, and the comb that reads it: five teeth d = ceil(bits / 5)
         * apart, so the edges of the teeth are where a column changes meaning; an even scalar
         * takes another way (n - k, and the result negated) than an odd one. */
        PROVEN_TEST_ASSERT(proven_crypto_ec_test_generator_table(PROVEN_CRYPTO_EC_P256) && proven_crypto_ec_test_generator_table(PROVEN_CRYPTO_EC_P384),
            "every entry of each curve's table of the generator is what the general multiplication gives", "");
        {
            static const char *const orders[2] = { "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",
                                                   "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973" };
            proven_u64 x = 0x2545f4914f6cdd1dull;
            proven_size_t tried = 0, agreed = 0;
            for (int cv = 0; cv < 2; ++cv) {
                const proven_crypto_ec_curve_t curve = cv == 0 ? PROVEN_CRYPTO_EC_P256 : PROVEN_CRYPTO_EC_P384;
                const proven_size_t sz = proven_crypto_ec_size(curve), d = (sz * 8 + 4) / 5;
                proven_byte_t n[48], k[48];
                for (proven_size_t i = 0; i < sz; ++i) { char h[3] = { orders[cv][2 * i], orders[cv][2 * i + 1], 0 }; n[i] = (proven_byte_t)strtoul(h, NULL, 16); }
                for (int which = 0; which < 60; ++which) {
                    memset(k, 0, sz);
                    if (which == 1) k[sz - 1] = 1;
                    else if (which == 2) k[sz - 1] = 15;
                    else if (which == 3) k[sz - 1] = 16;
                    else if (which == 4) { memcpy(k, n, sz); k[sz - 1] -= 1; }                 /* n - 1 */
                    else if (which == 5) { memcpy(k, n, sz); k[sz - 1] -= 2; }                 /* n - 2 */
                    else if (which >= 6 && which < 21) {
                        /* 2^(t d), 2^(t d) - 1 and 2^(t d + d - 1), for each tooth t - the last
                         * tooth's past the curve's bits where it is cut short */
                        const proven_size_t t = (proven_size_t)(which - 6) / 3, kind = (proven_size_t)(which - 6) % 3;
                        proven_size_t bit = kind == 2 ? t * d + d - 1 : t * d;
                        if (bit >= sz * 8 - 1) bit = sz * 8 - 2;                                /* below the order */
                        if (kind == 1) { for (proven_size_t b = 0; b < bit; ++b) k[sz - 1 - b / 8] |= (proven_byte_t)(1u << (b % 8)); }
                        else k[sz - 1 - bit / 8] |= (proven_byte_t)(1u << (bit % 8));
                    } else if (which >= 21) {
                        for (proven_size_t i = 0; i < sz; ++i) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; k[i] = (proven_byte_t)(x >> 32); }
                        k[0] &= 0x7f;                                                           /* below the order */
                        if (which % 7 == 0) memset(k + sz / 4, 0, sz / 4);                      /* a run of zero columns in one tooth */
                    }
                    tried++;
                    if (proven_crypto_ec_test_base_mul(curve, k)) agreed++;
                }
            }
            PROVEN_TEST_ASSERT(tried == 120 && agreed == tried,
                "k G by the table agrees with the general walk for k of 0, 1, 15, 16, n - 1, n - 2, the edges of every tooth and thirty-nine random scalars, on both curves", "");
        }

        /* Verification's walk is variable time, with formulas that are wrong for P + P,
         * P + (-P) and the point at infinity and branch to them instead. The branches first,
         * then the whole of k1 G + k2 Q against the constant-time code - with Q = G and the
         * scalars chosen so the walk meets those cases: k1 = k2 (each step of G's part can meet
         * Q's), k1 = n - k2 (the sum is the point at infinity), and either one zero. */
        {
            static const char *const orders[2] = { "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",
                                                   "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973" };
            proven_u64 x = 0x9e3779b97f4a7c15ull;
            proven_size_t tried = 0, agreed = 0, add_tried = 0, add_agreed = 0;
            for (int cv = 0; cv < 2; ++cv) {
                const proven_crypto_ec_curve_t curve = cv == 0 ? PROVEN_CRYPTO_EC_P256 : PROVEN_CRYPTO_EC_P384;
                const proven_size_t sz = proven_crypto_ec_size(curve);
                proven_byte_t n[48], k1[48], k2[48], one[48], g[97], q[97], ng[97];
                for (proven_size_t i = 0; i < sz; ++i) { char h[3] = { orders[cv][2 * i], orders[cv][2 * i + 1], 0 }; n[i] = (proven_byte_t)strtoul(h, NULL, 16); }
                memset(one, 0, sz); one[sz - 1] = 1;
                PROVEN_TEST_ASSERT(proven_crypto_ec_public(curve, one, g), "the generator, encoded", "");
                memcpy(one, n, sz); one[sz - 1] -= 1;
                PROVEN_TEST_ASSERT(proven_crypto_ec_public(curve, one, ng), "minus the generator, encoded", "");
                for (int which = 0; which < 40; ++which) {
                    for (proven_size_t i = 0; i < sz; ++i) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; k1[i] = (proven_byte_t)(x >> 24); }
                    k1[0] &= 0x7f;
                    if (which < 8) {
                        add_tried++;
                        if (which == 0) { memset(k1, 0, sz); k1[sz - 1] = 1; }
                        if (which == 1) { memcpy(k1, n, sz); k1[sz - 1] -= 1; }
                        if (proven_crypto_ec_test_vartime_add(curve, k1)) add_agreed++;
                    }
                    for (proven_size_t i = 0; i < sz; ++i) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; k2[i] = (proven_byte_t)(x >> 24); }
                    k2[0] &= 0x7f;
                    memcpy(q, g, 1 + 2 * sz);
                    if (which < 10) {
                        /* the generator's own cases */
                        if (which % 5 == 0) memcpy(k2, k1, sz);                                 /* k G + k G */
                        else if (which % 5 == 1) {                                              /* k G + (n - k) G */
                            proven_byte_t b = 0;
                            for (proven_size_t i = sz; i-- > 0;) { int dv = n[i] - k1[i] - b; b = dv < 0; k2[i] = (proven_byte_t)dv; }
                        }
                        else if (which % 5 == 2) memset(k1, 0, sz);
                        else if (which % 5 == 3) memset(k2, 0, sz);
                        else { memset(k1, 0, sz); memset(k2, 0, sz); }
                    } else if (which < 14) {
                        memcpy(q, ng, 1 + 2 * sz);                                              /* Q = -G, k1 = k2: the sum is zero */
                        memcpy(k2, k1, sz);
                    } else if (which < 18) {
                        memset(one, 0, sz);
                        for (proven_size_t i = 0; i < sz; ++i) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; one[i] = (proven_byte_t)(x >> 24); }
                        one[0] &= 0x7f; one[sz - 1] |= 1;
                        PROVEN_TEST_ASSERT(proven_crypto_ec_public(curve, one, q), "a random point", "");
                        if (which == 14) { memset(k1, 0, sz); k1[sz - 1] = 1; memset(k2, 0, sz); k2[sz - 1] = 1; }
                        if (which == 15) { memcpy(k1, n, sz); k1[sz - 1] -= 1; memcpy(k2, k1, sz); }
                    } else if (which < 22) {
                        memset(one, 0, sz);
                        for (proven_size_t i = 0; i < sz; ++i) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; one[i] = (proven_byte_t)(x >> 24); }
                        one[0] &= 0x7f; one[sz - 1] |= 1;
                        PROVEN_TEST_ASSERT(proven_crypto_ec_public(curve, one, q), "a random point", "");
                        if (which % 2) memset(k1 + sz / 4, 0xff, sz / 2);                       /* long runs of ones in the NAF */
                        else memset(k2 + sz / 4, 0, sz / 2);                                    /* and of zeros */
                    }
                    tried++;
                    if (proven_crypto_ec_test_mul2(curve, k1, k2, (proven_mem_view_t){ q, 1 + 2 * sz })) agreed++;
                }
            }
            PROVEN_TEST_ASSERT(add_tried == 16 && add_agreed == add_tried,
                "the variable-time additions agree with the complete formulas on P + P, P + (-P), the point at infinity on either side and P + 2P, for P = G, -G and six random multiples, on both curves", "");
            PROVEN_TEST_ASSERT(tried == 80 && agreed == tried,
                "k1 G + k2 Q in variable time agrees with the constant-time code on both curves: Q = G with k1 = k2, with k1 = n - k2 and with either or both zero; Q = -G with k1 = k2; random Q with long runs of ones and zeros; and random scalars", "");
        }
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("P-256's own field against the general code",
        "Public keys, shared secrets, signatures and verifications on P-256 with the field code of P-256 alone and with the general one: the same bytes and verdicts, for scalars at the ends of the range and random ones.",
        "Check the p256_* functions in src/proven/crypto_ec.c (the general code is crypto_mp.c's).");
    {
        static const proven_byte_t n_minus_1[32] = { 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                                     0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17, 0x9e, 0x84, 0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x50 };
        proven_size_t same = 0, cases = 0;
        static proven_byte_t k[40][32];
        for (int i = 0; i < 40; ++i) {
            if (i == 0) { memset(k[i], 0, 32); k[i][31] = 1; }                        /* 1 */
            else if (i == 1) { memset(k[i], 0, 32); k[i][31] = 2; }                   /* 2 */
            else if (i == 2) memcpy(k[i], n_minus_1, 32);                             /* n - 1: -G */
            else if (i == 3) { memcpy(k[i], n_minus_1, 32); k[i][31] -= 1; }          /* n - 2 */
            else if (i == 4) { memset(k[i], 0, 32); k[i][0] = 0x7f; memset(k[i] + 1, 0xff, 31); }   /* below n, nearly all ones */
            else for (int j = 0; j < 32; ++j) k[i][j] = (proven_byte_t)(i * 37 + j * 101 + (i * j) % 251);
            if (i >= 5 && k[i][0] >= 0xff) k[i][0] = 0x7e;
        }
        static proven_byte_t pub_a[2][65], pub_b[2][65], sh[2][32], sig[2][64], digest[32];
        for (int i = 0; i < 40; ++i) {
            const int j = (i + 7) % 40;
            for (int j2 = 0; j2 < 32; ++j2) digest[j2] = (proven_byte_t)(i + j2 * 3);
            bool ok[2][5];
            for (int general = 0; general < 2; ++general) {
                proven_crypto_ec_force_general_(general != 0);
                ok[general][0] = proven_crypto_ec_public(PROVEN_CRYPTO_EC_P256, k[i], pub_a[general]);
                ok[general][1] = proven_crypto_ec_public(PROVEN_CRYPTO_EC_P256, k[j], pub_b[general]);
                ok[general][2] = proven_crypto_ecdh(PROVEN_CRYPTO_EC_P256, k[i], (proven_mem_view_t){ pub_b[general], 65 }, sh[general]);
                ok[general][3] = proven_crypto_ecdsa_sign(PROVEN_CRYPTO_EC_P256, PROVEN_HMAC_SHA256, k[i], (proven_mem_view_t){ digest, 32 }, sig[general]);
                ok[general][4] = proven_crypto_ecdsa_verify(PROVEN_CRYPTO_EC_P256, (proven_mem_view_t){ pub_a[general], 65 }, (proven_mem_view_t){ digest, 32 },
                                                            (proven_mem_view_t){ sig[general], 32 }, (proven_mem_view_t){ sig[general] + 32, 32 });
            }
            proven_crypto_ec_force_general_(false);
            cases++;
            bool row = memcmp(ok[0], ok[1], sizeof ok[0]) == 0 && ok[0][0] && ok[0][1] && ok[0][2] && ok[0][3] && ok[0][4];
            row = row && memcmp(pub_a[0], pub_a[1], 65) == 0 && memcmp(pub_b[0], pub_b[1], 65) == 0 && memcmp(sh[0], sh[1], 32) == 0 && memcmp(sig[0], sig[1], 64) == 0;
            if (row) same++;
        }
        PROVEN_TEST_ASSERT(cases == 40 && same == cases, "every public key, shared secret, signature and verification is the same from both", "");
        /* A point off the curve is refused by both. */
        static proven_byte_t bad[65];
        memcpy(bad, pub_a[0], 65); bad[64] ^= 1;
        bool refused = true;
        for (int general = 0; general < 2; ++general) {
            proven_crypto_ec_force_general_(general != 0);
            refused = refused && !proven_crypto_ecdh(PROVEN_CRYPTO_EC_P256, k[5], (proven_mem_view_t){ bad, 65 }, sh[0]);
        }
        proven_crypto_ec_force_general_(false);
        PROVEN_TEST_ASSERT(refused, "a point off the curve is refused by both", "");
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
        /* RFC 7748 section 5.2: k and u both the base point's encoding, k replaced by the result
         * and u by the old k, once and a thousand times. A thousand ladders over values nobody
         * chose pass through more of the field's carries than any list of cases. */
        {
            static const proven_byte_t once[32] = { 0x42,0x2c,0x8e,0x7a,0x62,0x27,0xd7,0xbc,0xa1,0x35,0x0b,0x3e,0x2b,0xb7,0x27,0x9f,
                                                    0x78,0x97,0xb8,0x7b,0xb6,0x85,0x4b,0x78,0x3c,0x60,0xe8,0x03,0x11,0xae,0x30,0x79 };
            static const proven_byte_t thousand[32] = { 0x68,0x4c,0xf5,0x9b,0xa8,0x33,0x09,0x55,0x28,0x00,0xef,0x56,0x6f,0x2f,0x4d,0x3c,
                                                        0x1c,0x38,0x87,0xc4,0x93,0x60,0xe3,0x87,0x5f,0x2e,0xb9,0x4d,0x99,0x53,0x2c,0x51 };
            proven_byte_t k[32] = { 9 }, u[32] = { 9 }, r[32];
            bool ok = true, first = false;
            for (int i = 0; i < 1000 && ok; ++i) {
                ok = proven_crypto_x25519(r, k, u);
                memcpy(u, k, 32); memcpy(k, r, 32);
                if (i == 0) first = memcmp(k, once, 32) == 0;
            }
            PROVEN_TEST_ASSERT(ok && first && memcmp(k, thousand, 32) == 0, "the iterated X25519 of RFC 7748, after one round and after a thousand", "");
        }
        /* What a coordinate's encoding may be: the 256th bit is not part of it, and a value of
         * p or more is read modulo p - so u and u + p give the same answer, to the byte. */
        {
            proven_byte_t k[32], u[32], v[32], a[32], b[32];
            bool ok = true;
            for (int i = 0; i < 32; ++i) k[i] = (proven_byte_t)(0xa5 ^ (i * 29));
            for (int small = 2; small < 19; ++small) {
                memset(u, 0, 32); u[0] = (proven_byte_t)small;
                memset(v, 0xff, 32); v[0] = (proven_byte_t)(0xed + small); v[31] = 0x7f;      /* p + small, below 2^255 */
                bool ra = proven_crypto_x25519(a, k, u), rb = proven_crypto_x25519(b, k, v);
                ok = ok && ra == rb && memcmp(a, b, 32) == 0;
                u[31] |= 0x80;
                rb = proven_crypto_x25519(b, k, u);
                ok = ok && ra == rb && memcmp(a, b, 32) == 0;
            }
            memset(v, 0xff, 32); v[0] = 0xed; v[31] = 0x7f;                                   /* p itself is zero */
            ok = ok && !proven_crypto_x25519(b, k, v);
            v[0] = 0xee;                                                                      /* and p + 1 is one */
            ok = ok && !proven_crypto_x25519(b, k, v);
            PROVEN_TEST_ASSERT(ok, "a coordinate of p or more, and one with the 256th bit set, are the number they are modulo p", "");
        }
        /* Pairs of keys from a fixed stream, scalars and all: both sides of an exchange reach
         * the same secret, and a signature by each key verifies and does not verify a changed
         * message. No oracle: the two sides are each other's. */
        {
            proven_u64 x = 0x9e3779b97f4a7c15ull;
            proven_byte_t ka[32], kb[32], pa[32], pb[32], sa[32], sb[32], sig[64], msg[48];
            bool ok = true;
            for (int round = 0; round < 48 && ok; ++round) {
                for (int i = 0; i < 32; ++i) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; ka[i] = (proven_byte_t)(x >> 24); kb[i] = (proven_byte_t)(x >> 40); }
                if (round == 0) { memset(ka, 0xff, 32); memset(kb, 0, 32); }                  /* the extreme scalars too */
                if (round == 1) { memset(ka, 0x80, 32); memset(kb, 0x7f, 32); }
                for (int i = 0; i < 48; ++i) msg[i] = (proven_byte_t)(ka[i % 32] + i);
                proven_crypto_x25519_public(pa, ka); proven_crypto_x25519_public(pb, kb);
                ok = ok && proven_crypto_x25519(sa, ka, pb) && proven_crypto_x25519(sb, kb, pa) && memcmp(sa, sb, 32) == 0;
                proven_crypto_ed25519_public(pa, ka);
                proven_crypto_ed25519_sign(sig, ka, pa, (proven_mem_view_t){ msg, (proven_size_t)round });
                ok = ok && proven_crypto_ed25519_verify(pa, (proven_mem_view_t){ msg, (proven_size_t)round }, sig);
                ok = ok && !proven_crypto_ed25519_verify(pa, (proven_mem_view_t){ msg, (proven_size_t)round + 1 }, sig);
                proven_crypto_ed25519_public(pb, kb);
                ok = ok && !proven_crypto_ed25519_verify(pb, (proven_mem_view_t){ msg, (proven_size_t)round }, sig);
            }
            PROVEN_TEST_ASSERT(ok, "forty-eight pairs of keys agree on their secret, and each key's signature verifies under it alone", "");
        }
        /* The base point's table and the comb that reads it (five teeth 51 apart; an even
         * scalar takes L - k and a negation), then verification's variable-time s B + k Q
         * against the constant-time code: Q = B with s = k, Q = -B with s = k (the sum is the
         * identity), either or both zero, L - 1, random points, long runs. */
        {
            PROVEN_TEST_ASSERT(proven_crypto_ed25519_test_table(), "every entry of the base point's table is what the general multiplication gives", "");
            static const proven_byte_t ell[32] = { 0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                                                   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10 };   /* L, little-endian */
            proven_byte_t base[32], nbase[32], q[32], seed[32], s[32], k[32];
            memset(base, 0x66, 32); base[0] = 0x58;                                           /* y = 4/5, x even */
            memcpy(nbase, base, 32); nbase[31] |= 0x80;                                       /* -B */
            proven_u64 x = 0x2545f4914f6cdd1dull;
            proven_size_t tried = 0, agreed = 0, mtried = 0, magreed = 0;
            for (int which = 0; which < 40; ++which) {
                for (int i = 0; i < 32; ++i) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; s[i] = (proven_byte_t)(x >> 24); k[i] = (proven_byte_t)(x >> 40); }
                s[31] &= 0x0f; k[31] &= 0x0f;                                                 /* below 2^252 < L */
                if (which == 1) { memset(s, 0, 32); s[0] = 1; }
                else if (which == 2) { memcpy(s, ell, 32); s[0] -= 1; }                       /* L - 1 */
                else if (which == 3) { memcpy(s, ell, 32); s[0] -= 2; }                       /* L - 2 */
                else if (which == 4) { memset(s, 0, 32); s[0] = 16; }
                else if (which == 5) memset(s, 0, 32);
                else if (which >= 6 && which < 21) {
                    const int t = (which - 6) / 3, kind = (which - 6) % 3;
                    int bit = kind == 2 ? t * 51 + 50 : t * 51;
                    if (bit > 251) bit = 251;
                    memset(s, 0, 32);
                    if (kind == 1) { for (int b = 0; b < bit; ++b) s[b / 8] |= (proven_byte_t)(1u << (b % 8)); }
                    else s[bit / 8] |= (proven_byte_t)(1u << (bit % 8));
                }
                tried++;
                if (proven_crypto_ed25519_test_base_mul(s)) agreed++;

                memcpy(q, base, 32);
                if (which < 5) memcpy(k, s, 32);                                              /* s B + s B */
                else if (which < 10) { memcpy(q, nbase, 32); memcpy(k, s, 32); }              /* s B - s B */
                else if (which == 10) memset(k, 0, 32);
                else if (which == 11) { memset(s, 0, 32); memset(k, 0, 32); }
                else if (which == 12) memset(s, 0, 32);
                else {
                    for (int i = 0; i < 32; ++i) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; seed[i] = (proven_byte_t)(x >> 24); }
                    proven_crypto_ed25519_public(q, seed);
                    if (which == 13) { memcpy(k, ell, 32); k[0] -= 1; }
                    if (which % 3 == 0) memset(k + 8, 0xff, 12);                              /* long runs in the NAF */
                    if (which % 3 == 1) memset(k + 8, 0, 12);
                }
                mtried++;
                if (proven_crypto_ed25519_test_mul2(s, k, q)) magreed++;
            }
            PROVEN_TEST_ASSERT(tried == 40 && agreed == tried,
                "k B by the table agrees with the general walk for k of 0, 1, 16, L - 1, L - 2, the edges of every tooth and random scalars", "");
            PROVEN_TEST_ASSERT(mtried == 40 && magreed == mtried,
                "s B + k Q in variable time agrees with the constant-time code: Q = B and Q = -B with s = k, either or both scalars zero, L - 1, random points with long runs", "");
        }
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
