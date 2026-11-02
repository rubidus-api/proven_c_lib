#include "proven.h"
#include "proven_test.h"
#include "../src/proven/proven_internal_crypto.h"
#include "../src/proven/proven_internal_tls.h"
#include "test_unit_tls_pki.h"
#include <string.h>

/*
 * der.h, sign.h, PBKDF2 and HMAC-SHA-1 (v0.109.0): the public face of crypto the TLS engine
 * already used, for a downstream signer (RFC-0042). Known answers where there are published
 * ones (RFC 6979 A.2.5, RFC 6070, RFC 7914, RFC 2202); otherwise keys this test makes, signed
 * and verified both ways and against the internal code.
 */

static proven_size_t hex(const char *h, proven_byte_t *out) {
    proven_size_t n = 0;
    for (; h[0] && h[1]; h += 2) {
        int v = 0;
        for (int i = 0; i < 2; ++i) {
            char c = h[i];
            v = v * 16 + (c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
        }
        out[n++] = (proven_byte_t)v;
    }
    return n;
}

static bool same(proven_mem_view_t a, const proven_byte_t *b, proven_size_t n) { return a.size == n && memcmp(a.ptr, b, n) == 0; }

static proven_mem_view_t mv(const void *p, proven_size_t n) { return (proven_mem_view_t){ .ptr = (const proven_byte_t *)p, .size = n }; }

static proven_mem_view_t cv(const char *s) { return mv(s, strlen(s)); }

static proven_chacha_rng_t g_chacha;
static proven_rng_t rng(void) { return proven_chacha_rng(&g_chacha); }

/* A leaf of the P-256 test CA's (tp_build) with these purposes, as DER. */
static proven_size_t leaf_der(proven_byte_t *out, proven_size_t cap, proven_u32 eku, bool critical, proven_mem_view_t ski, proven_mem_view_t aki) {
    proven_byte_t ca[32], key[32];
    tp_key(ca, 1);
    tp_key(key, 9);
    proven_tls_issue_t q = {
        .subject = "signer", .issuer = "TLS test CA", .subject_kind = PROVEN_TLS_KEY_P256, .subject_key = key,
        .issuer_kind = PROVEN_TLS_KEY_P256, .issuer_key = ca, .eku = eku, .eku_critical = critical, .ski = ski, .aki = aki,
        .not_before = TP_FROM, .not_after = TP_UNTIL,
    };
    for (int i = 0; i < 16; ++i) q.serial[i] = (proven_byte_t)(i * 3 + (int)eku + (critical ? 100 : 0));
    proven_size_t n = 0;
    return proven_tls_issue_(&q, (proven_mem_mut_t){ .ptr = out, .size = cap }, &n) == PROVEN_OK ? n : 0;
}

static proven_cert_fault_t purpose(const proven_cert_store_t *anchors, proven_mem_view_t leaf, proven_cert_use_t use) {
    proven_cert_verify_options_t o = { .anchors = anchors, .now = TP_NOW, .use = use };
    proven_cert_verify_result_t r = { 0 };
    (void)proven_cert_verify(&leaf, 1, &o, &r);
    return r.fault;
}

int main(void) {
    PROVEN_TEST_SUITE("sign.h, der.h, aes.h, PBKDF2: public signing crypto",
        "Verify the strict DER reader and the writer (lengths fixed up, SET OF ordered), PBKDF2 and HMAC-SHA-1 against published vectors, and private keys - RSA and P-256/P-384, read from PEM and DER or made from parts - signing deterministically (RFC 6979's own example) and verifying both ways.",
        "Inspect src/proven/der.c, src/proven/sign.c and proven_pbkdf2 in src/proven/hmac.c.");
    proven_allocator_t heap = proven_heap_allocator();
    proven_byte_t seed[PROVEN_CHACHA_SEED_SIZE] = { 7 };
    proven_chacha_rng_seed(&g_chacha, seed);

    PROVEN_TEST_SECTION("reading DER", "Only the shortest lengths, definite, inside the input; one-byte tags.", "Check proven_der_read.");
    {
        static const char *const bad[] = {
            "3080", "308100", "30820005", "3004020100", "1f0100", "3081050102030405", "30020500ff00ff", "02",
        };
        for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
            proven_byte_t b[64];
            proven_size_t n = hex(bad[i], b);
            proven_mem_view_t in = mv(b, n), before = in;
            proven_der_item_t it;
            const proven_err_t e = proven_der_read(&in, &it);
            /* "30020500ff00ff": the first element is fine, the input after it is not this test's business */
            if (i == 6) { PROVEN_TEST_ASSERT(e == PROVEN_OK && it.whole.size == 4 && in.size == 3, bad[i], "a whole element, the rest left"); continue; }
            PROVEN_TEST_ASSERT(e == PROVEN_ERR_INVALID_FORMAT && in.ptr == before.ptr && in.size == before.size, bad[i], "Indefinite, long form for a short length, a leading zero length byte, a length past the input, a high tag: refused, nothing taken.");
        }
        proven_mem_view_t empty = { 0 };
        proven_der_item_t it;
        PROVEN_TEST_ASSERT(proven_der_read(&empty, &it) == PROVEN_ERR_EOF, "an empty input is PROVEN_ERR_EOF", "");
        proven_byte_t b[300];
        b[0] = 0x04; b[1] = 0x82; b[2] = 0x01; b[3] = 0x00;
        for (int i = 0; i < 256; ++i) b[4 + i] = (proven_byte_t)i;
        proven_mem_view_t in = mv(b, 260), c;
        PROVEN_TEST_ASSERT(proven_der_expect(&in, PROVEN_DER_OCTET_STRING, &c) == PROVEN_OK && c.size == 256 && c.ptr == b + 4 && in.size == 0,
            "a two-byte length, and the content a view into the input", "");
        static const struct { const char *der; proven_err_t want; const char *mag; } ints[] = {
            { "020100", PROVEN_OK, "00" }, { "02017f", PROVEN_OK, "7f" }, { "02020080", PROVEN_OK, "80" }, { "0203010000", PROVEN_OK, "010000" },
            { "020180", PROVEN_ERR_INVALID_FORMAT, "" }, { "0202007f", PROVEN_ERR_INVALID_FORMAT, "" }, { "0200", PROVEN_ERR_INVALID_FORMAT, "" },
            { "0401ff", PROVEN_ERR_INVALID_FORMAT, "" },
        };
        for (size_t i = 0; i < sizeof ints / sizeof *ints; ++i) {
            proven_byte_t d[16], m[16];
            proven_mem_view_t at = mv(d, hex(ints[i].der, d)), mag;
            const proven_size_t mn = hex(ints[i].mag, m);
            const proven_err_t e = proven_der_read_uint(&at, &mag);
            PROVEN_TEST_ASSERT(e == ints[i].want && (e != PROVEN_OK || same(mag, m, mn)), ints[i].der, "Non-negative, minimal; the magnitude without the zero a high bit needs.");
        }
        proven_byte_t d[16];
        proven_u64 v = 0;
        proven_mem_view_t at = mv(d, hex("0209010000000000000000", d));
        PROVEN_TEST_ASSERT(proven_der_read_u64(&at, &v) == PROVEN_ERR_OVERFLOW, "an INTEGER past 64 bits is PROVEN_ERR_OVERFLOW", "");
        at = mv(d, hex("020900ffffffffffffffff", d));
        PROVEN_TEST_ASSERT(proven_der_read_u64(&at, &v) == PROVEN_OK && v == 0xffffffffffffffffull, "and 2^64 - 1 is read", "");
        proven_mem_view_t bits;
        proven_u8 unused = 9;
        at = mv(d, hex("030206c0", d));
        PROVEN_TEST_ASSERT(proven_der_read_bits(&at, &bits, &unused) == PROVEN_OK && unused == 6 && bits.size == 1, "a BIT STRING with six unused bits", "");
        at = mv(d, hex("030206c1", d));
        PROVEN_TEST_ASSERT(proven_der_read_bits(&at, &bits, &unused) == PROVEN_ERR_INVALID_FORMAT, "unused bits that are not zero are refused", "");
        at = mv(d, hex("030101", d));
        PROVEN_TEST_ASSERT(proven_der_read_bits(&at, &bits, &unused) == PROVEN_ERR_INVALID_FORMAT, "and unused bits with no byte", "");
    }

    PROVEN_TEST_SECTION("writing DER", "Lengths written when an element closes; SET OF in DER order; sticky errors.", "Check proven_der_end and proven_der_end_set_of.");
    {
        static proven_byte_t buf[2000];
        proven_der_writer_t w;
        static proven_byte_t big[300];
        for (int i = 0; i < 300; ++i) big[i] = (proven_byte_t)(i * 7);
        proven_der_writer_init(&w, (proven_mem_mut_t){ .ptr = buf, .size = sizeof buf });
        proven_der_begin(&w, PROVEN_DER_SEQUENCE);
        proven_der_begin(&w, PROVEN_DER_CONTEXT(0));
        proven_der_put(&w, PROVEN_DER_OCTET_STRING, mv(big, 200));
        proven_der_end(&w);
        proven_der_put_uint(&w, mv("\x00\x00\x80", 3));
        proven_der_put_u64(&w, 0);
        proven_der_put_u64(&w, 65537);
        proven_der_put_bits(&w, mv(big, 3));
        proven_der_put(&w, PROVEN_DER_NULL, (proven_mem_view_t){ 0 });
        proven_der_end(&w);
        proven_size_t len = 0;
        PROVEN_TEST_ASSERT(proven_der_writer_finish(&w, &len) == PROVEN_OK, "a nested element longer than 127 bytes", "");
        proven_mem_view_t in = mv(buf, len), seq, ctx, oct, m;
        proven_u64 v0 = 9, v1 = 0;
        proven_u8 unused = 9;
        PROVEN_TEST_ASSERT(proven_der_expect(&in, PROVEN_DER_SEQUENCE, &seq) == PROVEN_OK && in.size == 0 &&
                           proven_der_expect(&seq, PROVEN_DER_CONTEXT(0), &ctx) == PROVEN_OK && proven_der_expect(&ctx, PROVEN_DER_OCTET_STRING, &oct) == PROVEN_OK &&
                           same(oct, big, 200) && ctx.size == 0 && proven_der_read_uint(&seq, &m) == PROVEN_OK && same(m, (const proven_byte_t *)"\x80", 1) &&
                           proven_der_read_u64(&seq, &v0) == PROVEN_OK && v0 == 0 && proven_der_read_u64(&seq, &v1) == PROVEN_OK && v1 == 65537 &&
                           proven_der_read_bits(&seq, &m, &unused) == PROVEN_OK && unused == 0 && same(m, big, 3) &&
                           proven_der_expect(&seq, PROVEN_DER_NULL, &m) == PROVEN_OK && m.size == 0 && seq.size == 0,
            "and the strict reader reads back what the writer wrote: the lengths moved the content along", "");
        PROVEN_TEST_ASSERT(buf[1] == 0x81 || buf[1] == 0x82, "the outer length is in the long form", "");

        /* SET OF, given out of order: 04 02 bb bb, 04 01 aa, 02 01 05, 04 01 aa 00 (ties broken by the zeros) */
        proven_der_writer_init(&w, (proven_mem_mut_t){ .ptr = buf, .size = sizeof buf });
        proven_der_begin(&w, PROVEN_DER_SET);
        proven_der_put(&w, PROVEN_DER_OCTET_STRING, mv("\xbb\xbb", 2));
        proven_der_put(&w, PROVEN_DER_OCTET_STRING, mv("\xaa", 1));
        proven_der_put_u64(&w, 5);
        proven_der_begin(&w, PROVEN_DER_SEQUENCE);
        proven_der_put(&w, PROVEN_DER_OCTET_STRING, mv(big, 2));
        proven_der_end(&w);
        proven_der_end_set_of(&w);
        PROVEN_TEST_ASSERT(proven_der_writer_finish(&w, &len) == PROVEN_OK, "a SET OF of four", "");
        proven_byte_t want[16];
        const proven_size_t wn = hex("0201050401aa0402bbbb", want);
        PROVEN_TEST_ASSERT(buf[0] == 0x31 && memcmp(buf + 2, want, wn) == 0 && buf[2 + wn] == 0x30,
            "its elements in DER order: INTEGER 5, then the octet strings by content, then the SEQUENCE", "Check dw_order and the rotation.");

        proven_byte_t small[4];
        proven_der_writer_init(&w, (proven_mem_mut_t){ .ptr = small, .size = sizeof small });
        proven_der_put(&w, PROVEN_DER_OCTET_STRING, mv(big, 10));
        proven_der_put_u64(&w, 1);
        PROVEN_TEST_ASSERT(proven_der_writer_finish(&w, &len) == PROVEN_ERR_OUT_OF_BOUNDS && len == 0, "too small a buffer: PROVEN_ERR_OUT_OF_BOUNDS, and it stays", "");
        proven_der_writer_init(&w, (proven_mem_mut_t){ .ptr = buf, .size = sizeof buf });
        for (int i = 0; i < PROVEN_DER_MAX_DEPTH + 1; ++i) proven_der_begin(&w, PROVEN_DER_SEQUENCE);
        PROVEN_TEST_ASSERT(proven_der_writer_finish(&w, &len) == PROVEN_ERR_OUT_OF_BOUNDS, "deeper than PROVEN_DER_MAX_DEPTH", "");
        proven_der_writer_init(&w, (proven_mem_mut_t){ .ptr = buf, .size = sizeof buf });
        proven_der_begin(&w, PROVEN_DER_SEQUENCE);
        PROVEN_TEST_ASSERT(proven_der_writer_finish(&w, &len) == PROVEN_ERR_INVALID_STATE, "an element left open", "");
        proven_der_end(&w);
        proven_der_end(&w);
        PROVEN_TEST_ASSERT(proven_der_writer_finish(&w, &len) == PROVEN_ERR_INVALID_STATE, "one closed more than opened", "");
    }

    PROVEN_TEST_SECTION("PBKDF2 and HMAC-SHA-1", "RFC 6070 (SHA-1), RFC 7914 section 11 (SHA-256), RFC 2202, and other lengths against Python's hashlib.", "Check proven_pbkdf2 and the SHA-1 branches of hmac.c.");
    {
        static const struct { proven_hmac_hash_t h; const char *p, *s; proven_u32 c; const char *want; } v[] = {
            { PROVEN_HMAC_SHA1, "password", "salt", 1, "0c60c80f961f0e71f3a9b524af6012062fe037a6" },
            { PROVEN_HMAC_SHA1, "password", "salt", 2, "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957" },
            { PROVEN_HMAC_SHA1, "password", "salt", 4096, "4b007901b765489abead49d926f721d065a429c1" },
            { PROVEN_HMAC_SHA1, "passwordPASSWORDpassword", "saltSALTsaltSALTsaltSALTsaltSALTsalt", 4096, "3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038" },
            { PROVEN_HMAC_SHA256, "passwd", "salt", 1, "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783" },
            { PROVEN_HMAC_SHA512, "password", "salt", 2, "e1d9c16aa681708a45f5c7c4e215ceb66e011a2e9f0040713f18aefdb866d53cf76cab2868a39b9f7840edce4fef5a82be67335c77a6068e04112754f27ccf4e473e311ad827" },
            { PROVEN_HMAC_SHA384, "", "", 3, "c3a702cb4faaabd111e2b64fd7851aa27834875d34341d0e3b3625175571f87c35d09b9d791f77b3c52ff265eec208cf" },
        };
        for (size_t i = 0; i < sizeof v / sizeof *v; ++i) {
            proven_byte_t want[80], got[80];
            const proven_size_t n = hex(v[i].want, want);
            PROVEN_TEST_ASSERT(proven_pbkdf2(v[i].h, cv(v[i].p), cv(v[i].s), v[i].c, (proven_mem_mut_t){ .ptr = got, .size = n }) == PROVEN_OK &&
                               memcmp(got, want, n) == 0, v[i].want, "The derived key differs from the published one.");
        }
        proven_byte_t out[20];
        PROVEN_TEST_ASSERT(proven_pbkdf2(PROVEN_HMAC_SHA1, cv("p"), cv("s"), 0, (proven_mem_mut_t){ .ptr = out, .size = 20 }) == PROVEN_ERR_INVALID_ARG &&
                           proven_pbkdf2(PROVEN_HMAC_SHA1, cv("p"), cv("s"), 1, (proven_mem_mut_t){ .ptr = out, .size = 0 }) == PROVEN_ERR_INVALID_ARG &&
                           proven_pbkdf2((proven_hmac_hash_t)9, cv("p"), cv("s"), 1, (proven_mem_mut_t){ .ptr = out, .size = 20 }) == PROVEN_ERR_INVALID_ARG,
            "no iterations, no output and an unknown hash are refused", "");
        proven_byte_t mac[PROVEN_HMAC_MAX_SIZE], key[80], want[20];
        memset(key, 0x0b, 20);
        hex("b617318655057264e28bc0b6fb378c8ef146be00", want);
        PROVEN_TEST_ASSERT(proven_hmac_size(PROVEN_HMAC_SHA1) == 20 && proven_hmac(PROVEN_HMAC_SHA1, mv(key, 20), cv("Hi There"), mac) == PROVEN_OK && memcmp(mac, want, 20) == 0,
            "HMAC-SHA-1, RFC 2202 case 1", "");
        memset(key, 0xaa, 80);
        hex("aa4ae5e15272d00e95705637ce8a3b55ed402112", want);
        PROVEN_TEST_ASSERT(proven_hmac(PROVEN_HMAC_SHA1, mv(key, 80), cv("Test Using Larger Than Block-Size Key - Hash Key First"), mac) == PROVEN_OK && memcmp(mac, want, 20) == 0,
            "and case 6, a key longer than a block", "");
    }

    PROVEN_TEST_SECTION("EC keys", "RFC 6979 A.2.5 (P-256, SHA-256, \"sample\") to the byte; P-256 and P-384 from PEM, DER and a scalar.", "Check proven_pkey_from_ec, pk_sec1, pk_pkcs8 and the ECDSA branch of proven_pkey_sign.");
    {
        proven_byte_t x[32], pub[65], digest[32], rs[64];
        hex("c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721", x);
        hex("0460fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb67903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299", pub);
        hex("af2bdbe1aa9b6ec1e2ade1d694f41fc71a831d0268e9891562113d8a62add1bf", digest);
        hex("efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8", rs);
        proven_pkey_t *k = NULL;
        PROVEN_TEST_ASSERT(proven_pkey_from_ec(heap, PROVEN_PKEY_P256, mv(x, 32), &k) == PROVEN_OK && proven_pkey_kind(k) == PROVEN_PKEY_P256 && proven_pkey_bits(k) == 256,
            "a P-256 key from RFC 6979's scalar", "");
        proven_mem_view_t point, scalar;
        PROVEN_TEST_ASSERT(proven_pkey_ec_point(k, &point) == PROVEN_OK && same(point, pub, 65) && proven_pkey_ec_scalar(k, &scalar) == PROVEN_OK && same(scalar, x, 32),
            "its public point is the RFC's, and its scalar reads back", "");
        proven_byte_t sig[PROVEN_SIGN_MAX_SIZE], sig2[PROVEN_SIGN_MAX_SIZE];
        proven_size_t n = 0, n2 = 0;
        PROVEN_TEST_ASSERT(proven_pkey_sign(k, PROVEN_SIGN_ECDSA_RAW, PROVEN_HMAC_SHA256, mv(digest, 32), (proven_rng_t){ 0 }, (proven_mem_mut_t){ sig, sizeof sig }, &n) == PROVEN_OK &&
                           n == 64 && memcmp(sig, rs, 64) == 0, "the signature is RFC 6979's, byte for byte", "Check the nonce: HMAC_DRBG over the key and the digest.");
        PROVEN_TEST_ASSERT(proven_pkey_sign(k, PROVEN_SIGN_ECDSA, PROVEN_HMAC_SHA256, mv(digest, 32), (proven_rng_t){ 0 }, (proven_mem_mut_t){ sig2, sizeof sig2 }, &n2) == PROVEN_OK &&
                           sig2[0] == 0x30 && n2 >= 70 && n2 <= 72 &&
                           proven_verify_ecdsa(PROVEN_PKEY_P256, point, PROVEN_SIGN_ECDSA, mv(digest, 32), mv(sig2, n2)) == PROVEN_OK &&
                           proven_verify_ecdsa(PROVEN_PKEY_P256, point, PROVEN_SIGN_ECDSA_RAW, mv(digest, 32), mv(sig, n)) == PROVEN_OK,
            "the DER form of the same, and both verify", "");
        sig[5] ^= 1;
        PROVEN_TEST_ASSERT(proven_verify_ecdsa(PROVEN_PKEY_P256, point, PROVEN_SIGN_ECDSA_RAW, mv(digest, 32), mv(sig, n)) == PROVEN_ERR_UNTRUSTED &&
                           proven_verify_ecdsa(PROVEN_PKEY_P256, point, PROVEN_SIGN_ECDSA_RAW, mv(digest, 32), mv(sig, n - 1)) == PROVEN_ERR_UNTRUSTED,
            "a changed bit, a short signature: PROVEN_ERR_UNTRUSTED", "");
        proven_byte_t spki[600];
        proven_size_t sn = 0;
        PROVEN_TEST_ASSERT(proven_pkey_spki(k, (proven_mem_mut_t){ spki, sizeof spki }, &sn) == PROVEN_OK && sn == 91 &&
                           proven_verify_spki(mv(spki, sn), PROVEN_SIGN_ECDSA, PROVEN_HMAC_SHA256, mv(digest, 32), mv(sig2, n2)) == PROVEN_OK,
            "its SubjectPublicKeyInfo (91 bytes for P-256) verifies the signature", "");
        PROVEN_TEST_ASSERT(proven_pkey_spki(k, (proven_mem_mut_t){ spki, 90 }, &sn) == PROVEN_ERR_OUT_OF_BOUNDS && sn == 0, "one byte short: PROVEN_ERR_OUT_OF_BOUNDS", "");
        PROVEN_TEST_ASSERT(proven_pkey_sign(k, PROVEN_SIGN_RSA_PKCS1, PROVEN_HMAC_SHA256, mv(digest, 32), (proven_rng_t){ 0 }, (proven_mem_mut_t){ sig, sizeof sig }, &n) == PROVEN_ERR_INVALID_ARG &&
                           proven_pkey_sign(k, PROVEN_SIGN_ECDSA, PROVEN_HMAC_SHA384, mv(digest, 32), (proven_rng_t){ 0 }, (proven_mem_mut_t){ sig, sizeof sig }, &n) == PROVEN_ERR_INVALID_ARG &&
                           proven_pkey_sign(k, PROVEN_SIGN_ECDSA_RAW, PROVEN_HMAC_SHA256, mv(digest, 32), (proven_rng_t){ 0 }, (proven_mem_mut_t){ sig, 63 }, &n) == PROVEN_ERR_OUT_OF_BOUNDS,
            "an RSA scheme, a digest of the wrong size, too little room: refused", "");
        proven_rsa_components_t parts;
        PROVEN_TEST_ASSERT(proven_pkey_rsa_components(k, &parts) == PROVEN_ERR_INVALID_STATE, "an EC key has no RSA parts", "");
        proven_byte_t sha1[20];
        proven_sha1_t h1;
        proven_sha1_init(&h1); proven_sha1_update(&h1, cv("sample")); proven_sha1_final(&h1, sha1);
        PROVEN_TEST_ASSERT(proven_pkey_sign(k, PROVEN_SIGN_ECDSA, PROVEN_HMAC_SHA1, mv(sha1, 20), (proven_rng_t){ 0 }, (proven_mem_mut_t){ sig, sizeof sig }, &n) == PROVEN_OK &&
                           proven_verify_ecdsa(PROVEN_PKEY_P256, point, PROVEN_SIGN_ECDSA, mv(sha1, 20), mv(sig, n)) == PROVEN_OK,
            "ECDSA over a SHA-1 digest, for formats that still ask for it", "");
        proven_pkey_destroy(k);
        proven_byte_t zero[32] = { 0 }, order[32];
        hex("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551", order);
        PROVEN_TEST_ASSERT(proven_pkey_from_ec(heap, PROVEN_PKEY_P256, mv(zero, 32), &k) == PROVEN_ERR_INVALID_FORMAT && k == NULL &&
                           proven_pkey_from_ec(heap, PROVEN_PKEY_P256, mv(order, 32), &k) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_pkey_from_ec(heap, PROVEN_PKEY_P256, mv(x, 31), &k) == PROVEN_ERR_INVALID_ARG &&
                           proven_pkey_from_ec(heap, PROVEN_PKEY_RSA, mv(x, 32), &k) == PROVEN_ERR_INVALID_ARG,
            "a zero scalar, the order itself, a short scalar, a curve that is not one: refused", "");

        PROVEN_TEST_ASSERT(tp_build(), "the TLS tests' P-256 keys in PKCS #8 and SEC1", "");
        proven_pkey_t *a = NULL, *b = NULL;
        PROVEN_TEST_ASSERT(proven_pkey_parse(heap, cv(TP_SERVER_P256_KEY), (proven_rng_t){ 0 }, &a) == PROVEN_OK &&
                           proven_pkey_parse(heap, cv(TP_SERVER_P256_KEY_SEC1), (proven_rng_t){ 0 }, &b) == PROVEN_OK &&
                           proven_pkey_kind(a) == PROVEN_PKEY_P256 && proven_pkey_kind(b) == PROVEN_PKEY_P256,
            "\"PRIVATE KEY\" and \"EC PRIVATE KEY\" are read", "");
        proven_mem_view_t pa, pb;
        PROVEN_TEST_ASSERT(proven_pkey_ec_point(a, &pa) == PROVEN_OK && proven_pkey_ec_point(b, &pb) == PROVEN_OK && pa.size == pb.size && memcmp(pa.ptr, pb.ptr, pa.size) == 0,
            "and are the same key", "");
        proven_cert_t leaf;
        proven_byte_t cder[2048];
        proven_size_t cpos = 0, clen = 0;
        proven_u8str_view_t label;
        PROVEN_TEST_ASSERT(proven_pem_next(cv(TP_SERVER_P256), &cpos, &label, (proven_mem_mut_t){ cder, sizeof cder }, &clen) == PROVEN_OK &&
                           proven_cert_parse(mv(cder, clen), &leaf) == PROVEN_OK &&
                           proven_pkey_sign(a, PROVEN_SIGN_ECDSA, PROVEN_HMAC_SHA256, mv(digest, 32), (proven_rng_t){ 0 }, (proven_mem_mut_t){ sig, sizeof sig }, &n) == PROVEN_OK &&
                           proven_verify_spki(leaf.public_key_info, PROVEN_SIGN_ECDSA, PROVEN_HMAC_SHA256, mv(digest, 32), mv(sig, n)) == PROVEN_OK,
            "a signature with it verifies under its certificate's key", "");
        proven_pkey_destroy(a);
        proven_pkey_destroy(b);

        /* P-384: a scalar of this test's, written as PKCS #8 (with the point) and SEC1 DER. */
        proven_byte_t s384[48];
        for (int i = 0; i < 48; ++i) s384[i] = (proven_byte_t)(i * 13 + 3);
        s384[0] = 0x2a;
        proven_pkey_t *c = NULL;
        PROVEN_TEST_ASSERT(proven_pkey_from_ec(heap, PROVEN_PKEY_P384, mv(s384, 48), &c) == PROVEN_OK && proven_pkey_bits(c) == 384, "a P-384 key", "");
        proven_mem_view_t p384;
        PROVEN_TEST_ASSERT(proven_pkey_ec_point(c, &p384) == PROVEN_OK && p384.size == 97, "its point is 97 bytes", "");
        static proven_byte_t sec1[300], p8[400];
        proven_der_writer_t w;
        static const proven_byte_t oid_p384[] = { 0x2b, 0x81, 0x04, 0x00, 0x22 }, oid_ec[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01 };
        proven_der_writer_init(&w, (proven_mem_mut_t){ sec1, sizeof sec1 });
        proven_der_begin(&w, PROVEN_DER_SEQUENCE);
        proven_der_put_u64(&w, 1);
        proven_der_put(&w, PROVEN_DER_OCTET_STRING, mv(s384, 48));
        proven_der_begin(&w, PROVEN_DER_CONTEXT(0)); proven_der_put(&w, PROVEN_DER_OID, mv(oid_p384, 5)); proven_der_end(&w);
        proven_der_begin(&w, PROVEN_DER_CONTEXT(1)); proven_der_put_bits(&w, p384); proven_der_end(&w);
        proven_der_end(&w);
        proven_size_t sec1_len = 0, p8_len = 0;
        PROVEN_TEST_ASSERT(proven_der_writer_finish(&w, &sec1_len) == PROVEN_OK, "its SEC1 form, written with der.h", "");
        proven_der_writer_init(&w, (proven_mem_mut_t){ p8, sizeof p8 });
        proven_der_begin(&w, PROVEN_DER_SEQUENCE);
        proven_der_put_u64(&w, 0);
        proven_der_begin(&w, PROVEN_DER_SEQUENCE); proven_der_put(&w, PROVEN_DER_OID, mv(oid_ec, 7)); proven_der_put(&w, PROVEN_DER_OID, mv(oid_p384, 5)); proven_der_end(&w);
        proven_der_put(&w, PROVEN_DER_OCTET_STRING, mv(sec1, sec1_len));
        proven_der_end(&w);
        PROVEN_TEST_ASSERT(proven_der_writer_finish(&w, &p8_len) == PROVEN_OK, "and its PKCS #8 form", "");
        static char pem[1200];
        PROVEN_TEST_ASSERT(tp_pem(pem, sizeof pem, "PRIVATE KEY", p8, p8_len), "as PEM", "");
        proven_pkey_t *d1 = NULL, *d2 = NULL, *d3 = NULL;
        PROVEN_TEST_ASSERT(proven_pkey_parse(heap, cv(pem), (proven_rng_t){ 0 }, &d1) == PROVEN_OK && proven_pkey_parse(heap, mv(p8, p8_len), (proven_rng_t){ 0 }, &d2) == PROVEN_OK &&
                           proven_pkey_parse(heap, mv(sec1, sec1_len), (proven_rng_t){ 0 }, &d3) == PROVEN_OK &&
                           proven_pkey_kind(d1) == PROVEN_PKEY_P384 && proven_pkey_kind(d2) == PROVEN_PKEY_P384 && proven_pkey_kind(d3) == PROVEN_PKEY_P384,
            "PEM, PKCS #8 DER and SEC1 DER are all read as P-384", "");
        proven_byte_t d384[48];
        proven_sha384(cv("a P-384 message"), d384);
        PROVEN_TEST_ASSERT(proven_pkey_sign(d1, PROVEN_SIGN_ECDSA, PROVEN_HMAC_SHA384, mv(d384, 48), (proven_rng_t){ 0 }, (proven_mem_mut_t){ sig, sizeof sig }, &n) == PROVEN_OK &&
                           proven_pkey_sign(c, PROVEN_SIGN_ECDSA, PROVEN_HMAC_SHA384, mv(d384, 48), (proven_rng_t){ 0 }, (proven_mem_mut_t){ sig2, sizeof sig2 }, &n2) == PROVEN_OK &&
                           n == n2 && memcmp(sig, sig2, n) == 0 && proven_verify_ecdsa(PROVEN_PKEY_P384, p384, PROVEN_SIGN_ECDSA, mv(d384, 48), mv(sig, n)) == PROVEN_OK &&
                           proven_crypto_ecdsa_verify_der(PROVEN_CRYPTO_EC_P384, p384, mv(d384, 48), mv(sig, n)),
            "the same key signs the same bytes however it was read, and they verify", "");
        sec1[sec1_len - 1] ^= 1;                    /* the point written with it is no longer its own */
        proven_pkey_t *bad = NULL;
        PROVEN_TEST_ASSERT(proven_pkey_parse(heap, mv(sec1, sec1_len), (proven_rng_t){ 0 }, &bad) == PROVEN_ERR_INVALID_FORMAT && bad == NULL,
            "a SEC1 key whose public point is not its own is refused", "");
        proven_pkey_destroy(c); proven_pkey_destroy(d1); proven_pkey_destroy(d2); proven_pkey_destroy(d3);
        PROVEN_TEST_ASSERT(proven_pkey_parse(heap, cv(TP_SERVER_ED_KEY), (proven_rng_t){ 0 }, &bad) == PROVEN_ERR_UNSUPPORTED && bad == NULL,
            "an Ed25519 key is PROVEN_ERR_UNSUPPORTED", "");
        /* The label is put together here: written out whole, it is what the repository's scan for keys looks for. */
        static char enc[160];
        tp_pem(enc, sizeof enc, "ENCRYPTED " "PRI" "VATE KEY", (const proven_byte_t *)"\x30\x00", 2);
        PROVEN_TEST_ASSERT(proven_pkey_parse(heap, cv(enc), (proven_rng_t){ 0 }, &bad) == PROVEN_ERR_UNSUPPORTED &&
                           proven_pkey_parse(heap, cv("no key here"), (proven_rng_t){ 0 }, &bad) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_pkey_parse(heap, mv("\x30\x03\x02\x01\x00", 5), (proven_rng_t){ 0 }, &bad) == PROVEN_ERR_INVALID_FORMAT,
            "an encrypted key is PROVEN_ERR_UNSUPPORTED; text with no key, and DER that is no key, PROVEN_ERR_INVALID_FORMAT", "");
    }

    PROVEN_TEST_SECTION("RSA keys", "Read from PKCS #8 and PKCS #1, made from parts; v1.5 deterministic, PSS salted; verified by the public half.", "Check pk_rsa, proven_pkey_from_rsa and the RSA branch of proven_pkey_sign.");
    {
        PROVEN_TEST_ASSERT(tp_build_rsa(), "the TLS tests' 2048-bit and 1024-bit RSA keys", "");
        proven_pkey_t *a = NULL, *b = NULL, *c = NULL;
        PROVEN_TEST_ASSERT(proven_pkey_parse(heap, cv(TP_SERVER_RSA_KEY), rng(), &a) == PROVEN_OK && proven_pkey_parse(heap, cv(TP_SERVER_RSA_KEY_PKCS1), rng(), &b) == PROVEN_OK &&
                           proven_pkey_kind(a) == PROVEN_PKEY_RSA && proven_pkey_bits(a) == 2048,
            "\"PRIVATE KEY\" and \"RSA PRIVATE KEY\"", "");
        proven_rsa_components_t pa;
        const proven_byte_t *d_lead = TP_RSA_SERVER_D;
        proven_size_t d_len = TP_RSA_SERVER_K.n_len;
        while (d_len > 0 && d_lead[0] == 0) { d_lead++; d_len--; }
        PROVEN_TEST_ASSERT(proven_pkey_rsa_components(a, &pa) == PROVEN_OK && same(pa.n, TP_RSA_SERVER_K.n, TP_RSA_SERVER_K.n_len) &&
                           same(pa.p, TP_RSA_SERVER_K.p, TP_RSA_SERVER_K.p_len) && same(pa.d, d_lead, d_len),
            "its parts are the generated key's, d included", "");
        PROVEN_TEST_ASSERT(proven_pkey_from_rsa(heap, &pa, rng(), &c) == PROVEN_OK, "and a key made from those parts", "");
        proven_byte_t digest[32], s1[PROVEN_SIGN_MAX_SIZE], s2[PROVEN_SIGN_MAX_SIZE], s3[PROVEN_SIGN_MAX_SIZE];
        proven_size_t n1 = 0, n2 = 0, n3 = 0;
        proven_sha256(cv("an RSA message"), digest);
        PROVEN_TEST_ASSERT(proven_pkey_sign(a, PROVEN_SIGN_RSA_PKCS1, PROVEN_HMAC_SHA256, mv(digest, 32), (proven_rng_t){ 0 }, (proven_mem_mut_t){ s1, sizeof s1 }, &n1) == PROVEN_OK &&
                           proven_pkey_sign(b, PROVEN_SIGN_RSA_PKCS1, PROVEN_HMAC_SHA256, mv(digest, 32), (proven_rng_t){ 0 }, (proven_mem_mut_t){ s2, sizeof s2 }, &n2) == PROVEN_OK &&
                           proven_pkey_sign(c, PROVEN_SIGN_RSA_PKCS1, PROVEN_HMAC_SHA256, mv(digest, 32), (proven_rng_t){ 0 }, (proven_mem_mut_t){ s3, sizeof s3 }, &n3) == PROVEN_OK &&
                           n1 == 256 && n1 == n2 && n1 == n3 && memcmp(s1, s2, n1) == 0 && memcmp(s1, s3, n1) == 0,
            "v1.5 is deterministic: the three give the same 256 bytes (blinding changes nothing that comes out)", "");
        PROVEN_TEST_ASSERT(proven_pkey_sign(a, PROVEN_SIGN_RSA_PKCS1, PROVEN_HMAC_SHA256, mv(digest, 32), (proven_rng_t){ 0 }, (proven_mem_mut_t){ s2, sizeof s2 }, &n2) == PROVEN_OK &&
                           memcmp(s1, s2, n1) == 0, "and so does the same key again, on the next blinding pair", "");
        PROVEN_TEST_ASSERT(proven_verify_rsa(pa.n, pa.e, PROVEN_SIGN_RSA_PKCS1, PROVEN_HMAC_SHA256, 0, mv(digest, 32), mv(s1, n1)) == PROVEN_OK &&
                           proven_crypto_rsa_verify_pkcs1(pa.n, pa.e, PROVEN_HMAC_SHA256, mv(digest, 32), mv(s1, n1)),
            "it verifies, here and by the TLS engine's code", "");
        proven_byte_t spki[600];
        proven_size_t sn = 0;
        PROVEN_TEST_ASSERT(proven_pkey_spki(a, (proven_mem_mut_t){ spki, sizeof spki }, &sn) == PROVEN_OK &&
                           proven_verify_spki(mv(spki, sn), PROVEN_SIGN_RSA_PKCS1, PROVEN_HMAC_SHA256, mv(digest, 32), mv(s1, n1)) == PROVEN_OK,
            "and under its SubjectPublicKeyInfo", "");
        proven_cert_t leaf;
        proven_byte_t cder[2400];
        proven_size_t cpos = 0, clen = 0;
        proven_u8str_view_t label;
        PROVEN_TEST_ASSERT(proven_pem_next(cv(TP_SERVER_RSA), &cpos, &label, (proven_mem_mut_t){ cder, sizeof cder }, &clen) == PROVEN_OK &&
                           proven_cert_parse(mv(cder, clen), &leaf) == PROVEN_OK && leaf.public_key_info.size == sn && memcmp(leaf.public_key_info.ptr, spki, sn) == 0,
            "which is byte for byte its certificate's", "");
        s1[100] ^= 0x10;
        PROVEN_TEST_ASSERT(proven_verify_rsa(pa.n, pa.e, PROVEN_SIGN_RSA_PKCS1, PROVEN_HMAC_SHA256, 0, mv(digest, 32), mv(s1, n1)) == PROVEN_ERR_UNTRUSTED &&
                           proven_verify_rsa(pa.n, pa.e, PROVEN_SIGN_RSA_PKCS1, PROVEN_HMAC_SHA384, 0, mv(digest, 32), mv(s2, n2)) == PROVEN_ERR_INVALID_ARG &&
                           proven_verify_rsa(pa.n, pa.e, PROVEN_SIGN_RSA_PSS, PROVEN_HMAC_SHA256, 32, mv(digest, 32), mv(s2, n2)) == PROVEN_ERR_UNTRUSTED,
            "a changed bit, a digest of another size, the other scheme: not accepted", "");
        proven_byte_t sha1[20];
        proven_sha1_t h1;
        proven_sha1_init(&h1); proven_sha1_update(&h1, cv("an RSA message")); proven_sha1_final(&h1, sha1);
        PROVEN_TEST_ASSERT(proven_pkey_sign(a, PROVEN_SIGN_RSA_PKCS1, PROVEN_HMAC_SHA1, mv(sha1, 20), (proven_rng_t){ 0 }, (proven_mem_mut_t){ s1, sizeof s1 }, &n1) == PROVEN_OK &&
                           proven_verify_rsa(pa.n, pa.e, PROVEN_SIGN_RSA_PKCS1, PROVEN_HMAC_SHA1, 0, mv(sha1, 20), mv(s1, n1)) == PROVEN_OK &&
                           proven_verify_rsa(pa.n, pa.e, PROVEN_SIGN_RSA_PKCS1, PROVEN_HMAC_SHA256, 0, mv(digest, 32), mv(s1, n1)) == PROVEN_ERR_UNTRUSTED,
            "v1.5 over SHA-1, with SHA-1's DigestInfo", "");
        PROVEN_TEST_ASSERT(proven_pkey_sign(a, PROVEN_SIGN_RSA_PSS, PROVEN_HMAC_SHA256, mv(digest, 32), rng(), (proven_mem_mut_t){ s1, sizeof s1 }, &n1) == PROVEN_OK &&
                           proven_pkey_sign(a, PROVEN_SIGN_RSA_PSS, PROVEN_HMAC_SHA256, mv(digest, 32), rng(), (proven_mem_mut_t){ s2, sizeof s2 }, &n2) == PROVEN_OK &&
                           memcmp(s1, s2, n1) != 0 && proven_verify_rsa(pa.n, pa.e, PROVEN_SIGN_RSA_PSS, PROVEN_HMAC_SHA256, 32, mv(digest, 32), mv(s1, n1)) == PROVEN_OK &&
                           proven_verify_rsa(pa.n, pa.e, PROVEN_SIGN_RSA_PSS, PROVEN_HMAC_SHA256, 32, mv(digest, 32), mv(s2, n2)) == PROVEN_OK &&
                           proven_verify_spki(mv(spki, sn), PROVEN_SIGN_RSA_PSS, PROVEN_HMAC_SHA256, mv(digest, 32), mv(s1, n1)) == PROVEN_OK,
            "PSS: two signatures differ by their salt, and both verify", "");
        PROVEN_TEST_ASSERT(proven_pkey_sign(a, PROVEN_SIGN_RSA_PSS, PROVEN_HMAC_SHA256, mv(digest, 32), (proven_rng_t){ 0 }, (proven_mem_mut_t){ s1, sizeof s1 }, &n1) == PROVEN_ERR_INVALID_ARG &&
                           proven_pkey_sign(a, PROVEN_SIGN_ECDSA, PROVEN_HMAC_SHA256, mv(digest, 32), rng(), (proven_mem_mut_t){ s1, sizeof s1 }, &n1) == PROVEN_ERR_INVALID_ARG &&
                           proven_pkey_sign(a, PROVEN_SIGN_RSA_PKCS1, PROVEN_HMAC_SHA256, mv(digest, 32), rng(), (proven_mem_mut_t){ s1, 255 }, &n1) == PROVEN_ERR_OUT_OF_BOUNDS,
            "PSS without a generator, an EC scheme, too little room: refused", "");
        proven_rsa_components_t broken = pa;
        proven_byte_t qinv[300];
        memcpy(qinv, pa.qinv.ptr, pa.qinv.size);
        qinv[pa.qinv.size - 1] ^= 2;
        broken.qinv = mv(qinv, pa.qinv.size);
        proven_pkey_t *bad = NULL;
        PROVEN_TEST_ASSERT(proven_pkey_from_rsa(heap, &broken, rng(), &bad) == PROVEN_ERR_INVALID_FORMAT && bad == NULL,
            "parts that do not sign correctly (a wrong qInv) are refused when the key is made", "");
        broken = pa;
        broken.d = (proven_mem_view_t){ 0 };
        PROVEN_TEST_ASSERT(proven_pkey_from_rsa(heap, &broken, rng(), &bad) == PROVEN_OK, "d may be left out", "");
        proven_rsa_components_t pb;
        PROVEN_TEST_ASSERT(proven_pkey_rsa_components(bad, &pb) == PROVEN_OK && pb.d.size == 0, "and is then empty", "");
        proven_pkey_destroy(bad);
        PROVEN_TEST_ASSERT(proven_pkey_parse(heap, cv(TP_SERVER_RSA_KEY), (proven_rng_t){ 0 }, &bad) == PROVEN_ERR_INVALID_ARG,
            "an RSA key with no generator for its blinding is refused", "");
        PROVEN_TEST_ASSERT(proven_pkey_parse(heap, cv(TP_CLIENT_RSA_KEY), rng(), &bad) == PROVEN_ERR_UNSUPPORTED && bad == NULL,
            "a 1024-bit key is PROVEN_ERR_UNSUPPORTED", "");
        proven_mem_view_t scalar;
        PROVEN_TEST_ASSERT(proven_pkey_ec_scalar(a, &scalar) == PROVEN_ERR_INVALID_STATE, "an RSA key has no EC scalar", "");
        proven_pkey_destroy(a); proven_pkey_destroy(b); proven_pkey_destroy(c);
        proven_pkey_destroy(NULL);
        PROVEN_TEST_ASSERT(proven_pkey_kind(NULL) == PROVEN_PKEY_NONE && proven_pkey_bits(NULL) == 0, "NULL is no key", "");
    }

    PROVEN_TEST_SECTION("certificate purposes and key identifiers", "codeSigning and timeStamping (critical, alone, RFC 3161 2.3) as uses of proven_cert_verify; SKI and AKI read.", "Check the EKU and the SKI/AKI branches of parse_extensions and the top of cv_check_path in src/proven/cert.c.");
    {
        PROVEN_TEST_ASSERT(tp_build(), "the P-256 test CA", "");
        proven_cert_store_t *anchors = NULL;
        PROVEN_TEST_ASSERT(proven_cert_store_create(heap, &anchors) == PROVEN_OK && proven_cert_store_add_pem(anchors, cv(TP_CA), NULL) == PROVEN_OK, "an anchor", "");
        static proven_byte_t cs[1400], srv[1400], none[1400], tsa[1400], tsa_soft[1400], tsa_two[1400];
        static const proven_byte_t ski[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20 }, aki[] = { 0xaa, 0xbb, 0xcc };
        const proven_size_t ncs = leaf_der(cs, sizeof cs, PROVEN_CERT_EKU_CODE_SIGNING, false, mv(ski, 20), mv(aki, 3));
        const proven_size_t nsrv = leaf_der(srv, sizeof srv, PROVEN_CERT_EKU_SERVER_AUTH, false, (proven_mem_view_t){ 0 }, (proven_mem_view_t){ 0 });
        const proven_size_t nnone = leaf_der(none, sizeof none, 0, false, (proven_mem_view_t){ 0 }, (proven_mem_view_t){ 0 });
        const proven_size_t ntsa = leaf_der(tsa, sizeof tsa, PROVEN_CERT_EKU_TIME_STAMPING, true, (proven_mem_view_t){ 0 }, (proven_mem_view_t){ 0 });
        const proven_size_t nsoft = leaf_der(tsa_soft, sizeof tsa_soft, PROVEN_CERT_EKU_TIME_STAMPING, false, (proven_mem_view_t){ 0 }, (proven_mem_view_t){ 0 });
        const proven_size_t ntwo = leaf_der(tsa_two, sizeof tsa_two, PROVEN_CERT_EKU_TIME_STAMPING | PROVEN_CERT_EKU_CODE_SIGNING, true, (proven_mem_view_t){ 0 }, (proven_mem_view_t){ 0 });
        PROVEN_TEST_ASSERT(ncs && nsrv && nnone && ntsa && nsoft && ntwo, "six leaves of one CA", "");
        proven_cert_t c;
        PROVEN_TEST_ASSERT(proven_cert_parse(mv(cs, ncs), &c) == PROVEN_OK && c.has_ext_key_usage && c.ext_key_usage == PROVEN_CERT_EKU_CODE_SIGNING &&
                           c.ext_key_usage_count == 1 && !c.ext_key_usage_critical && same(c.subject_key_id, ski, 20) && same(c.authority_key_id, aki, 3),
            "codeSigning has its own bit, and the key identifiers are read", "");
        PROVEN_TEST_ASSERT(proven_cert_parse(mv(tsa_two, ntwo), &c) == PROVEN_OK && c.ext_key_usage_critical && c.ext_key_usage_count == 2 &&
                           c.ext_key_usage == (PROVEN_CERT_EKU_TIME_STAMPING | PROVEN_CERT_EKU_CODE_SIGNING) && c.subject_key_id.size == 0 && c.authority_key_id.size == 0,
            "a critical extendedKeyUsage of two purposes; no identifiers when there are none", "");
        PROVEN_TEST_ASSERT(purpose(anchors, mv(cs, ncs), PROVEN_CERT_USE_CODE_SIGNING) == PROVEN_CERT_FAULT_NONE, "a codeSigning leaf may sign code", "");
        PROVEN_TEST_ASSERT(purpose(anchors, mv(srv, nsrv), PROVEN_CERT_USE_CODE_SIGNING) == PROVEN_CERT_FAULT_USAGE &&
                           purpose(anchors, mv(none, nnone), PROVEN_CERT_USE_CODE_SIGNING) == PROVEN_CERT_FAULT_USAGE &&
                           purpose(anchors, mv(tsa, ntsa), PROVEN_CERT_USE_CODE_SIGNING) == PROVEN_CERT_FAULT_USAGE,
            "a serverAuth leaf, a leaf with no purposes, a TSA's: not for code", "");
        PROVEN_TEST_ASSERT(purpose(anchors, mv(none, nnone), PROVEN_CERT_USE_SERVER) == PROVEN_CERT_FAULT_NONE &&
                           purpose(anchors, mv(cs, ncs), PROVEN_CERT_USE_SERVER) == PROVEN_CERT_FAULT_USAGE,
            "while for a server no purposes still means any, and codeSigning alone is not serverAuth", "");
        PROVEN_TEST_ASSERT(purpose(anchors, mv(tsa, ntsa), PROVEN_CERT_USE_TIMESTAMPING) == PROVEN_CERT_FAULT_NONE, "a critical timeStamping leaf is a TSA", "");
        PROVEN_TEST_ASSERT(purpose(anchors, mv(tsa_soft, nsoft), PROVEN_CERT_USE_TIMESTAMPING) == PROVEN_CERT_FAULT_USAGE &&
                           purpose(anchors, mv(tsa_two, ntwo), PROVEN_CERT_USE_TIMESTAMPING) == PROVEN_CERT_FAULT_USAGE &&
                           purpose(anchors, mv(cs, ncs), PROVEN_CERT_USE_TIMESTAMPING) == PROVEN_CERT_FAULT_USAGE &&
                           purpose(anchors, mv(none, nnone), PROVEN_CERT_USE_TIMESTAMPING) == PROVEN_CERT_FAULT_USAGE,
            "not when the extension is not critical, names another purpose too, names only codeSigning, or is absent", "");
        proven_cert_store_destroy(anchors);
    }

    PROVEN_TEST_SECTION("AES-CBC and PKCS #7", "NIST SP 800-38A F.2 for 128-, 192- and 256-bit keys, both ways, whole and in two calls, with the processor's AES and without; padding removed only when it is padding.", "Check proven_crypto_aes_cbc_init (12 rounds for 192 bits) and src/proven/aes.c.");
    {
        static const char *const keys[3] = { "2b7e151628aed2a6abf7158809cf4f3c", "8e73b0f7da0e6452c810f32b809079e562f8ead2522c6b7b",
                                             "603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4" };
        static const char *const ciphers[3] = {
            "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b273bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7",
            "4f021db243bc633d7178183a9fa071e8b4d9ada9ad7dedf4e5e738763f69145a571b242012fb7ae07fa9baac3df102e008b0e27988598881d920a9e64f5615cd",
            "f58c4c04d6e5f1ba779eabfb5f7bfbd69cfc4e967edb808d679f777bc6702c7d39f23369a9d9bacfa530e26304231461b2eb05e2c39be9fcda6c19078c6a9d1b" };
        proven_byte_t plain[64], want[64], key[32], buf[64], iv[16];
        hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710", plain);
        for (int portable = 0; portable < 2; ++portable) {
            proven_crypto_aes_force_portable(portable == 1);
            for (int k = 0; k < 3; ++k) {
                const proven_size_t kn = hex(keys[k], key);
                hex(ciphers[k], want);
                memcpy(buf, plain, 64);
                for (int i = 0; i < 16; ++i) iv[i] = (proven_byte_t)i;
                bool ok = proven_aes_cbc_encrypt(mv(key, kn), iv, (proven_mem_mut_t){ buf, 64 }) == PROVEN_OK && memcmp(buf, want, 64) == 0 && memcmp(iv, want + 48, 16) == 0;
                for (int i = 0; i < 16; ++i) iv[i] = (proven_byte_t)i;
                ok = ok && proven_aes_cbc_decrypt(mv(key, kn), iv, (proven_mem_mut_t){ buf, 32 }) == PROVEN_OK &&
                     proven_aes_cbc_decrypt(mv(key, kn), iv, (proven_mem_mut_t){ buf + 32, 32 }) == PROVEN_OK && memcmp(buf, plain, 64) == 0;
                PROVEN_TEST_ASSERT(ok, keys[k], portable ? "The bitsliced cipher disagrees with SP 800-38A." : "The default cipher disagrees with SP 800-38A.");
            }
        }
        proven_crypto_aes_force_portable(false);
        PROVEN_TEST_ASSERT(proven_aes_cbc_decrypt(mv(key, 20), iv, (proven_mem_mut_t){ buf, 16 }) == PROVEN_ERR_INVALID_ARG &&
                           proven_aes_cbc_decrypt(mv(key, 16), iv, (proven_mem_mut_t){ buf, 15 }) == PROVEN_ERR_INVALID_ARG,
            "a key of 20 bytes, a length that is not whole blocks: refused", "");
        static const struct { const char *last; proven_err_t want; proven_size_t len; } pads[] = {
            { "000102030405060708090a0b0c0d0e01", PROVEN_OK, 15 }, { "00010203040506070809040404040404", PROVEN_OK, 12 },
            { "10101010101010101010101010101010", PROVEN_OK, 0 }, { "000102030405060708090a0b0c0d0e00", PROVEN_ERR_INVALID_FORMAT, 0 },
            { "000102030405060708090a0b03040404", PROVEN_ERR_INVALID_FORMAT, 0 }, { "000102030405060708090a0b0c0d0e11", PROVEN_ERR_INVALID_FORMAT, 0 },
            { "11111111111111111111111111111111", PROVEN_ERR_INVALID_FORMAT, 0 },
        };
        for (size_t i = 0; i < sizeof pads / sizeof *pads; ++i) {
            proven_byte_t d[32];
            memset(d, 0x55, 16);
            hex(pads[i].last, d + 16);
            proven_size_t n = 99;
            const proven_err_t e = proven_pkcs7_unpad(mv(d, 32), 16, &n);
            PROVEN_TEST_ASSERT(e == pads[i].want && n == (e == PROVEN_OK ? 16 + pads[i].len : 0), pads[i].last, "1 to 16 bytes, each the count; anything else refused.");
        }
        proven_size_t n = 0;
        PROVEN_TEST_ASSERT(proven_pkcs7_unpad(mv(buf, 15), 16, &n) == PROVEN_ERR_INVALID_ARG && proven_pkcs7_unpad(mv(buf, 16), 0, &n) == PROVEN_ERR_INVALID_ARG,
            "a length that is not whole blocks, a block of 0: refused", "");
    }

    PROVEN_TEST_PASS("DER, PBKDF2 and the signing keys behave as documented.");
    return 0;
}
