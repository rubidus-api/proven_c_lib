#include "proven.h"
#include "proven_test.h"
#include "test_unit_tls_pki.h"
#include <string.h>

/*
 * RSA signing: the constant-time exponentiation, the CRT with its check, blinding, the two
 * encodings, and reading a key.
 *
 * The keys are made when the test starts (tp_build_rsa) from a fixed seed. What a signature
 * must be is established three ways: the library's own verifier accepts it - and that verifier
 * was checked against other implementations' vectors when it was written; the CRT result
 * equals a plain exponentiation by d that shares no code path with it; and a deterministic
 * signature does not depend on whether, or with what, it was blinded.
 */

void proven_crypto_rsa_test_min_bits(proven_size_t bits);

static tp_stream_t g_stream = { .seed = "test_unit_crypto_rsa bytes", .counter = 0 };
static void fill(proven_byte_t *out, proven_size_t n) { tp_stream(&g_stream, out, n); }

static proven_mem_view_t view(const proven_byte_t *p, proven_size_t n) { return (proven_mem_view_t){ .ptr = p, .size = n }; }

/* sig^e mod n, then that to the power d: both with the plain exponentiation. */
static bool plain_roundtrip(const proven_crypto_rsa_key_t *k, const proven_byte_t *d, const proven_byte_t *sig) {
    static proven_crypto_mp_mod_t mod;
    proven_u32 s[PROVEN_CRYPTO_MP_MAX], em[PROVEN_CRYPTO_MP_MAX], back[PROVEN_CRYPTO_MP_MAX], e[2], dl[PROVEN_CRYPTO_MP_MAX];
    if (!proven_crypto_mp_mod_init(&mod, k->n, k->n_len)) return false;
    if (!proven_crypto_mp_load_be(s, mod.limbs, sig, k->n_len) || !proven_crypto_mp_load_be(e, 2, k->e, k->e_len) || !proven_crypto_mp_load_be(dl, mod.limbs, d, k->n_len)) return false;
    proven_crypto_mp_to_mont(&mod, em, s);
    proven_crypto_mp_pow(&mod, em, em, e, 2);
    proven_crypto_mp_pow(&mod, back, em, dl, mod.limbs);
    proven_crypto_mp_from_mont(&mod, back, back);
    return proven_crypto_mp_eq(back, s, mod.limbs) != 0;
}

static void key_cases(const char *which, const proven_crypto_rsa_key_t *k, const proven_byte_t *d) {
    static proven_crypto_rsa_blind_t blind;
    proven_byte_t sig[512], sig2[512], digest[64], salt[64], seed[512];
    const proven_mem_view_t n = view(k->n, k->n_len), e = view(k->e, k->e_len);
    static const int hashes[3] = { PROVEN_HMAC_SHA256, PROVEN_HMAC_SHA384, PROVEN_HMAC_SHA512 };
    fill(seed, k->n_len);
    PROVEN_TEST_ASSERT(proven_crypto_rsa_blind_make(k, seed, &blind), "a blinding pair is made from random bytes", which);
    for (int h = 0; h < 3; ++h) {
        proven_size_t hlen = proven_hmac_size((proven_hmac_hash_t)hashes[h]);
        fill(digest, hlen); fill(salt, hlen);
        proven_mem_view_t dg = view(digest, hlen);
        PROVEN_TEST_ASSERT(proven_crypto_rsa_sign_pkcs1(k, NULL, hashes[h], dg, sig) && proven_crypto_rsa_verify_pkcs1(n, e, hashes[h], dg, view(sig, k->n_len)),
            "a PKCS #1 v1.5 signature is accepted by the verifier", which);
        PROVEN_TEST_ASSERT(plain_roundtrip(k, d, sig), "and is what a plain exponentiation by d gives: the CRT agrees with a path that shares none of its code", which);
        PROVEN_TEST_ASSERT(proven_crypto_rsa_sign_pkcs1(k, &blind, hashes[h], dg, sig2) && memcmp(sig, sig2, k->n_len) == 0, "blinded, it is the same signature", which);
        proven_crypto_rsa_blind_next(k, &blind);
        PROVEN_TEST_ASSERT(proven_crypto_rsa_sign_pkcs1(k, &blind, hashes[h], dg, sig2) && memcmp(sig, sig2, k->n_len) == 0, "and the same again with the pair squared", which);
        digest[0] ^= 1;
        PROVEN_TEST_ASSERT(!proven_crypto_rsa_verify_pkcs1(n, e, hashes[h], dg, view(sig, k->n_len)), "it does not verify for another digest", which);
        digest[0] ^= 1;

        /* PSS with a salt as long as the digest needs 2 * hlen + 2 bytes: a 1024-bit key has no room for SHA-512's. */
        bool fits = (k->n_len * 8 - 1 + 7) / 8 >= 2 * hlen + 2;
        bool made = proven_crypto_rsa_sign_pss(k, &blind, hashes[h], dg, view(salt, hlen), sig);
        PROVEN_TEST_ASSERT(made == fits, fits ? "a PSS signature is made" : "PSS with a salt the modulus has no room for is refused", which);
        if (made) {
            PROVEN_TEST_ASSERT(proven_crypto_rsa_verify_pss(n, e, hashes[h], hlen, dg, view(sig, k->n_len)) && plain_roundtrip(k, d, sig), "and verifies, with its salt length", which);
            salt[0] ^= 1;
            PROVEN_TEST_ASSERT(proven_crypto_rsa_sign_pss(k, NULL, hashes[h], dg, view(salt, hlen), sig2) && memcmp(sig, sig2, k->n_len) != 0 &&
                               proven_crypto_rsa_verify_pss(n, e, hashes[h], hlen, dg, view(sig2, k->n_len)), "another salt gives another signature, as valid", which);
            PROVEN_TEST_ASSERT(proven_crypto_rsa_sign_pss(k, NULL, hashes[h], dg, view(salt, 0), sig2) && proven_crypto_rsa_verify_pss(n, e, hashes[h], 0, dg, view(sig2, k->n_len)) &&
                               !proven_crypto_rsa_verify_pss(n, e, hashes[h], hlen, dg, view(sig2, k->n_len)), "an empty salt is a signature too, and is not taken for one with a salt", which);
        }
    }
    /* The pair stays a pair however often it is squared. */
    fill(digest, 32);
    PROVEN_TEST_ASSERT(proven_crypto_rsa_sign_pkcs1(k, NULL, PROVEN_HMAC_SHA256, view(digest, 32), sig), "a reference signature", which);
    bool same = true;
    for (int i = 0; i < 40; ++i) {
        proven_crypto_rsa_blind_next(k, &blind);
        same = same && proven_crypto_rsa_sign_pkcs1(k, &blind, PROVEN_HMAC_SHA256, view(digest, 32), sig2) && memcmp(sig, sig2, k->n_len) == 0;
    }
    PROVEN_TEST_ASSERT(same, "forty squarings on, the blinding pair still cancels exactly", which);

    /* A fault in any one CRT value: nothing comes out. */
    static proven_crypto_rsa_key_t broken;
    for (int part = 0; part < 3; ++part) {
        broken = *k;
        proven_byte_t *field = part == 0 ? broken.dp : part == 1 ? broken.dq : broken.qinv;
        field[(part == 1 ? k->q_len : k->p_len) - 1] ^= 0x04;
        memset(sig2, 0xa5, sizeof sig2);
        bool out = proven_crypto_rsa_sign_pkcs1(&broken, NULL, PROVEN_HMAC_SHA256, view(digest, 32), sig2);
        bool untouched = true;
        for (proven_size_t i = 0; i < k->n_len; ++i) untouched = untouched && sig2[i] == 0xa5;
        PROVEN_TEST_ASSERT(!out && untouched, part == 0 ? "a wrong dp yields no signature, and nothing is written" : part == 1 ? "nor does a wrong dq" : "nor a wrong qinv", which);
    }
    PROVEN_TEST_ASSERT(!proven_crypto_rsa_sign_pkcs1(k, NULL, PROVEN_HMAC_SHA256, view(digest, 31), sig2) && !proven_crypto_rsa_sign_pkcs1(k, NULL, 99, view(digest, 32), sig2) &&
                       !proven_crypto_rsa_sign_pss(k, NULL, PROVEN_HMAC_SHA256, view(digest, 33), view(salt, 32), sig2), "a digest of the wrong length, and a hash that does not exist, are refused", which);

    /* Written out and read back, it is the same key. */
    static proven_byte_t der[2600];
    static proven_crypto_rsa_key_t back;
    proven_size_t len = proven_tls_rsa_key_der_(k, d, false, (proven_mem_mut_t){ .ptr = der, .size = sizeof der });
    PROVEN_TEST_ASSERT(len > 0 && proven_crypto_rsa_key_parse(view(der, len), &back) && back.n_len == k->n_len && back.p_len == k->p_len && back.q_len == k->q_len &&
                       memcmp(back.n, k->n, k->n_len) == 0 && memcmp(back.p, k->p, k->p_len) == 0 && memcmp(back.q, k->q, k->q_len) == 0 &&
                       memcmp(back.dp, k->dp, k->p_len) == 0 && memcmp(back.dq, k->dq, k->q_len) == 0 && memcmp(back.qinv, k->qinv, k->p_len) == 0,
        "an RSAPrivateKey written and read back is the same key", which);
    PROVEN_TEST_ASSERT(!proven_crypto_rsa_key_parse(view(der, len - 1), &back) && !proven_crypto_rsa_key_parse(view(der, 0), &back), "cut short, it is not a key", which);
    der[len] = 0;
    PROVEN_TEST_ASSERT(!proven_crypto_rsa_key_parse(view(der, len + 1), &back), "nor with a byte after it", which);
    /* The last byte of the structure is the end of qinv; the last byte of n is easy to find: flip a bit of the modulus. */
    for (proven_size_t i = 0; i + k->n_len <= len; ++i) {
        if (memcmp(der + i, k->n, k->n_len) == 0) { der[i + k->n_len / 2] ^= 0x10; break; }
    }
    PROVEN_TEST_ASSERT(!proven_crypto_rsa_key_parse(view(der, len), &back), "a modulus that is not the product of the two primes is refused", which);
}

int main(void) {
    PROVEN_TEST_SUITE("RSA signing",
        "The exponentiation for secret exponents, signing by CRT with its check, blinding, PKCS #1 v1.5 and PSS, and reading and making keys.",
        "Inspect src/proven/crypto_rsa.c (rsa_private, rsa_garner, proven_crypto_rsa_blind_make) and proven_crypto_mp_pow_ct in src/proven/crypto_mp.c. The keys and every random byte come from fixed seeds, so a failure repeats exactly.");

    PROVEN_TEST_ASSERT(tp_build() && tp_build_rsa(), "RSA keys of 2048 and 1024 bits are made", "");

    PROVEN_TEST_SECTION("the exponentiation for secret exponents", "proven_crypto_mp_pow_ct against the plain square-and-multiply, on random bases and exponents.", "Check the window selection and the table in proven_crypto_mp_pow_ct.");
    {
        static proven_crypto_mp_mod_t mod;
        const proven_crypto_rsa_key_t *keys[2] = { &TP_RSA_SERVER_K, &TP_RSA_SMALL_K };
        bool agree = true, edge = true;
        for (int which = 0; which < 2; ++which) {
            for (int half = 0; half < 2; ++half) {
                const proven_byte_t *prime = half ? keys[which]->q : keys[which]->p;
                proven_size_t len = half ? keys[which]->q_len : keys[which]->p_len;
                PROVEN_TEST_ASSERT(proven_crypto_mp_mod_init(&mod, prime, len), "a prime as a modulus", "");
                proven_u32 base[PROVEN_CRYPTO_MP_CT_MAX], ex[PROVEN_CRYPTO_MP_CT_MAX], a[PROVEN_CRYPTO_MP_CT_MAX], b[PROVEN_CRYPTO_MP_CT_MAX];
                proven_byte_t bytes[PROVEN_CRYPTO_RSA_PRIME_MAX_BYTES];
                for (int round = 0; round < 6; ++round) {
                    fill(bytes, len);
                    proven_crypto_mp_reduce_be(&mod, base, bytes, len);
                    proven_crypto_mp_to_mont(&mod, base, base);
                    fill(bytes, len);
                    /* Short exponents too: leading zero limbs must change nothing but the answer. */
                    for (proven_size_t i = 0; i < len && i < (proven_size_t)round * (len / 6); ++i) bytes[i] = 0;
                    PROVEN_TEST_ASSERT(proven_crypto_mp_load_be(ex, mod.limbs, bytes, len), "an exponent", "");
                    agree = agree && proven_crypto_mp_pow_ct(&mod, a, base, ex);
                    proven_crypto_mp_pow(&mod, b, base, ex, mod.limbs);
                    agree = agree && proven_crypto_mp_eq(a, b, mod.limbs) != 0;
                }
                /* x^0 = 1 and x^1 = x. */
                for (proven_size_t i = 0; i < mod.limbs; ++i) ex[i] = 0;
                edge = edge && proven_crypto_mp_pow_ct(&mod, a, base, ex);
                proven_crypto_mp_from_mont(&mod, a, a);
                for (proven_size_t i = 0; i < mod.limbs; ++i) edge = edge && a[i] == (i == 0 ? 1u : 0u);
                ex[0] = 1;
                edge = edge && proven_crypto_mp_pow_ct(&mod, a, base, ex) && proven_crypto_mp_eq(a, base, mod.limbs) != 0;
            }
        }
        PROVEN_TEST_ASSERT(agree, "twenty-four random powers modulo four primes agree with square-and-multiply, short exponents included", "");
        PROVEN_TEST_ASSERT(edge, "the zeroth power is one and the first is the base", "");
        static proven_crypto_mp_mod_t big;
        proven_u32 x[PROVEN_CRYPTO_MP_MAX] = { 2 };
        PROVEN_TEST_ASSERT(proven_crypto_mp_mod_init(&big, TP_RSA_SERVER_K.n, TP_RSA_SERVER_K.n_len) && big.limbs == 64 && proven_crypto_mp_pow_ct(&big, x, x, x), "a modulus of 64 limbs is within its bound", "");
        static proven_byte_t wide[PROVEN_CRYPTO_MP_CT_MAX * 4 + 4];
        memset(wide, 0xff, sizeof wide);
        PROVEN_TEST_ASSERT(proven_crypto_mp_mod_init(&big, wide, sizeof wide) && !proven_crypto_mp_pow_ct(&big, x, x, x), "one of 67 limbs is refused: the table is sized for a prime, not for a modulus", "");
    }

    PROVEN_TEST_SECTION("a 2048-bit key", "Every hash, both encodings, blinded and not, a fault in each CRT value, and the key written and read back.", "");
    key_cases("2048 bits", &TP_RSA_SERVER_K, TP_RSA_SERVER_D);

    PROVEN_TEST_SECTION("a 1024-bit key", "The same with a key below the accepted size, which a test hook lets in.", "");
    {
        static proven_crypto_rsa_key_t back;
        static proven_byte_t der[1400];
        proven_size_t len = proven_tls_rsa_key_der_(&TP_RSA_SMALL_K, TP_RSA_SMALL_D, false, (proven_mem_mut_t){ .ptr = der, .size = sizeof der });
        PROVEN_TEST_ASSERT(len > 0 && !proven_crypto_rsa_key_parse(view(der, len), &back), "a 1024-bit key is not accepted for signing", "");
        proven_crypto_rsa_test_min_bits(1024);
        key_cases("1024 bits", &TP_RSA_SMALL_K, TP_RSA_SMALL_D);
        proven_crypto_rsa_test_min_bits(0);
    }

    PROVEN_TEST_SECTION("making a key", "What proven_crypto_rsa_generate refuses, and that two keys from one stream differ.", "Check rsa_is_prime and the exact arithmetic for d, dp and dq.");
    {
        static proven_crypto_rsa_key_t k;
        proven_byte_t d[512];
        tp_stream_t st = { .seed = "a second stream, a second key", .counter = 0 };
        PROVEN_TEST_ASSERT(!proven_crypto_rsa_generate(1000, tp_stream, &st, &k, d) && !proven_crypto_rsa_generate(512, tp_stream, &st, &k, d) &&
                           !proven_crypto_rsa_generate(8192, tp_stream, &st, &k, d) && !proven_crypto_rsa_generate(2048, NULL, &st, &k, d), "sizes outside 1024 to 4096, sizes that are not a multiple of 64, and no random source are refused", "");
        PROVEN_TEST_ASSERT(proven_crypto_rsa_generate(1024, tp_stream, &st, &k, d) && k.n_len == 128 && (k.n[0] & 0x80) && memcmp(k.n, TP_RSA_SMALL_K.n, 128) != 0 && memcmp(k.p, k.q, 64) != 0,
            "a key of exactly the size asked for, with two different primes, and not the key another stream gave", "");
        /* e * d = 1 modulo p - 1 shows in the smallest way: (m^e)^d = m for a small m. */
        static proven_crypto_mp_mod_t mod;
        proven_u32 m[PROVEN_CRYPTO_MP_MAX] = { 0 }, c[PROVEN_CRYPTO_MP_MAX], e[2], dl[PROVEN_CRYPTO_MP_MAX];
        m[0] = 0x12345678u;
        PROVEN_TEST_ASSERT(proven_crypto_mp_mod_init(&mod, k.n, k.n_len) && proven_crypto_mp_load_be(e, 2, k.e, k.e_len) && proven_crypto_mp_load_be(dl, mod.limbs, d, k.n_len), "its numbers", "");
        proven_crypto_mp_to_mont(&mod, c, m);
        proven_crypto_mp_pow(&mod, c, c, e, 2);
        proven_crypto_mp_pow(&mod, c, c, dl, mod.limbs);
        proven_crypto_mp_from_mont(&mod, c, c);
        PROVEN_TEST_ASSERT(proven_crypto_mp_eq(c, m, mod.limbs) != 0, "its d undoes its e", "");
    }

    PROVEN_TEST_PASS("RSA signatures are made by CRT, checked before release, unchanged by blinding, and accepted by the verifier.");
    return 0;
}
