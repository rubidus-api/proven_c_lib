#include "proven.h"
#include "proven_test.h"
#include "test_unit_crypto_vectors.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * aead.h, kex.h and what sign.h, hash.h and hash_legacy.h gained in v0.111.0 (RFC-0043): the
 * crypto the TLS and QUIC engines use, given a public face so that the network library needs
 * nothing else of the base library. The published vectors test_unit_crypto runs through the
 * internal code (RFC 8439, FIPS 197 and the GCM cases, RFC 7748, RFC 8032) run here through the
 * public functions, with their errors; the exchanges are checked by agreeing both ways.
 */

#define MAX_FIELDS 12
static char g_line[20000];
static proven_byte_t g_bytes[MAX_FIELDS][2100];
static proven_size_t g_len[MAX_FIELDS];
static char *g_field[MAX_FIELDS];

/* One case's fields, each decoded as hex where it is hex ("-" is empty): test_unit_crypto's form. */
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

static proven_mem_view_t mv(const void *p, proven_size_t n) { return (proven_mem_view_t){ .ptr = (const proven_byte_t *)p, .size = n }; }
static proven_mem_mut_t mm(void *p, proven_size_t n) { return (proven_mem_mut_t){ .ptr = (proven_byte_t *)p, .size = n }; }

static proven_byte_t g_out[2100], g_out2[2100];

static bool all_zero(const proven_byte_t *p, proven_size_t n) {
    for (proven_size_t i = 0; i < n; ++i) if (p[i] != 0) return false;
    return true;
}

/* "abc" padded to one block of `block` bytes with a big- or little-endian bit length. */
static void abc_block(proven_byte_t *b, proven_size_t block, bool little) {
    memset(b, 0, block);
    memcpy(b, "abc", 3);
    b[3] = 0x80;
    if (little) b[block - 8] = 24; else b[block - 1] = 24;
}

int main(void) {
    PROVEN_TEST_SUITE("aead.h, kex.h and the v0.111.0 additions to sign.h and the hashes",
        "Run RFC 8439's, FIPS 197's, the GCM cases', RFC 7748's and RFC 8032's vectors through the public AEAD, cipher, X25519 and Ed25519 functions; agree ECDH and finite-field DH keys both ways; check ECDSA from a scalar against a key's, and the compression functions against the hashes.",
        "Inspect src/proven/aead.c, src/proven/kex.c, src/proven/dh.c and the v0.111.0 functions at the end of src/proven/sign.c; the arithmetic itself is test_unit_crypto's business.");

    PROVEN_TEST_SECTION("ChaCha20-Poly1305 and ChaCha20", "Every case seals to the expected bytes and opens; a changed tag is PROVEN_ERR_UNTRUSTED with zeros left; the raw stream matches.",
        "Check proven_chacha20_poly1305_seal/open and proven_chacha20 in src/proven/aead.c.");
    {
        proven_size_t good = 0;
        for (proven_size_t i = 0; i < COUNT(CRV_CHACHA); ++i) {
            split(CRV_CHACHA[i]);
            proven_byte_t tag[16];
            proven_chacha20_poly1305_seal(g_bytes[0], g_bytes[1], V(2), V(3), g_out, tag);
            bool ok = memcmp(g_out, g_bytes[4], g_len[3]) == 0 && memcmp(tag, g_bytes[5], 16) == 0;
            ok = ok && proven_chacha20_poly1305_open(g_bytes[0], g_bytes[1], V(2), V(4), g_bytes[5], g_out2) == PROVEN_OK &&
                 memcmp(g_out2, g_bytes[3], g_len[3]) == 0;
            tag[0] ^= 1;
            memset(g_out2, 0xee, g_len[4]);
            ok = ok && proven_chacha20_poly1305_open(g_bytes[0], g_bytes[1], V(2), V(4), tag, g_out2) == PROVEN_ERR_UNTRUSTED && all_zero(g_out2, g_len[4]);
            if (ok) good++;
        }
        PROVEN_TEST_ASSERT(good == COUNT(CRV_CHACHA), "every AEAD case, and every forgery refused leaving zeros", "");
        good = 0;
        for (proven_size_t i = 0; i < COUNT(CRV_CHACHA20); ++i) {
            split(CRV_CHACHA20[i]);
            proven_chacha20(g_bytes[0], (proven_u32)strtoul(g_field[1], NULL, 16), g_bytes[2], g_bytes[3], g_out, g_len[3]);
            if (memcmp(g_out, g_bytes[4], g_len[4]) == 0) good++;
        }
        PROVEN_TEST_ASSERT(good == COUNT(CRV_CHACHA20), "every ChaCha20 keystream case", "");
    }

    PROVEN_TEST_SECTION("AES-GCM and the AES block", "The GCM cases seal and open; one-block cases (FIPS 197) through proven_aes_encrypt_block; keys of 16 and 32 bytes only.",
        "Check proven_aes_gcm_init/seal/open and proven_aes_init/encrypt_block in src/proven/aead.c.");
    {
        proven_size_t gcm = 0, gcm_good = 0, blocks = 0, blocks_good = 0;
        for (proven_size_t i = 0; i < COUNT(CRV_GCM); ++i) {
            split(CRV_GCM[i]);
            if (g_len[1] != 16 && g_len[1] != 32) continue;
            if (g_field[0][0] == 'b') {
                proven_aes_t a;
                blocks++;
                if (proven_aes_init(&a, V(1)) == PROVEN_OK) {
                    proven_aes_encrypt_block(&a, g_bytes[2], g_out);
                    if (memcmp(g_out, g_bytes[3], 16) == 0) blocks_good++;
                }
                continue;
            }
            proven_aes_gcm_t ctx;
            proven_byte_t tag[16];
            gcm++;
            if (proven_aes_gcm_init(&ctx, V(1)) != PROVEN_OK) continue;
            proven_aes_gcm_seal(&ctx, g_bytes[2], V(3), V(4), g_out, tag);
            bool ok = memcmp(g_out, g_bytes[5], g_len[4]) == 0 && memcmp(tag, g_bytes[6], 16) == 0;
            ok = ok && proven_aes_gcm_open(&ctx, g_bytes[2], V(3), V(5), g_bytes[6], g_out2) == PROVEN_OK && memcmp(g_out2, g_bytes[4], g_len[4]) == 0;
            tag[15] ^= 0x40;
            memset(g_out2, 0xee, g_len[5]);
            ok = ok && proven_aes_gcm_open(&ctx, g_bytes[2], V(3), V(5), tag, g_out2) == PROVEN_ERR_UNTRUSTED && all_zero(g_out2, g_len[5]);
            proven_mem_wipe(mm(&ctx, sizeof ctx));
            if (ok) gcm_good++;
        }
        PROVEN_TEST_ASSERT(gcm > 0 && gcm_good == gcm && blocks > 0 && blocks_good == blocks, "every GCM case and every one-block case", "");
        proven_aes_gcm_t ctx;
        proven_aes_t a;
        proven_byte_t key[32] = { 0 };
        PROVEN_TEST_ASSERT(proven_aes_gcm_init(&ctx, mv(key, 24)) == PROVEN_ERR_INVALID_ARG && proven_aes_init(&a, mv(key, 24)) == PROVEN_ERR_INVALID_ARG &&
                           proven_aes_gcm_init(NULL, mv(key, 16)) == PROVEN_ERR_INVALID_ARG && proven_aes_init(&a, mv(NULL, 16)) == PROVEN_ERR_INVALID_ARG,
            "a 24-byte key and null pointers: PROVEN_ERR_INVALID_ARG", "");
        (void)proven_aes_hw_available();
    }

    PROVEN_TEST_SECTION("X25519 and Ed25519", "RFC 7748's and RFC 8032's cases and the rest of the set; a small-order point is PROVEN_ERR_UNTRUSTED; a changed signature is refused.",
        "Check proven_x25519 in src/proven/kex.c and the Ed25519 functions at the end of src/proven/sign.c.");
    {
        proven_size_t x = 0, x_good = 0, e = 0, e_good = 0;
        for (proven_size_t i = 0; i < COUNT(CRV_X25519); ++i) {
            split(CRV_X25519[i]);
            proven_byte_t o[64];
            if (g_field[0][0] == 'x') {
                x++;
                proven_x25519_public(o, g_bytes[1]);
                bool ok = memcmp(o, g_bytes[2], 32) == 0;
                ok = ok && proven_x25519(o, g_bytes[1], g_bytes[3]) == PROVEN_OK && memcmp(o, g_bytes[4], 32) == 0;
                static const proven_byte_t small[32] = { 0 };
                ok = ok && proven_x25519(o, g_bytes[1], small) == PROVEN_ERR_UNTRUSTED;
                if (ok) x_good++;
            } else if (g_field[0][0] == 'e') {
                e++;
                proven_ed25519_public(o, g_bytes[1]);
                bool ok = memcmp(o, g_bytes[2], 32) == 0;
                proven_ed25519_sign(o, g_bytes[1], g_bytes[2], V(3));
                ok = ok && memcmp(o, g_bytes[4], 64) == 0 && proven_ed25519_verify(g_bytes[2], V(3), g_bytes[4]) == PROVEN_OK;
                g_bytes[4][40] ^= 1;
                ok = ok && proven_ed25519_verify(g_bytes[2], V(3), g_bytes[4]) == PROVEN_ERR_UNTRUSTED;
                if (ok) e_good++;
            }
        }
        PROVEN_TEST_ASSERT(x > 0 && x_good == x && e > 0 && e_good == e, "every X25519 and Ed25519 case", "");
        PROVEN_TEST_ASSERT(proven_x25519(g_out, NULL, g_out2) == PROVEN_ERR_INVALID_ARG && proven_ed25519_verify(NULL, mv(NULL, 0), g_out) == PROVEN_ERR_INVALID_ARG,
            "null pointers: PROVEN_ERR_INVALID_ARG", "");
    }

    PROVEN_TEST_SECTION("ECDH and ECDSA from a scalar", "Two keys agree both ways on P-256 and P-384; a point off the curve and a zero scalar are PROVEN_ERR_INVALID_FORMAT; ECDSA from a scalar gives a key's own signature.",
        "Check proven_ec_public and proven_ecdh in src/proven/kex.c, and proven_ecdsa_sign_scalar in src/proven/sign.c.");
    {
        for (int curve = 0; curve < 2; ++curve) {
            const proven_pkey_kind_t kind = curve ? PROVEN_PKEY_P384 : PROVEN_PKEY_P256;
            const proven_size_t size = curve ? 48 : 32;
            proven_byte_t a[48], b[48], pa[97], pb[97], s1[48], s2[48];
            for (proven_size_t i = 0; i < size; ++i) { a[i] = (proven_byte_t)(i + 1); b[i] = (proven_byte_t)(0x40 + i); }
            bool ok = proven_ec_public(kind, mv(a, size), mm(pa, sizeof pa)) == PROVEN_OK && proven_ec_public(kind, mv(b, size), mm(pb, sizeof pb)) == PROVEN_OK &&
                      pa[0] == 0x04 && pb[0] == 0x04;
            ok = ok && proven_ecdh(kind, mv(a, size), mv(pb, 1 + 2 * size), mm(s1, size)) == PROVEN_OK &&
                 proven_ecdh(kind, mv(b, size), mv(pa, 1 + 2 * size), mm(s2, size)) == PROVEN_OK && memcmp(s1, s2, size) == 0;
            PROVEN_TEST_ASSERT(ok, curve ? "P-384: the two sides agree" : "P-256: the two sides agree", "");
            pb[size] ^= 1;
            PROVEN_TEST_ASSERT(proven_ecdh(kind, mv(a, size), mv(pb, 1 + 2 * size), mm(s1, size)) == PROVEN_ERR_INVALID_FORMAT &&
                               proven_ecdh(kind, mv(a, size), mv(pa, 1 + 2 * size), mm(s1, size - 1)) == PROVEN_ERR_OUT_OF_BOUNDS &&
                               proven_ec_public(kind, mv(a, size), mm(pa, 2 * size)) == PROVEN_ERR_OUT_OF_BOUNDS,
                "a point off the curve: PROVEN_ERR_INVALID_FORMAT; too little room: PROVEN_ERR_OUT_OF_BOUNDS", "");
            memset(a, 0, sizeof a);
            PROVEN_TEST_ASSERT(proven_ec_public(kind, mv(a, size), mm(pa, sizeof pa)) == PROVEN_ERR_INVALID_FORMAT &&
                               proven_ec_public(PROVEN_PKEY_RSA, mv(a, size), mm(pa, sizeof pa)) == PROVEN_ERR_INVALID_ARG,
                "a zero scalar: PROVEN_ERR_INVALID_FORMAT; an RSA \"curve\": PROVEN_ERR_INVALID_ARG", "");
        }
        /* The same scalar as a key and as itself: the same deterministic signature. */
        proven_byte_t scalar[32], digest[32], rs[64], sig[PROVEN_SIGN_MAX_SIZE], point[65];
        for (int i = 0; i < 32; ++i) scalar[i] = (proven_byte_t)(0xc0 ^ i);
        proven_sha256(mv("a message", 9), digest);
        proven_pkey_t *key = NULL;
        proven_size_t len = 0;
        PROVEN_TEST_ASSERT(proven_pkey_from_ec(proven_heap_allocator(), PROVEN_PKEY_P256, mv(scalar, 32), &key) == PROVEN_OK &&
                           proven_pkey_sign(key, PROVEN_SIGN_ECDSA_RAW, PROVEN_HMAC_SHA256, mv(digest, 32), (proven_rng_t){ 0 }, mm(sig, sizeof sig), &len) == PROVEN_OK &&
                           proven_ecdsa_sign_scalar(PROVEN_PKEY_P256, PROVEN_HMAC_SHA256, mv(scalar, 32), mv(digest, 32), rs) == PROVEN_OK &&
                           len == 64 && memcmp(rs, sig, 64) == 0,
            "ECDSA from the scalar is the key's own signature, byte for byte (RFC 6979)", "");
        PROVEN_TEST_ASSERT(proven_ec_public(PROVEN_PKEY_P256, mv(scalar, 32), mm(point, 65)) == PROVEN_OK &&
                           proven_verify_ecdsa(PROVEN_PKEY_P256, mv(point, 65), PROVEN_SIGN_ECDSA_RAW, mv(digest, 32), mv(rs, 64)) == PROVEN_OK &&
                           proven_ecdsa_sign_scalar(PROVEN_PKEY_P256, PROVEN_HMAC_SHA256, mv(scalar, 32), mv(digest, 20), rs) == PROVEN_OK &&
                           proven_verify_ecdsa(PROVEN_PKEY_P256, mv(point, 65), PROVEN_SIGN_ECDSA_RAW, mv(digest, 20), mv(rs, 64)) == PROVEN_OK,
            "and it verifies; a 20-byte digest (TLS 1.0's SHA-1) is signed and verified too", "");
        PROVEN_TEST_ASSERT(proven_ecdsa_sign_scalar(PROVEN_PKEY_P256, PROVEN_HMAC_SHA256, mv(scalar, 31), mv(digest, 32), rs) == PROVEN_ERR_INVALID_ARG,
            "a scalar of the wrong size: PROVEN_ERR_INVALID_ARG", "");
        proven_pkey_destroy(key);
    }

    PROVEN_TEST_SECTION("finite-field Diffie-Hellman", "Both sides agree over ffdhe2048; only the six primes with generator 2; peer values 0, 1 and p-1 refused.",
        "Check src/proven/dh.c.");
    {
        const proven_mem_view_t p = proven_dh_ffdhe2048();
        static const proven_byte_t two = 2, three = 3;
        PROVEN_TEST_ASSERT(p.size == 256 && proven_dh_known(p, mv(&two, 1)) && !proven_dh_known(p, mv(&three, 1)) && proven_dh_exponent_len(p.size) == 32,
            "ffdhe2048: known with 2, not with 3; a 32-byte exponent", "");
        proven_byte_t x[32], y[32], px[256], py[256], sx[256], sy[256];
        proven_size_t lx = 0, ly = 0;
        for (int i = 0; i < 32; ++i) { x[i] = (proven_byte_t)(i * 5 + 3); y[i] = (proven_byte_t)(i * 11 + 7); }
        PROVEN_TEST_ASSERT(proven_dh_public(p, mv(x, 32), px) == PROVEN_OK && proven_dh_public(p, mv(y, 32), py) == PROVEN_OK &&
                           proven_dh_shared(p, mv(x, 32), mv(py, 256), sx, &lx) == PROVEN_OK && proven_dh_shared(p, mv(y, 32), mv(px, 256), sy, &ly) == PROVEN_OK &&
                           lx == ly && lx > 0 && memcmp(sx, sy, lx) == 0,
            "the two sides agree", "");
        proven_byte_t odd[256];
        memcpy(odd, p.ptr, 256);
        odd[200] ^= 0x10;
        proven_byte_t one[256] = { 0 };
        one[255] = 1;
        proven_byte_t top[256];
        memcpy(top, p.ptr, 256);
        top[255] -= 1;
        PROVEN_TEST_ASSERT(proven_dh_public(mv(odd, 256), mv(x, 32), px) == PROVEN_ERR_UNSUPPORTED &&
                           proven_dh_shared(p, mv(x, 32), mv(one, 256), sx, &lx) == PROVEN_ERR_INVALID_FORMAT && lx == 0 &&
                           proven_dh_shared(p, mv(x, 32), mv(top, 256), sx, &lx) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_dh_public(p, mv(x, 30), px) == PROVEN_ERR_INVALID_ARG,
            "another prime: PROVEN_ERR_UNSUPPORTED; peer 1 and p-1: PROVEN_ERR_INVALID_FORMAT; an exponent not a multiple of 4: PROVEN_ERR_INVALID_ARG", "");
    }

    PROVEN_TEST_SECTION("the hashes' compression functions", "One block of \"abc\", padded by hand, from each initial state gives the hash's own digest.",
        "Check proven_sha256_compress and proven_sha512_compress in src/proven/hash.c, proven_sha1_compress and proven_md5_compress in src/proven/hash_legacy.c.");
    {
        proven_byte_t block[128], want[64], got[64];
        proven_u32 s256[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
        abc_block(block, 64, false);
        proven_sha256_compress(s256, block);
        for (int i = 0; i < 8; ++i) for (int k = 0; k < 4; ++k) got[4 * i + k] = (proven_byte_t)(s256[i] >> (24 - 8 * k));
        proven_sha256(mv("abc", 3), want);
        PROVEN_TEST_ASSERT(memcmp(got, want, 32) == 0, "SHA-256", "");
        proven_u64 s512[8] = { 0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
                               0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull };
        abc_block(block, 128, false);
        proven_sha512_compress(s512, block);
        for (int i = 0; i < 8; ++i) for (int k = 0; k < 8; ++k) got[8 * i + k] = (proven_byte_t)(s512[i] >> (56 - 8 * k));
        proven_sha512(mv("abc", 3), want);
        PROVEN_TEST_ASSERT(memcmp(got, want, 64) == 0, "SHA-512", "");
        proven_u32 s1[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
        abc_block(block, 64, false);
        proven_sha1_compress(s1, block);
        for (int i = 0; i < 5; ++i) for (int k = 0; k < 4; ++k) got[4 * i + k] = (proven_byte_t)(s1[i] >> (24 - 8 * k));
        proven_sha1(mv("abc", 3), want);
        PROVEN_TEST_ASSERT(memcmp(got, want, 20) == 0, "SHA-1", "");
        proven_u32 m5[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
        abc_block(block, 64, true);
        proven_md5_compress(m5, block);
        for (int i = 0; i < 4; ++i) for (int k = 0; k < 4; ++k) got[4 * i + k] = (proven_byte_t)(m5[i] >> (8 * k));
        proven_md5(mv("abc", 3), want);
        PROVEN_TEST_ASSERT(memcmp(got, want, 16) == 0, "MD5 (its words little-endian)", "");
    }

    PROVEN_TEST_PASS("The AEADs, ciphers, exchanges and block functions behave as documented.");
    return 0;
}
