#ifndef PROVEN_INTERNAL_CRYPTO_H
#define PROVEN_INTERNAL_CRYPTO_H

/* The cryptographic primitives under the TLS unit. Internal: nothing here is public API, and
 * the names may change between releases. Tests reach it by path.
 *
 * Rules every function here keeps for data marked SECRET in its comment: no branch on it, no
 * memory index from it, no early exit that depends on it. */

#include "proven/types.h"
#include "proven/memory.h"

/* Where secret-derived data becomes public on purpose - a tag that matched, a signature, the
 * fact that a key is in range - the code says so with this. It does nothing in any build of
 * the library; the private constant-time check defines PROVEN_CT_CHECK and then it tells
 * Valgrind that the bytes may be branched on from here. Every use is a claim to review. */
#ifdef PROVEN_CT_CHECK
#include <valgrind/memcheck.h>
#define PROVEN_CT_PUBLIC(p, n) ((void)VALGRIND_MAKE_MEM_DEFINED((p), (n)))
#else
#define PROVEN_CT_PUBLIC(p, n) ((void)(p), (void)(n))
#endif

#define PROVEN_CRYPTO_AEAD_KEY_MAX 32
#define PROVEN_CRYPTO_AEAD_NONCE   12
#define PROVEN_CRYPTO_AEAD_TAG     16

/* ---- ChaCha20 and Poly1305 (RFC 8439) ---- */

/* XOR `len` bytes of keystream into `out` from `in` (they may be the same), starting at block
 * `counter`. SECRET: key, in. */
void proven_crypto_chacha20(const proven_byte_t key[32], proven_u32 counter, const proven_byte_t nonce[12],
                            const proven_byte_t *in, proven_byte_t *out, proven_size_t len);

typedef struct {
    proven_u32 r[5];
    proven_u32 h[5];
    proven_u32 pad[4];
    proven_byte_t buf[16];
    proven_size_t buf_len;
} proven_crypto_poly1305_t;

void proven_crypto_poly1305_init(proven_crypto_poly1305_t *st, const proven_byte_t key[32]);
void proven_crypto_poly1305_update(proven_crypto_poly1305_t *st, const proven_byte_t *m, proven_size_t len);
void proven_crypto_poly1305_final(proven_crypto_poly1305_t *st, proven_byte_t tag[16]);

/* The AEAD of RFC 8439 section 2.8. `out` receives plain.size bytes and may be `plain.ptr`. */
void proven_crypto_chacha20poly1305_seal(const proven_byte_t key[32], const proven_byte_t nonce[12],
                                         proven_mem_view_t aad, proven_mem_view_t plain,
                                         proven_byte_t *out, proven_byte_t tag[16]);
/* False when the tag does not match; `out` is then zeroed, not left with unauthenticated text. */
[[nodiscard]] bool proven_crypto_chacha20poly1305_open(const proven_byte_t key[32], const proven_byte_t nonce[12],
                                                       proven_mem_view_t aad, proven_mem_view_t cipher,
                                                       const proven_byte_t tag[16], proven_byte_t *out);

/* ---- Multi-precision modular arithmetic (Montgomery form), constant-time ---- */

/* Numbers are arrays of 32-bit limbs, least significant first, `limbs` long for a modulus of
 * that many limbs. Every operation takes the same time for the same modulus size whatever the
 * values are, except where a parameter is marked PUBLIC. */
#define PROVEN_CRYPTO_MP_MAX 256                 /* limbs: moduli up to 8192 bits */

typedef struct {
    proven_size_t limbs;
    proven_size_t bits;                          /* bit length of the modulus */
    proven_u32 n0inv;                            /* -n^-1 mod 2^32 */
    proven_u32 n[PROVEN_CRYPTO_MP_MAX];
    proven_u32 rr[PROVEN_CRYPTO_MP_MAX];         /* R^2 mod n, R = 2^(32 * limbs) */
} proven_crypto_mp_mod_t;

/* The modulus from big-endian bytes. False when it is even, below 3, or too large. */
[[nodiscard]] bool proven_crypto_mp_mod_init(proven_crypto_mp_mod_t *mod, const proven_byte_t *be, proven_size_t len);
/* The same for a SECRET modulus of exactly `len` bytes (8 or more): nothing but its oddness
 * is examined, and `bits` is len * 8 whatever its leading bits are. */
[[nodiscard]] bool proven_crypto_mp_mod_init_secret(proven_crypto_mp_mod_t *mod, const proven_byte_t *be, proven_size_t len);
/* Load big-endian bytes into `limbs` limbs. False when the value does not fit. */
[[nodiscard]] bool proven_crypto_mp_load_be(proven_u32 *out, proven_size_t limbs, const proven_byte_t *be, proven_size_t len);
void proven_crypto_mp_store_be(proven_byte_t *out, proven_size_t len, const proven_u32 *a, proven_size_t limbs);
/* All-ones when a < b, zero otherwise. */
proven_u32 proven_crypto_mp_lt(const proven_u32 *a, const proven_u32 *b, proven_size_t limbs);
/* All-ones when a is zero. */
proven_u32 proven_crypto_mp_is_zero(const proven_u32 *a, proven_size_t limbs);
/* All-ones when a equals b. */
proven_u32 proven_crypto_mp_eq(const proven_u32 *a, const proven_u32 *b, proven_size_t limbs);
/* out = mask ? a : b, for a mask of all ones or zero. */
void proven_crypto_mp_select(proven_u32 *out, const proven_u32 *a, const proven_u32 *b, proven_u32 mask, proven_size_t limbs);
/* out = a * b / R mod n. Inputs below n; `out` may be either input. */
void proven_crypto_mp_montmul(const proven_crypto_mp_mod_t *mod, proven_u32 *out, const proven_u32 *a, const proven_u32 *b);
void proven_crypto_mp_to_mont(const proven_crypto_mp_mod_t *mod, proven_u32 *out, const proven_u32 *a);
void proven_crypto_mp_from_mont(const proven_crypto_mp_mod_t *mod, proven_u32 *out, const proven_u32 *a);
void proven_crypto_mp_add(const proven_crypto_mp_mod_t *mod, proven_u32 *out, const proven_u32 *a, const proven_u32 *b);
void proven_crypto_mp_sub(const proven_crypto_mp_mod_t *mod, proven_u32 *out, const proven_u32 *a, const proven_u32 *b);
/* out = (big-endian value of any length) mod n, bit by bit. */
void proven_crypto_mp_reduce_be(const proven_crypto_mp_mod_t *mod, proven_u32 *out, const proven_byte_t *be, proven_size_t len);
/* out = base^exp mod n, base and out in Montgomery form. The exponent is PUBLIC: its bits
 * decide which multiplications happen. */
void proven_crypto_mp_pow(const proven_crypto_mp_mod_t *mod, proven_u32 *out, const proven_u32 *base,
                          const proven_u32 *exp, proven_size_t exp_limbs);
/* The largest modulus, in limbs, that proven_crypto_mp_pow_ct accepts: a prime factor of a
 * 4096-bit RSA key, with room for factors of unequal size. Its table is sized by this. */
#define PROVEN_CRYPTO_MP_CT_MAX 66
/* out = base^exp mod n, base and out in Montgomery form, for a SECRET exponent - and the
 * modulus may be secret too. `exp` has exactly as many limbs as the modulus, zero-padded: the
 * number of steps depends on the modulus's size alone, never on the exponent's own length or
 * bits (fixed four-bit windows, every table lookup a masked pass over the whole table).
 * False when the modulus has more than PROVEN_CRYPTO_MP_CT_MAX limbs. */
[[nodiscard]] bool proven_crypto_mp_pow_ct(const proven_crypto_mp_mod_t *mod, proven_u32 *out, const proven_u32 *base, const proven_u32 *exp);
/* out = a * b, a plain product of alen + blen limbs. `out` may not overlap an input. */
/* base^exp for a SECRET exponent of `exp_limbs` limbs (a PUBLIC count) and a public modulus of
 * up to PROVEN_CRYPTO_MP_DH_MAX limbs: finite-field Diffie-Hellman. Base and result in
 * Montgomery form. Its table is 8 KiB of stack. */
#define PROVEN_CRYPTO_MP_DH_MAX 128
[[nodiscard]] bool proven_crypto_mp_pow_ct_short(const proven_crypto_mp_mod_t *mod, proven_u32 *out, const proven_u32 *base, const proven_u32 *exp, proven_size_t exp_limbs);
void proven_crypto_mp_mul(proven_u32 *out, const proven_u32 *a, proven_size_t alen, const proven_u32 *b, proven_size_t blen);
/* out = a^-1 mod n for a prime n (Fermat), in Montgomery form. Zero stays zero. The modulus
 * is PUBLIC here: it is used as the exponent of the public exponentiation. */
void proven_crypto_mp_inv_prime(const proven_crypto_mp_mod_t *mod, proven_u32 *out, const proven_u32 *a);

/* ---- The NIST prime curves P-256 and P-384 ---- */

typedef enum { PROVEN_CRYPTO_EC_P256, PROVEN_CRYPTO_EC_P384 } proven_crypto_ec_curve_t;

#define PROVEN_CRYPTO_EC_MAX_BYTES 48            /* a coordinate or scalar of the larger curve */

/* Bytes in a coordinate or scalar: 32 or 48; 0 for an unknown curve. */
proven_size_t proven_crypto_ec_size(proven_crypto_ec_curve_t curve);

/* The public point (0x04 | X | Y, 1 + 2 * size bytes) of a private scalar (big-endian, size
 * bytes, SECRET). False when the scalar is zero or not below the group order. */
[[nodiscard]] bool proven_crypto_ec_public(proven_crypto_ec_curve_t curve, const proven_byte_t *priv, proven_byte_t *pub_out);

/* ECDH: the X coordinate of priv * peer. False when `peer` is not a valid point of the curve
 * (wrong form, coordinate out of range, not on the curve) or the scalar is out of range. */
[[nodiscard]] bool proven_crypto_ecdh(proven_crypto_ec_curve_t curve, const proven_byte_t *priv,
                                      proven_mem_view_t peer, proven_byte_t *shared_out);

/* ECDSA over a digest, with the deterministic nonce of RFC 6979 computed with HMAC over
 * `hmac_hash` (the hash that produced the digest). `sig_out` receives r | s, 2 * size bytes. */
[[nodiscard]] bool proven_crypto_ecdsa_sign(proven_crypto_ec_curve_t curve, int hmac_hash, const proven_byte_t *priv,
                                            proven_mem_view_t digest, proven_byte_t *sig_out);

/* True when (r, s), each big-endian of any length, is a valid signature of `digest` under the
 * public point `pub`. Nothing here is secret. */
[[nodiscard]] bool proven_crypto_ecdsa_verify(proven_crypto_ec_curve_t curve, proven_mem_view_t pub, proven_mem_view_t digest,
                                              proven_mem_view_t r, proven_mem_view_t s);

/* The same for a signature in its DER form, SEQUENCE { INTEGER r, INTEGER s }, read strictly:
 * minimal lengths, minimal non-negative integers, nothing after the end. */
[[nodiscard]] bool proven_crypto_ecdsa_verify_der(proven_crypto_ec_curve_t curve, proven_mem_view_t pub, proven_mem_view_t digest,
                                                  proven_mem_view_t sig);

/* ---- Curve25519: X25519 (RFC 7748) and Ed25519 (RFC 8032) ---- */

/* out = scalar * u. SECRET: scalar. False when the result is all zeros (the peer sent a point
 * of small order), which a key exchange must refuse. */
[[nodiscard]] bool proven_crypto_x25519(proven_byte_t out[32], const proven_byte_t scalar[32], const proven_byte_t u[32]);
void proven_crypto_x25519_public(proven_byte_t out[32], const proven_byte_t scalar[32]);

/* The public key of a 32-byte seed (SECRET). */
void proven_crypto_ed25519_public(proven_byte_t pub[32], const proven_byte_t seed[32]);
void proven_crypto_ed25519_sign(proven_byte_t sig[64], const proven_byte_t seed[32], const proven_byte_t pub[32],
                                proven_mem_view_t msg);
/* Refuses S not below the group order, a public key or R that is not a canonical encoding of a
 * point, and of course a signature that does not verify. */
[[nodiscard]] bool proven_crypto_ed25519_verify(const proven_byte_t pub[32], proven_mem_view_t msg, const proven_byte_t sig[64]);

/* ---- Hash block functions, exposed for the constant-time record MAC of tls_legacy.c ---- */
void proven_sha1_compress_(proven_u32 state[5], const proven_byte_t block[64]);
void proven_md5_compress_(proven_u32 state[4], const proven_byte_t block[64]);
void proven_sha256_compress_(proven_u32 state[8], const proven_byte_t block[64]);

/* ---- AES-GCM (NIST SP 800-38D), 96-bit nonces ---- */

/* Two implementations behind one interface. The portable one is bitsliced: the S-box is
 * computed (an inversion in GF(2^8)) with boolean operations over bit planes, never looked up,
 * and GHASH multiplies bit by bit under masks. The hardware one (platform/proven_sys_aes.c)
 * uses the AES and carry-less multiply instructions where the processor has them, chosen at
 * run time. There is no table-driven AES here in any configuration. */
typedef struct {
    int rounds;                                  /* 10 or 14 */
    bool hw;
    proven_byte_t rk[240];                       /* round keys as bytes */
    proven_byte_t h[16];                         /* the GHASH key */
} proven_crypto_aes_gcm_t;

/* key_len is 16 or 32. False otherwise. SECRET: key. */
[[nodiscard]] bool proven_crypto_aes_gcm_init(proven_crypto_aes_gcm_t *ctx, const proven_byte_t *key, proven_size_t key_len);
void proven_crypto_aes_gcm_seal(const proven_crypto_aes_gcm_t *ctx, const proven_byte_t nonce[12],
                                proven_mem_view_t aad, proven_mem_view_t plain, proven_byte_t *out, proven_byte_t tag[16]);
[[nodiscard]] bool proven_crypto_aes_gcm_open(const proven_crypto_aes_gcm_t *ctx, const proven_byte_t nonce[12],
                                              proven_mem_view_t aad, proven_mem_view_t cipher,
                                              const proven_byte_t tag[16], proven_byte_t *out);
/* One block under the key schedule, for tests against FIPS 197. */
void proven_crypto_aes_encrypt_block(const proven_crypto_aes_gcm_t *ctx, const proven_byte_t in[16], proven_byte_t out[16]);
/* CBC over whole blocks, in place; `len` is a multiple of 16 and `iv` is left holding the last
 * ciphertext block, which is the next call's IV. Encryption uses whichever block cipher the
 * context has; decryption is the portable, bitsliced inverse cipher in every configuration -
 * these exist for the legacy TLS suites, where constant time matters and speed does not.
 * SECRET: the key, and the plaintext. */
void proven_crypto_aes_cbc_encrypt(const proven_crypto_aes_gcm_t *ctx, proven_byte_t iv[16], proven_byte_t *data, proven_size_t len);
void proven_crypto_aes_cbc_decrypt(const proven_crypto_aes_gcm_t *ctx, proven_byte_t iv[16], proven_byte_t *data, proven_size_t len);
/* A test hook: when set, contexts initialised afterwards use the portable code even where the
 * hardware path exists. Not for use while other threads initialise contexts. */
void proven_crypto_aes_force_portable(bool on);
/* True when a context initialised now would use the hardware path. */
bool proven_crypto_aes_hw_available(void);

/* ---- RSA signature verification (RFC 8017). A public operation: nothing is secret. ---- */

#define PROVEN_CRYPTO_RSA_MIN_BITS 2048
#define PROVEN_CRYPTO_RSA_MAX_BITS 8192

/* `hash` is a proven_hmac_hash_t naming the digest's hash. `n` and `e` are big-endian. Both
 * refuse a modulus outside 2048..8192 bits, an even or tiny exponent, and a signature that is
 * not exactly the modulus length or not below the modulus. */
[[nodiscard]] bool proven_crypto_rsa_verify_pkcs1(proven_mem_view_t n, proven_mem_view_t e, int hash,
                                                  proven_mem_view_t digest, proven_mem_view_t sig);
/* RSASSA-PSS with MGF1 over the same hash and a salt of `salt_len` bytes. */
[[nodiscard]] bool proven_crypto_rsa_verify_pss(proven_mem_view_t n, proven_mem_view_t e, int hash, proven_size_t salt_len,
                                                proven_mem_view_t digest, proven_mem_view_t sig);

/* ---- RSA signing (RFC 8017). Everything in a key but n and e is SECRET. ---- */

#define PROVEN_CRYPTO_RSA_SIGN_MAX_BITS 4096
#define PROVEN_CRYPTO_RSA_SIGN_MAX_BYTES (PROVEN_CRYPTO_RSA_SIGN_MAX_BITS / 8)
#define PROVEN_CRYPTO_RSA_PRIME_MAX_BYTES (PROVEN_CRYPTO_MP_CT_MAX * 4)

/* A private key in the form signing uses: the two primes and the CRT values, big-endian.
 * `dp` and `qinv` are p_len bytes, `dq` is q_len bytes, zero-padded on the left. */
typedef struct {
    proven_size_t n_len, e_len, p_len, q_len;
    proven_byte_t n[PROVEN_CRYPTO_RSA_SIGN_MAX_BYTES];
    proven_byte_t e[8];
    proven_byte_t p[PROVEN_CRYPTO_RSA_PRIME_MAX_BYTES], q[PROVEN_CRYPTO_RSA_PRIME_MAX_BYTES];
    proven_byte_t dp[PROVEN_CRYPTO_RSA_PRIME_MAX_BYTES], dq[PROVEN_CRYPTO_RSA_PRIME_MAX_BYTES];
    proven_byte_t qinv[PROVEN_CRYPTO_RSA_PRIME_MAX_BYTES];
} proven_crypto_rsa_key_t;

/* A blinding pair for one key: r^e mod n and r^-1 mod n, n_len bytes each. SECRET. A
 * signature made with it computes on c * r^e instead of c, so that what the private
 * exponentiation works on is not what the caller - or an attacker - chose. */
typedef struct {
    proven_byte_t factor[PROVEN_CRYPTO_RSA_SIGN_MAX_BYTES];
    proven_byte_t unfactor[PROVEN_CRYPTO_RSA_SIGN_MAX_BYTES];
} proven_crypto_rsa_blind_t;

/* Read an RSAPrivateKey (PKCS #1, DER). False unless it is a two-prime key of 2048 to 4096
 * bits with n = p * q, an odd public exponent of at most 64 bits, and CRT values in range.
 * It does not prove the key is sound - only signing and verifying does (the caller's job). */
[[nodiscard]] bool proven_crypto_rsa_key_parse(proven_mem_view_t der, proven_crypto_rsa_key_t *out);
/* A blinding pair from n_len random bytes. False (try other bytes) when they are not usable. */
[[nodiscard]] bool proven_crypto_rsa_blind_make(const proven_crypto_rsa_key_t *key, const proven_byte_t *random, proven_crypto_rsa_blind_t *out);
/* The next pair: both halves squared, which is again a pair. */
void proven_crypto_rsa_blind_next(const proven_crypto_rsa_key_t *key, proven_crypto_rsa_blind_t *blind);
/* Sign. `sig` receives n_len bytes. `blind` may be null (no blinding). The result is checked
 * with the public key before it is released: false - and nothing in `sig` - when the check
 * fails, as after a fault in one half of the CRT, or when the modulus is too small for the
 * encoding. PKCS #1 v1.5 is deterministic; PSS takes the salt (usually as long as the digest). */
[[nodiscard]] bool proven_crypto_rsa_sign_pkcs1(const proven_crypto_rsa_key_t *key, const proven_crypto_rsa_blind_t *blind, int hash,
                                                proven_mem_view_t digest, proven_byte_t *sig);
[[nodiscard]] bool proven_crypto_rsa_sign_pss(const proven_crypto_rsa_key_t *key, const proven_crypto_rsa_blind_t *blind, int hash,
                                              proven_mem_view_t digest, proven_mem_view_t salt, proven_byte_t *sig);

/* Make a key of `bits` bits (a multiple of 64, 1024 to 4096) with e = 65537, drawing from
 * `random`. `d` receives the private exponent, n_len bytes, for writing the key out.
 *
 * For test identities and self-issued certificates: candidates are random, tested by trial
 * division and Miller-Rabin (5 rounds at 512 bits and above per prime, 8 below). The search
 * itself is not constant-time - how many candidates were tried is visible - which is the usual
 * state of key generation and one more reason long-lived keys come from elsewhere. */
typedef void (*proven_crypto_random_fn)(void *ctx, proven_byte_t *out, proven_size_t len);
/* PKCS #1 v1.5 type 1 over `data` as it stands, with no DigestInfo: the signature of TLS 1.0
 * and 1.1, whose `data` is an MD5 hash followed by a SHA-1 hash. For nothing else. */
[[nodiscard]] bool proven_crypto_rsa_sign_pkcs1_raw(const proven_crypto_rsa_key_t *key, const proven_crypto_rsa_blind_t *blind, proven_mem_view_t data, proven_byte_t *sig);
[[nodiscard]] bool proven_crypto_rsa_verify_pkcs1_raw(proven_mem_view_t n, proven_mem_view_t e, proven_mem_view_t data, proven_mem_view_t sig);
/* PKCS #1 v1.5 encryption (type 2) of `msg` under a public key, for a key exchange. `random`
 * is as many unpredictable bytes as the modulus has. `out` receives the modulus's length. */
[[nodiscard]] bool proven_crypto_rsa_encrypt_pkcs1(proven_mem_view_t n, proven_mem_view_t e, proven_mem_view_t msg, const proven_byte_t *random,
                                                   proven_byte_t *out, proven_size_t *out_len);
/* The server's half: decrypt a ClientKeyExchange and give back the 48-byte premaster - or, if
 * anything at all is wrong with what came out, `fallback` instead (48 random bytes drawn
 * BEFORE this call). There is no return value on purpose: nothing may be done differently for
 * a bad ciphertext than for a good one, or the key can be used by anyone who can ask
 * (Bleichenbacher 1998; ROBOT, 2017). The selection is by mask; `version` is the one the
 * ClientHello carried. SECRET: the key, the blinding pair, `fallback`, `out`. */
void proven_crypto_rsa_decrypt_premaster(const proven_crypto_rsa_key_t *key, const proven_crypto_rsa_blind_t *blind, proven_mem_view_t cipher,
                                         proven_u16 version, const proven_byte_t fallback[48], proven_byte_t out[48]);
[[nodiscard]] bool proven_crypto_rsa_generate(proven_size_t bits, proven_crypto_random_fn random, void *ctx,
                                              proven_crypto_rsa_key_t *out, proven_byte_t *d);

#endif
