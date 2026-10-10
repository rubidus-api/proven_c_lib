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

#endif
