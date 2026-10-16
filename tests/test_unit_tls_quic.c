#include "proven.h"
#include "proven_test.h"
#include "../src/proven/proven_internal_quic.h"
#include "test_unit_tls_pki.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * TLS 1.3 in QUIC mode (RFC 9001 sections 4 and 8), with no network: a client and a server
 * engine back to back, handshake bytes handed across one encryption level at a time as the
 * QUIC layer would hand over CRYPTO frames. What must hold: both sides reach the same secret at
 * every level in each direction, under each suite, through a retry, a resumption and a client
 * certificate; the transport parameters arrive; QUIC's rules (TLS 1.3 only, no session id, no
 * ChangeCipherSpec, no KeyUpdate, a protocol agreed, parameters present and well formed) are
 * kept and their breaches end the handshake with the code RFC 9001 names; every bit change of
 * a flight ends it too, and a flight cut anywhere completes when the rest arrives.
 *
 * One check reaches past this engine: the client's Handshake-level secret opens the records a
 * server running over TCP sends for the same ClientHello, so the key schedule in QUIC mode is
 * TLS 1.3's own.
 */

#ifndef PROVEN_NO_HTTP3

static proven_u64 g_rng = 0x2545f4914f6cdd1dull;
static void test_random(void *ctx, proven_byte_t *out, proven_size_t len) {
    (void)ctx;
    for (proven_size_t i = 0; i < len; ++i) {
        g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
        out[i] = (proven_byte_t)(g_rng >> 24);
    }
}
static proven_i64 g_now = TP_NOW;
static proven_i64 test_now(void *ctx) { return *(proven_i64 *)ctx; }
static proven_mem_view_t pem(const char *s) { return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }
static proven_u8str_view_t sv(const char *s) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }

static proven_allocator_t g_alloc;
static proven_cert_store_t *g_anchors;
static const proven_u8str_view_t H3[1] = { { (const proven_byte_t *)"h3", 2 } };
static const proven_u8str_view_t H2[1] = { { (const proven_byte_t *)"h2", 2 } };

static proven_tls_options_t client_options(void) {
    return (proven_tls_options_t){ .alloc = g_alloc, .anchors = g_anchors, .random = test_random, .now = test_now, .now_ctx = &g_now, .alpn = H3, .alpn_count = 1 };
}
static proven_tls_options_t server_options(const char *cert, const char *key) {
    return (proven_tls_options_t){ .alloc = g_alloc, .random = test_random, .now = test_now, .now_ctx = &g_now, .alpn = H3, .alpn_count = 1,
                                   .certificate_pem = pem(cert), .private_key_pem = pem(key) };
}
static proven_tls_config_t *config(const proven_tls_options_t *o) {
    proven_tls_config_t *c = NULL;
    PROVEN_TEST_ASSERT(proven_tls_config_create(o, &c) == PROVEN_OK, "a config", "");
    return c;
}

/* The parameters each side sends: different values both ways, so that a mix-up shows. */
static proven_byte_t g_ctp[128], g_stp[128];
static proven_size_t g_ctp_len, g_stp_len;
static void make_params(void) {
    proven_quic_tp_t t;
    proven_quic_tp_defaults(&t);
    t.has_initial_scid = true; t.initial_scid.len = 8; memcpy(t.initial_scid.id, "clientid", 8);
    t.initial_max_data = 1048576; t.initial_max_streams_bidi = 100; t.max_idle_timeout = 30000;
    g_ctp_len = proven_quic_tp_encode(&t, false, (proven_mem_mut_t){ g_ctp, sizeof g_ctp });
    proven_quic_tp_defaults(&t);
    t.has_initial_scid = true; t.initial_scid.len = 4; memcpy(t.initial_scid.id, "srv1", 4);
    t.has_original_dcid = true; t.original_dcid.len = 8; memcpy(t.original_dcid.id, "firstdst", 8);
    t.has_reset_token = true; memset(t.reset_token, 0x5a, 16);
    t.initial_max_data = 262144; t.initial_max_streams_uni = 3; t.disable_active_migration = true;
    g_stp_len = proven_quic_tp_encode(&t, true, (proven_mem_mut_t){ g_stp, sizeof g_stp });
}

/* How bytes cross: `chunk` bytes per feed (0: all at once); when `flip_dir` is 0 (client to
 * server) or 1, the bit numbered `flip_bit` among the bytes that direction carries at the
 * Initial and Handshake levels is changed on the way. */
typedef struct { proven_size_t chunk; int flip_dir; long flip_bit; long seen[2]; } wire_t;

static proven_byte_t g_buf[70000];

/* Move what `from` has pending at every level to `to`. Returns true when anything moved. */
static bool move(proven_tls_conn_t *from, proven_tls_conn_t *to, int dir, wire_t *w) {
    bool any = false;
    for (int lv = 0; lv < PROVEN_QUIC_LEVELS; ++lv) {
        proven_mem_view_t out = proven_tls_quic_pending_(from, (proven_quic_level_t)lv);
        if (out.size == 0) continue;
        proven_size_t n = out.size;
        memcpy(g_buf, out.ptr, n);
        proven_tls_quic_sent_(from, (proven_quic_level_t)lv, n);
        any = true;
        if (w && lv < PROVEN_QUIC_LEVEL_APPLICATION) {
            long at = w->flip_bit - 8 * w->seen[dir];
            if (w->flip_dir == dir && at >= 0 && at < 8 * (long)n) g_buf[at / 8] ^= (proven_byte_t)(1u << (at % 8));
            w->seen[dir] += (long)n;
        }
        proven_size_t step = (w && w->chunk) ? w->chunk : n;
        for (proven_size_t at = 0; at < n; at += step) {
            proven_size_t k = n - at < step ? n - at : step;
            if (proven_tls_quic_feed_(to, (proven_quic_level_t)lv, (proven_mem_view_t){ g_buf + at, k }) != PROVEN_OK) return true;
        }
    }
    return any;
}

static void pump(proven_tls_conn_t *c, proven_tls_conn_t *s, wire_t *w) {
    for (int round = 0; round < 20; ++round) {
        bool a = move(c, s, 0, w);
        bool b = move(s, c, 1, w);
        if (!a && !b) break;
    }
}

typedef struct { proven_tls_conn_t *c, *s; } pair_t;

static pair_t pair_make(proven_tls_config_t *cc, proven_tls_config_t *sc, proven_tls_session_t *session) {
    pair_t p = { 0 };
    PROVEN_TEST_ASSERT(proven_tls_quic_client_create_(cc, sv("example.test"), session, (proven_mem_view_t){ g_ctp, g_ctp_len }, &p.c) == PROVEN_OK, "a QUIC-mode client", "");
    PROVEN_TEST_ASSERT(proven_tls_quic_server_create_(sc, (proven_mem_view_t){ g_stp, g_stp_len }, &p.s) == PROVEN_OK, "a QUIC-mode server", "");
    return p;
}
static void pair_free(pair_t *p) { proven_tls_conn_destroy(p->c); proven_tls_conn_destroy(p->s); p->c = p->s = NULL; }

/* Both sides at the same secret for `level` in each direction, of the suite's length. */
static bool secrets_agree(const pair_t *p, proven_quic_level_t level) {
    proven_byte_t a[PROVEN_HMAC_MAX_SIZE], b[PROVEN_HMAC_MAX_SIZE];
    proven_size_t n1 = proven_tls_quic_secret_(p->c, level, true, a), n2 = proven_tls_quic_secret_(p->s, level, false, b);
    bool up = n1 > 0 && n1 == n2 && memcmp(a, b, n1) == 0;
    proven_byte_t x[PROVEN_HMAC_MAX_SIZE], y[PROVEN_HMAC_MAX_SIZE];
    proven_size_t n3 = proven_tls_quic_secret_(p->s, level, true, x), n4 = proven_tls_quic_secret_(p->c, level, false, y);
    bool down = n3 > 0 && n3 == n4 && memcmp(x, y, n3) == 0;
    /* The two directions are different secrets. */
    return up && down && memcmp(a, x, n1) != 0 && n1 == proven_tls_quic_suite_(p->c)->hash_len;
}

static bool established_both(const pair_t *p) { return proven_tls_is_established(p->c) && proven_tls_is_established(p->s); }

/* Find extension `type` in a ClientHello (the whole message, with its four-byte header). */
static bool hello_ext(const proven_byte_t *m, proven_size_t n, proven_u16 type, proven_mem_view_t *data) {
    proven_size_t at = 4 + 2 + 32;
    if (at >= n) return false;
    at += 1 + m[at];
    at += 2 + (((proven_size_t)m[at] << 8) | m[at + 1]);
    at += 1 + m[at];
    proven_size_t end = at + 2 + (((proven_size_t)m[at] << 8) | m[at + 1]);
    at += 2;
    while (at + 4 <= end && end <= n) {
        proven_u16 t = (proven_u16)((m[at] << 8) | m[at + 1]);
        proven_size_t len = ((proven_size_t)m[at + 2] << 8) | m[at + 3];
        if (t == type) { *data = (proven_mem_view_t){ m + at + 4, len }; return true; }
        at += 4 + len;
    }
    return false;
}

int main(void) {
    PROVEN_TEST_SUITE("TLS 1.3 in QUIC mode",
        "Two engines in QUIC mode, handshake bytes per encryption level: the same secrets on both sides at every level, the transport parameters, QUIC's rules, and every change refused.",
        "Inspect the QUIC-mode parts of src/proven/tls13.c (quic_emit, keys_read, keys_write, proven_tls_quic_*) and src/proven/quic_crypto.c for the parameters.");

    g_alloc = proven_heap_allocator();
    PROVEN_TEST_ASSERT(tp_build(), "the test's certificates and keys are issued", "");
    PROVEN_TEST_ASSERT(proven_cert_store_create(g_alloc, &g_anchors) == PROVEN_OK && proven_cert_store_add_pem(g_anchors, pem(TP_CA), NULL) == PROVEN_OK, "trust anchors", "");
    make_params();
    PROVEN_TEST_ASSERT(g_ctp_len > 0 && g_stp_len > 0, "both sides' transport parameters encode", "");

    proven_tls_options_t co = client_options(), so = server_options(TP_SERVER_ED, TP_SERVER_ED_KEY);
    proven_tls_config_t *cc = config(&co), *sc = config(&so);
    so = server_options(TP_SERVER_P256, TP_SERVER_P256_KEY);
    proven_tls_config_t *sc256 = config(&so);

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a handshake, level by level", "The client's first flight at Initial, the server's at Initial and Handshake, the client's Finished at Handshake, a ticket at Application.",
        "A secret that differs between the sides means a message reached one transcript and not the other.");
    {
        pair_t p = pair_make(cc, sc, NULL);
        PROVEN_TEST_ASSERT(proven_tls_quic_write_level_(p.c) == PROVEN_QUIC_LEVEL_INITIAL && proven_tls_quic_pending_(p.c, PROVEN_QUIC_LEVEL_INITIAL).size > 0 &&
                           proven_tls_quic_pending_(p.c, PROVEN_QUIC_LEVEL_HANDSHAKE).size == 0, "the ClientHello waits at Initial when the client is made", "");
        proven_mem_view_t ch = proven_tls_quic_pending_(p.c, PROVEN_QUIC_LEVEL_INITIAL);
        PROVEN_TEST_ASSERT(ch.ptr[0] == 1 && ((proven_size_t)ch.ptr[1] << 16 | (proven_size_t)ch.ptr[2] << 8 | ch.ptr[3]) + 4 == ch.size,
            "it is one handshake message with no record header", "");
        proven_mem_view_t ext;
        PROVEN_TEST_ASSERT(ch.ptr[38] == 0, "legacy_session_id is empty: no middlebox compatibility mode (RFC 9001 section 8.4)", "");
        PROVEN_TEST_ASSERT(hello_ext(ch.ptr, ch.size, 0x39, &ext) && ext.size == g_ctp_len && memcmp(ext.ptr, g_ctp, g_ctp_len) == 0,
            "quic_transport_parameters carries the client's parameters as given", "");
        PROVEN_TEST_ASSERT(hello_ext(ch.ptr, ch.size, 43, &ext) && ext.size == 3 && ext.ptr[0] == 2 && ext.ptr[1] == 3 && ext.ptr[2] == 4,
            "supported_versions names TLS 1.3 alone, though the config allows 1.2", "");
        PROVEN_TEST_ASSERT(!hello_ext(ch.ptr, ch.size, 28, &ext) && !hello_ext(ch.ptr, ch.size, 23, &ext) && !hello_ext(ch.ptr, ch.size, 0xff01, &ext),
            "no record_size_limit, and none of what only a TLS 1.2 server reads", "");
        proven_size_t suites_at = 4 + 2 + 32 + 1;
        PROVEN_TEST_ASSERT(ch.ptr[suites_at] == 0 && ch.ptr[suites_at + 1] == 6, "three suites, all TLS 1.3's", "");

        pump(p.c, p.s, NULL);
        PROVEN_TEST_ASSERT(established_both(&p), "both sides complete", "");
        PROVEN_TEST_ASSERT(proven_tls_quic_error_(p.c) == 0 && proven_tls_quic_error_(p.s) == 0, "with no error", "");
        PROVEN_TEST_ASSERT(secrets_agree(&p, PROVEN_QUIC_LEVEL_HANDSHAKE) && secrets_agree(&p, PROVEN_QUIC_LEVEL_APPLICATION), "the same secrets at Handshake and Application, each way", "");
        proven_byte_t x[PROVEN_HMAC_MAX_SIZE];
        PROVEN_TEST_ASSERT(proven_tls_quic_secret_(p.c, PROVEN_QUIC_LEVEL_INITIAL, true, x) == 0, "no Initial secret comes from TLS: it comes from the connection ID", "");
        PROVEN_TEST_ASSERT(proven_tls_quic_read_level_(p.c) == PROVEN_QUIC_LEVEL_APPLICATION && proven_tls_quic_write_level_(p.c) == PROVEN_QUIC_LEVEL_APPLICATION &&
                           proven_tls_quic_read_level_(p.s) == PROVEN_QUIC_LEVEL_APPLICATION && proven_tls_quic_write_level_(p.s) == PROVEN_QUIC_LEVEL_APPLICATION,
            "both sides read and write at Application", "");
        const proven_quic_tp_t *ct = proven_tls_quic_peer_params_(p.s), *st = proven_tls_quic_peer_params_(p.c);
        PROVEN_TEST_ASSERT(ct && ct->initial_max_data == 1048576 && ct->initial_max_streams_bidi == 100 && ct->max_idle_timeout == 30000 &&
                           ct->initial_scid.len == 8 && memcmp(ct->initial_scid.id, "clientid", 8) == 0, "the server has the client's parameters", "");
        PROVEN_TEST_ASSERT(st && st->initial_max_data == 262144 && st->initial_max_streams_uni == 3 && st->disable_active_migration && st->has_reset_token &&
                           st->reset_token[15] == 0x5a && st->has_original_dcid && memcmp(st->original_dcid.id, "firstdst", 8) == 0,
            "and the client the server's", "");
        proven_u8str_view_t a = proven_tls_alpn(p.c), b = proven_tls_alpn(p.s);
        PROVEN_TEST_ASSERT(a.size == 2 && memcmp(a.ptr, "h3", 2) == 0 && b.size == 2 && memcmp(b.ptr, "h3", 2) == 0, "both name h3", "");
        PROVEN_TEST_ASSERT(proven_tls_version(p.c) == PROVEN_TLS_VERSION_1_3, "TLS 1.3", "");
        proven_tls_quic_secret_drop_(p.c, PROVEN_QUIC_LEVEL_HANDSHAKE);
        PROVEN_TEST_ASSERT(proven_tls_quic_secret_(p.c, PROVEN_QUIC_LEVEL_HANDSHAKE, true, x) == 0 && proven_tls_quic_secret_(p.c, PROVEN_QUIC_LEVEL_APPLICATION, true, x) > 0,
            "a dropped level's secrets are gone and the others stay", "");

        /* The record interface is not this connection's. */
        proven_size_t used = 7;
        proven_byte_t buf[8];
        PROVEN_TEST_ASSERT(proven_tls_feed(p.c, (proven_mem_view_t){ buf, 1 }, &used) == PROVEN_ERR_INVALID_STATE && used == 0 &&
                           proven_tls_pending_output(p.s).size == 0 && proven_tls_read(p.c, (proven_mem_mut_t){ buf, 8 }).err == PROVEN_ERR_INVALID_STATE &&
                           proven_tls_write(p.c, (proven_mem_view_t){ buf, 8 }).err == PROVEN_ERR_INVALID_STATE && proven_tls_close(p.c) == PROVEN_ERR_INVALID_STATE &&
                           proven_tls_key_update(p.c) == PROVEN_ERR_UNSUPPORTED,
            "feed, pending output, read, write and close are refused in QUIC mode, and a key update is unsupported", "");

        /* KeyUpdate from the peer: 0x010a (RFC 9001 section 6). */
        static const proven_byte_t key_update[5] = { 24, 0, 0, 1, 0 };
        PROVEN_TEST_ASSERT(proven_tls_quic_feed_(p.c, PROVEN_QUIC_LEVEL_APPLICATION, (proven_mem_view_t){ key_update, 5 }) == PROVEN_ERR_PROTOCOL &&
                           proven_tls_quic_error_(p.c) == 0x010a && !proven_tls_is_established(p.c), "a KeyUpdate message ends the connection with 0x010a", "");
        PROVEN_TEST_ASSERT(proven_tls_quic_feed_(p.c, PROVEN_QUIC_LEVEL_APPLICATION, (proven_mem_view_t){ key_update, 5 }) == PROVEN_ERR_INVALID_STATE, "and the connection takes nothing more", "");
        pair_free(&p);
    }

    PROVEN_TEST_SECTION("every suite, both key types, a retry, a resumption, a client certificate",
        "Each path of the TLS 1.3 handshake in QUIC mode ends with the same secrets on both sides.", "Check the place each path installs keys: keys_read and keys_write.");
    {
        static const proven_u16 suites[3] = { PROVEN_TLS_AES_128_GCM_SHA256, PROVEN_TLS_AES_256_GCM_SHA384, PROVEN_TLS_CHACHA20_POLY1305_SHA256 };
        for (int i = 0; i < 3; ++i) {
            proven_tls_test_knobs(suites[i], false);
            pair_t p = pair_make(cc, i == 1 ? sc256 : sc, NULL);
            pump(p.c, p.s, NULL);
            PROVEN_TEST_ASSERT(established_both(&p) && proven_tls_quic_suite_(p.c)->id == suites[i] && proven_tls_quic_suite_(p.s)->id == suites[i] &&
                               secrets_agree(&p, PROVEN_QUIC_LEVEL_HANDSHAKE) && secrets_agree(&p, PROVEN_QUIC_LEVEL_APPLICATION),
                "the suite agreed, and the same secrets of its length at both levels", "");
            pair_free(&p);
        }
        proven_tls_test_knobs(0, true);
        pair_t p = pair_make(cc, sc256, NULL);
        pump(p.c, p.s, NULL);
        proven_tls_test_knobs(0, false);
        PROVEN_TEST_ASSERT(established_both(&p) && secrets_agree(&p, PROVEN_QUIC_LEVEL_HANDSHAKE) && secrets_agree(&p, PROVEN_QUIC_LEVEL_APPLICATION),
            "a HelloRetryRequest for P-256: the second ClientHello at Initial again, and the same secrets", "");
        pair_free(&p);

        /* A ticket arrives at Application; the next connection resumes with it. */
        static proven_tls_session_t session;
        memset(&session, 0, sizeof session);
        p = pair_make(cc, sc, &session);
        pump(p.c, p.s, NULL);
        PROVEN_TEST_ASSERT(established_both(&p) && !proven_tls_resumed(p.c), "a full handshake that leaves a ticket", "");
        pair_free(&p);
        p = pair_make(cc, sc, &session);
        pump(p.c, p.s, NULL);
        PROVEN_TEST_ASSERT(established_both(&p) && proven_tls_resumed(p.c) && proven_tls_resumed(p.s) &&
                           secrets_agree(&p, PROVEN_QUIC_LEVEL_HANDSHAKE) && secrets_agree(&p, PROVEN_QUIC_LEVEL_APPLICATION),
            "the next one resumes, with the same secrets on both sides", "");
        pair_free(&p);

        proven_tls_options_t o = client_options();
        o.certificate_pem = pem(TP_CLIENT); o.private_key_pem = pem(TP_CLIENT_KEY);
        proven_tls_config_t *cc_auth = config(&o);
        o = server_options(TP_SERVER_P256, TP_SERVER_P256_KEY);
        o.client_auth = PROVEN_TLS_CLIENT_AUTH_REQUIRE; o.anchors = g_anchors;
        proven_tls_config_t *sc_auth = config(&o);
        p = pair_make(cc_auth, sc_auth, NULL);
        pump(p.c, p.s, NULL);
        proven_byte_t key[32];
        PROVEN_TEST_ASSERT(established_both(&p) && proven_tls_peer_key_sha256(p.s, key) && secrets_agree(&p, PROVEN_QUIC_LEVEL_APPLICATION),
            "a client certificate, sent at Handshake and verified", "");
        pair_free(&p);
        p = pair_make(cc, sc_auth, NULL);
        pump(p.c, p.s, NULL);
        PROVEN_TEST_ASSERT(!proven_tls_is_established(p.s) && proven_tls_quic_error_(p.s) == 0x0100 + 116, "and without one, certificate_required as 0x0174", "");
        pair_free(&p);
        proven_tls_config_destroy(cc_auth); proven_tls_config_destroy(sc_auth);
    }

    PROVEN_TEST_SECTION("what a QUIC-mode connection is made from", "TLS 1.3, a protocol to name, parameters that fit.", "Check quic_attach.");
    {
        proven_tls_conn_t *x = (proven_tls_conn_t *)1;
        proven_tls_options_t o = client_options();
        o.alpn = NULL; o.alpn_count = 0;
        proven_tls_config_t *no_alpn = config(&o);
        o = client_options(); o.max_version = PROVEN_TLS_VERSION_1_2;
        proven_tls_config_t *old = config(&o);
        static proven_byte_t big[1025];
        PROVEN_TEST_ASSERT(proven_tls_quic_client_create_(no_alpn, sv("example.test"), NULL, (proven_mem_view_t){ g_ctp, g_ctp_len }, &x) == PROVEN_ERR_INVALID_ARG &&
                           proven_tls_quic_client_create_(old, sv("example.test"), NULL, (proven_mem_view_t){ g_ctp, g_ctp_len }, &x) == PROVEN_ERR_INVALID_ARG &&
                           proven_tls_quic_client_create_(cc, sv("example.test"), NULL, (proven_mem_view_t){ big, sizeof big }, &x) == PROVEN_ERR_INVALID_ARG &&
                           proven_tls_quic_client_create_(cc, sv("example.test"), NULL, (proven_mem_view_t){ NULL, 4 }, &x) == PROVEN_ERR_INVALID_ARG,
            "a client config with no ALPN or no TLS 1.3, and parameters over 1,024 bytes or missing, are refused", "");
        o = server_options(TP_SERVER_ED, TP_SERVER_ED_KEY); o.alpn = NULL; o.alpn_count = 0;
        proven_tls_config_t *s_no_alpn = config(&o);
        PROVEN_TEST_ASSERT(proven_tls_quic_server_create_(s_no_alpn, (proven_mem_view_t){ g_stp, g_stp_len }, &x) == PROVEN_ERR_INVALID_ARG &&
                           proven_tls_quic_server_create_(cc, (proven_mem_view_t){ g_stp, g_stp_len }, &x) == PROVEN_ERR_INVALID_ARG,
            "a server config with no ALPN, or with no certificate, is refused", "");
        PROVEN_TEST_ASSERT(proven_tls_quic_feed_(NULL, 0, (proven_mem_view_t){ g_ctp, 1 }) == PROVEN_ERR_INVALID_ARG && proven_tls_quic_pending_(NULL, 0).size == 0 &&
                           proven_tls_quic_error_(NULL) == 0 && proven_tls_quic_suite_(NULL) == NULL && proven_tls_quic_peer_params_(NULL) == NULL,
            "null connections", "");
        proven_tls_conn_t *tcp = NULL;
        PROVEN_TEST_ASSERT(proven_tls_client_create(cc, sv("example.test"), NULL, &tcp) == PROVEN_OK &&
                           proven_tls_quic_feed_(tcp, 0, (proven_mem_view_t){ g_ctp, 1 }) == PROVEN_ERR_INVALID_ARG && proven_tls_quic_pending_(tcp, 0).size == 0,
            "a connection over TCP is not one in QUIC mode", "");
        proven_tls_conn_destroy(tcp);
        proven_tls_config_destroy(no_alpn); proven_tls_config_destroy(old); proven_tls_config_destroy(s_no_alpn);
    }

    PROVEN_TEST_SECTION("QUIC's rules, broken", "Each refusal ends the handshake with the code RFC 9001 or RFC 9000 names for it.", "The code is in proven_tls_quic_error_.");
    {
        /* No protocol in common: no_application_protocol, 0x0178. */
        proven_tls_options_t o = server_options(TP_SERVER_ED, TP_SERVER_ED_KEY);
        o.alpn = H2;
        proven_tls_config_t *s_h2 = config(&o);
        pair_t p = pair_make(cc, s_h2, NULL);
        pump(p.c, p.s, NULL);
        PROVEN_TEST_ASSERT(!proven_tls_is_established(p.s) && proven_tls_quic_error_(p.s) == 0x0178, "no protocol in common: 0x0178", "");
        pair_free(&p);
        proven_tls_config_destroy(s_h2);

        /* Bytes at the wrong level: 0x010a. */
        p = pair_make(cc, sc, NULL);
        proven_mem_view_t ch = proven_tls_quic_pending_(p.c, PROVEN_QUIC_LEVEL_INITIAL);
        PROVEN_TEST_ASSERT(proven_tls_quic_feed_(p.s, PROVEN_QUIC_LEVEL_HANDSHAKE, ch) == PROVEN_ERR_PROTOCOL && proven_tls_quic_error_(p.s) == 0x010a,
            "a ClientHello at Handshake: 0x010a", "");
        pair_free(&p);
        p = pair_make(cc, sc, NULL);
        ch = proven_tls_quic_pending_(p.c, PROVEN_QUIC_LEVEL_INITIAL);
        PROVEN_TEST_ASSERT(proven_tls_quic_feed_(p.s, PROVEN_QUIC_LEVEL_APPLICATION, ch) == PROVEN_ERR_PROTOCOL && proven_tls_quic_error_(p.s) == 0x010a,
            "or at Application", "");
        pair_free(&p);
        p = pair_make(cc, sc, NULL);
        (void)move(p.c, p.s, 0, NULL);
        proven_mem_view_t sh = proven_tls_quic_pending_(p.s, PROVEN_QUIC_LEVEL_INITIAL), flight = proven_tls_quic_pending_(p.s, PROVEN_QUIC_LEVEL_HANDSHAKE);
        PROVEN_TEST_ASSERT(sh.size > 0 && flight.size > 0, "the server's ServerHello waits at Initial and the rest of its flight at Handshake", "");
        static proven_byte_t both[4096];
        memcpy(both, sh.ptr, sh.size); memcpy(both + sh.size, flight.ptr, flight.size);
        PROVEN_TEST_ASSERT(proven_tls_quic_feed_(p.c, PROVEN_QUIC_LEVEL_INITIAL, (proven_mem_view_t){ both, sh.size + flight.size }) == PROVEN_ERR_PROTOCOL &&
                           proven_tls_quic_error_(p.c) == 0x010a, "the server's whole flight at Initial: what follows the ServerHello is at the wrong level", "");
        pair_free(&p);

        /* Malformed transport parameters: TRANSPORT_PARAMETER_ERROR, 0x08. */
        static const proven_byte_t bad_tp[] = { 0x0f, 0x00, 0x03, 0x02, 0x44, 0x4b };      /* max_udp_payload_size 1099 */
        proven_tls_conn_t *c = NULL, *s = NULL;
        PROVEN_TEST_ASSERT(proven_tls_quic_client_create_(cc, sv("example.test"), NULL, (proven_mem_view_t){ bad_tp, sizeof bad_tp }, &c) == PROVEN_OK &&
                           proven_tls_quic_server_create_(sc, (proven_mem_view_t){ g_stp, g_stp_len }, &s) == PROVEN_OK, "a client that sends a value out of range", "");
        p = (pair_t){ c, s };
        pump(p.c, p.s, NULL);
        PROVEN_TEST_ASSERT(!proven_tls_is_established(p.s) && proven_tls_quic_error_(p.s) == PROVEN_QUIC_TRANSPORT_PARAMETER_ERROR && proven_tls_quic_peer_params_(p.s) == NULL,
            "the server ends it with 0x08", "");
        pair_free(&p);
        PROVEN_TEST_ASSERT(proven_tls_quic_client_create_(cc, sv("example.test"), NULL, (proven_mem_view_t){ g_ctp, g_ctp_len }, &c) == PROVEN_OK &&
                           proven_tls_quic_server_create_(sc, (proven_mem_view_t){ g_ctp, g_ctp_len }, &s) == PROVEN_OK, "a server that sends no original_destination_connection_id", "");
        p = (pair_t){ c, s };
        pump(p.c, p.s, NULL);
        PROVEN_TEST_ASSERT(!proven_tls_is_established(p.c) && proven_tls_quic_error_(p.c) == PROVEN_QUIC_TRANSPORT_PARAMETER_ERROR, "the client ends it with 0x08", "");
        pair_free(&p);

        /* A ClientHello made for TCP, given to a QUIC-mode server: its session id is refused as
         * PROTOCOL_VIOLATION; with the session id taken out, the missing parameters are
         * missing_extension, 0x016d. */
        proven_tls_conn_t *tcp = NULL;
        PROVEN_TEST_ASSERT(proven_tls_client_create(cc, sv("example.test"), NULL, &tcp) == PROVEN_OK, "a client over TCP", "");
        proven_mem_view_t rec = proven_tls_pending_output(tcp);
        static proven_byte_t hello[2048];
        proven_size_t n = rec.size - 5;
        memcpy(hello, rec.ptr + 5, n);
        PROVEN_TEST_ASSERT(proven_tls_quic_server_create_(sc, (proven_mem_view_t){ g_stp, g_stp_len }, &s) == PROVEN_OK &&
                           proven_tls_quic_feed_(s, PROVEN_QUIC_LEVEL_INITIAL, (proven_mem_view_t){ hello, n }) == PROVEN_ERR_PROTOCOL &&
                           proven_tls_quic_error_(s) == PROVEN_QUIC_PROTOCOL_VIOLATION, "a session id: PROTOCOL_VIOLATION, 0x0a", "");
        proven_tls_conn_destroy(s);
        PROVEN_TEST_ASSERT(hello[38] == 32, "the TCP hello has a 32-byte session id", "");
        memmove(hello + 39, hello + 39 + 32, n - 39 - 32);
        hello[38] = 0;
        n -= 32;
        hello[1] = (proven_byte_t)((n - 4) >> 16); hello[2] = (proven_byte_t)((n - 4) >> 8); hello[3] = (proven_byte_t)(n - 4);
        PROVEN_TEST_ASSERT(proven_tls_quic_server_create_(sc, (proven_mem_view_t){ g_stp, g_stp_len }, &s) == PROVEN_OK &&
                           proven_tls_quic_feed_(s, PROVEN_QUIC_LEVEL_INITIAL, (proven_mem_view_t){ hello, n }) == PROVEN_ERR_PROTOCOL &&
                           proven_tls_quic_error_(s) == 0x016d, "without one, no quic_transport_parameters: missing_extension, 0x016d", "");
        proven_tls_conn_destroy(s);
        proven_tls_conn_destroy(tcp);

        /* A hello with no ALPN at all, made QUIC-shaped - no session id, the parameters added
         * as its last extension: no_application_protocol, 0x0178, though over TCP a client
         * may omit ALPN. */
        proven_tls_options_t o2 = client_options();
        o2.alpn = NULL; o2.alpn_count = 0;
        proven_tls_config_t *cc_none = config(&o2);
        PROVEN_TEST_ASSERT(proven_tls_client_create(cc_none, sv("example.test"), NULL, &tcp) == PROVEN_OK, "a client over TCP with no ALPN", "");
        rec = proven_tls_pending_output(tcp);
        n = rec.size - 5;
        memcpy(hello, rec.ptr + 5, n);
        memmove(hello + 39, hello + 39 + 32, n - 39 - 32);
        hello[38] = 0;
        n -= 32;
        proven_size_t at = 39;
        at += 2 + (((proven_size_t)hello[at] << 8) | hello[at + 1]);
        at += 1 + hello[at];
        proven_size_t ext_len = ((proven_size_t)hello[at] << 8) | hello[at + 1];
        PROVEN_TEST_ASSERT(at + 2 + ext_len == n, "the extensions end the hello", "");
        hello[n] = 0x00; hello[n + 1] = 0x39; hello[n + 2] = (proven_byte_t)(g_ctp_len >> 8); hello[n + 3] = (proven_byte_t)g_ctp_len;
        memcpy(hello + n + 4, g_ctp, g_ctp_len);
        n += 4 + g_ctp_len;
        ext_len += 4 + g_ctp_len;
        hello[at] = (proven_byte_t)(ext_len >> 8); hello[at + 1] = (proven_byte_t)ext_len;
        hello[1] = (proven_byte_t)((n - 4) >> 16); hello[2] = (proven_byte_t)((n - 4) >> 8); hello[3] = (proven_byte_t)(n - 4);
        PROVEN_TEST_ASSERT(proven_tls_quic_server_create_(sc, (proven_mem_view_t){ g_stp, g_stp_len }, &s) == PROVEN_OK &&
                           proven_tls_quic_feed_(s, PROVEN_QUIC_LEVEL_INITIAL, (proven_mem_view_t){ hello, n }) == PROVEN_ERR_PROTOCOL &&
                           proven_tls_quic_error_(s) == 0x0178 && proven_tls_quic_peer_params_(s) != NULL,
            "its parameters read, no protocol named: 0x0178", "");
        proven_tls_conn_destroy(s);
        proven_tls_conn_destroy(tcp);
        proven_tls_config_destroy(cc_none);
    }

    PROVEN_TEST_SECTION("the key schedule is TLS 1.3's", "A server over TCP answers the QUIC-mode client's hello; the client's Handshake secret opens its records, and their EncryptedExtensions, lacking the parameters, is refused.",
        "If the records do not open, QUIC mode hands out a secret TLS over TCP would not derive.");
    {
        proven_tls_conn_t *c = NULL, *tcp = NULL;
        PROVEN_TEST_ASSERT(proven_tls_quic_client_create_(cc, sv("example.test"), NULL, (proven_mem_view_t){ g_ctp, g_ctp_len }, &c) == PROVEN_OK &&
                           proven_tls_server_create(sc, &tcp) == PROVEN_OK, "a QUIC-mode client, a server over TCP", "");
        proven_mem_view_t ch = proven_tls_quic_pending_(c, PROVEN_QUIC_LEVEL_INITIAL);
        static proven_byte_t rec[4096], flight[8192];
        rec[0] = 22; rec[1] = 3; rec[2] = 1; rec[3] = (proven_byte_t)(ch.size >> 8); rec[4] = (proven_byte_t)ch.size;
        memcpy(rec + 5, ch.ptr, ch.size);
        proven_size_t used = 0;
        PROVEN_TEST_ASSERT(proven_tls_feed(tcp, (proven_mem_view_t){ rec, 5 + ch.size }, &used) == PROVEN_OK && used == 5 + ch.size,
            "the server over TCP takes the hello, passing over quic_transport_parameters as it always has", "");
        proven_mem_view_t out = proven_tls_pending_output(tcp);
        PROVEN_TEST_ASSERT(out.size > 5 && out.ptr[0] == 22, "it answers with a ServerHello in the clear (and no ChangeCipherSpec: the session id was empty)", "");
        proven_size_t sh_len = ((proven_size_t)out.ptr[3] << 8) | out.ptr[4];
        PROVEN_TEST_ASSERT(proven_tls_quic_feed_(c, PROVEN_QUIC_LEVEL_INITIAL, (proven_mem_view_t){ out.ptr + 5, sh_len }) == PROVEN_OK &&
                           proven_tls_quic_read_level_(c) == PROVEN_QUIC_LEVEL_HANDSHAKE, "the QUIC-mode client takes the ServerHello and reads at Handshake", "");
        proven_byte_t secret[PROVEN_HMAC_MAX_SIZE];
        proven_size_t slen = proven_tls_quic_secret_(c, PROVEN_QUIC_LEVEL_HANDSHAKE, false, secret);
        proven_tls_keys_t keys;
        memset(&keys, 0, sizeof keys);
        proven_tls13_set_keys(&keys, proven_tls_quic_suite_(c), secret);
        proven_size_t at = 5 + sh_len, got = 0, records = 0;
        bool opened = slen > 0;
        while (opened && at + 5 <= out.size) {
            proven_size_t len = ((proven_size_t)out.ptr[at + 3] << 8) | out.ptr[at + 4];
            memcpy(rec, out.ptr + at, 5 + len);
            proven_byte_t type = 0;
            proven_mem_mut_t content;
            opened = proven_tls13_open(&keys, rec, len, &type, &content) && type == 22;
            if (opened) { memcpy(flight + got, content.ptr, content.size); got += content.size; records++; }
            at += 5 + len;
        }
        PROVEN_TEST_ASSERT(opened && records >= 1 && got > 0 && flight[0] == 8, "every record after the ServerHello opens under the client's Handshake read secret, an EncryptedExtensions first", "");
        PROVEN_TEST_ASSERT(proven_tls_quic_feed_(c, PROVEN_QUIC_LEVEL_HANDSHAKE, (proven_mem_view_t){ flight, got }) == PROVEN_ERR_PROTOCOL &&
                           proven_tls_quic_error_(c) == 0x016d, "and EncryptedExtensions with no quic_transport_parameters is missing_extension, 0x016d", "");
        proven_tls_conn_destroy(c); proven_tls_conn_destroy(tcp);
    }

    PROVEN_TEST_SECTION("flights cut anywhere", "Handshake bytes a byte at a time, and in pieces of 7 and 64, complete as whole flights do.", "Check the reassembly in handshake_bytes.");
    {
        static const proven_size_t chunks[] = { 1, 7, 64 };
        for (int i = 0; i < 3; ++i) {
            pair_t p = pair_make(cc, sc256, NULL);
            wire_t w = { .chunk = chunks[i], .flip_dir = -1 };
            pump(p.c, p.s, &w);
            PROVEN_TEST_ASSERT(established_both(&p) && secrets_agree(&p, PROVEN_QUIC_LEVEL_HANDSHAKE) && secrets_agree(&p, PROVEN_QUIC_LEVEL_APPLICATION),
                "complete, with the same secrets", "");
            pair_free(&p);
        }
    }

    PROVEN_TEST_SECTION("every bit change of a flight", "A changed bit at Initial or Handshake, in either direction, never leaves both sides established.",
        "A change that completes means some byte of the handshake reaches neither the transcript nor a check.");
    {
        /* The number of bits each direction carries below Application. */
        pair_t p = pair_make(cc, sc, NULL);
        wire_t w = { .flip_dir = -1 };
        pump(p.c, p.s, &w);
        PROVEN_TEST_ASSERT(established_both(&p), "the unchanged run completes", "");
        pair_free(&p);
        long bits[2] = { 8 * w.seen[0], 8 * w.seen[1] };
        PROVEN_TEST_INFO("{} bits client to server, {} bits server to client", PROVEN_ARG(bits[0]), PROVEN_ARG(bits[1]));
        long completed = 0, refused = 0, stalled = 0;
        for (int dir = 0; dir < 2; ++dir) {
            for (long bit = 0; bit < bits[dir]; ++bit) {
                p = pair_make(cc, sc, NULL);
                wire_t f = { .flip_dir = dir, .flip_bit = bit };
                pump(p.c, p.s, &f);
                if (established_both(&p)) completed++;
                else if (proven_tls_quic_error_(p.c) != 0 || proven_tls_quic_error_(p.s) != 0) refused++;
                else stalled++;
                pair_free(&p);
            }
        }
        PROVEN_TEST_INFO("{} changed handshakes: {} refused with an error code, {} waiting for bytes that never come", PROVEN_ARG(bits[0] + bits[1]), PROVEN_ARG(refused), PROVEN_ARG(stalled));
        PROVEN_TEST_ASSERT(completed == 0, "no changed handshake completes", "");
        PROVEN_TEST_ASSERT(refused > stalled, "most end with an error code; the rest only wait (a length made longer)", "");
    }

    proven_tls_config_destroy(cc); proven_tls_config_destroy(sc); proven_tls_config_destroy(sc256);
    proven_cert_store_destroy(g_anchors);
    PROVEN_TEST_PASS("TLS 1.3 in QUIC mode: the same secrets on both sides at every level, QUIC's rules kept, every change refused.");
    return 0;
}

#else

int main(void) {
    PROVEN_TEST_SUITE("TLS 1.3 in QUIC mode", "Built with PROVEN_NO_HTTP3: nothing to test.", "");
    PROVEN_TEST_PASS("skipped: PROVEN_NO_HTTP3");
    return 0;
}

#endif
