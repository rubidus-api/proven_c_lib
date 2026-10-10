#ifndef TEST_UNIT_TLS_PKI_H
#define TEST_UNIT_TLS_PKI_H

/*
 * The certificates and keys the TLS tests use, made when the test starts.
 *
 * Nothing here is stored: the keys are derived from a fixed pattern at run time and the
 * certificates are issued from them with the library's own (internal) certificate writer, so
 * that no key material sits in the repository. They are valid 2026 to 2036, except the one
 * that expires in 2027; tp_build() must be called once before any TP_ name is used.
 */

#include "proven.h"
#include "../src/proven/proven_internal_tls.h"
#include <string.h>

/* 2027-06-01 and 2029-06-01, seconds since 1970 */
#define TP_NOW 1811808000LL
#define TP_LATER 1874966400LL
#define TP_FROM 1767225600LL                      /* 2026-01-01 */
#define TP_UNTIL 2082758400LL                     /* 2036-01-01 */
#define TP_EXPIRY 1798761600LL                    /* 2027-01-01 */

static char TP_CA[2048], TP_OTHER_CA[2048];
static char TP_SERVER_P256[2048], TP_SERVER_P256_KEY[512], TP_SERVER_P256_KEY_SEC1[512];
static char TP_SERVER_ED[2048], TP_SERVER_ED_KEY[512];
static char TP_SERVER_EXPIRED[2048], TP_SERVER_OTHER_NAME[2048];
static char TP_CLIENT[2048], TP_CLIENT_KEY[512];
static char TP_STRANGER[2048], TP_STRANGER_KEY[512];
static char TP_RSA_KEY[512], TP_P384_KEY[512];    /* shapes of keys this version does not take */

static void tp_key(proven_byte_t out[32], proven_byte_t which) {
    /* A pattern, not a secret. The first byte keeps a P-256 scalar well inside the order. */
    for (int i = 0; i < 32; ++i) out[i] = (proven_byte_t)(which * 37 + i * 11 + 5);
    out[0] = 0x01;
}

static bool tp_pem(char *out, proven_size_t cap, const char *label, const proven_byte_t *der, proven_size_t n) {
    proven_size_t len = 0;
    if (proven_tls_pem_write_(label, (proven_mem_view_t){ .ptr = der, .size = n }, (proven_mem_mut_t){ .ptr = (proven_byte_t *)out, .size = cap - 1 }, &len) != PROVEN_OK) return false;
    out[len] = 0;
    return true;
}

static bool tp_cert(char *out, const char *subject, proven_tls_key_kind_t kind, const proven_byte_t *key,
                    const char *issuer, proven_tls_key_kind_t issuer_kind, const proven_byte_t *issuer_key,
                    bool ca, proven_u32 eku, proven_i64 until, const char *dns) {
    proven_u8str_view_t names[3] = {
        { .ptr = (const proven_byte_t *)dns, .size = strlen(dns) },
        { .ptr = (const proven_byte_t *)"localhost", .size = 9 },
        { .ptr = (const proven_byte_t *)"127.0.0.1", .size = 9 },
    };
    proven_tls_issue_t q = {
        .subject = subject, .issuer = issuer, .subject_kind = kind, .subject_key = key, .issuer_kind = issuer_kind, .issuer_key = issuer_key,
        .is_ca = ca, .eku = eku, .names = names, .name_count = ca ? 0 : (strcmp(dns, "example.test") == 0 ? 3 : 1),
        .not_before = TP_FROM, .not_after = until,
    };
    for (int i = 0; i < 16; ++i) q.serial[i] = (proven_byte_t)(subject[0] + i * 7 + (int)(until & 0xff));
    proven_byte_t der[1400];
    proven_size_t n = 0;
    return proven_tls_issue_(&q, (proven_mem_mut_t){ .ptr = der, .size = sizeof der }, &n) == PROVEN_OK && tp_pem(out, 2048, "CERTIFICATE", der, n);
}

static bool tp_key_pem(char *out, proven_tls_key_kind_t kind, const proven_byte_t *key, bool sec1) {
    proven_byte_t der[80];
    proven_size_t n = proven_tls_key_der_(kind, key, sec1, der);
    return n > 0 && tp_pem(out, 512, sec1 ? "EC PRIVATE KEY" : "PRIVATE KEY", der, n);
}

static bool tp_build(void) {
    proven_byte_t ca[32], other[32], p256[32], ed[32], client[32], stranger[32];
    const proven_u32 SRV = PROVEN_CERT_EKU_SERVER_AUTH, CLI = PROVEN_CERT_EKU_CLIENT_AUTH;
    tp_key(ca, 1); tp_key(other, 2); tp_key(p256, 3); tp_key(ed, 4); tp_key(client, 5); tp_key(stranger, 6);
    bool ok = true;
    ok = ok && tp_cert(TP_CA, "TLS test CA", PROVEN_TLS_KEY_P256, ca, "TLS test CA", PROVEN_TLS_KEY_P256, ca, true, 0, TP_UNTIL, "");
    ok = ok && tp_cert(TP_OTHER_CA, "Another CA", PROVEN_TLS_KEY_ED25519, other, "Another CA", PROVEN_TLS_KEY_ED25519, other, true, 0, TP_UNTIL, "");
    ok = ok && tp_cert(TP_SERVER_P256, "server", PROVEN_TLS_KEY_P256, p256, "TLS test CA", PROVEN_TLS_KEY_P256, ca, false, SRV, TP_UNTIL, "example.test");
    ok = ok && tp_cert(TP_SERVER_ED, "server", PROVEN_TLS_KEY_ED25519, ed, "TLS test CA", PROVEN_TLS_KEY_P256, ca, false, SRV, TP_UNTIL, "example.test");
    ok = ok && tp_cert(TP_SERVER_EXPIRED, "server", PROVEN_TLS_KEY_P256, p256, "TLS test CA", PROVEN_TLS_KEY_P256, ca, false, SRV, TP_EXPIRY, "example.test");
    ok = ok && tp_cert(TP_SERVER_OTHER_NAME, "server", PROVEN_TLS_KEY_P256, p256, "TLS test CA", PROVEN_TLS_KEY_P256, ca, false, SRV, TP_UNTIL, "elsewhere.test");
    ok = ok && tp_cert(TP_CLIENT, "client", PROVEN_TLS_KEY_P256, client, "TLS test CA", PROVEN_TLS_KEY_P256, ca, false, CLI, TP_UNTIL, "example.test");
    ok = ok && tp_cert(TP_STRANGER, "stranger", PROVEN_TLS_KEY_P256, stranger, "Another CA", PROVEN_TLS_KEY_ED25519, other, false, SRV | CLI, TP_UNTIL, "example.test");
    ok = ok && tp_key_pem(TP_SERVER_P256_KEY, PROVEN_TLS_KEY_P256, p256, false) && tp_key_pem(TP_SERVER_P256_KEY_SEC1, PROVEN_TLS_KEY_P256, p256, true);
    ok = ok && tp_key_pem(TP_SERVER_ED_KEY, PROVEN_TLS_KEY_ED25519, ed, false) && tp_key_pem(TP_CLIENT_KEY, PROVEN_TLS_KEY_P256, client, false);
    ok = ok && tp_key_pem(TP_STRANGER_KEY, PROVEN_TLS_KEY_P256, stranger, false);
    /* Well-formed PKCS #8 for algorithms this version does not sign with: the identifiers are
     * real, the key bytes are filler. They only have to be recognised and refused. */
    static const proven_byte_t rsa[] = { 0x30, 0x1b, 0x02, 0x01, 0x00, 0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01, 0x05, 0x00,
                                         0x04, 0x07, 0x30, 0x05, 0x02, 0x01, 0x00, 0x02, 0x00 };
    static const proven_byte_t p384[] = { 0x30, 0x1b, 0x02, 0x01, 0x00, 0x30, 0x10, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01, 0x06, 0x05, 0x2b, 0x81, 0x04, 0x00, 0x22,
                                          0x04, 0x04, 0x01, 0x02, 0x03, 0x04 };
    ok = ok && tp_pem(TP_RSA_KEY, sizeof TP_RSA_KEY, "PRIVATE KEY", rsa, sizeof rsa) && tp_pem(TP_P384_KEY, sizeof TP_P384_KEY, "PRIVATE KEY", p384, sizeof p384);
    return ok;
}

/*
 * RSA identities, for the tests that need them. Separate from tp_build() because making an
 * RSA key takes real time - searching for primes - and most tests have no use for one:
 * tp_build_rsa() is called only where an RSA key is the subject. The keys come from a fixed
 * seed, so a failure repeats; like the others, they exist only while the test runs.
 *
 *   TP_RSA_SERVER_K   2048 bits: the size a real server would have
 *   TP_RSA_SMALL_K    1024 bits: a client certificate's key, and quick
 *   TP_RSA_CA_K       1024 bits: a certificate authority that signs with RSA
 *
 * The two small ones are below what the library accepts, on purpose: tests that use them
 * lower the limit with proven_crypto_rsa_test_min_bits(1024) and put it back.
 */
[[maybe_unused]] static proven_crypto_rsa_key_t TP_RSA_SERVER_K, TP_RSA_SMALL_K, TP_RSA_CA_K;
[[maybe_unused]] static proven_byte_t TP_RSA_SERVER_D[512], TP_RSA_SMALL_D[512], TP_RSA_CA_D[512];
[[maybe_unused]] static char TP_RSA_CA[4096];                              /* the RSA authority's own certificate */
[[maybe_unused]] static char TP_SERVER_RSA[4096], TP_SERVER_RSA_KEY[4096], TP_SERVER_RSA_KEY_PKCS1[4096];   /* 2048-bit key, issued by the P-256 CA */
[[maybe_unused]] static char TP_SERVER_RSA_BY_RSA[4096];                   /* the same key, issued by the RSA CA */
[[maybe_unused]] static char TP_CLIENT_RSA[4096], TP_CLIENT_RSA_KEY[4096]; /* 1024-bit key, issued by the P-256 CA */

typedef struct { char seed[32]; proven_u64 counter; } tp_stream_t;     /* the seed: a string of at most 31 characters */
[[maybe_unused]] static void tp_stream(void *ctx, proven_byte_t *out, proven_size_t len) {
    tp_stream_t *st = ctx;
    while (len > 0) {
        proven_byte_t in[40], block[32];
        memcpy(in, st->seed, 32);
        for (int i = 0; i < 8; ++i) in[32 + i] = (proven_byte_t)(st->counter >> (8 * i));
        st->counter++;
        proven_sha256((proven_mem_view_t){ .ptr = in, .size = sizeof in }, block);
        proven_size_t n = len < 32 ? len : 32;
        memcpy(out, block, n);
        out += n; len -= n;
    }
}

[[maybe_unused]] static bool tp_cert_rsa(char *out, proven_size_t cap, const char *subject, const proven_crypto_rsa_key_t *subject_rsa,
                                         const char *issuer, proven_tls_key_kind_t issuer_kind, const proven_byte_t *issuer_key, const proven_crypto_rsa_key_t *issuer_rsa,
                                         bool ca, proven_u32 eku) {
    proven_u8str_view_t names[3] = {
        { .ptr = (const proven_byte_t *)"example.test", .size = 12 },
        { .ptr = (const proven_byte_t *)"localhost", .size = 9 },
        { .ptr = (const proven_byte_t *)"127.0.0.1", .size = 9 },
    };
    proven_tls_issue_t q = {
        .subject = subject, .issuer = issuer, .subject_kind = PROVEN_TLS_KEY_RSA, .subject_rsa = subject_rsa,
        .issuer_kind = issuer_kind, .issuer_key = issuer_key, .issuer_rsa = issuer_rsa,
        .is_ca = ca, .eku = eku, .names = names, .name_count = ca ? 0 : 3, .not_before = TP_FROM, .not_after = TP_UNTIL,
    };
    for (int i = 0; i < 16; ++i) q.serial[i] = (proven_byte_t)(subject[0] + i * 5 + (issuer_rsa ? 3 : 1));
    static proven_byte_t der[2400];
    proven_size_t n = 0;
    return proven_tls_issue_(&q, (proven_mem_mut_t){ .ptr = der, .size = sizeof der }, &n) == PROVEN_OK && tp_pem(out, cap, "CERTIFICATE", der, n);
}

[[maybe_unused]] static bool tp_build_rsa(void) {
    proven_byte_t ca[32];
    static proven_byte_t der[2600];
    tp_stream_t st = { .seed = "proven_c_lib RSA test keys", .counter = 0 };
    const proven_u32 SRV = PROVEN_CERT_EKU_SERVER_AUTH, CLI = PROVEN_CERT_EKU_CLIENT_AUTH;
    tp_key(ca, 1);                                                  /* the P-256 CA of tp_build() */
    bool ok = proven_crypto_rsa_generate(2048, tp_stream, &st, &TP_RSA_SERVER_K, TP_RSA_SERVER_D) &&
              proven_crypto_rsa_generate(1024, tp_stream, &st, &TP_RSA_SMALL_K, TP_RSA_SMALL_D) &&
              proven_crypto_rsa_generate(1024, tp_stream, &st, &TP_RSA_CA_K, TP_RSA_CA_D);
    ok = ok && tp_cert_rsa(TP_SERVER_RSA, sizeof TP_SERVER_RSA, "server", &TP_RSA_SERVER_K, "TLS test CA", PROVEN_TLS_KEY_P256, ca, NULL, false, SRV);
    ok = ok && tp_cert_rsa(TP_CLIENT_RSA, sizeof TP_CLIENT_RSA, "client", &TP_RSA_SMALL_K, "TLS test CA", PROVEN_TLS_KEY_P256, ca, NULL, false, CLI);
    ok = ok && tp_cert_rsa(TP_RSA_CA, sizeof TP_RSA_CA, "RSA test CA", &TP_RSA_CA_K, "RSA test CA", PROVEN_TLS_KEY_RSA, NULL, &TP_RSA_CA_K, true, 0);
    ok = ok && tp_cert_rsa(TP_SERVER_RSA_BY_RSA, sizeof TP_SERVER_RSA_BY_RSA, "server", &TP_RSA_SERVER_K, "RSA test CA", PROVEN_TLS_KEY_RSA, NULL, &TP_RSA_CA_K, false, SRV);
    proven_size_t n = ok ? proven_tls_rsa_key_der_(&TP_RSA_SERVER_K, TP_RSA_SERVER_D, true, (proven_mem_mut_t){ .ptr = der, .size = sizeof der }) : 0;
    ok = ok && n > 0 && tp_pem(TP_SERVER_RSA_KEY, sizeof TP_SERVER_RSA_KEY, "PRIVATE KEY", der, n);
    n = ok ? proven_tls_rsa_key_der_(&TP_RSA_SERVER_K, TP_RSA_SERVER_D, false, (proven_mem_mut_t){ .ptr = der, .size = sizeof der }) : 0;
    ok = ok && n > 0 && tp_pem(TP_SERVER_RSA_KEY_PKCS1, sizeof TP_SERVER_RSA_KEY_PKCS1, "RSA PRIVATE KEY", der, n);
    n = ok ? proven_tls_rsa_key_der_(&TP_RSA_SMALL_K, TP_RSA_SMALL_D, true, (proven_mem_mut_t){ .ptr = der, .size = sizeof der }) : 0;
    ok = ok && n > 0 && tp_pem(TP_CLIENT_RSA_KEY, sizeof TP_CLIENT_RSA_KEY, "PRIVATE KEY", der, n);
    return ok;
}

#endif
