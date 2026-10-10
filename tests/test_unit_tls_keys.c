#include "proven.h"
#include "proven_test.h"
#include "../src/proven/proven_internal_tls.h"
#include "test_unit_tls_vectors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * TLS 1.3 below the state machine - the transcript hash, the key schedule and record
 * protection - replayed against RFC 8448, "Example Handshake Traces for TLS 1.3".
 *
 * The RFC prints, for five handshakes, every message, every secret with the inputs it was
 * derived from, and every record as it went on the wire. test_unit_tls_vectors.h is that text
 * turned into steps by a private generator; this test walks the steps in the RFC's order,
 * keeps its own transcript, and requires every value to come out as printed:
 *
 *   M hex                      a handshake message enters the transcript
 *   H hex                      a HelloRetryRequest: the transcript is replaced, then it enters
 *   B prefix hash key finished the PSK binder: hash of the truncated ClientHello, and its MAC
 *   X salt ikm secret          HKDF-Extract ("-" is the zero string)
 *   D prk info expanded flag   HKDF-Expand-Label; flag T: the context must be the transcript
 *                              hash at this point, E: the hash of nothing
 *   F base key finished        Finished: its key, and the MAC over the transcript hash now
 *   K who secret key iv        who (c or s) starts writing under this traffic secret
 *   R who type payload record  a protected record sent by who
 *   P type payload record      a record in the clear
 *
 * The traces use TLS_AES_128_GCM_SHA256 throughout: SHA-384 and ChaCha20 are not covered here.
 */

#define MAX_FIELDS 6
static char g_line[8192];
static proven_byte_t g_bytes[MAX_FIELDS][2048];
static proven_size_t g_len[MAX_FIELDS];
static char *g_field[MAX_FIELDS];

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

typedef struct { unsigned messages, extracts, expands, bound, finished, keys, records, clear, binders; } tally_t;

/* Replay one trace. Returns the number of steps that came out as the RFC prints them. */
static proven_size_t replay(const char *const *steps, proven_size_t count, tally_t *tally) {
    const proven_tls_suite_t *suite = proven_tls_suite_find(PROVEN_TLS_AES_128_GCM_SHA256);
    proven_tls_transcript_t transcript;
    proven_tls_keys_t write_keys[2];
    proven_byte_t hash[PROVEN_HMAC_MAX_SIZE], out[64];
    static proven_byte_t record[2048 + 32];
    proven_size_t good = 0;
    memset(write_keys, 0, sizeof write_keys);
    proven_tls_transcript_init(&transcript, suite->hash);
    for (proven_size_t i = 0; i < count; ++i) {
        split(steps[i]);
        bool ok = false;
        switch (g_field[0][0]) {
        case 'M':
            proven_tls_transcript_update(&transcript, (proven_mem_view_t){ g_bytes[1], g_len[1] });
            ok = true; tally->messages++;
            break;
        case 'H':
            proven_tls_transcript_retry(&transcript);
            proven_tls_transcript_update(&transcript, (proven_mem_view_t){ g_bytes[1], g_len[1] });
            ok = true; tally->messages++;
            break;
        case 'B': {
            proven_tls_transcript_t partial;
            proven_tls_transcript_init(&partial, suite->hash);
            proven_tls_transcript_update(&partial, (proven_mem_view_t){ g_bytes[1], g_len[1] });
            ok = proven_tls_transcript_hash(&partial, hash) == 32 && memcmp(hash, g_bytes[2], 32) == 0;
            proven_tls13_finished(suite->hash, g_bytes[3], hash, out);
            ok = ok && memcmp(out, g_bytes[4], 32) == 0;
            tally->binders++;
            break; }
        case 'X':
            proven_tls13_extract(suite->hash, g_field[1][0] == '-' ? NULL : g_bytes[1], (proven_mem_view_t){ g_bytes[2], g_len[2] }, out);
            ok = memcmp(out, g_bytes[3], 32) == 0;
            tally->extracts++;
            break;
        case 'D': {
            /* info: uint16 length, a length-prefixed label that starts "tls13 ", a length-prefixed context. */
            const proven_byte_t *info = g_bytes[2];
            proven_size_t out_len = ((proven_size_t)info[0] << 8) | info[1], label_len = info[2], ctx_len = info[3 + label_len];
            char label[40] = { 0 };
            ok = label_len > 6 && label_len < 38 && memcmp(info + 3, "tls13 ", 6) == 0 && 4 + label_len + ctx_len == g_len[2] && out_len == g_len[3];
            if (!ok) break;
            memcpy(label, info + 9, label_len - 6);
            const proven_byte_t *ctx = info + 4 + label_len;
            proven_tls13_expand_label(suite->hash, g_bytes[1], label, (proven_mem_view_t){ ctx, ctx_len }, out, out_len);
            ok = memcmp(out, g_bytes[3], out_len) == 0;
            if (g_field[4][0] == 'T') {
                ok = ok && ctx_len == 32 && proven_tls_transcript_hash(&transcript, hash) == 32 && memcmp(hash, ctx, 32) == 0;
                proven_tls13_derive_secret(suite->hash, g_bytes[1], label, hash, out);
                ok = ok && memcmp(out, g_bytes[3], 32) == 0;
                tally->bound++;
            } else if (g_field[4][0] == 'E') {
                proven_tls_transcript_t empty;
                proven_tls_transcript_init(&empty, suite->hash);
                ok = ok && proven_tls_transcript_hash(&empty, hash) == 32 && memcmp(hash, ctx, 32) == 0;
            }
            tally->expands++;
            break; }
        case 'F': {
            proven_byte_t key[32];
            proven_tls13_expand_label(suite->hash, g_bytes[1], "finished", (proven_mem_view_t){ key, 0 }, key, 32);
            ok = memcmp(key, g_bytes[2], 32) == 0;
            (void)proven_tls_transcript_hash(&transcript, hash);
            proven_tls13_finished(suite->hash, g_bytes[1], hash, out);
            ok = ok && memcmp(out, g_bytes[3], 32) == 0;
            tally->finished++;
            break; }
        case 'K': {
            proven_tls_keys_t *k = &write_keys[g_field[1][0] == 's'];
            proven_tls13_set_keys(k, suite, g_bytes[2]);
            ok = k->active && !k->chacha && k->seq == 0;
            if (g_field[4][0] != '-') ok = ok && memcmp(k->iv, g_bytes[4], 12) == 0;
            if (g_field[3][0] != '-') {
                /* The key itself is inside the cipher context: check it by what it encrypts. */
                proven_crypto_aes_gcm_t want;
                proven_byte_t zero[16] = { 0 }, a[16], b[16];
                ok = ok && proven_crypto_aes_gcm_init(&want, g_bytes[3], 16);
                proven_crypto_aes_encrypt_block(&want, zero, a);
                proven_crypto_aes_encrypt_block(&k->aes, zero, b);
                ok = ok && memcmp(a, b, 16) == 0;
            }
            tally->keys++;
            break; }
        case 'R': {
            proven_tls_keys_t *k = &write_keys[g_field[1][0] == 's'];
            proven_byte_t type = (proven_byte_t)atoi(g_field[2]), got_type = 0;
            proven_mem_mut_t content;
            proven_u64 seq = k->seq;
            proven_size_t n = proven_tls13_seal(k, type, (proven_mem_view_t){ g_bytes[3], g_len[3] }, record);
            ok = k->active && n == g_len[4] && memcmp(record, g_bytes[4], n) == 0 && k->seq == seq + 1;
            /* And back: the peer reads with the same keys at the same sequence number. */
            proven_tls_keys_t reader = *k;
            reader.seq = seq;
            ok = ok && proven_tls13_open(&reader, record, n - PROVEN_TLS_RECORD_HEADER, &got_type, &content) && got_type == type &&
                 content.size == g_len[3] && memcmp(content.ptr, g_bytes[3], g_len[3]) == 0;
            /* One changed bit anywhere in the record is refused. */
            memcpy(record, g_bytes[4], n);
            record[n / 2] ^= 0x04;
            reader = *k; reader.seq = seq;
            ok = ok && !proven_tls13_open(&reader, record, n - PROVEN_TLS_RECORD_HEADER, &got_type, &content);
            /* And so is the right record under the wrong sequence number. */
            memcpy(record, g_bytes[4], n);
            reader = *k; reader.seq = seq + 1;
            ok = ok && !proven_tls13_open(&reader, record, n - PROVEN_TLS_RECORD_HEADER, &got_type, &content);
            tally->records++;
            break; }
        case 'P':
            ok = g_len[3] == g_len[2] + 5 && g_bytes[3][0] == (proven_byte_t)atoi(g_field[1]) && memcmp(g_bytes[3] + 5, g_bytes[2], g_len[2]) == 0 &&
                 ((proven_size_t)g_bytes[3][3] << 8 | g_bytes[3][4]) == g_len[2];
            tally->clear++;
            break;
        default: break;
        }
        if (ok) good++;
        else fprintf(stderr, "step %u (%c) disagrees with the RFC\n", (unsigned)i, g_field[0][0]);
    }
    return good;
}

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

int main(void) {
    PROVEN_TEST_SUITE("TLS 1.3: transcript, key schedule and records against RFC 8448",
        "Five handshakes replayed step by step; every secret, key, MAC and protected record must be the one the RFC prints.",
        "Inspect src/proven/tls_keys.c. The failing step's letter says which function; the vector header carries the RFC's values.");

    tally_t t = { 0 };

    PROVEN_TEST_SECTION("section 3, a simple 1-RTT handshake", "The whole schedule: early, handshake and master secrets, traffic secrets, Finished, the resumption secret, and every record.", "");
    PROVEN_TEST_ASSERT(replay(TLSV_SIMPLE, COUNT(TLSV_SIMPLE), &t) == COUNT(TLSV_SIMPLE) && COUNT(TLSV_SIMPLE) >= 35, "every step as printed", "");

    PROVEN_TEST_SECTION("section 4, resumption with a pre-shared key", "The PSK as the early secret, the binder over the truncated ClientHello, and the early traffic secret.", "");
    PROVEN_TEST_ASSERT(replay(TLSV_RESUMED, COUNT(TLSV_RESUMED), &t) == COUNT(TLSV_RESUMED) && t.binders == 1, "every step as printed, the binder among them", "");

    PROVEN_TEST_SECTION("section 5, HelloRetryRequest", "The first ClientHello replaced in the transcript by its hash, and a P-256 key share.", "");
    PROVEN_TEST_ASSERT(replay(TLSV_RETRY, COUNT(TLSV_RETRY), &t) == COUNT(TLSV_RETRY), "every step as printed", "");

    PROVEN_TEST_SECTION("section 6, client authentication", "The client's Certificate and CertificateVerify in the transcript before its Finished.", "");
    PROVEN_TEST_ASSERT(replay(TLSV_CLIENT_AUTH, COUNT(TLSV_CLIENT_AUTH), &t) == COUNT(TLSV_CLIENT_AUTH), "every step as printed", "");

    PROVEN_TEST_SECTION("section 7, compatibility mode", "ChangeCipherSpec records travel and are not part of the transcript.", "");
    PROVEN_TEST_ASSERT(replay(TLSV_COMPAT, COUNT(TLSV_COMPAT), &t) == COUNT(TLSV_COMPAT), "every step as printed", "");

    PROVEN_TEST_ASSERT(t.bound >= 30 && t.records >= 25 && t.finished == 10 && t.extracts == 15 && t.keys >= 20,
        "the replay covered what it claims: transcript-bound secrets, protected records, Finished values, extractions, key changes", "");

    PROVEN_TEST_SECTION("what the traces do not exercise", "The other two suites, padding, and the refusals of the record layer.", "Check proven_tls13_seal and proven_tls13_open.");
    {
        static const proven_u16 ids[3] = { PROVEN_TLS_AES_128_GCM_SHA256, PROVEN_TLS_AES_256_GCM_SHA384, PROVEN_TLS_CHACHA20_POLY1305_SHA256 };
        static proven_byte_t rec[PROVEN_TLS_RECORD_HEADER + PROVEN_TLS_MAX_PLAINTEXT + PROVEN_TLS_MAX_EXPANSION + 64], text[PROVEN_TLS_MAX_PLAINTEXT];
        bool all = true;
        for (proven_size_t i = 0; i < sizeof text; ++i) text[i] = (proven_byte_t)(i * 13 + 5);
        for (int s = 0; s < 3; ++s) {
            const proven_tls_suite_t *suite = proven_tls_suite_find(ids[s]);
            proven_byte_t secret[48];
            proven_tls_keys_t w, r;
            for (int i = 0; i < 48; ++i) secret[i] = (proven_byte_t)(i + s);
            all = all && suite && suite->hash_len == (s == 1 ? 48u : 32u);
            proven_tls13_set_keys(&w, suite, secret);
            proven_tls13_set_keys(&r, suite, secret);
            static const proven_size_t sizes[] = { 0, 1, 15, 16, 17, 255, 256, 16383, 16384 };
            for (proven_size_t k = 0; k < sizeof sizes / sizeof sizes[0]; ++k) {
                proven_byte_t type = 0;
                proven_mem_mut_t content;
                proven_size_t n = proven_tls13_seal(&w, PROVEN_TLS_CT_APPLICATION, (proven_mem_view_t){ text, sizes[k] }, rec);
                all = all && n == 5 + sizes[k] + 17 && rec[0] == 23 && rec[1] == 3 && rec[2] == 3;
                all = all && proven_tls13_open(&r, rec, n - 5, &type, &content) && type == PROVEN_TLS_CT_APPLICATION &&
                      content.size == sizes[k] && memcmp(content.ptr, text, sizes[k]) == 0;
            }
            all = all && w.seq == 9 && r.seq == 9;
        }
        PROVEN_TEST_ASSERT(all, "the three suites seal and open records from empty to 2^14 bytes, counting sequence numbers", "");
        PROVEN_TEST_ASSERT(proven_tls_suite_find(0x1304) == NULL && proven_tls_suite_find(0x002f) == NULL, "a suite that is not implemented is not found", "");

        /* Padding: zeros after the content type are removed; a record of only zeros is refused. */
        const proven_tls_suite_t *suite = proven_tls_suite_find(PROVEN_TLS_CHACHA20_POLY1305_SHA256);
        proven_byte_t secret[32] = { 7 }, inner[40], type = 0;
        proven_tls_keys_t w, r;
        proven_mem_mut_t content;
        proven_tls13_set_keys(&w, suite, secret); proven_tls13_set_keys(&r, suite, secret);
        memset(inner, 0, sizeof inner);
        memcpy(inner, "hello", 5); inner[5] = PROVEN_TLS_CT_HANDSHAKE;          /* content, type, then 33 zeros and the seal's own type byte */
        proven_size_t n = proven_tls13_seal(&w, 0, (proven_mem_view_t){ inner, 39 }, rec);
        PROVEN_TEST_ASSERT(proven_tls13_open(&r, rec, n - 5, &type, &content) && type == PROVEN_TLS_CT_HANDSHAKE && content.size == 5 && memcmp(content.ptr, "hello", 5) == 0,
            "padding after the content type is stripped", "");
        memset(inner, 0, sizeof inner);
        n = proven_tls13_seal(&w, 0, (proven_mem_view_t){ inner, 20 }, rec);
        PROVEN_TEST_ASSERT(!proven_tls13_open(&r, rec, n - 5, &type, &content), "a record that decrypts to nothing but zeros has no content type and is refused", "");
        PROVEN_TEST_ASSERT(!proven_tls13_open(&r, rec, 16, &type, &content) && !proven_tls13_open(&r, rec, PROVEN_TLS_MAX_CIPHERTEXT + 1, &type, &content),
            "a body shorter than a tag plus a type, or longer than the limit, is refused before any decryption", "");
    }

    PROVEN_TEST_PASS("the key schedule and the record layer reproduce RFC 8448.");
    return 0;
}
