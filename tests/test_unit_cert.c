#include "proven.h"
#include "proven_test.h"
#include "test_unit_cert_vectors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * X.509: parsing, names, PEM, the store, and chain verification.
 *
 * The certificates are in test_unit_cert_vectors.h: chains made with another implementation
 * (Python `cryptography`, OpenSSL underneath) by a private generator, one for each thing the
 * validator accepts and each thing it refuses. Their dates are fixed and the test supplies the
 * time, so the result does not depend on the day it runs.
 */

static proven_size_t unhex(const char *hex, proven_byte_t *out) {
    proven_size_t n = strlen(hex) / 2;
    for (proven_size_t i = 0; i < n; ++i) {
        char c[3] = { hex[2 * i], hex[2 * i + 1], 0 };
        out[i] = (proven_byte_t)strtoul(c, NULL, 16);
    }
    return n;
}

static proven_u8str_view_t sv(const char *s) {
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}

static proven_byte_t g_der[8][2048];

int main(void) {
    PROVEN_TEST_SUITE("X.509 certificates",
        "A strict reader, host-name matching, PEM, a store of anchors, and path validation with every refusal it claims.",
        "Inspect src/proven/cert.c. A chain accepted here that should be refused is a security defect, not a test nuisance.");

    proven_allocator_t alloc = proven_heap_allocator();

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("reading one certificate",
        "The fields validation needs come out as views into the DER, and anything that is not exactly one certificate is refused.",
        "Check proven_cert_parse and der_read.");
    {
        proven_cert_t c;
        proven_size_t n = unhex(CV_LEAF, g_der[0]);
        proven_mem_view_t der = { .ptr = g_der[0], .size = n };
        PROVEN_TEST_ASSERT(proven_cert_parse(der, &c) == PROVEN_OK, "a leaf parses", "");
        PROVEN_TEST_ASSERT(c.version == 3 && c.key_kind == PROVEN_CERT_KEY_EC_P256 && c.key.size == 65 && c.key.ptr[0] == 0x04,
            "version 3, a P-256 key as an uncompressed point", "");
        PROVEN_TEST_ASSERT(c.sig_kind == PROVEN_CERT_SIG_ECDSA && c.sig_hash == PROVEN_HMAC_SHA256, "signed with ECDSA over SHA-256", "");
        PROVEN_TEST_ASSERT(!c.is_ca && c.has_ext_key_usage && c.ext_key_usage == PROVEN_CERT_EKU_SERVER_AUTH && !c.unknown_critical,
            "not a CA, for server authentication, nothing critical left unread", "");
        PROVEN_TEST_ASSERT(c.not_before == 1767225600 && c.not_after == 1893456000,
            "validity from 2026-01-01 to 2030-01-01 as seconds since 1970", "");
        PROVEN_TEST_ASSERT(c.der.ptr == g_der[0] && c.tbs.ptr > g_der[0] && c.tbs.ptr + c.tbs.size < g_der[0] + n &&
                           c.public_key_info.ptr > c.tbs.ptr && c.subject.size > 2 && c.issuer.size > 2 && c.serial.size > 0,
            "every view points into the caller's bytes", "");

        proven_size_t pos = 0, dns = 0, ip = 0;
        proven_cert_name_kind_t kind;
        proven_mem_view_t v;
        bool first_ok = false;
        while (proven_cert_alt_name_next(&c, &pos, &kind, &v)) {
            if (kind == PROVEN_CERT_NAME_DNS) { if (dns == 0) first_ok = v.size == 12 && memcmp(v.ptr, "example.test", 12) == 0; dns++; }
            if (kind == PROVEN_CERT_NAME_IP) ip++;
        }
        PROVEN_TEST_ASSERT(dns == 2 && ip == 2 && first_ok, "four alternative names: two DNS names and two addresses", "");

        PROVEN_TEST_ASSERT(proven_cert_matches_host(&c, sv("example.test")) && proven_cert_matches_host(&c, sv("Example.TEST")) &&
                           proven_cert_matches_host(&c, sv("x.wild.example.test")) && proven_cert_matches_host(&c, sv("192.0.2.1")) &&
                           proven_cert_matches_host(&c, sv("2001:db8::1")) && proven_cert_matches_host(&c, sv("2001:db8:0:0:0:0:0:1")),
            "the names it is for, in any case and any spelling of the address", "");
        PROVEN_TEST_ASSERT(!proven_cert_matches_host(&c, sv("")) && !proven_cert_matches_host(&c, sv("example.tes")) &&
                           !proven_cert_matches_host(&c, sv("xexample.test")) && !proven_cert_matches_host(&c, sv("sub.example.test")) &&
                           !proven_cert_matches_host(&c, sv(".wild.example.test")) && !proven_cert_matches_host(&c, sv("x.y.wild.example.test")) &&
                           !proven_cert_matches_host(&c, sv("192.0.2.01")) && !proven_cert_matches_host(&c, sv("192.0.2")) &&
                           !proven_cert_matches_host(&c, sv("2001:db8::2")) && !proven_cert_matches_host(&c, sv("leaf")),
            "and none it is not for: a prefix, a suffix, a subdomain, an empty label, two labels under the wildcard, another address, the common name", "");

        proven_byte_t pin[32], want[32];
        proven_sha256(c.public_key_info, want);
        PROVEN_TEST_ASSERT(proven_cert_key_sha256(&c, pin) == PROVEN_OK && memcmp(pin, want, 32) == 0 &&
                           proven_cert_key_sha256(NULL, pin) == PROVEN_ERR_INVALID_ARG,
            "the pin is SHA-256 of the SubjectPublicKeyInfo", "");

        proven_cert_t root;
        proven_size_t rn = unhex(CV_ROOT, g_der[1]);
        PROVEN_TEST_ASSERT(proven_cert_parse((proven_mem_view_t){ g_der[1], rn }, &root) == PROVEN_OK && root.is_ca &&
                           root.key_kind == PROVEN_CERT_KEY_RSA && root.rsa_n.size == 256 && (root.rsa_n.ptr[0] & 0x80) && root.rsa_e.size == 3 &&
                           root.has_key_usage && (root.key_usage & PROVEN_CERT_KU_KEY_CERT_SIGN) && (root.key_usage & PROVEN_CERT_KU_CRL_SIGN) &&
                           !(root.key_usage & PROVEN_CERT_KU_KEY_ENCIPHERMENT),
            "an RSA root: a CA, a 2048-bit modulus without its sign byte, the key usages it has", "");
        proven_cert_t inter;
        proven_size_t in = unhex(CV_INT, g_der[2]);
        PROVEN_TEST_ASSERT(proven_cert_parse((proven_mem_view_t){ g_der[2], in }, &inter) == PROVEN_OK && inter.has_path_len && inter.path_len == 0 &&
                           inter.sig_kind == PROVEN_CERT_SIG_RSA_PKCS1 && inter.signature.size == 256,
            "an intermediate: its path length, and the RSA signature over it", "");

        /* Every truncation, and every single-byte change of the structure's first bytes, either
         * fails or parses - but never reads outside the buffer (the sanitizer builds watch). */
        proven_size_t refused = 0;
        for (proven_size_t cut = 0; cut < n; ++cut) {
            if (proven_cert_parse((proven_mem_view_t){ g_der[0], cut }, &c) != PROVEN_OK) refused++;
        }
        PROVEN_TEST_ASSERT(refused == n, "every proper prefix of a certificate is refused", "");
        proven_size_t survived = 0;
        for (proven_size_t at = 0; at < n; ++at) {
            for (int bit = 0; bit < 8; bit += 3) {
                g_der[0][at] ^= (proven_byte_t)(1u << bit);
                if (proven_cert_parse(der, &c) == PROVEN_OK) survived++;
                g_der[0][at] ^= (proven_byte_t)(1u << bit);
            }
        }
        PROVEN_TEST_ASSERT(survived < 3 * n, "a sweep of single-bit changes over the whole certificate ends, some refused", "");
        g_der[0][n] = 0;
        PROVEN_TEST_ASSERT(proven_cert_parse((proven_mem_view_t){ g_der[0], n + 1 }, &c) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_cert_parse(der, NULL) == PROVEN_ERR_INVALID_ARG &&
                           proven_cert_parse((proven_mem_view_t){ NULL, 0 }, &c) == PROVEN_ERR_INVALID_ARG,
            "a trailing byte is PROVEN_ERR_INVALID_FORMAT; a null argument is PROVEN_ERR_INVALID_ARG", "");
        /* A length in the long form that would fit the short form. */
        static const proven_byte_t non_minimal[] = { 0x30, 0x81, 0x03, 0x02, 0x01, 0x00 };
        static const proven_byte_t indefinite[] = { 0x30, 0x80, 0x02, 0x01, 0x00, 0x00, 0x00 };
        PROVEN_TEST_ASSERT(proven_cert_parse((proven_mem_view_t){ non_minimal, sizeof non_minimal }, &c) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_cert_parse((proven_mem_view_t){ indefinite, sizeof indefinite }, &c) == PROVEN_ERR_INVALID_FORMAT,
            "a non-minimal length and an indefinite length are refused", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("PEM",
        "Blocks are found among other text, decoded, and a broken one is reported rather than guessed at.",
        "Check proven_pem_next.");
    {
        static char text[8192];
        static proven_byte_t out[2048];
        proven_size_t rn = unhex(CV_ROOT, g_der[1]);
        snprintf(text, sizeof text, "some words first\n%s\ntrailing words\n-----BEGIN PRIVATE THING-----\nAAEC\n-----END PRIVATE THING-----\n", CV_ROOT_PEM);
        proven_mem_view_t pem = { .ptr = (const proven_byte_t *)text, .size = strlen(text) };
        proven_size_t pos = 0, len = 0;
        proven_u8str_view_t label;
        PROVEN_TEST_ASSERT(proven_pem_next(pem, &pos, &label, (proven_mem_mut_t){ out, sizeof out }, &len) == PROVEN_OK &&
                           label.size == 11 && memcmp(label.ptr, "CERTIFICATE", 11) == 0 && len == rn && memcmp(out, g_der[1], rn) == 0,
            "the first block: its label and the same bytes as the DER", "");
        PROVEN_TEST_ASSERT(proven_pem_next(pem, &pos, &label, (proven_mem_mut_t){ out, sizeof out }, &len) == PROVEN_OK &&
                           label.size == 13 && len == 3 && out[0] == 0 && out[1] == 1 && out[2] == 2,
            "the second block, with another label", "");
        PROVEN_TEST_ASSERT(proven_pem_next(pem, &pos, &label, (proven_mem_mut_t){ out, sizeof out }, &len) == PROVEN_ERR_NOT_FOUND,
            "then PROVEN_ERR_NOT_FOUND", "");
        pos = 0;
        PROVEN_TEST_ASSERT(proven_pem_next(pem, &pos, &label, (proven_mem_mut_t){ out, 10 }, &len) == PROVEN_ERR_OUT_OF_BOUNDS && len == rn && pos == 0,
            "a short buffer is PROVEN_ERR_OUT_OF_BOUNDS with the size needed, and the position does not move", "");
        static const char *const broken[] = {
            "-----BEGIN CERTIFICATE-----\nAAEC\n",
            "-----BEGIN CERTIFICATE-----\nAAEC\n-----END SOMETHING ELSE-----\n",
            "-----BEGIN CERTIFICATE-----\nAA!C\n-----END CERTIFICATE-----\n",
            "-----BEGIN CERTIFICATE-----\nAAECA\n-----END CERTIFICATE-----\n",
            "-----BEGIN CERTIFICATE-----\nAA=C\n-----END CERTIFICATE-----\n",
            "-----BEGIN CERTIFICATE-----\nAAE=\n-----END CERTIFICATE-----\n=",
            "-----BEGIN CERTIFICATE-----\nAB==\n-----END CERTIFICATE-----\n",
        };
        bool all = true;
        for (proven_size_t i = 0; i < sizeof broken / sizeof broken[0]; ++i) {
            pos = 0;
            proven_err_t e = proven_pem_next((proven_mem_view_t){ (const proven_byte_t *)broken[i], strlen(broken[i]) }, &pos, &label, (proven_mem_mut_t){ out, sizeof out }, &len);
            /* The sixth is well formed (the stray '=' is after the block): it must decode. */
            if (i == 5) all = all && e == PROVEN_OK && len == 2;
            else all = all && e == PROVEN_ERR_INVALID_ENCODING;
        }
        PROVEN_TEST_ASSERT(all, "no END line, another END label, a bad symbol, an impossible length, misplaced padding, stray bits: PROVEN_ERR_INVALID_ENCODING", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the store",
        "Anchors are copied in; a bundle is taken entry by entry.",
        "Check proven_cert_store_*.");
    {
        proven_cert_store_t *store = NULL;
        PROVEN_TEST_ASSERT(proven_cert_store_create(alloc, &store) == PROVEN_OK && proven_cert_store_count(store) == 0 &&
                           proven_cert_store_count(NULL) == 0, "an empty store", "");
        static char bundle[16384];
        snprintf(bundle, sizeof bundle, "%s# a comment between entries\n-----BEGIN CERTIFICATE-----\nAAEC\n-----END CERTIFICATE-----\n%s", CV_ROOT_PEM, CV_ROOT_PEM);
        proven_size_t added = 99;
        PROVEN_TEST_ASSERT(proven_cert_store_add_pem(store, (proven_mem_view_t){ (const proven_byte_t *)bundle, strlen(bundle) }, &added) == PROVEN_OK &&
                           added == 2 && proven_cert_store_count(store) == 2,
            "a bundle of three blocks, one of them not a certificate: two are taken and the third is skipped", "");
        memset(bundle, 0, sizeof bundle);
        proven_size_t n = unhex(CV_INT, g_der[2]);
        for (int i = 0; i < 40; ++i) {
            PROVEN_TEST_ASSERT(proven_cert_store_add_der(store, (proven_mem_view_t){ g_der[2], n }) == PROVEN_OK, "add", "");
        }
        memset(g_der[2], 0, sizeof g_der[2]);
        PROVEN_TEST_ASSERT(proven_cert_store_count(store) == 42, "the store grows, and keeps its own copies after the caller's are gone", "");
        static const char nothing[] = "no certificate here\n";
        PROVEN_TEST_ASSERT(proven_cert_store_add_pem(store, (proven_mem_view_t){ (const proven_byte_t *)nothing, sizeof nothing - 1 }, &added) == PROVEN_ERR_NOT_FOUND && added == 0 &&
                           proven_cert_store_add_der(store, (proven_mem_view_t){ (const proven_byte_t *)nothing, 5 }) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_cert_store_add_der(NULL, (proven_mem_view_t){ (const proven_byte_t *)nothing, 5 }) == PROVEN_ERR_INVALID_ARG &&
                           proven_cert_store_create(alloc, NULL) == PROVEN_ERR_INVALID_ARG && proven_cert_store_count(store) == 42,
            "text with no certificate is PROVEN_ERR_NOT_FOUND, bytes that are not one are PROVEN_ERR_INVALID_FORMAT, and neither changes the store", "");
        proven_cert_store_destroy(store);
        proven_cert_store_destroy(NULL);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("chain verification",
        "Each case is a chain from another implementation, with the answer it must get: the error code and the precise fault.",
        "The case text names what was built. Check cv_search and cv_check_path.");
    {
        proven_size_t passed = 0;
        const proven_size_t total = sizeof CV_CASES / sizeof CV_CASES[0];
        for (proven_size_t i = 0; i < total; ++i) {
            const cv_case_t *t = &CV_CASES[i];
            proven_cert_store_t *store = NULL;
            proven_mem_view_t chain[4];
            proven_size_t count = 0;
            PROVEN_TEST_ASSERT(proven_cert_store_create(alloc, &store) == PROVEN_OK, "store", "");
            for (int a = 0; a < 3 && t->anchors[a]; ++a) {
                proven_size_t n = unhex(t->anchors[a], g_der[7]);
                PROVEN_TEST_ASSERT(proven_cert_store_add_der(store, (proven_mem_view_t){ g_der[7], n }) == PROVEN_OK, "anchor", t->text);
            }
            for (int c = 0; c < 4 && t->chain[c]; ++c) {
                proven_size_t n = unhex(t->chain[c], g_der[c]);
                chain[count++] = (proven_mem_view_t){ g_der[c], n };
            }
            proven_cert_verify_options_t opt = { .anchors = store, .host = sv(t->host), .now = t->now, .use = (proven_cert_use_t)t->use };
            proven_cert_verify_result_t res = { PROVEN_CERT_FAULT_TOO_DEEP, 77 };
            proven_err_t e = proven_cert_verify(chain, count, &opt, &res);
            bool ok = e == t->err && res.fault == t->fault && res.depth == t->depth;
            if (!ok) fprintf(stderr, "case %u (%s): err %d fault %d depth %u, wanted %d %d %u\n", (unsigned)i, t->text, (int)e, (int)res.fault, (unsigned)res.depth, (int)t->err, (int)t->fault, t->depth);
            PROVEN_TEST_ASSERT(ok, t->text, "");
            /* Without a result struct the error code is the same. */
            PROVEN_TEST_ASSERT(proven_cert_verify(chain, count, &opt, NULL) == t->err, "the same without a result", t->text);
            passed++;
            proven_cert_store_destroy(store);
        }
        PROVEN_TEST_ASSERT(passed == total && total >= 40, "every case", "");

        proven_cert_store_t *store = NULL;
        PROVEN_TEST_ASSERT(proven_cert_store_create(alloc, &store) == PROVEN_OK, "store", "");
        proven_size_t n = unhex(CV_LEAF, g_der[0]);
        proven_mem_view_t chain[1] = { { g_der[0], n } };
        proven_cert_verify_options_t opt = { .anchors = store, .now = CV_NOW };
        proven_cert_verify_result_t res;
        PROVEN_TEST_ASSERT(proven_cert_verify(chain, 1, &opt, &res) == PROVEN_ERR_UNTRUSTED && res.fault == PROVEN_CERT_FAULT_NO_ISSUER,
            "an empty store trusts nothing", "");
        PROVEN_TEST_ASSERT(proven_cert_verify(NULL, 1, &opt, &res) == PROVEN_ERR_INVALID_ARG && proven_cert_verify(chain, 0, &opt, &res) == PROVEN_ERR_INVALID_ARG &&
                           proven_cert_verify(chain, 1, NULL, &res) == PROVEN_ERR_INVALID_ARG,
            "no chain, an empty chain and no options are PROVEN_ERR_INVALID_ARG", "");
        opt.anchors = NULL;
        PROVEN_TEST_ASSERT(proven_cert_verify(chain, 1, &opt, &res) == PROVEN_ERR_INVALID_ARG, "and so is no store", "");
        proven_cert_store_destroy(store);
    }

    PROVEN_TEST_PASS("certificates are read strictly and chains are accepted and refused as another implementation built them to be.");
    return 0;
}
