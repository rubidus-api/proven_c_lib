#include "proven.h"
#include "proven_test.h"
#include "../src/proven/proven_internal_tls.h"
#include "test_unit_tls12_vectors.h"
#include <stdio.h>
#include <string.h>

/*
 * TLS 1.2 below the state machine: the PRF, the extended master secret, Finished, the key
 * block, and record protection for the AEAD suites.
 *
 * The script in test_unit_tls12_vectors.h is replayed line by line. Its first two lines are
 * the PRF test vectors published on the IETF TLS list; the rest was computed outside this
 * library, by a generator whose own PRF first reproduced those two. The letters:
 *
 *   P hash secret label seed out                 the PRF
 *   M hash premaster session_hash master         the extended master secret
 *   F hash master who handshake_hash verify      Finished; who is c or s
 *   K suite master client_random server_random   install keys from the key block
 *   R who type plaintext record                  who seals this as its next record
 */

static proven_size_t unhex(const char *s, proven_size_t n, proven_byte_t *out) {
    if (n == 1 && s[0] == '-') return 0;
    for (proven_size_t i = 0; i + 1 < n; i += 2) {
        unsigned v = 0;
        for (int k = 0; k < 2; ++k) { char c = s[i + (proven_size_t)k]; v = v * 16 + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10); }
        out[i / 2] = (proven_byte_t)v;
    }
    return n / 2;
}

/* The next space-separated field of a line. */
static const char *field(const char **at, proven_size_t *len) {
    const char *p = *at;
    while (*p == ' ') ++p;
    const char *start = p;
    while (*p && *p != ' ') ++p;
    *len = (proven_size_t)(p - start);
    *at = p;
    return start;
}

static proven_hmac_hash_t hash_of(const char *name, proven_size_t n) { return n == 6 && memcmp(name, "sha384", 6) == 0 ? PROVEN_HMAC_SHA384 : PROVEN_HMAC_SHA256; }

int main(void) {
    PROVEN_TEST_SUITE("TLS 1.2 key derivation and records",
        "The PRF against the test vectors published on the IETF TLS list, then the extended master secret, Finished, the key block and AEAD records against values computed outside the library.",
        "Inspect src/proven/tls12_keys.c. The failing line's letter names the function; tests/test_unit_tls12_vectors.h carries the values, written by a private generator.");

    static proven_byte_t a[4096], b[4096], c[4096], out[4096], record[4096];
    static proven_tls_keys_t write_keys[2], read_keys[2];        /* [0] the client's direction, [1] the server's */
    int counts[128] = { 0 }, failed = 0;
    char label[64];
    for (proven_size_t i = 0; i < sizeof TLS12V / sizeof TLS12V[0] && !failed; ++i) {
        const char *at = TLS12V[i];
        proven_size_t n1, n2, n3, n4, n5;
        char kind = at[0];
        at += 1;
        const char *f1 = field(&at, &n1), *f2 = field(&at, &n2), *f3 = field(&at, &n3), *f4 = field(&at, &n4), *f5 = field(&at, &n5);
        bool ok = false;
        if (kind == 'P') {
            proven_size_t sl = unhex(f2, n2, a), ll = unhex(f3, n3, (proven_byte_t *)label), dl = unhex(f4, n4, b), ol = unhex(f5, n5, c);
            label[ll] = 0;
            proven_tls12_prf(hash_of(f1, n1), (proven_mem_view_t){ a, sl }, label, (proven_mem_view_t){ b, dl }, out, ol);
            ok = memcmp(out, c, ol) == 0;
        } else if (kind == 'M') {
            proven_size_t pl = unhex(f2, n2, a), hl = unhex(f3, n3, b);
            (void)unhex(f4, n4, c);
            proven_tls12_master_secret(hash_of(f1, n1), (proven_mem_view_t){ a, pl }, (proven_mem_view_t){ b, hl }, out);
            ok = memcmp(out, c, PROVEN_TLS12_MASTER_SIZE) == 0;
        } else if (kind == 'F') {
            (void)unhex(f2, n2, a);
            proven_size_t hl = unhex(f4, n4, b);
            (void)unhex(f5, n5, c);
            proven_tls12_finished(hash_of(f1, n1), a, f3[0] == 's', (proven_mem_view_t){ b, hl }, out);
            ok = memcmp(out, c, PROVEN_TLS12_VERIFY_SIZE) == 0;
        } else if (kind == 'K') {
            proven_byte_t id[2];
            (void)unhex(f1, n1, id); (void)unhex(f2, n2, a); (void)unhex(f3, n3, b); (void)unhex(f4, n4, c);
            const proven_tls12_suite_t *suite = proven_tls12_suite_find((proven_u16)(id[0] << 8 | id[1]));
            ok = suite != NULL;
            if (ok) {
                proven_tls12_set_keys(&write_keys[0], &write_keys[1], suite, a, b, c);
                proven_tls12_set_keys(&read_keys[0], &read_keys[1], suite, a, b, c);
            }
        } else if (kind == 'R') {
            int who = f1[0] == 's';
            proven_byte_t type = (proven_byte_t)atoi(f2);
            proven_size_t pl = unhex(f3, n3, a), rl = unhex(f4, n4, b);
            proven_size_t made = proven_tls12_seal(&write_keys[who], type, (proven_mem_view_t){ a, pl }, record);
            ok = made == rl && memcmp(record, b, rl) == 0;
            /* And the generator's record opens to its plaintext, under the same direction's keys. */
            proven_mem_mut_t content = { 0 };
            ok = ok && proven_tls12_open(&read_keys[who], b, rl - PROVEN_TLS_RECORD_HEADER, &content) && content.size == pl && (pl == 0 || memcmp(content.ptr, a, pl) == 0);
        }
        counts[(int)kind]++;
        if (!ok) { failed = 1; fprintf(stderr, "line %u (%c) differs\n", (unsigned)i, kind); }
    }
    PROVEN_TEST_ASSERT(!failed, "every line of the script is reproduced", "");
    PROVEN_TEST_ASSERT(counts['P'] == 18 && counts['M'] == 4 && counts['F'] == 4 && counts['K'] == 6 && counts['R'] == 48,
        "eighteen PRF outputs (two of them published), four master secrets, four Finished values, six key blocks and forty-eight records were checked", "");

    PROVEN_TEST_SECTION("what record protection refuses", "Records that were changed, repeated, cut short or sealed for the other direction.", "Check proven_tls12_open: the additional data carries the sequence number, the type and the length.");
    {
        proven_byte_t master[48], cr[32], sr[32], plain[40];
        memset(master, 7, sizeof master); memset(cr, 1, sizeof cr); memset(sr, 2, sizeof sr); memset(plain, 0x5a, sizeof plain);
        static const proven_u16 ids[3] = { 0xc02b, 0xc030, 0xcca9 };
        for (int s = 0; s < 3; ++s) {
            const proven_tls12_suite_t *suite = proven_tls12_suite_find(ids[s]);
            static proven_tls_keys_t cw, sw, cr_keys, sr_keys;
            proven_mem_mut_t content;
            proven_tls12_set_keys(&cw, &sw, suite, master, cr, sr);
            proven_tls12_set_keys(&cr_keys, &sr_keys, suite, master, cr, sr);
            proven_size_t n = proven_tls12_seal(&cw, PROVEN_TLS_CT_APPLICATION, (proven_mem_view_t){ plain, sizeof plain }, record);
            proven_size_t overhead = n - PROVEN_TLS_RECORD_HEADER - sizeof plain;
            PROVEN_TEST_ASSERT(overhead == (suite->chacha ? 16u : 24u), "a record is its content, a 16-byte tag, and with AES-GCM an 8-byte explicit nonce", "");
            memcpy(b, record, n);
            b[n - 1] ^= 1;
            PROVEN_TEST_ASSERT(!proven_tls12_open(&cr_keys, b, n - 5, &content), "a changed tag is refused", "");
            memcpy(b, record, n); b[0] = PROVEN_TLS_CT_HANDSHAKE;
            PROVEN_TEST_ASSERT(!proven_tls12_open(&cr_keys, b, n - 5, &content), "a changed content type is refused: it is authenticated though it travels in the clear", "");
            memcpy(b, record, n);
            PROVEN_TEST_ASSERT(!proven_tls12_open(&sr_keys, b, n - 5, &content), "a record sealed for the other direction is refused", "");
            memcpy(b, record, n);
            PROVEN_TEST_ASSERT(!proven_tls12_open(&cr_keys, b, n - 6, &content), "cut short by a byte, it is refused", "");
            memcpy(b, record, n);
            PROVEN_TEST_ASSERT(proven_tls12_open(&cr_keys, b, n - 5, &content) && content.size == sizeof plain && memcmp(content.ptr, plain, sizeof plain) == 0, "unchanged, it opens", "");
            memcpy(b, record, n);
            PROVEN_TEST_ASSERT(!proven_tls12_open(&cr_keys, b, n - 5, &content), "and a second time it does not: the sequence number has moved on", "");
            PROVEN_TEST_ASSERT(!proven_tls12_open(&cr_keys, b, 15, &content) && !proven_tls12_open(&cr_keys, b, PROVEN_TLS_MAX_CIPHERTEXT + 1, &content), "lengths below a tag and above the maximum are refused on sight", "");
        }
        PROVEN_TEST_ASSERT(proven_tls12_suite_find(0x1301) == NULL && proven_tls12_suite_find(0xc013) == NULL && proven_tls12_suite_find(0x009c) == NULL,
            "a TLS 1.3 suite, a CBC suite and a static-RSA suite are not among the 1.2 suites", "");
    }

    PROVEN_TEST_PASS("the TLS 1.2 PRF, key derivation and record protection reproduce values computed elsewhere.");
    return 0;
}
