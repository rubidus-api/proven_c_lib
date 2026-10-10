#include "proven.h"
#include "proven_test.h"
#include "../src/proven/proven_internal_tls.h"
#include "test_unit_tls_pki.h"
#include "test_unit_tls_vectors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The TLS 1.3 engine, with no network: a client connection and a server connection wired back
 * to back in memory. What one puts in its pending output the test hands to the other - whole,
 * a byte at a time, or with a bit changed.
 *
 * The clock and the random source are the test's own (the config takes both), so every run is
 * the same run and certificates can be made to expire by moving a variable. The certificates
 * and keys are made at start by test_unit_tls_pki.h, so that no key is stored in the tree. The
 * last section replays two handshakes of RFC 8448 through the client, record for record.
 */

static proven_u64 g_rng = 0x9e3779b97f4a7c15ull;
static void test_random(void *ctx, proven_byte_t *out, proven_size_t len) {
    (void)ctx;
    for (proven_size_t i = 0; i < len; ++i) {
        g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
        out[i] = (proven_byte_t)(g_rng >> 24);
    }
}
static proven_i64 test_now(void *ctx) { return *(proven_i64 *)ctx; }

static proven_mem_view_t pem(const char *s) { return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }
static proven_u8str_view_t sv(const char *s) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }

/* A heap that counts what is live, to measure what a connection holds. */
typedef struct { proven_allocator_t heap; proven_size_t live; } counting_t;
typedef struct { proven_size_t size; proven_size_t pad; } count_head_t;
static proven_result_mem_mut_t count_alloc(void *ctx, proven_size_t size, proven_size_t align) {
    counting_t *c = ctx; (void)align;
    proven_result_mem_mut_t m = c->heap.alloc_fn(c->heap.ctx, size + sizeof(count_head_t), 16);
    if (m.err != PROVEN_OK) return m;
    ((count_head_t *)(void *)m.value.ptr)->size = size;
    c->live += size;
    m.value.ptr += sizeof(count_head_t); m.value.size = size;
    return m;
}
static void count_free(void *ctx, void *ptr) {
    counting_t *c = ctx;
    if (!ptr) return;
    count_head_t *h = (count_head_t *)(void *)((proven_byte_t *)ptr - sizeof(count_head_t));
    c->live -= h->size;
    c->heap.free_fn(c->heap.ctx, h);
}
static proven_result_mem_mut_t count_realloc(void *ctx, void *old, proven_size_t old_size, proven_size_t new_size, proven_size_t align) {
    proven_result_mem_mut_t m = count_alloc(ctx, new_size, align);
    if (m.err != PROVEN_OK) return m;
    memcpy(m.value.ptr, old, old_size < new_size ? old_size : new_size);
    count_free(ctx, old);
    return m;
}

static counting_t g_count;
static proven_allocator_t g_alloc;
static proven_cert_store_t *g_anchors, *g_other_anchors;
static proven_i64 g_now = TP_NOW;

static proven_tls_options_t base_options(void) {
    proven_tls_options_t o = { .alloc = g_alloc, .random = test_random, .now = test_now, .now_ctx = &g_now };
    return o;
}

static proven_tls_config_t *client_config(const proven_tls_options_t *extra) {
    proven_tls_options_t o = extra ? *extra : base_options();
    if (!o.anchors && o.verify == PROVEN_TLS_VERIFY_CHAIN) o.anchors = g_anchors;
    proven_tls_config_t *c = NULL;
    PROVEN_TEST_ASSERT(proven_tls_config_create(&o, &c) == PROVEN_OK, "a client config", "");
    return c;
}

static proven_tls_config_t *server_config(const char *cert, const char *key, const proven_tls_options_t *extra) {
    proven_tls_options_t o = extra ? *extra : base_options();
    o.certificate_pem = pem(cert); o.private_key_pem = pem(key);
    proven_tls_config_t *c = NULL;
    PROVEN_TEST_ASSERT(proven_tls_config_create(&o, &c) == PROVEN_OK, "a server config", "");
    return c;
}

/* Move everything pending from `from` to `to`, `chunk` bytes per feed (0: all at once),
 * flipping bit `flip_bit` of byte number `flip_at` of the stream when *counter reaches it. */
typedef struct { proven_size_t chunk; long flip_at; long *counter; } wire_t;
static proven_err_t g_err_c, g_err_s;

/* Bytes taken from one side that the other would not consume yet (it wants its application
 * data read first). They wait here, per direction, and go first next time. */
static proven_byte_t g_stash[2][70000];
static proven_size_t g_stash_len[2];

static bool move(proven_tls_conn_t *from, proven_tls_conn_t *to, proven_err_t *to_err, const wire_t *w, int dir) {
    bool any = false;
    for (;;) {
        if (g_stash_len[dir] == 0) {
            proven_mem_view_t out = proven_tls_pending_output(from);
            if (out.size == 0) return any;
            proven_size_t n = out.size < sizeof g_stash[0] ? out.size : sizeof g_stash[0];
            memcpy(g_stash[dir], out.ptr, n);
            proven_tls_output_sent(from, n);
            if (w && w->counter) {
                if (w->flip_at >= *w->counter && w->flip_at < *w->counter + (long)n) g_stash[dir][w->flip_at - *w->counter] ^= 0x10;
                *w->counter += (long)n;
            }
            g_stash_len[dir] = n;
        }
        any = true;
        proven_size_t at = 0, n = g_stash_len[dir];
        bool stalled = false;
        while (at < n && *to_err == PROVEN_OK && !stalled) {
            proven_size_t step = (w && w->chunk) ? w->chunk : n - at, used = 0;
            if (step > n - at) step = n - at;
            proven_err_t e = proven_tls_feed(to, (proven_mem_view_t){ .ptr = g_stash[dir] + at, .size = step }, &used);
            if (e != PROVEN_OK) { *to_err = e; break; }
            at += used;
            stalled = used < step;                    /* it wants its application data read first */
        }
        memmove(g_stash[dir], g_stash[dir] + at, n - at);
        g_stash_len[dir] = n - at;
        if (*to_err != PROVEN_OK) { g_stash_len[dir] = 0; return any; }
        if (stalled) return any;
    }
}

/* Run both sides until neither has anything for the other. */
static void pump(proven_tls_conn_t *c, proven_tls_conn_t *s, const wire_t *cs, const wire_t *sc) {
    for (int round = 0; round < 40; ++round) {
        bool a = move(c, s, &g_err_s, cs, 0);
        bool b = move(s, c, &g_err_c, sc, 1);
        if (!a && !b) break;
    }
}

typedef struct { proven_tls_conn_t *c, *s; } pair_t;

static pair_t connect_pair(const proven_tls_config_t *cc, const proven_tls_config_t *sc, const char *name, proven_tls_session_t *session, const wire_t *cs, const wire_t *scw) {
    pair_t p = { NULL, NULL };
    g_err_c = PROVEN_OK; g_err_s = PROVEN_OK;
    g_stash_len[0] = 0; g_stash_len[1] = 0;
    PROVEN_TEST_ASSERT(proven_tls_client_create(cc, sv(name), session, &p.c) == PROVEN_OK, "a client connection", "");
    PROVEN_TEST_ASSERT(proven_tls_server_create(sc, &p.s) == PROVEN_OK, "a server connection", "");
    pump(p.c, p.s, cs, scw);
    return p;
}

static void close_pair(pair_t *p) { proven_tls_conn_destroy(p->c); proven_tls_conn_destroy(p->s); p->c = NULL; p->s = NULL; }

static bool both_up(const pair_t *p) {
    return g_err_c == PROVEN_OK && g_err_s == PROVEN_OK && proven_tls_is_established(p->c) && proven_tls_is_established(p->s);
}

/* Send `n` patterned bytes from one side and read them on the other, exactly. */
static bool transfer(proven_tls_conn_t *from, proven_tls_conn_t *to, proven_size_t n, proven_byte_t seed) {
    static proven_byte_t data[200000], got[200000];
    static proven_byte_t wire[70000];
    for (proven_size_t i = 0; i < n; ++i) data[i] = (proven_byte_t)(i * 31 + seed);
    proven_size_t sent = 0, received = 0;
    for (int guard = 0; guard < 2000 && (sent < n || received < n); ++guard) {
        if (sent < n) {
            proven_result_size_t w = proven_tls_write(from, (proven_mem_view_t){ .ptr = data + sent, .size = n - sent });
            if (w.err != PROVEN_OK) return false;
            sent += w.value;
        }
        proven_mem_view_t out = proven_tls_pending_output(from);
        proven_size_t take = out.size < sizeof wire ? out.size : sizeof wire;
        memcpy(wire, out.ptr, take);
        proven_tls_output_sent(from, take);
        proven_size_t at = 0;
        while (at < take) {
            proven_size_t used = 0;
            if (proven_tls_feed(to, (proven_mem_view_t){ .ptr = wire + at, .size = take - at }, &used) != PROVEN_OK) return false;
            at += used;
            for (;;) {
                proven_result_size_t r = proven_tls_read(to, (proven_mem_mut_t){ .ptr = got + received, .size = sizeof got - received });
                if (r.err == PROVEN_ERR_NEED_MORE) break;
                if (r.err != PROVEN_OK) return false;
                received += r.value;
            }
        }
        if (take == 0 && sent >= n) break;
    }
    return sent == n && received == n && memcmp(data, got, n) == 0;
}

static bool view_same(proven_mem_view_t a, proven_mem_view_t b) { return a.size == b.size && memcmp(a.ptr, b.ptr, a.size) == 0; }

static proven_size_t unhex(const char *hex, proven_byte_t *out) {
    proven_size_t n = strlen(hex) / 2;
    for (proven_size_t i = 0; i < n; ++i) { char c[3] = { hex[2 * i], hex[2 * i + 1], 0 }; out[i] = (proven_byte_t)strtoul(c, NULL, 16); }
    return n;
}

/* Replay one RFC 8448 trace through the client. The script gives the client's X25519 key (K),
 * its ClientHello (H), the server's certificate (X), then every record in the order it was
 * sent: the engine is fed the server's (s) and must itself produce the client's (c), byte for
 * byte. An A line is the application data inside the record before it. */
static bool replay(const char *const *steps, proven_size_t count) {
    static proven_byte_t bytes[2048], payload[256], hello[600], key[32], cert_der[1024], got[256];
    proven_size_t hello_len = 0, cert_len = 0;
    proven_tls_conn_t *c = NULL;
    proven_tls_config_t *cfg = NULL;
    bool ok = true;
    /* The trace's keys are 1024-bit RSA, which nothing else here accepts. */
    proven_crypto_rsa_test_min_bits(1024);
    for (proven_size_t i = 0; i < count && ok; ++i) {
        const char *line = steps[i];
        proven_size_t n = unhex(line + (line[0] == 'A' ? 4 : 2), bytes);
        switch (line[0]) {
        case 'K': memcpy(key, bytes, 32); break;
        case 'H': memcpy(hello, bytes, n); hello_len = n; break;
        case 'X': memcpy(cert_der, bytes, n); cert_len = n; break;
        case 'c':
            if (!c) {
                /* The server is verified by its key alone: the trace's certificate chains to nothing. */
                proven_byte_t pin[1][32];
                proven_cert_t leaf;
                ok = cert_len > 0 && proven_cert_parse((proven_mem_view_t){ cert_der, cert_len }, &leaf) == PROVEN_OK &&
                     proven_cert_key_sha256(&leaf, pin[0]) == PROVEN_OK;
                proven_tls_options_t o = base_options();
                o.verify = PROVEN_TLS_VERIFY_PIN_ONLY; o.pins = (const proven_byte_t (*)[32])pin; o.pin_count = 1; o.no_resumption = true;
                ok = ok && proven_tls_config_create(&o, &cfg) == PROVEN_OK &&
                     proven_tls_client_create_replay(cfg, sv("server"), (proven_mem_view_t){ hello, hello_len }, key, &c) == PROVEN_OK;
                if (!ok) break;
            }
            if (proven_tls_pending_output(c).size == 0 && proven_tls_is_established(c)) {
                /* Nothing is waiting: this record is the application's doing. Data if the next
                 * line carries some, otherwise the close. */
                if (i + 1 < count && steps[i + 1][0] == 'A' && steps[i + 1][2] == 'c') {
                    proven_size_t pn = unhex(steps[i + 1] + 4, payload);
                    proven_result_size_t w = proven_tls_write(c, (proven_mem_view_t){ payload, pn });
                    ok = w.err == PROVEN_OK && w.value == pn;
                } else {
                    ok = proven_tls_close(c) == PROVEN_OK;
                }
            }
            {
                proven_mem_view_t out = proven_tls_pending_output(c);
                ok = ok && out.size >= n && memcmp(out.ptr, bytes, n) == 0;
                if (ok) proven_tls_output_sent(c, n);
            }
            break;
        case 's': {
            proven_size_t used = 0;
            ok = c && proven_tls_feed(c, (proven_mem_view_t){ bytes, n }, &used) == PROVEN_OK && used == n;
            break; }
        case 'A':
            if (line[2] == 's') {
                proven_result_size_t r = proven_tls_read(c, (proven_mem_mut_t){ got, sizeof got });
                ok = r.err == PROVEN_OK && r.value == n && memcmp(got, bytes, n) == 0;
            }
            break;
        default: break;
        }
        if (!ok) fprintf(stderr, "replay: step %u (%c) differs\n", (unsigned)i, line[0]);
    }
    /* Everything the client had to say has been accounted for, and the server closed properly. */
    ok = ok && c && proven_tls_pending_output(c).size == 0 && proven_tls_read(c, (proven_mem_mut_t){ got, sizeof got }).err == PROVEN_ERR_EOF;
    proven_crypto_rsa_test_min_bits(0);
    proven_tls_conn_destroy(c);
    proven_tls_config_destroy(cfg);
    return ok;
}

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

int main(void) {
    PROVEN_TEST_SUITE("the TLS 1.3 engine",
        "A client and a server connection wired together in memory: every suite and key type, retries, ALPN, each way verification fails, client certificates, resumption, tampering, and RFC 8448 replayed.",
        "Inspect src/proven/tls13.c (the state machine) and src/proven/tls_config.c (identity, tickets). The clock and the random source are the test's, so a failure repeats exactly.");

    g_count.heap = proven_heap_allocator();
    g_alloc = (proven_allocator_t){ .ctx = &g_count, .alloc_fn = count_alloc, .realloc_fn = count_realloc, .free_fn = count_free };
    PROVEN_TEST_ASSERT(tp_build(), "the test's certificates and keys are issued", "Check src/proven/tls_issue.c.");
    PROVEN_TEST_ASSERT(proven_cert_store_create(g_alloc, &g_anchors) == PROVEN_OK && proven_cert_store_add_pem(g_anchors, pem(TP_CA), NULL) == PROVEN_OK &&
                       proven_cert_store_create(g_alloc, &g_other_anchors) == PROVEN_OK && proven_cert_store_add_pem(g_other_anchors, pem(TP_OTHER_CA), NULL) == PROVEN_OK,
        "trust anchors", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a configuration is checked when it is made",
        "Everything that can be wrong with the options is an error from proven_tls_config_create, not from the first handshake.",
        "Check proven_tls_config_create.");
    {
        proven_tls_config_t *c = (proven_tls_config_t *)1;
        proven_tls_options_t o = base_options();
        PROVEN_TEST_ASSERT(proven_tls_config_create(NULL, &c) == PROVEN_ERR_INVALID_ARG && proven_tls_config_create(&o, NULL) == PROVEN_ERR_INVALID_ARG, "null arguments", "");
        PROVEN_TEST_ASSERT(proven_tls_config_create(&o, &c) == PROVEN_ERR_INVALID_ARG, "a client that would verify a chain needs anchors", "");
        o.alloc = (proven_allocator_t){ 0 }; o.anchors = g_anchors;
        PROVEN_TEST_ASSERT(proven_tls_config_create(&o, &c) == PROVEN_ERR_INVALID_ARG, "no allocator", "");
        o = base_options(); o.verify = PROVEN_TLS_VERIFY_PIN_ONLY;
        PROVEN_TEST_ASSERT(proven_tls_config_create(&o, &c) == PROVEN_ERR_INVALID_ARG, "PIN_ONLY with no pins", "");
        o = base_options(); o.anchors = g_anchors; o.certificate_pem = pem(TP_SERVER_P256);
        PROVEN_TEST_ASSERT(proven_tls_config_create(&o, &c) == PROVEN_ERR_INVALID_ARG, "a certificate with no key", "");
        o = base_options(); o.anchors = g_anchors; o.private_key_pem = pem(TP_SERVER_P256_KEY);
        PROVEN_TEST_ASSERT(proven_tls_config_create(&o, &c) == PROVEN_ERR_INVALID_ARG, "a key with no certificate", "");
        o = base_options(); o.certificate_pem = pem("not PEM at all"); o.private_key_pem = pem(TP_SERVER_P256_KEY);
        PROVEN_TEST_ASSERT(proven_tls_config_create(&o, &c) == PROVEN_ERR_INVALID_FORMAT, "a certificate file with no certificate is PROVEN_ERR_INVALID_FORMAT", "");
        o.certificate_pem = pem(TP_SERVER_P256); o.private_key_pem = pem(TP_CA);
        PROVEN_TEST_ASSERT(proven_tls_config_create(&o, &c) == PROVEN_ERR_INVALID_FORMAT, "a key file with no key", "");
        o.certificate_pem = pem(TP_SERVER_P256); o.private_key_pem = pem(TP_RSA_KEY);
        PROVEN_TEST_ASSERT(proven_tls_config_create(&o, &c) == PROVEN_ERR_INVALID_FORMAT, "something labelled an RSA key that is not one is PROVEN_ERR_INVALID_FORMAT", "");
        o.certificate_pem = pem(TP_SERVER_P256); o.private_key_pem = pem(TP_P384_KEY);
        PROVEN_TEST_ASSERT(proven_tls_config_create(&o, &c) == PROVEN_ERR_UNSUPPORTED, "and so is a P-384 key", "");
        o.certificate_pem = pem(TP_SERVER_P256); o.private_key_pem = pem(TP_CLIENT_KEY);
        PROVEN_TEST_ASSERT(proven_tls_config_create(&o, &c) == PROVEN_ERR_INVALID_STATE, "a key that is not the certificate's is PROVEN_ERR_INVALID_STATE", "");
        o.certificate_pem = pem(TP_SERVER_ED); o.private_key_pem = pem(TP_SERVER_P256_KEY);
        PROVEN_TEST_ASSERT(proven_tls_config_create(&o, &c) == PROVEN_ERR_INVALID_STATE, "nor a key of another kind", "");
        o.certificate_pem = pem(TP_SERVER_P256); o.private_key_pem = pem(TP_SERVER_P256_KEY); o.client_auth = PROVEN_TLS_CLIENT_AUTH_REQUIRE;
        PROVEN_TEST_ASSERT(proven_tls_config_create(&o, &c) == PROVEN_ERR_INVALID_ARG, "a server that asks for client certificates needs anchors to check them against", "");
        o.client_auth = PROVEN_TLS_CLIENT_AUTH_NONE;
        proven_u8str_view_t bad_alpn[1] = { { (const proven_byte_t *)"", 0 } };
        o.alpn = bad_alpn; o.alpn_count = 1;
        PROVEN_TEST_ASSERT(proven_tls_config_create(&o, &c) == PROVEN_ERR_INVALID_ARG && c == (proven_tls_config_t *)1, "an empty ALPN name; and nothing was written to the out pointer by any of these", "");
        o.alpn = NULL; o.alpn_count = 0; o.private_key_pem = pem(TP_SERVER_P256_KEY_SEC1);
        PROVEN_TEST_ASSERT(proven_tls_config_create(&o, &c) == PROVEN_OK, "the same key as an \"EC PRIVATE KEY\" block is accepted", "");
        proven_tls_conn_t *conn = NULL;
        PROVEN_TEST_ASSERT(proven_tls_server_create(c, &conn) == PROVEN_OK, "and serves", "");
        proven_tls_conn_destroy(conn);
        proven_tls_config_destroy(c);
        proven_tls_config_destroy(NULL);

        proven_tls_config_t *cc = client_config(NULL);
        static char long_name[300];
        memset(long_name, 'a', 254);
        PROVEN_TEST_ASSERT(proven_tls_server_create(cc, &conn) == PROVEN_ERR_INVALID_ARG, "a config with no certificate cannot serve", "");
        PROVEN_TEST_ASSERT(proven_tls_client_create(cc, sv(""), NULL, &conn) == PROVEN_ERR_INVALID_ARG && proven_tls_client_create(cc, sv(long_name), NULL, &conn) == PROVEN_ERR_INVALID_ARG &&
                           proven_tls_client_create(NULL, sv("x"), NULL, &conn) == PROVEN_ERR_INVALID_ARG,
            "a client needs the name it means to reach, of at most 253 bytes", "");
        proven_tls_config_destroy(cc);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a self-signed identity",
        "proven_tls_self_signed makes a certificate this library's own reader accepts, for the names and the period asked, and a key that matches it.",
        "Check src/proven/tls_issue.c.");
    {
        static proven_byte_t cert_pem[1024], key_pem[256], der[1024];
        proven_size_t cert_len = 0, key_len = 0, pos = 0, der_len = 0;
        proven_u8str_view_t names[3] = { PROVEN_LIT("box.example.test"), PROVEN_LIT("192.0.2.7"), PROVEN_LIT("2001:db8::7") }, label;
        PROVEN_TEST_ASSERT(proven_tls_self_signed(names, 3, TP_NOW - 10, TP_NOW + 1000, test_random, NULL, (proven_mem_mut_t){ cert_pem, sizeof cert_pem }, &cert_len,
                                                  (proven_mem_mut_t){ key_pem, sizeof key_pem }, &key_len) == PROVEN_OK && cert_len > 300 && key_len > 100, "made", "");
        proven_cert_t c;
        PROVEN_TEST_ASSERT(proven_pem_next((proven_mem_view_t){ cert_pem, cert_len }, &pos, &label, (proven_mem_mut_t){ der, sizeof der }, &der_len) == PROVEN_OK &&
                           proven_u8str_view_eq(label, PROVEN_LIT("CERTIFICATE")) && proven_cert_parse((proven_mem_view_t){ der, der_len }, &c) == PROVEN_OK,
            "the certificate is PEM that the strict reader parses", "");
        PROVEN_TEST_ASSERT(c.version == 3 && c.key_kind == PROVEN_CERT_KEY_ED25519 && c.sig_kind == PROVEN_CERT_SIG_ED25519 && !c.is_ca && view_same(c.subject, c.issuer) &&
                           c.not_before == TP_NOW - 10 && c.not_after == TP_NOW + 1000 && (c.key_usage & PROVEN_CERT_KU_DIGITAL_SIGNATURE) &&
                           c.ext_key_usage == (PROVEN_CERT_EKU_SERVER_AUTH | PROVEN_CERT_EKU_CLIENT_AUTH) && !c.unknown_critical,
            "version 3, Ed25519, self-issued, exactly the period asked for, usable by a server and a client", "");
        PROVEN_TEST_ASSERT(proven_cert_matches_host(&c, PROVEN_LIT("box.example.test")) && proven_cert_matches_host(&c, PROVEN_LIT("192.0.2.7")) &&
                           proven_cert_matches_host(&c, PROVEN_LIT("2001:db8:0:0:0:0:0:7")) && !proven_cert_matches_host(&c, PROVEN_LIT("example.test")),
            "it is for the DNS name and the two addresses, and nothing else", "");
        /* The pair works: a server with it, a client that holds the certificate as its anchor. */
        proven_cert_store_t *own = NULL;
        PROVEN_TEST_ASSERT(proven_cert_store_create(g_alloc, &own) == PROVEN_OK && proven_cert_store_add_pem(own, (proven_mem_view_t){ cert_pem, cert_len }, NULL) == PROVEN_OK, "as an anchor", "");
        proven_tls_options_t so = base_options(), co = base_options();
        so.certificate_pem = (proven_mem_view_t){ cert_pem, cert_len }; so.private_key_pem = (proven_mem_view_t){ key_pem, key_len };
        co.anchors = own;
        proven_tls_config_t *sc = NULL, *cc = NULL;
        PROVEN_TEST_ASSERT(proven_tls_config_create(&so, &sc) == PROVEN_OK && proven_tls_config_create(&co, &cc) == PROVEN_OK, "the key is the certificate's, as the config checks", "");
        pair_t p = connect_pair(cc, sc, "192.0.2.7", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p), "and a client that holds the certificate connects to the server that has the key", "");
        close_pair(&p);
        g_now = TP_NOW + 1001;
        p = connect_pair(cc, sc, "192.0.2.7", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(g_err_c == PROVEN_ERR_EXPIRED, "one second past its end it has expired", "");
        close_pair(&p);
        g_now = TP_NOW;
        proven_tls_config_destroy(sc); proven_tls_config_destroy(cc); proven_cert_store_destroy(own);

        proven_u8str_view_t empty_name[1] = { { (const proven_byte_t *)"", 0 } };
        PROVEN_TEST_ASSERT(proven_tls_self_signed(names, 0, 1, 2, test_random, NULL, (proven_mem_mut_t){ cert_pem, sizeof cert_pem }, &cert_len, (proven_mem_mut_t){ key_pem, sizeof key_pem }, &key_len) == PROVEN_ERR_INVALID_ARG &&
                           proven_tls_self_signed(empty_name, 1, 1, 2, test_random, NULL, (proven_mem_mut_t){ cert_pem, sizeof cert_pem }, &cert_len, (proven_mem_mut_t){ key_pem, sizeof key_pem }, &key_len) == PROVEN_ERR_INVALID_ARG &&
                           proven_tls_self_signed(names, 3, 5, 5, test_random, NULL, (proven_mem_mut_t){ cert_pem, sizeof cert_pem }, &cert_len, (proven_mem_mut_t){ key_pem, sizeof key_pem }, &key_len) == PROVEN_ERR_INVALID_ARG,
            "no names, an empty name, and an end that is not after the start are PROVEN_ERR_INVALID_ARG", "");
        PROVEN_TEST_ASSERT(proven_tls_self_signed(names, 3, 1, 2, test_random, NULL, (proven_mem_mut_t){ cert_pem, 100 }, &cert_len, (proven_mem_mut_t){ key_pem, sizeof key_pem }, &key_len) == PROVEN_ERR_OUT_OF_BOUNDS &&
                           proven_tls_self_signed(names, 3, 1, 2, test_random, NULL, (proven_mem_mut_t){ cert_pem, sizeof cert_pem }, &cert_len, (proven_mem_mut_t){ key_pem, 50 }, &key_len) == PROVEN_ERR_OUT_OF_BOUNDS,
            "a buffer too small for either is PROVEN_ERR_OUT_OF_BOUNDS", "");
        /* Dates on each side of 2050, where the encoding changes. */
        PROVEN_TEST_ASSERT(proven_tls_self_signed(names, 1, 2524607999LL, 2524608001LL, test_random, NULL, (proven_mem_mut_t){ cert_pem, sizeof cert_pem }, &cert_len, (proven_mem_mut_t){ key_pem, sizeof key_pem }, &key_len) == PROVEN_OK, "a period across 2050", "");
        pos = 0;
        PROVEN_TEST_ASSERT(proven_pem_next((proven_mem_view_t){ cert_pem, cert_len }, &pos, &label, (proven_mem_mut_t){ der, sizeof der }, &der_len) == PROVEN_OK &&
                           proven_cert_parse((proven_mem_view_t){ der, der_len }, &c) == PROVEN_OK && c.not_before == 2524607999LL && c.not_after == 2524608001LL,
            "2049-12-31 23:59:59 and 2050-01-01 00:00:01 come back as written, each in its own time encoding", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a handshake, and data both ways",
        "Each of the three suites with each kind of server key; then data from one byte to several records, in both directions.",
        "Check client_message and server_message in src/proven/tls13.c.");
    {
        static const proven_u16 suites[3] = { PROVEN_TLS_AES_128_GCM_SHA256, PROVEN_TLS_AES_256_GCM_SHA384, PROVEN_TLS_CHACHA20_POLY1305_SHA256 };
        proven_tls_config_t *cc = client_config(NULL);
        bool all = true;
        for (int s = 0; s < 3; ++s) {
            for (int k = 0; k < 2; ++k) {
                proven_tls_config_t *sc = server_config(k ? TP_SERVER_ED : TP_SERVER_P256, k ? TP_SERVER_ED_KEY : TP_SERVER_P256_KEY, NULL);
                proven_tls_test_knobs(suites[s], false);
                pair_t p = connect_pair(cc, sc, "example.test", NULL, NULL, NULL);
                proven_byte_t ck[32], want[32];
                proven_cert_t leaf;
                static proven_byte_t der[1024];
                proven_size_t pos = 0, len = 0;
                proven_u8str_view_t label;
                bool ok = both_up(&p) && proven_tls_cipher_suite(p.c) == suites[s] && proven_tls_cipher_suite(p.s) == suites[s] &&
                          !proven_tls_resumed(p.c) && !proven_tls_resumed(p.s) && proven_tls_alert_sent(p.c) == -1 && proven_tls_alert_received(p.s) == -1;
                ok = ok && proven_pem_next(pem(k ? TP_SERVER_ED : TP_SERVER_P256), &pos, &label, (proven_mem_mut_t){ der, sizeof der }, &len) == PROVEN_OK &&
                     proven_cert_parse((proven_mem_view_t){ der, len }, &leaf) == PROVEN_OK && proven_cert_key_sha256(&leaf, want) == PROVEN_OK &&
                     proven_tls_peer_key_sha256(p.c, ck) && memcmp(ck, want, 32) == 0 && !proven_tls_peer_key_sha256(p.s, ck) &&
                     proven_tls_peer_certificate(p.c).size == 0;
                ok = ok && transfer(p.c, p.s, 1, 1) && transfer(p.s, p.c, 1, 2) && transfer(p.c, p.s, 1000, 3) && transfer(p.s, p.c, 16384, 4) &&
                     transfer(p.c, p.s, 16385, 5) && transfer(p.s, p.c, 100000, 6) && transfer(p.c, p.s, 150000, 7);
                all = all && ok;
                close_pair(&p);
                proven_tls_config_destroy(sc);
            }
        }
        proven_tls_test_knobs(0, false);
        PROVEN_TEST_ASSERT(all, "three suites by two server key types: established, the server's key as the client saw it, and data of 1 to 150,000 bytes each way", "");

        proven_tls_config_t *sc = server_config(TP_SERVER_ED, TP_SERVER_ED_KEY, NULL);
        wire_t slow = { .chunk = 1 };
        pair_t p = connect_pair(cc, sc, "example.test", NULL, &slow, &slow);
        PROVEN_TEST_ASSERT(both_up(&p) && transfer(p.c, p.s, 5000, 9), "the same handshake delivered one byte at a time in both directions", "");

        /* Reading and closing. */
        proven_byte_t buf[64];
        PROVEN_TEST_ASSERT(proven_tls_read(p.c, (proven_mem_mut_t){ buf, sizeof buf }).err == PROVEN_ERR_NEED_MORE, "nothing to read is PROVEN_ERR_NEED_MORE", "");
        PROVEN_TEST_ASSERT(proven_tls_write(p.c, (proven_mem_view_t){ (const proven_byte_t *)"last words", 10 }).value == 10 && proven_tls_close(p.c) == PROVEN_OK &&
                           proven_tls_close(p.c) == PROVEN_OK && proven_tls_write(p.c, (proven_mem_view_t){ buf, 1 }).err == PROVEN_ERR_INVALID_STATE,
            "after proven_tls_close nothing more is written, and closing twice is harmless", "");
        pump(p.c, p.s, NULL, NULL);
        proven_result_size_t r = proven_tls_read(p.s, (proven_mem_mut_t){ buf, sizeof buf });
        PROVEN_TEST_ASSERT(r.err == PROVEN_OK && r.value == 10 && memcmp(buf, "last words", 10) == 0, "what was written before the close arrives", "");
        pump(p.c, p.s, NULL, NULL);
        PROVEN_TEST_ASSERT(proven_tls_read(p.s, (proven_mem_mut_t){ buf, sizeof buf }).err == PROVEN_ERR_EOF && g_err_s == PROVEN_OK, "and then the read reports PROVEN_ERR_EOF: a proper close, not a cut", "");
        PROVEN_TEST_ASSERT(transfer(p.s, p.c, 300, 1), "the other direction is still open after one side has closed", "");
        close_pair(&p);

        /* HelloRetryRequest: the server will not use the share the client sent. */
        proven_tls_test_knobs(0, true);
        p = connect_pair(cc, sc, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p) && transfer(p.c, p.s, 2000, 5) && transfer(p.s, p.c, 2000, 6), "a HelloRetryRequest to P-256, and data over the result", "");
        close_pair(&p);
        p = connect_pair(cc, sc, "example.test", NULL, &slow, &slow);
        PROVEN_TEST_ASSERT(both_up(&p), "the retry delivered a byte at a time", "");
        close_pair(&p);
        proven_tls_test_knobs(0, false);

        /* Key update. */
        p = connect_pair(cc, sc, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p) && proven_tls_key_update(p.c) == PROVEN_OK && transfer(p.c, p.s, 500, 1) && transfer(p.s, p.c, 500, 2) &&
                           proven_tls_key_update(p.s) == PROVEN_OK && proven_tls_key_update(p.s) == PROVEN_OK && transfer(p.s, p.c, 500, 3) && transfer(p.c, p.s, 500, 4),
            "each side replaces its sending key, twice in a row for one, and data still arrives", "");
        close_pair(&p);
        proven_tls_config_destroy(sc);
        proven_tls_config_destroy(cc);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("ALPN",
        "The server chooses, by its own order, among what the client offers; nothing in common ends the handshake.",
        "Check the ALPN handling in server_client_hello and client_encrypted_extensions.");
    {
        proven_u8str_view_t client_list[2] = { PROVEN_LIT("http/1.1"), PROVEN_LIT("h2") }, server_list[2] = { PROVEN_LIT("h2"), PROVEN_LIT("http/1.1") };
        proven_u8str_view_t odd[1] = { PROVEN_LIT("gopher") };
        proven_tls_options_t co = base_options(), so = base_options();
        co.alpn = client_list; co.alpn_count = 2; so.alpn = server_list; so.alpn_count = 2;
        proven_tls_config_t *cc = client_config(&co), *sc = server_config(TP_SERVER_ED, TP_SERVER_ED_KEY, &so);
        pair_t p = connect_pair(cc, sc, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p) && proven_u8str_view_eq(proven_tls_alpn(p.c), PROVEN_LIT("h2")) && proven_u8str_view_eq(proven_tls_alpn(p.s), PROVEN_LIT("h2")),
            "both sides report the server's first choice", "");
        close_pair(&p);
        proven_tls_config_destroy(sc);
        so.alpn = odd; so.alpn_count = 1;
        sc = server_config(TP_SERVER_ED, TP_SERVER_ED_KEY, &so);
        p = connect_pair(cc, sc, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(g_err_s == PROVEN_ERR_PROTOCOL && proven_tls_alert_sent(p.s) == 120 && g_err_c == PROVEN_ERR_PROTOCOL && proven_tls_alert_received(p.c) == 120 &&
                           !proven_tls_is_established(p.c) && !proven_tls_is_established(p.s),
            "nothing in common: the server sends no_application_protocol and both ends report PROVEN_ERR_PROTOCOL", "");
        close_pair(&p);
        proven_tls_config_destroy(sc);
        sc = server_config(TP_SERVER_ED, TP_SERVER_ED_KEY, NULL);
        p = connect_pair(cc, sc, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p) && proven_tls_alpn(p.c).size == 0 && proven_tls_alpn(p.s).size == 0, "a server with no list agrees on none, and the connection stands", "");
        close_pair(&p);
        proven_tls_config_destroy(sc); proven_tls_config_destroy(cc);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a server the client must not believe",
        "Each way verification fails gives its own error, its own fault, and the alert the RFC names; the server learns of it.",
        "Check cert_process and alert_for_fault.");
    {
        proven_tls_config_t *cc = client_config(NULL);
        proven_tls_config_t *sc = server_config(TP_STRANGER, TP_STRANGER_KEY, NULL);
        pair_t p = connect_pair(cc, sc, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(g_err_c == PROVEN_ERR_UNTRUSTED && proven_tls_peer_fault(p.c) == PROVEN_CERT_FAULT_NO_ISSUER && proven_tls_alert_sent(p.c) == 48 &&
                           g_err_s == PROVEN_ERR_PROTOCOL && proven_tls_alert_received(p.s) == 48 && !proven_tls_is_established(p.c),
            "a certificate under an unknown CA: PROVEN_ERR_UNTRUSTED, unknown_ca, and the server is told", "");
        /* After a failure. */
        proven_byte_t b[8];
        proven_size_t used = 0;
        PROVEN_TEST_ASSERT(proven_tls_feed(p.c, (proven_mem_view_t){ b, 0 }, &used) == PROVEN_ERR_INVALID_STATE &&
                           proven_tls_read(p.c, (proven_mem_mut_t){ b, sizeof b }).err == PROVEN_ERR_INVALID_STATE &&
                           proven_tls_write(p.c, (proven_mem_view_t){ b, 1 }).err == PROVEN_ERR_INVALID_STATE && proven_tls_close(p.c) == PROVEN_ERR_INVALID_STATE,
            "a failure is reported once; afterwards every call is PROVEN_ERR_INVALID_STATE", "");
        close_pair(&p);
        proven_tls_config_destroy(sc);

        sc = server_config(TP_SERVER_OTHER_NAME, TP_SERVER_P256_KEY, NULL);
        p = connect_pair(cc, sc, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(g_err_c == PROVEN_ERR_NAME_MISMATCH && proven_tls_peer_fault(p.c) == PROVEN_CERT_FAULT_NAME_MISMATCH && proven_tls_alert_sent(p.c) == 42,
            "a valid certificate for another name: PROVEN_ERR_NAME_MISMATCH, bad_certificate", "");
        close_pair(&p);
        proven_tls_config_destroy(sc);

        sc = server_config(TP_SERVER_EXPIRED, TP_SERVER_P256_KEY, NULL);
        p = connect_pair(cc, sc, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(g_err_c == PROVEN_ERR_EXPIRED && proven_tls_alert_sent(p.c) == 45 && proven_tls_alert_received(p.s) == 45, "an expired certificate: PROVEN_ERR_EXPIRED, certificate_expired", "");
        close_pair(&p);
        g_now = 1735689600;                                    /* 2025-01-01: before any of them begins */
        p = connect_pair(cc, sc, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(g_err_c == PROVEN_ERR_NOT_YET_VALID, "the clock set before the certificates begin: PROVEN_ERR_NOT_YET_VALID", "");
        close_pair(&p);
        g_now = TP_NOW;
        proven_tls_config_destroy(sc);
        proven_tls_config_destroy(cc);

        /* Pins. */
        sc = server_config(TP_SERVER_P256, TP_SERVER_P256_KEY, NULL);
        proven_byte_t pin[2][32];
        static proven_byte_t der[1024];
        proven_size_t pos = 0, len = 0;
        proven_u8str_view_t label;
        proven_cert_t leaf;
        PROVEN_TEST_ASSERT(proven_pem_next(pem(TP_SERVER_P256), &pos, &label, (proven_mem_mut_t){ der, sizeof der }, &len) == PROVEN_OK &&
                           proven_cert_parse((proven_mem_view_t){ der, len }, &leaf) == PROVEN_OK && proven_cert_key_sha256(&leaf, pin[1]) == PROVEN_OK, "the server's pin", "");
        memset(pin[0], 0x5a, 32);
        proven_tls_options_t o = base_options();
        o.verify = PROVEN_TLS_VERIFY_PIN_ONLY; o.pins = (const proven_byte_t (*)[32])pin; o.pin_count = 2;
        cc = client_config(&o);
        proven_tls_conn_t *nameless = NULL;
        PROVEN_TEST_ASSERT(proven_tls_client_create(cc, sv(""), NULL, &nameless) == PROVEN_OK, "with PIN_ONLY no name is needed", "");
        proven_tls_conn_destroy(nameless);
        p = connect_pair(cc, sc, "whatever.invalid", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p), "PIN_ONLY: the key is one of the pins, and neither the name nor the chain is looked at", "");
        close_pair(&p);
        g_now = 2000000000;                                    /* 2033: nor are the dates */
        proven_tls_config_t *sx = server_config(TP_SERVER_EXPIRED, TP_SERVER_P256_KEY, NULL);
        p = connect_pair(cc, sx, "", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p), "nor the dates: the same key in an expired certificate is accepted by its pin", "");
        close_pair(&p);
        g_now = TP_NOW;
        proven_tls_config_destroy(sx);
        proven_tls_config_destroy(cc);
        o.pin_count = 1;                                       /* only the pin that is not the server's */
        cc = client_config(&o);
        p = connect_pair(cc, sc, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(g_err_c == PROVEN_ERR_UNTRUSTED && proven_tls_peer_fault(p.c) == PROVEN_CERT_FAULT_PIN_MISMATCH, "a key that is not pinned: PROVEN_ERR_UNTRUSTED with the fault PIN_MISMATCH", "");
        close_pair(&p);
        proven_tls_config_destroy(cc);
        o.verify = PROVEN_TLS_VERIFY_CHAIN; o.anchors = g_anchors;
        cc = client_config(&o);
        p = connect_pair(cc, sc, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(g_err_c == PROVEN_ERR_UNTRUSTED && proven_tls_peer_fault(p.c) == PROVEN_CERT_FAULT_PIN_MISMATCH, "with a chain AND pins, a good chain does not excuse an unpinned key", "");
        close_pair(&p);
        proven_tls_config_destroy(cc);
        o.pin_count = 2;
        cc = client_config(&o);
        p = connect_pair(cc, sc, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p), "and with both satisfied the connection stands", "");
        close_pair(&p);
        proven_tls_config_destroy(cc); proven_tls_config_destroy(sc);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("client certificates",
        "A server that requires one gets it or refuses; one that only requests lets a client without in.",
        "Check the CertificateRequest path and server_message.");
    {
        proven_tls_options_t so = base_options(), co = base_options();
        so.anchors = g_anchors; so.client_auth = PROVEN_TLS_CLIENT_AUTH_REQUIRE; so.keep_peer_certificate = true;
        co.certificate_pem = pem(TP_CLIENT); co.private_key_pem = pem(TP_CLIENT_KEY);
        proven_tls_config_t *sc = server_config(TP_SERVER_ED, TP_SERVER_ED_KEY, &so), *cc = client_config(&co), *plain = client_config(NULL);
        pair_t p = connect_pair(cc, sc, "example.test", NULL, NULL, NULL);
        proven_byte_t k[32];
        proven_cert_t got;
        PROVEN_TEST_ASSERT(both_up(&p) && proven_tls_peer_key_sha256(p.s, k) && proven_cert_parse(proven_tls_peer_certificate(p.s), &got) == PROVEN_OK &&
                           got.has_ext_key_usage && (got.ext_key_usage & PROVEN_CERT_EKU_CLIENT_AUTH) && transfer(p.c, p.s, 100, 1),
            "required and given: the server holds the client's key hash and, as configured, its certificate", "");
        close_pair(&p);
        p = connect_pair(plain, sc, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(g_err_s == PROVEN_ERR_UNTRUSTED && proven_tls_alert_sent(p.s) == 116 && !proven_tls_is_established(p.s), "required and absent: the server refuses with certificate_required", "");
        /* The client finished its side before the server refused; it learns on its next read. */
        PROVEN_TEST_ASSERT(g_err_c == PROVEN_ERR_PROTOCOL && proven_tls_alert_received(p.c) == 116, "and the client is told", "");
        close_pair(&p);
        proven_tls_config_destroy(sc);
        so.client_auth = PROVEN_TLS_CLIENT_AUTH_REQUEST;
        sc = server_config(TP_SERVER_ED, TP_SERVER_ED_KEY, &so);
        p = connect_pair(plain, sc, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p) && !proven_tls_peer_key_sha256(p.s, k) && proven_tls_peer_certificate(p.s).size == 0, "requested and absent: the client is let in, and the server knows it presented nothing", "");
        close_pair(&p);
        proven_tls_config_destroy(cc);
        co.certificate_pem = pem(TP_STRANGER); co.private_key_pem = pem(TP_STRANGER_KEY);
        cc = client_config(&co);
        p = connect_pair(cc, sc, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(g_err_s == PROVEN_ERR_UNTRUSTED && !proven_tls_is_established(p.s), "a client certificate the server cannot verify is refused even when one was only requested", "");
        close_pair(&p);
        proven_tls_config_destroy(cc); proven_tls_config_destroy(sc); proven_tls_config_destroy(plain);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("RSA keys for this side",
        "A server with an RSA key, in both of its file forms; a client certificate with one; a chain signed with one; and the keys that are refused.",
        "Check cfg_rsa and cfg_match_key in src/proven/tls_config.c, and the RSA branch of cv_build in src/proven/tls13.c. The signing itself is test_unit_crypto_rsa's business.");
    {
        proven_tls_config_t *bad = NULL;
        PROVEN_TEST_ASSERT(tp_build_rsa(), "RSA identities are made", "");
        proven_tls_config_t *cc = client_config(NULL);
        for (int form = 0; form < 2; ++form) {
            proven_tls_config_t *sc = server_config(TP_SERVER_RSA, form ? TP_SERVER_RSA_KEY_PKCS1 : TP_SERVER_RSA_KEY, NULL);
            pair_t p = connect_pair(cc, sc, "example.test", NULL, NULL, NULL);
            PROVEN_TEST_ASSERT(both_up(&p) && transfer(p.c, p.s, 3000, 5) && transfer(p.s, p.c, 70000, 6),
                form ? "and the same key as an RSA PRIVATE KEY file" : "a server with a 2048-bit RSA key in PKCS #8 completes a handshake and carries data", "");
            close_pair(&p);
            /* Every handshake signs with the next blinding pair. */
            bool all = true;
            for (int i = 0; i < 12 && form == 0; ++i) {
                p = connect_pair(cc, sc, "example.test", NULL, NULL, NULL);
                all = all && both_up(&p);
                close_pair(&p);
            }
            PROVEN_TEST_ASSERT(all, "twelve more handshakes on the same configuration all complete", "");
            if (form == 0) {
                /* Resumption does not sign: the session from an RSA handshake resumes like any other. */
                proven_tls_session_t session;
                memset(&session, 0, sizeof session);
                p = connect_pair(cc, sc, "example.test", &session, NULL, NULL);
                PROVEN_TEST_ASSERT(both_up(&p) && transfer(p.s, p.c, 10, 1), "a session is taken from an RSA handshake", "");
                close_pair(&p);
                p = connect_pair(cc, sc, "example.test", &session, NULL, NULL);
                PROVEN_TEST_ASSERT(both_up(&p) && proven_tls_resumed(p.c) && proven_tls_resumed(p.s), "and resumed", "");
                close_pair(&p);
            }
            proven_tls_config_destroy(sc);
        }

        /* Keys that do not belong, or are not acceptable. */
        proven_tls_options_t o = base_options();
        o.certificate_pem = pem(TP_SERVER_RSA); o.private_key_pem = pem(TP_SERVER_P256_KEY);
        PROVEN_TEST_ASSERT(proven_tls_config_create(&o, &bad) == PROVEN_ERR_INVALID_STATE, "an RSA certificate with a P-256 key is PROVEN_ERR_INVALID_STATE", "");
        o.certificate_pem = pem(TP_SERVER_P256); o.private_key_pem = pem(TP_SERVER_RSA_KEY);
        PROVEN_TEST_ASSERT(proven_tls_config_create(&o, &bad) == PROVEN_ERR_INVALID_STATE, "and a P-256 certificate with an RSA key", "");
        o.certificate_pem = pem(TP_CLIENT_RSA); o.private_key_pem = pem(TP_SERVER_RSA_KEY);
        PROVEN_TEST_ASSERT(proven_tls_config_create(&o, &bad) == PROVEN_ERR_INVALID_STATE, "and an RSA certificate with another RSA key", "");
        o.certificate_pem = pem(TP_CLIENT_RSA); o.private_key_pem = pem(TP_CLIENT_RSA_KEY);
        PROVEN_TEST_ASSERT(proven_tls_config_create(&o, &bad) == PROVEN_ERR_INVALID_FORMAT, "a 1024-bit RSA key is refused: PROVEN_ERR_INVALID_FORMAT", "");

        /* With the size limit lowered for the test: an RSA client certificate, and a chain signed with RSA. */
        proven_crypto_rsa_test_min_bits(1024);
        {
            proven_tls_options_t so = base_options(), co = base_options();
            so.anchors = g_anchors; so.client_auth = PROVEN_TLS_CLIENT_AUTH_REQUIRE; so.keep_peer_certificate = true;
            co.certificate_pem = pem(TP_CLIENT_RSA); co.private_key_pem = pem(TP_CLIENT_RSA_KEY);
            proven_tls_config_t *sc = server_config(TP_SERVER_RSA, TP_SERVER_RSA_KEY, &so), *rc = client_config(&co);
            pair_t p = connect_pair(rc, sc, "example.test", NULL, NULL, NULL);
            proven_cert_t got;
            PROVEN_TEST_ASSERT(both_up(&p) && proven_cert_parse(proven_tls_peer_certificate(p.s), &got) == PROVEN_OK && got.key_kind == PROVEN_CERT_KEY_RSA && transfer(p.c, p.s, 100, 2),
                "RSA on both sides: the server signs with its key and accepts the client's RSA certificate", "");
            close_pair(&p);
            proven_tls_config_destroy(sc); proven_tls_config_destroy(rc);

            proven_cert_store_t *rsa_anchors = NULL;
            PROVEN_TEST_ASSERT(proven_cert_store_create(g_alloc, &rsa_anchors) == PROVEN_OK && proven_cert_store_add_pem(rsa_anchors, pem(TP_RSA_CA), NULL) == PROVEN_OK, "the RSA authority as an anchor", "");
            proven_tls_options_t ro = base_options();
            ro.anchors = rsa_anchors;
            proven_tls_config_t *by_rsa = server_config(TP_SERVER_RSA_BY_RSA, TP_SERVER_RSA_KEY, NULL), *believer = client_config(&ro);
            p = connect_pair(believer, by_rsa, "example.test", NULL, NULL, NULL);
            PROVEN_TEST_ASSERT(both_up(&p), "a certificate the test's issuer signed with RSA is verified by the client: the issuing signature is a real one", "");
            close_pair(&p);
            p = connect_pair(cc, by_rsa, "example.test", NULL, NULL, NULL);
            PROVEN_TEST_ASSERT(g_err_c == PROVEN_ERR_UNTRUSTED && !proven_tls_is_established(p.c), "and a client that trusts only the other authority refuses it", "");
            close_pair(&p);
            proven_tls_config_destroy(by_rsa); proven_tls_config_destroy(believer);
            proven_cert_store_destroy(rsa_anchors);
        }
        proven_crypto_rsa_test_min_bits(0);
        proven_tls_config_destroy(cc);
    }

    PROVEN_TEST_SECTION("TLS 1.2",
        "The six suites with each kind of server key; which version two configurations agree on; the extended master secret, the downgrade mark and ChangeCipherSpec, each refused where it must be; renegotiation declined; client certificates; tickets.",
        "Check the TLS 1.2 part of src/proven/tls13.c: client12_server_hello, server12_client_hello, record_process12. Its key derivation and records are test_unit_tls12_keys's business.");
    {
        proven_tls_options_t o12 = base_options(), o13 = base_options();
        o12.max_version = PROVEN_TLS_VERSION_1_2;
        o13.min_version = PROVEN_TLS_VERSION_1_3;
        proven_tls_config_t *c_any = client_config(NULL), *c_12 = client_config(&o12), *c_13 = client_config(&o13);
        proven_tls_config_t *s_any = server_config(TP_SERVER_P256, TP_SERVER_P256_KEY, NULL), *s_12 = server_config(TP_SERVER_P256, TP_SERVER_P256_KEY, &o12),
                            *s_13 = server_config(TP_SERVER_P256, TP_SERVER_P256_KEY, &o13);

        /* Every suite, every kind of key. */
        static const proven_u16 order13[3] = { PROVEN_TLS_AES_128_GCM_SHA256, PROVEN_TLS_AES_256_GCM_SHA384, PROVEN_TLS_CHACHA20_POLY1305_SHA256 };
        static const proven_u16 ec_suites[3] = { 0xc02b, 0xc02c, 0xcca9 }, rsa_suites[3] = { 0xc02f, 0xc030, 0xcca8 };
        bool all = true;
        for (int k = 0; k < 3; ++k) {
            proven_tls_config_t *sc = k == 0 ? server_config(TP_SERVER_P256, TP_SERVER_P256_KEY, &o12) : k == 1 ? server_config(TP_SERVER_ED, TP_SERVER_ED_KEY, &o12)
                                                                                                           : server_config(TP_SERVER_RSA, TP_SERVER_RSA_KEY, &o12);
            for (int su = 0; su < 3; ++su) {
                proven_tls_test_knobs(order13[su], false);
                pair_t p = connect_pair(c_any, sc, "example.test", NULL, NULL, NULL);
                proven_u16 want = k == 2 ? rsa_suites[su] : ec_suites[su];
                proven_byte_t key[32];
                bool ok = both_up(&p) && proven_tls_version(p.c) == PROVEN_TLS_VERSION_1_2 && proven_tls_version(p.s) == PROVEN_TLS_VERSION_1_2 &&
                          proven_tls_cipher_suite(p.c) == want && proven_tls_cipher_suite(p.s) == want && proven_tls_peer_key_sha256(p.c, key) && !proven_tls_resumed(p.c);
                ok = ok && transfer(p.c, p.s, 1, 1) && transfer(p.s, p.c, 1, 2) && transfer(p.c, p.s, 16384, 3) && transfer(p.s, p.c, 16385, 4) && transfer(p.c, p.s, 100000, 5) && transfer(p.s, p.c, 150000, 6);
                ok = ok && proven_tls_key_update(p.c) == PROVEN_ERR_UNSUPPORTED;
                all = all && ok;
                close_pair(&p);
            }
            proven_tls_config_destroy(sc);
        }
        proven_tls_test_knobs(0, false);
        PROVEN_TEST_ASSERT(all, "three ciphers by three kinds of server key: TLS 1.2 established with the suite for that key, the server's key seen, data of 1 to 150,000 bytes each way, and no key update", "");
        {
            /* P-256 for the key exchange when the server will not take X25519, and a byte at a time. */
            proven_tls_test_knobs(0, true);
            wire_t slow = { .chunk = 1, .flip_at = -1 };
            pair_t p = connect_pair(c_any, s_12, "example.test", NULL, &slow, &slow);
            PROVEN_TEST_ASSERT(both_up(&p) && proven_tls_version(p.c) == PROVEN_TLS_VERSION_1_2 && transfer(p.c, p.s, 3000, 9), "the key exchange on P-256, with every record delivered a byte at a time", "");
            close_pair(&p);
            proven_tls_test_knobs(0, false);
        }

        /* Which version. */
        pair_t p = connect_pair(c_any, s_any, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p) && proven_tls_version(p.c) == PROVEN_TLS_VERSION_1_3 && proven_tls_version(p.s) == PROVEN_TLS_VERSION_1_3, "two default configurations agree on TLS 1.3", "");
        close_pair(&p);
        p = connect_pair(c_12, s_any, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p) && proven_tls_version(p.c) == PROVEN_TLS_VERSION_1_2 && proven_tls_version(p.s) == PROVEN_TLS_VERSION_1_2, "a client limited to 1.2 gets 1.2 from a default server", "");
        close_pair(&p);
        p = connect_pair(c_13, s_any, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p) && proven_tls_version(p.s) == PROVEN_TLS_VERSION_1_3, "a client that insists on 1.3 gets it", "");
        close_pair(&p);
        p = connect_pair(c_12, s_13, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(g_err_s == PROVEN_ERR_PROTOCOL && proven_tls_alert_sent(p.s) == 70 && g_err_c == PROVEN_ERR_PROTOCOL && proven_tls_alert_received(p.c) == 70 && proven_tls_version(p.c) == 0,
            "a 1.2 client and a server that insists on 1.3: protocol_version, and no version was agreed", "");
        close_pair(&p);
        p = connect_pair(c_13, s_12, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(!both_up(&p) && g_err_s == PROVEN_ERR_PROTOCOL && proven_tls_alert_sent(p.s) == 70, "a client that insists on 1.3 and a server limited to 1.2: refused by the server", "");
        close_pair(&p);
        {
            proven_tls_options_t bad = base_options();
            proven_tls_config_t *none = NULL;
            bad.anchors = g_anchors; bad.min_version = PROVEN_TLS_VERSION_1_3; bad.max_version = PROVEN_TLS_VERSION_1_2;
            PROVEN_TEST_ASSERT(proven_tls_config_create(&bad, &none) == PROVEN_ERR_INVALID_ARG, "a minimum above the maximum is PROVEN_ERR_INVALID_ARG", "");
            bad.min_version = 0x0302; bad.max_version = 0;
            PROVEN_TEST_ASSERT(proven_tls_config_create(&bad, &none) == PROVEN_ERR_INVALID_ARG, "and so is a version that is neither 1.2 nor 1.3", "");
        }

        /* The three things a TLS 1.2 handshake is refused for. */
        proven_tls_test_knobs12(1);
        p = connect_pair(c_any, s_any, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(g_err_c == PROVEN_ERR_PROTOCOL && proven_tls_alert_sent(p.c) == 47 && !proven_tls_is_established(p.c),
            "a server that could have spoken 1.3 and answers 1.2 marks its random, and a client that offered 1.3 refuses: illegal_parameter", "");
        close_pair(&p);
        p = connect_pair(c_12, s_any, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p), "the same mark means nothing to a client that never offered 1.3", "");
        close_pair(&p);
        proven_tls_test_knobs12(2);
        p = connect_pair(c_12, s_12, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(g_err_s == PROVEN_ERR_PROTOCOL && proven_tls_alert_sent(p.s) == 40, "a client without the extended master secret is refused by the server: handshake_failure", "");
        close_pair(&p);
        proven_tls_test_knobs12(4);
        p = connect_pair(c_12, s_12, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(g_err_c == PROVEN_ERR_PROTOCOL && proven_tls_alert_sent(p.c) == 40, "and a server that does not answer it, by the client", "");
        close_pair(&p);
        proven_tls_test_knobs12(8);
        p = connect_pair(c_12, s_any, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(g_err_s == PROVEN_ERR_PROTOCOL && proven_tls_alert_sent(p.s) == 86 && proven_tls_alert_received(p.c) == 86,
            "a 1.2 hello that says it is a fallback, to a server that speaks 1.3: inappropriate_fallback", "");
        close_pair(&p);
        p = connect_pair(c_12, s_12, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p), "the same hello to a server whose best is 1.2 is in order", "");
        close_pair(&p);
        proven_tls_test_knobs12(0);
        {
            /* ChangeCipherSpec where none is due. The server's first flight is four records:
             * ServerHello, Certificate, ServerKeyExchange, ServerHelloDone. */
            static const proven_byte_t ccs[6] = { 20, 3, 3, 0, 1, 1 };
            proven_tls_conn_t *c = NULL, *sv_ = NULL;
            proven_size_t used = 0;
            PROVEN_TEST_ASSERT(proven_tls_client_create(c_12, sv("example.test"), NULL, &c) == PROVEN_OK && proven_tls_server_create(s_12, &sv_) == PROVEN_OK, "a client and a server", "");
            static proven_byte_t kept_hello[2048];
            proven_mem_view_t out = proven_tls_pending_output(c);
            proven_mem_view_t hello = { kept_hello, out.size < sizeof kept_hello ? out.size : 0 };
            memcpy(kept_hello, out.ptr, hello.size);
            proven_tls_output_sent(c, out.size);
            PROVEN_TEST_ASSERT(hello.size > 0 && proven_tls_feed(sv_, hello, &used) == PROVEN_OK && used == hello.size, "the server has the ClientHello", "");
            PROVEN_TEST_ASSERT(proven_tls_feed(sv_, (proven_mem_view_t){ ccs, 6 }, &used) == PROVEN_ERR_PROTOCOL && proven_tls_alert_sent(sv_) == 10,
                "a ChangeCipherSpec before the client's key exchange is refused by the server: unexpected_message", "");
            proven_tls_conn_destroy(sv_);
            PROVEN_TEST_ASSERT(proven_tls_server_create(s_12, &sv_) == PROVEN_OK && proven_tls_feed(sv_, hello, &used) == PROVEN_OK, "another server, as far", "");
            proven_mem_view_t flight = proven_tls_pending_output(sv_);
            proven_size_t first = 5 + ((proven_size_t)flight.ptr[3] << 8 | flight.ptr[4]);
            PROVEN_TEST_ASSERT(flight.size > first && proven_tls_feed(c, (proven_mem_view_t){ flight.ptr, first }, &used) == PROVEN_OK && proven_tls_version(c) == PROVEN_TLS_VERSION_1_2,
                "the client has the ServerHello, and knows the version", "");
            PROVEN_TEST_ASSERT(proven_tls_feed(c, (proven_mem_view_t){ ccs, 6 }, &used) == PROVEN_ERR_PROTOCOL && proven_tls_alert_sent(c) == 10,
                "a ChangeCipherSpec in place of the server's certificate is refused by the client", "");
            proven_tls_conn_destroy(c); proven_tls_conn_destroy(sv_);
        }

        /* Renegotiation is declined, and the connection goes on. */
        p = connect_pair(c_12, s_12, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p) && proven_tls_test_send_handshake(p.s, 0), "a server asks its client to start again (HelloRequest)", "");
        pump(p.c, p.s, NULL, NULL);
        PROVEN_TEST_ASSERT(g_err_c == PROVEN_OK && g_err_s == PROVEN_OK && both_up(&p) && transfer(p.c, p.s, 500, 1) && transfer(p.s, p.c, 500, 2),
            "the client declines with a warning, and data still flows both ways", "");
        PROVEN_TEST_ASSERT(proven_tls_test_send_handshake(p.c, 1), "a client starts again by itself (a second ClientHello)", "");
        pump(p.c, p.s, NULL, NULL);
        PROVEN_TEST_ASSERT(g_err_c == PROVEN_OK && g_err_s == PROVEN_OK && both_up(&p) && transfer(p.c, p.s, 500, 3), "the server declines the same way", "");
        bool tolerated = true;
        for (int i = 0; i < 40 && g_err_c == PROVEN_OK; ++i) { tolerated = proven_tls_test_send_handshake(p.s, 0); pump(p.c, p.s, NULL, NULL); }
        PROVEN_TEST_ASSERT(tolerated && g_err_c == PROVEN_ERR_PROTOCOL && proven_tls_alert_sent(p.c) == 10, "a peer that keeps asking is given up on", "");
        close_pair(&p);

        /* Client certificates. */
        {
            proven_tls_options_t so = o12, co = base_options(), ro = o12;
            so.anchors = g_anchors; so.client_auth = PROVEN_TLS_CLIENT_AUTH_REQUIRE;
            ro.anchors = g_anchors; ro.client_auth = PROVEN_TLS_CLIENT_AUTH_REQUEST;
            co.certificate_pem = pem(TP_CLIENT); co.private_key_pem = pem(TP_CLIENT_KEY);
            proven_tls_config_t *require = server_config(TP_SERVER_P256, TP_SERVER_P256_KEY, &so), *request = server_config(TP_SERVER_ED, TP_SERVER_ED_KEY, &ro), *with = client_config(&co);
            proven_byte_t key[32];
            p = connect_pair(with, require, "example.test", NULL, NULL, NULL);
            PROVEN_TEST_ASSERT(both_up(&p) && proven_tls_version(p.s) == PROVEN_TLS_VERSION_1_2 && proven_tls_peer_key_sha256(p.s, key) && transfer(p.c, p.s, 100, 1),
                "a client certificate required and given: the server holds the client's key hash", "");
            close_pair(&p);
            p = connect_pair(c_any, require, "example.test", NULL, NULL, NULL);
            PROVEN_TEST_ASSERT(g_err_s == PROVEN_ERR_UNTRUSTED && proven_tls_alert_sent(p.s) == 40 && !proven_tls_is_established(p.s), "required and absent: the server refuses, with handshake_failure - TLS 1.2 has no better word", "");
            close_pair(&p);
            p = connect_pair(c_any, request, "example.test", NULL, NULL, NULL);
            PROVEN_TEST_ASSERT(both_up(&p) && !proven_tls_peer_key_sha256(p.s, key), "requested and absent: the client is let in, unidentified", "");
            close_pair(&p);
            proven_tls_options_t eo = base_options();
            eo.certificate_pem = pem(TP_SERVER_ED); eo.private_key_pem = pem(TP_SERVER_ED_KEY);
            proven_tls_config_t *ed_client = client_config(&eo);
            p = connect_pair(ed_client, request, "example.test", NULL, NULL, NULL);
            PROVEN_TEST_ASSERT(both_up(&p) && !proven_tls_peer_key_sha256(p.s, key), "a client whose key is Ed25519 presents no certificate in TLS 1.2, and is let in where one is only requested", "");
            close_pair(&p);
            proven_crypto_rsa_test_min_bits(1024);
            proven_tls_options_t rco = base_options();
            rco.certificate_pem = pem(TP_CLIENT_RSA); rco.private_key_pem = pem(TP_CLIENT_RSA_KEY);
            proven_tls_config_t *rsa_client = client_config(&rco);
            p = connect_pair(rsa_client, require, "example.test", NULL, NULL, NULL);
            PROVEN_TEST_ASSERT(both_up(&p) && proven_tls_peer_key_sha256(p.s, key), "an RSA client certificate is accepted", "");
            close_pair(&p);
            proven_crypto_rsa_test_min_bits(0);
            proven_tls_config_destroy(require); proven_tls_config_destroy(request); proven_tls_config_destroy(with); proven_tls_config_destroy(ed_client); proven_tls_config_destroy(rsa_client);
        }

        /* Tickets. */
        {
            proven_tls_session_t session;
            memset(&session, 0, sizeof session);
            p = connect_pair(c_12, s_12, "example.test", &session, NULL, NULL);
            PROVEN_TEST_ASSERT(both_up(&p) && !proven_tls_resumed(p.c) && transfer(p.s, p.c, 10, 1), "a first 1.2 connection leaves a session", "");
            close_pair(&p);
            p = connect_pair(c_12, s_12, "example.test", &session, NULL, NULL);
            proven_byte_t key[32];
            PROVEN_TEST_ASSERT(both_up(&p) && proven_tls_resumed(p.c) && proven_tls_resumed(p.s) && proven_tls_version(p.c) == PROVEN_TLS_VERSION_1_2 &&
                               proven_tls_peer_key_sha256(p.c, key) && transfer(p.c, p.s, 2000, 2) && transfer(p.s, p.c, 2000, 3),
                "the second resumes it: no certificate is sent, the server's key is the one remembered, and data flows", "");
            close_pair(&p);
            p = connect_pair(c_any, s_any, "example.test", &session, NULL, NULL);
            PROVEN_TEST_ASSERT(both_up(&p) && proven_tls_version(p.c) == PROVEN_TLS_VERSION_1_3 && !proven_tls_resumed(p.s), "a 1.2 session is not offered to, or honoured by, a 1.3 handshake", "");
            close_pair(&p);
            /* That 1.3 connection left a 1.3 session in the same place. */
            p = connect_pair(c_12, s_12, "example.test", &session, NULL, NULL);
            PROVEN_TEST_ASSERT(both_up(&p) && !proven_tls_resumed(p.c) && !proven_tls_resumed(p.s), "and a 1.3 session is not resumed by a 1.2 handshake", "");
            close_pair(&p);
            p = connect_pair(c_12, s_12, "example.test", &session, NULL, NULL);
            PROVEN_TEST_ASSERT(both_up(&p) && proven_tls_resumed(p.c), "which left a 1.2 one again", "");
            close_pair(&p);
            p = connect_pair(c_12, s_12, "elsewhere.test", &session, NULL, NULL);
            PROVEN_TEST_ASSERT(!proven_tls_resumed(p.s), "a session is offered only to the name it was made with", "");
            close_pair(&p);
            proven_i64 kept = g_now;
            memset(&session, 0, sizeof session);
            p = connect_pair(c_12, s_12, "example.test", &session, NULL, NULL);
            close_pair(&p);
            g_now = kept + 3 * 3600;
            p = connect_pair(c_12, s_12, "example.test", &session, NULL, NULL);
            PROVEN_TEST_ASSERT(both_up(&p) && !proven_tls_resumed(p.c), "three hours on, past the ticket's lifetime, it is not offered", "");
            close_pair(&p);
            g_now = kept;
            proven_tls_options_t nr = o12;
            nr.no_resumption = true;
            proven_tls_config_t *s_nr = server_config(TP_SERVER_P256, TP_SERVER_P256_KEY, &nr);
            memset(&session, 0, sizeof session);
            p = connect_pair(c_12, s_nr, "example.test", &session, NULL, NULL);
            close_pair(&p);
            p = connect_pair(c_12, s_nr, "example.test", &session, NULL, NULL);
            PROVEN_TEST_ASSERT(both_up(&p) && !proven_tls_resumed(p.c), "a server with resumption off gives no ticket", "");
            close_pair(&p);
            /* A ticket from another server's keys is just not a ticket. */
            proven_tls_config_t *s_other = server_config(TP_SERVER_P256, TP_SERVER_P256_KEY, &o12);
            memset(&session, 0, sizeof session);
            p = connect_pair(c_12, s_12, "example.test", &session, NULL, NULL);
            close_pair(&p);
            p = connect_pair(c_12, s_other, "example.test", &session, NULL, NULL);
            PROVEN_TEST_ASSERT(both_up(&p) && !proven_tls_resumed(p.c) && !proven_tls_resumed(p.s), "a ticket sealed by another server's keys leads to a full handshake", "");
            close_pair(&p);
            proven_tls_config_destroy(s_nr); proven_tls_config_destroy(s_other);
        }

        /* A changed bit anywhere in a 1.2 handshake. */
        {
            long total_cs = 0, total_sc = 0;
            wire_t count_cs = { .counter = &total_cs, .flip_at = -1 }, count_sc = { .counter = &total_sc, .flip_at = -1 };
            g_rng = 0x9e3779b97f4a7c15ull;
            p = connect_pair(c_12, s_12, "example.test", NULL, &count_cs, &count_sc);
            PROVEN_TEST_ASSERT(both_up(&p) && total_cs > 200 && total_sc > 500, "a clean 1.2 handshake, to learn how many bytes each side sends", "");
            close_pair(&p);
            long survived = 0, tried = 0;
            for (int dir = 0; dir < 2; ++dir) {
                long total = dir ? total_sc : total_cs;
                for (long at = 0; at < total; at += 5) {
                    long n_cs = 0, n_sc = 0;
                    wire_t w_cs = { .counter = &n_cs, .flip_at = dir == 0 ? at : -1 }, w_sc = { .counter = &n_sc, .flip_at = dir == 1 ? at : -1 };
                    g_rng = 0x9e3779b97f4a7c15ull;
                    p = connect_pair(c_12, s_12, "example.test", NULL, &w_cs, &w_sc);
                    tried++;
                    if (both_up(&p)) survived++;
                    close_pair(&p);
                }
            }
            fprintf(stderr, "[PROVEN][TEST][INFO] TLS 1.2: %ld single-bit changes tried, %ld left both sides established\n", tried, survived);
            PROVEN_TEST_ASSERT(tried > 150 && survived <= 6, "a changed bit in a 1.2 handshake leaves at most the unauthenticated record-version bytes unnoticed", "");
        }
        proven_tls_config_destroy(c_any); proven_tls_config_destroy(c_12); proven_tls_config_destroy(c_13);
        proven_tls_config_destroy(s_any); proven_tls_config_destroy(s_12); proven_tls_config_destroy(s_13);
    }

    PROVEN_TEST_SECTION("resumption",
        "A ticket from one connection shortens the next: no certificates, the same identity. A ticket that is old, damaged or for another name falls back or fails as it should.",
        "Check proven_tls_ticket_seal/_open, client_session, and the PSK branch of server_client_hello.");
    {
        proven_tls_options_t so = base_options();
        so.ticket_lifetime_s = 600;
        proven_tls_config_t *cc = client_config(NULL), *sc = server_config(TP_SERVER_P256, TP_SERVER_P256_KEY, &so);
        proven_tls_session_t session = { 0 }, saved;
        proven_byte_t first_key[32], again[32];
        pair_t p = connect_pair(cc, sc, "example.test", &session, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p) && !proven_tls_resumed(p.c) && proven_tls_peer_key_sha256(p.c, first_key), "the first connection is a full handshake", "");
        bool empty = true;
        for (proven_size_t i = 0; i < sizeof session.opaque; ++i) empty = empty && session.opaque[i] == 0;
        PROVEN_TEST_ASSERT(!empty, "and leaves a ticket in the session", "");
        close_pair(&p);
        saved = session;

        proven_size_t live_before = g_count.live;
        p = connect_pair(cc, sc, "example.test", &session, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p) && proven_tls_resumed(p.c) && proven_tls_resumed(p.s) && proven_tls_peer_key_sha256(p.c, again) && memcmp(first_key, again, 32) == 0 &&
                           transfer(p.c, p.s, 3000, 1) && transfer(p.s, p.c, 3000, 2),
            "the second resumes: both say so, the client still knows whose key it trusted, and data flows", "");
        close_pair(&p);
        PROVEN_TEST_ASSERT(g_count.live == live_before, "a closed pair has given back every byte it allocated", "");

        proven_tls_test_knobs(0, true);
        p = connect_pair(cc, sc, "example.test", &session, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p) && proven_tls_resumed(p.c) && proven_tls_resumed(p.s), "resumption through a HelloRetryRequest", "");
        close_pair(&p);
        proven_tls_test_knobs(PROVEN_TLS_AES_256_GCM_SHA384, false);
        session = saved;
        p = connect_pair(cc, sc, "example.test", &session, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p) && !proven_tls_resumed(p.c) && !proven_tls_resumed(p.s) && proven_tls_cipher_suite(p.c) == PROVEN_TLS_AES_256_GCM_SHA384,
            "a ticket made under SHA-256 is not used when the server picks a SHA-384 suite: a full handshake instead", "");
        close_pair(&p);
        proven_tls_test_knobs(0, false);

        session = saved;
        p = connect_pair(cc, sc, "localhost", &session, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p) && !proven_tls_resumed(p.c), "a session is not offered to another server name", "");
        close_pair(&p);

        session = saved;
        ((proven_tls_session_data_t *)(void *)session.opaque)->ticket[20] ^= 1;
        p = connect_pair(cc, sc, "example.test", &session, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p) && !proven_tls_resumed(p.c) && !proven_tls_resumed(p.s), "a damaged ticket is not an error: the server does not recognise it and the handshake is a full one", "");
        close_pair(&p);

        session = saved;
        ((proven_tls_session_data_t *)(void *)session.opaque)->psk[3] ^= 1;
        p = connect_pair(cc, sc, "example.test", &session, NULL, NULL);
        PROVEN_TEST_ASSERT(g_err_s == PROVEN_ERR_PROTOCOL && proven_tls_alert_sent(p.s) == 51 && !proven_tls_is_established(p.c) && !proven_tls_is_established(p.s),
            "a good ticket with the wrong key behind it IS an error: the binder does not verify, and that ends the connection", "");
        close_pair(&p);

        /* Age. The client stops offering a ticket past its lifetime; a server refuses one that
         * is too old by its own clock even when the client still offers it. */
        session = saved;
        g_now = TP_NOW + 599;
        p = connect_pair(cc, sc, "example.test", &session, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p) && proven_tls_resumed(p.s), "one second inside the lifetime the ticket still resumes", "");
        close_pair(&p);
        session = saved;
        g_now = TP_NOW + 601;
        p = connect_pair(cc, sc, "example.test", &session, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p) && !proven_tls_resumed(p.c) && !proven_tls_resumed(p.s), "one second past it, a full handshake", "");
        close_pair(&p);
        g_now = TP_NOW + 100000;                               /* many lifetimes later: both ticket keys have gone */
        session = saved;
        ((proven_tls_session_data_t *)(void *)session.opaque)->received_at = g_now;     /* a client that thinks the ticket is fresh */
        p = connect_pair(cc, sc, "example.test", &session, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p) && !proven_tls_resumed(p.s), "a ticket sealed under a key the server has since replaced twice is not honoured", "");
        close_pair(&p);
        g_now = TP_NOW;
        proven_tls_config_destroy(sc);

        so.no_resumption = true;
        sc = server_config(TP_SERVER_P256, TP_SERVER_P256_KEY, &so);
        memset(&session, 0, sizeof session);
        p = connect_pair(cc, sc, "example.test", &session, NULL, NULL);
        empty = true;
        for (proven_size_t i = 0; i < sizeof session.opaque; ++i) empty = empty && session.opaque[i] == 0;
        PROVEN_TEST_ASSERT(both_up(&p) && empty, "a server set to issue no tickets issues none", "");
        close_pair(&p);
        proven_tls_config_destroy(sc); proven_tls_config_destroy(cc);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("what a connection holds",
        "An established connection with nothing in flight keeps its state struct and nothing else.",
        "Check which buffers are freed in proven_tls_feed, proven_tls_read and proven_tls_output_sent, and hs_free.");
    {
        proven_tls_config_t *cc = client_config(NULL), *sc = server_config(TP_SERVER_ED, TP_SERVER_ED_KEY, NULL);
        proven_size_t before = g_count.live;
        pair_t p = connect_pair(cc, sc, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p) && transfer(p.c, p.s, 40000, 1) && transfer(p.s, p.c, 40000, 2), "a connection that has carried data", "");
        proven_size_t pair_idle = g_count.live - before;
        fprintf(stderr, "[PROVEN][TEST][INFO] an idle client and server connection together hold %u bytes\n", (unsigned)pair_idle);
        PROVEN_TEST_ASSERT(pair_idle <= 2 * 1100, "idle, the two together hold at most 2,200 bytes: no record buffer, no handshake state", "");
        close_pair(&p);
        PROVEN_TEST_ASSERT(g_count.live == before, "and nothing once destroyed", "");
        proven_tls_options_t o12 = base_options();
        o12.max_version = PROVEN_TLS_VERSION_1_2;
        proven_tls_config_t *c12 = client_config(&o12);
        before = g_count.live;
        p = connect_pair(c12, sc, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p) && proven_tls_version(p.c) == PROVEN_TLS_VERSION_1_2 && transfer(p.c, p.s, 40000, 1) && transfer(p.s, p.c, 40000, 2), "a TLS 1.2 connection that has carried data", "");
        fprintf(stderr, "[PROVEN][TEST][INFO] an idle TLS 1.2 pair holds %u bytes\n", (unsigned)(g_count.live - before));
        PROVEN_TEST_ASSERT(g_count.live - before == pair_idle, "holds exactly what a 1.3 one does", "");
        close_pair(&p);
        PROVEN_TEST_ASSERT(g_count.live == before, "and frees it all", "");
        proven_tls_config_destroy(c12);
        proven_tls_config_destroy(sc); proven_tls_config_destroy(cc);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a wire that lies",
        "One changed bit anywhere in either direction never produces two established connections; input that is not TLS is refused at once.",
        "Check record_process, handshake_bytes and the transcript handling.");
    {
        proven_tls_config_t *cc = client_config(NULL), *sc = server_config(TP_SERVER_ED, TP_SERVER_ED_KEY, NULL);
        long total_cs = 0, total_sc = 0;
        wire_t count_cs = { .counter = &total_cs, .flip_at = -1 }, count_sc = { .counter = &total_sc, .flip_at = -1 };
        pair_t p = connect_pair(cc, sc, "example.test", NULL, &count_cs, &count_sc);
        PROVEN_TEST_ASSERT(both_up(&p) && total_cs > 200 && total_sc > 700, "a clean handshake, to learn how many bytes each side sends", "");
        close_pair(&p);
        long survived = 0, tried = 0;
        for (int dir = 0; dir < 2; ++dir) {
            long total = dir ? total_sc : total_cs;
            for (long at = 0; at < total; at += 7) {
                long n_cs = 0, n_sc = 0;
                wire_t w_cs = { .counter = &n_cs, .flip_at = dir == 0 ? at : -1 }, w_sc = { .counter = &n_sc, .flip_at = dir == 1 ? at : -1 };
                g_rng = 0x9e3779b97f4a7c15ull;                 /* the same handshake every time */
                p = connect_pair(cc, sc, "example.test", NULL, &w_cs, &w_sc);
                tried++;
                if (both_up(&p)) survived++;
                close_pair(&p);
            }
        }
        fprintf(stderr, "[PROVEN][TEST][INFO] %ld single-bit changes tried, %ld left both sides established\n", tried, survived);
        /* The only bytes whose change both sides can survive are ones neither authenticates:
         * the legacy record-version byte of a record in the clear, the ChangeCipherSpec. */
        PROVEN_TEST_ASSERT(tried > 120 && survived <= 4, "a changed bit in the handshake leaves at most the unauthenticated legacy bytes unnoticed", "");

        /* Not TLS at all. */
        proven_tls_conn_t *s = NULL;
        proven_size_t used = 0;
        static const char http[] = "GET / HTTP/1.1\r\nHost: example.test\r\n\r\n";
        PROVEN_TEST_ASSERT(proven_tls_server_create(sc, &s) == PROVEN_OK, "a server", "");
        PROVEN_TEST_ASSERT(proven_tls_feed(s, (proven_mem_view_t){ (const proven_byte_t *)http, sizeof http - 1 }, &used) == PROVEN_ERR_PROTOCOL && proven_tls_alert_sent(s) == 10,
            "an HTTP request sent to a TLS server is PROVEN_ERR_PROTOCOL after its first five bytes", "");
        proven_mem_view_t alert = proven_tls_pending_output(s);
        PROVEN_TEST_ASSERT(alert.size == 7 && alert.ptr[0] == 21 && alert.ptr[5] == 2 && alert.ptr[6] == 10, "and the alert that says so is waiting to be sent", "");
        proven_tls_conn_destroy(s);
        static const proven_byte_t huge[5] = { 22, 3, 3, 0xff, 0xff };
        PROVEN_TEST_ASSERT(proven_tls_server_create(sc, &s) == PROVEN_OK && proven_tls_feed(s, (proven_mem_view_t){ huge, 5 }, &used) == PROVEN_ERR_PROTOCOL && proven_tls_alert_sent(s) == 22,
            "a record that claims 65,535 bytes is refused on its header, before any of it is waited for", "");
        proven_tls_conn_destroy(s);
        static const proven_byte_t app_first[10] = { 23, 3, 3, 0, 5, 1, 2, 3, 4, 5 };
        PROVEN_TEST_ASSERT(proven_tls_server_create(sc, &s) == PROVEN_OK && proven_tls_feed(s, (proven_mem_view_t){ app_first, 10 }, &used) == PROVEN_ERR_PROTOCOL,
            "application data before any handshake is refused", "");
        proven_tls_conn_destroy(s);
        proven_tls_conn_t *c = NULL;
        static const proven_byte_t fatal[7] = { 21, 3, 3, 0, 2, 2, 40 };
        PROVEN_TEST_ASSERT(proven_tls_client_create(cc, sv("example.test"), NULL, &c) == PROVEN_OK && proven_tls_feed(c, (proven_mem_view_t){ fatal, 7 }, &used) == PROVEN_ERR_PROTOCOL &&
                           proven_tls_alert_received(c) == 40 && proven_tls_alert_sent(c) == -1,
            "a fatal alert from the peer ends the connection; none is sent in return", "");
        proven_tls_conn_destroy(c);
        /* A peer that sends ChangeCipherSpec without end is not making progress. */
        PROVEN_TEST_ASSERT(proven_tls_client_create(cc, sv("example.test"), NULL, &c) == PROVEN_OK, "a client", "");
        static const proven_byte_t ccs[6] = { 20, 3, 3, 0, 1, 1 };
        proven_err_t flood = PROVEN_OK;
        int taken = 0;
        while (flood == PROVEN_OK && taken < 100) { flood = proven_tls_feed(c, (proven_mem_view_t){ ccs, 6 }, &used); taken++; }
        PROVEN_TEST_ASSERT(flood == PROVEN_ERR_PROTOCOL && taken > 1 && taken <= 20, "a run of ChangeCipherSpec records is tolerated briefly and then refused", "");
        proven_tls_conn_destroy(c);
        /* A peer that asks for key updates and never reads the answers. */
        p = connect_pair(cc, sc, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p), "a pair", "");
        {
            /* The client's own updates are KeyUpdate(not requested); to make a request the test
             * seals the message itself under the client's current sending key. */
            proven_err_t e = PROVEN_OK;
            int asked = 0;
            static proven_byte_t rec[64];
            static const proven_byte_t ask[5] = { 24, 0, 0, 1, 1 };
            proven_tls_keys_t k;
            proven_byte_t secret[48];
            proven_tls_test_peek_write(p.c, &k, secret);
            const proven_tls_suite_t *suite = proven_tls_suite_find(proven_tls_cipher_suite(p.c));
            while (e == PROVEN_OK && asked < 5000) {
                proven_size_t n = proven_tls13_seal(&k, PROVEN_TLS_CT_HANDSHAKE, (proven_mem_view_t){ ask, 5 }, rec);
                e = proven_tls_feed(p.s, (proven_mem_view_t){ rec, n }, &used);
                /* Having sent a KeyUpdate, the sender moves to its next key. */
                proven_byte_t next[48];
                proven_tls13_expand_label(suite->hash, secret, "traffic upd", (proven_mem_view_t){ next, 0 }, next, suite->hash_len);
                memcpy(secret, next, suite->hash_len);
                proven_tls13_set_keys(&k, suite, secret);
                asked++;
            }
            PROVEN_TEST_ASSERT(e == PROVEN_ERR_PROTOCOL && asked > 100 && asked < 5000 && proven_tls_pending_output(p.s).size < 200000,
                "key-update requests whose answers are never collected are refused once the unsent answers pass the output limit", "");
        }
        close_pair(&p);

        /* A second ClientHello where the server's answer was expected. */
        p = connect_pair(cc, sc, "example.test", NULL, NULL, NULL);
        PROVEN_TEST_ASSERT(both_up(&p), "a pair", "");
        proven_tls_conn_t *c2 = NULL;
        PROVEN_TEST_ASSERT(proven_tls_client_create(cc, sv("example.test"), NULL, &c2) == PROVEN_OK, "another client", "");
        proven_mem_view_t hello = proven_tls_pending_output(c2);
        PROVEN_TEST_ASSERT(proven_tls_feed(p.s, hello, &used) == PROVEN_ERR_PROTOCOL, "a handshake record in the clear on an established connection is refused", "");
        proven_tls_conn_destroy(c2);
        close_pair(&p);
        proven_tls_config_destroy(sc); proven_tls_config_destroy(cc);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("RFC 8448, through the client",
        "Two published handshakes replayed record for record: given the trace's ClientHello and key, the client must accept the server's records and send exactly the client's.",
        "Check the client path of src/proven/tls13.c; the records are in tests/test_unit_tls_vectors.h.");
    {
        PROVEN_TEST_ASSERT(replay(TLSV_REPLAY_SIMPLE, COUNT(TLSV_REPLAY_SIMPLE)), "section 3: the simple 1-RTT handshake, its application data and both closes", "");
        PROVEN_TEST_ASSERT(replay(TLSV_REPLAY_COMPAT, COUNT(TLSV_REPLAY_COMPAT)), "section 7: compatibility mode, ChangeCipherSpec records included", "");
    }

    proven_cert_store_destroy(g_anchors);
    proven_cert_store_destroy(g_other_anchors);
    PROVEN_TEST_ASSERT(g_count.live == 0, "everything allocated in this test has been freed", "");
    PROVEN_TEST_PASS("the engine completes, refuses and resumes handshakes as it should, and reproduces RFC 8448.");
    return 0;
}
