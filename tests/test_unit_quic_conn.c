#include "proven.h"
#include "proven_test.h"
#include "../src/proven/proven_internal_quic.h"
#include "test_unit_tls_pki.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The QUIC connection (RFC 9000, RFC 9002) between two engines over a simulated network: a
 * virtual clock, and a seeded network that delays, loses, duplicates and reorders datagrams,
 * capturing every one. What must hold: the handshake completes with and without a Retry;
 * streams deliver every byte in order under loss; flow control and the caps hold; each rule's
 * breach closes the connection with its code; closes, idle timeouts, stateless resets, version
 * negotiation, the amplification limit, path validation and key updates behave as RFC 9000
 * says; 0-RTT (RFC-0021) answers a request in one round trip, and a refusal or a loss of the
 * early data still delivers it; a NEW_TOKEN token validates the next connection's address; and every bit change or truncation of captured packets is dropped or refused, never
 * acted on.
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
static proven_tls_config_t *g_tls_client, *g_tls_server;
static const proven_u8str_view_t H3[1] = { { (const proven_byte_t *)"h3", 2 } };

/* ---- The network ---- */

typedef struct { proven_byte_t b[1500]; proven_size_t n; proven_u64 at; int to; proven_quic_addr_t from; } dg_t;

typedef struct net net_t;
struct net {
    proven_quic_conn_t *ep[2];          /* 0: the client, 1: the server */
    proven_quic_addr_t addr[2];
    proven_quic_config_t cfg[2];
    proven_tls_session_t session;
    dg_t *q; proven_size_t qn, qcap;
    proven_u64 now;
    unsigned loss, dup, reorder;        /* per thousand */
    proven_u64 delay, jitter;           /* microseconds */
    proven_u64 rng;
    bool require_retry;
    dg_t *cap; proven_size_t capn, capcap;   /* every datagram sent, when capturing */
    bool capture;
    proven_u64 sent[2], bytes[2], lost;
    proven_quic_addr_t last_to[2];
    bool reading_off[2];                /* an endpoint that reads nothing (flow control tests) */
    /* A hook that sees each datagram as it is delivered: false drops it. */
    bool (*hook)(net_t *n, int to, proven_byte_t *b, proven_size_t *len);
    int replies;                        /* stateless replies the server sent */
    proven_u64 until;                   /* the clock does not jump past it (0: no bound) */
};

static proven_u64 nrand(net_t *n) { n->rng ^= n->rng << 13; n->rng ^= n->rng >> 7; n->rng ^= n->rng << 17; return n->rng; }

static void net_push(net_t *n, int to, const proven_byte_t *b, proven_size_t len, const proven_quic_addr_t *from, proven_u64 at) {
    if (n->qn == n->qcap) { n->qcap = n->qcap ? n->qcap * 2 : 256; n->q = realloc(n->q, n->qcap * sizeof *n->q); }
    dg_t *d = &n->q[n->qn++];
    memcpy(d->b, b, len); d->n = len; d->at = at; d->to = to; d->from = *from;
}

static void net_send(net_t *n, int from_ep, const proven_byte_t *b, proven_size_t len, const proven_quic_addr_t *to_addr) {
    if (to_addr) n->last_to[from_ep] = *to_addr;
    ++n->sent[from_ep];
    n->bytes[from_ep] += len;
    if (n->capture) {
        if (n->capn == n->capcap) { n->capcap = n->capcap ? n->capcap * 2 : 256; n->cap = realloc(n->cap, n->capcap * sizeof *n->cap); }
        dg_t *d = &n->cap[n->capn++];
        memcpy(d->b, b, len); d->n = len; d->to = 1 - from_ep; d->from = n->addr[from_ep];
    }
    if (n->loss && nrand(n) % 1000 < n->loss) { ++n->lost; return; }
    proven_u64 at = n->now + n->delay + (n->jitter ? nrand(n) % n->jitter : 0);
    if (n->reorder && nrand(n) % 1000 < n->reorder) at += n->delay + 1000;
    net_push(n, 1 - from_ep, b, len, &n->addr[from_ep], at);
    if (n->dup && nrand(n) % 1000 < n->dup) net_push(n, 1 - from_ep, b, len, &n->addr[from_ep], at + 300);
}

static void net_init(net_t *n, unsigned seed) {
    memset(n, 0, sizeof *n);
    n->rng = 0x9e3779b97f4a7c15ull ^ ((proven_u64)seed * 0x100000001b3ull);
    n->now = 1000000;
    n->delay = 10000;
    n->addr[0] = (proven_quic_addr_t){ .len = 6, .b = { 10, 0, 0, 1, 0x30, 0x39 } };
    n->addr[1] = (proven_quic_addr_t){ .len = 6, .b = { 10, 0, 0, 2, 0x01, 0xbb } };
    for (int i = 0; i < 2; ++i) n->cfg[i] = (proven_quic_config_t){ .alloc = g_alloc, .tls = i ? g_tls_server : g_tls_client };
    for (int i = 0; i < 32; ++i) n->cfg[1].static_key[i] = (proven_byte_t)(0x40 + i);
}

static void net_start(net_t *n) {
    PROVEN_TEST_ASSERT(proven_quic_conn_client_create(&n->cfg[0], sv("example.test"), &n->session, &n->addr[1], n->now, &n->ep[0]) == PROVEN_OK,
        "a client is made", "");
}

static void net_free(net_t *n) {
    for (int i = 0; i < 2; ++i) proven_quic_conn_destroy(n->ep[i]);
    free(n->q); free(n->cap);
    n->q = NULL; n->cap = NULL;
    n->ep[0] = n->ep[1] = NULL;
}

/* A datagram reaches endpoint `to`; the server is made from its first one. */
static void deliver(net_t *n, int to, proven_byte_t *b, proven_size_t len, const proven_quic_addr_t *from) {
    if (n->hook && !n->hook(n, to, b, &len)) return;
    if (to == 1 && !n->ep[1]) {
        static proven_byte_t out[1500];
        proven_size_t olen = 0;
        proven_quic_accept_t acc;
        proven_quic_stateless_t r = proven_quic_server_stateless(&n->cfg[1], (proven_mem_view_t){ b, len }, from, n->now, n->require_retry,
                                                                 (proven_mem_mut_t){ out, sizeof out }, &olen, &acc);
        if (r == PROVEN_QUIC_STATELESS_REPLY) { ++n->replies; net_send(n, 1, out, olen, from); return; }
        if (r == PROVEN_QUIC_STATELESS_ACCEPT)
            PROVEN_TEST_ASSERT(proven_quic_conn_server_create(&n->cfg[1], &acc, (proven_mem_view_t){ b, len }, from, n->now, &n->ep[1]) == PROVEN_OK, "the server's connection", "");
        return;
    }
    if (n->ep[to]) PROVEN_TEST_ASSERT(proven_quic_conn_receive(n->ep[to], (proven_mem_view_t){ b, len }, from, n->now) == PROVEN_OK, "a datagram is taken", "");
}

/* One turn: each side sends what it may, the clock moves to the next thing, it happens. */
static void net_step(net_t *n) {
    static proven_byte_t out[1500];
    for (int i = 0; i < 2; ++i) {
        if (!n->ep[i]) continue;
        for (int k = 0; k < 512; ++k) {
            proven_size_t len = 0;
            proven_quic_addr_t to;
            PROVEN_TEST_ASSERT(proven_quic_conn_send(n->ep[i], n->now, (proven_mem_mut_t){ out, sizeof out }, &len, &to) == PROVEN_OK, "send", "");
            if (len == 0) break;
            PROVEN_TEST_ASSERT(len <= 1452, "a datagram within the size", "");
            net_send(n, i, out, len, &to);
        }
    }
    proven_u64 next = (proven_u64)-1;
    for (proven_size_t i = 0; i < n->qn; ++i) if (n->q[i].at < next) next = n->q[i].at;
    for (int i = 0; i < 2; ++i) if (n->ep[i]) { proven_u64 d = proven_quic_conn_deadline(n->ep[i]); if (d < next) next = d; }
    if (next == (proven_u64)-1) next = n->now + 1000000;
    if (n->until && next > n->until) next = n->until;
    if (next > n->now) n->now = next;
    /* Deliver in arrival order. */
    for (;;) {
        proven_size_t best = n->qn;
        for (proven_size_t i = 0; i < n->qn; ++i) if (n->q[i].at <= n->now && (best == n->qn || n->q[i].at < n->q[best].at)) best = i;
        if (best == n->qn) break;
        dg_t d = n->q[best];
        n->q[best] = n->q[--n->qn];
        deliver(n, d.to, d.b, d.n, &d.from);
    }
    for (int i = 0; i < 2; ++i) if (n->ep[i] && proven_quic_conn_deadline(n->ep[i]) <= n->now) proven_quic_conn_timeout(n->ep[i], n->now);
}

static proven_quic_stats_t stats(net_t *n, int i) {
    proven_quic_stats_t s = { 0 };
    if (n->ep[i]) proven_quic_conn_stats(n->ep[i], &s);
    return s;
}

/* Run until both sides are established, or the clock passes `limit` microseconds from now. */
static bool run_handshake(net_t *n, proven_u64 limit) {
    const proven_u64 end = n->now + limit;
    while (n->now < end) {
        net_step(n);
        if (n->ep[1] && stats(n, 0).confirmed && stats(n, 1).established) return true;
    }
    return false;
}

/* ---- Streams carrying a pattern: each byte a function of its stream and offset ---- */

static proven_byte_t pattern(proven_u64 id, proven_u64 off) { return (proven_byte_t)((off * 131 + id * 7 + (off >> 9)) & 0xff); }

typedef struct {
    proven_u64 id;
    proven_u64 total, written, read;
    bool fin_written, fin_read, bad, bidi, echo_read;
    proven_byte_t stage[4096];          /* the next bytes to write, made once */
    proven_u64 stage_off;
    proven_size_t stage_n;
    int owner;                          /* the endpoint that writes it */
} xfer_t;

typedef struct { xfer_t x[256]; int n; } xfers_t;

static xfer_t *xfer_find(xfers_t *t, proven_u64 id) { for (int i = 0; i < t->n; ++i) if (t->x[i].id == id) return &t->x[i]; return NULL; }

static void pump(net_t *n, xfers_t *t) {
    static proven_byte_t buf[65536];
    for (int e = 0; e < 2; ++e) {
        if (!n->ep[e]) continue;
        proven_quic_event_t ev;
        while (proven_quic_conn_poll(n->ep[e], &ev)) {
            if (ev.kind != PROVEN_QUIC_EV_STREAM_READABLE || n->reading_off[e]) continue;
            xfer_t *x = xfer_find(t, ev.stream_id);
            if (!x) continue;
            if (x->owner == e) {
                /* The answer on one's own bidirectional stream: nothing, then the end. */
                proven_size_t got = 0; bool fin = false;
                if (proven_quic_stream_read(n->ep[e], ev.stream_id, (proven_mem_mut_t){ buf, sizeof buf }, &got, &fin) == PROVEN_OK && fin) x->echo_read = true;
                if (got) x->bad = true;
                continue;
            }
            for (;;) {
                proven_size_t got = 0; bool fin = false;
                if (proven_quic_stream_read(n->ep[e], ev.stream_id, (proven_mem_mut_t){ buf, sizeof buf }, &got, &fin) != PROVEN_OK) break;
                for (proven_size_t i = 0; i < got; ++i) if (buf[i] != pattern(x->id, x->read + i)) x->bad = true;
                x->read += got;
                if (fin) {
                    x->fin_read = true;
                    if (x->bidi) { proven_size_t tk = 0; (void)proven_quic_stream_write(n->ep[e], ev.stream_id, (proven_mem_view_t){ buf, 0 }, true, &tk); }
                    break;
                }
                if (got == 0) break;
            }
        }
    }
    for (int i = 0; i < t->n; ++i) {
        xfer_t *x = &t->x[i];
        if (x->fin_written || !n->ep[x->owner]) continue;
        while (x->written < x->total) {
            if (x->stage_n == 0 || x->stage_off != x->written) {
                x->stage_n = (proven_size_t)(x->total - x->written < sizeof x->stage ? x->total - x->written : sizeof x->stage);
                x->stage_off = x->written;
                for (proven_size_t k = 0; k < x->stage_n; ++k) x->stage[k] = pattern(x->id, x->written + k);
            }
            const proven_size_t len = x->stage_n;
            proven_size_t taken = 0;
            if (proven_quic_stream_write(n->ep[x->owner], x->id, (proven_mem_view_t){ x->stage, len }, x->written + len == x->total, &taken) != PROVEN_OK) break;
            x->written += taken;
            if (taken < len) break;
        }
        if (x->written == x->total) x->fin_written = true;
    }
}

static bool xfers_done(const xfers_t *t) {
    for (int i = 0; i < t->n; ++i) if (!t->x[i].fin_read || t->x[i].read != t->x[i].total || t->x[i].bad || (t->x[i].bidi && !t->x[i].echo_read)) return false;
    return true;
}

static xfer_t *xfer_open(net_t *n, xfers_t *t, int owner, bool bidi, proven_u64 total) {
    proven_u64 id;
    PROVEN_TEST_ASSERT(proven_quic_stream_open(n->ep[owner], bidi, &id) == PROVEN_OK, "a stream opens", "");
    xfer_t *x = &t->x[t->n++];
    *x = (xfer_t){ .id = id, .total = total, .owner = owner, .bidi = bidi };
    return x;
}

static bool run_xfers(net_t *n, xfers_t *t, proven_u64 limit) {
    const proven_u64 end = n->now + limit;
    while (n->now < end) {
        pump(n, t);
        net_step(n);
        if (xfers_done(t)) return true;
    }
    return false;
}

static void fresh(net_t *n, unsigned seed) {
    net_init(n, seed);
    net_start(n);
    PROVEN_TEST_ASSERT(run_handshake(n, 10000000), "the handshake completes", "");
}

/* The CLOSED event of an endpoint, if it has come; other events are passed over. */
static bool closed_event(proven_quic_conn_t *c, proven_quic_event_t *out) {
    proven_quic_event_t ev;
    while (proven_quic_conn_poll(c, &ev)) if (ev.kind == PROVEN_QUIC_EV_CLOSED) { *out = ev; return true; }
    return false;
}

static void run_for(net_t *n, proven_u64 us) {
    const proven_u64 end = n->now + us;
    n->until = end;
    while (n->now < end) net_step(n);
    n->until = 0;
}

/* An allocator that remembers the largest single allocation since it was last reset. */
static proven_size_t g_rec_biggest;
static proven_result_mem_mut_t rec_alloc(void *ctx, proven_size_t size, proven_size_t align) {
    (void)ctx;
    if (size > g_rec_biggest) g_rec_biggest = size;
    return g_alloc.alloc_fn(g_alloc.ctx, size, align);
}
static proven_result_mem_mut_t rec_realloc(void *ctx, void *p, proven_size_t old, proven_size_t size, proven_size_t align) {
    (void)ctx;
    if (size > g_rec_biggest) g_rec_biggest = size;
    return g_alloc.realloc_fn(g_alloc.ctx, p, old, size, align);
}
static void rec_free(void *ctx, void *p) { (void)ctx; g_alloc.free_fn(g_alloc.ctx, p); }

/* Write `want` more bytes of the pattern on `x`, as many as the stream takes. */
static void write_pattern(net_t *n, xfer_t *x, proven_u64 *off, proven_u64 want) {
    static proven_byte_t chunk[4096];
    const proven_u64 end = *off + want;
    while (*off < end) {
        const proven_size_t len = (proven_size_t)(end - *off < sizeof chunk ? end - *off : sizeof chunk);
        for (proven_size_t k = 0; k < len; ++k) chunk[k] = pattern(x->id, *off + k);
        proven_size_t taken = 0;
        if (proven_quic_stream_write(n->ep[x->owner], x->id, (proven_mem_view_t){ chunk, len }, false, &taken) != PROVEN_OK) break;
        *off += taken;
        if (taken < len) break;
    }
}

/* Run until the reader has read `upto` bytes of `x` and a while longer, for the acknowledgements. */
static void drain_stream(net_t *n, xfers_t *t, const xfer_t *x, proven_u64 upto) {
    for (int r = 0; r < 400 && x->read < upto; ++r) { pump(n, t); run_for(n, 5000); }
    for (int r = 0; r < 20; ++r) { pump(n, t); run_for(n, 5000); }
}

/* Hooks. */
static int g_hook_count;
static bool hook_drop_all(net_t *n, int to, proven_byte_t *b, proven_size_t *len) { (void)n; (void)to; (void)b; (void)len; return false; }
static bool hook_server_deaf_after_first(net_t *n, int to, proven_byte_t *b, proven_size_t *len) {
    (void)n; (void)b; (void)len;
    if (to != 1) return true;
    return g_hook_count++ == 0;
}
static bool hook_server_deaf_first(net_t *n, int to, proven_byte_t *b, proven_size_t *len) {
    (void)n; (void)b; (void)len;
    return to != 1 || g_hook_count++ > 0;
}
static bool hook_other_version_first(net_t *n, int to, proven_byte_t *b, proven_size_t *len) {
    (void)n; (void)len;
    if (to == 1 && g_hook_count++ == 0) { b[1] = 0x1a; b[2] = 0x2a; b[3] = 0x3a; b[4] = 0x4a; }
    return true;
}
static bool hook_reset_from_server(net_t *n, int to, proven_byte_t *b, proven_size_t *len) {
    if (to != 1 || (b[0] & 0x80)) return true;
    proven_quic_cid_t dcid = { .len = 8 };
    memcpy(dcid.id, b + 1, 8);
    static proven_byte_t out[64];
    proven_size_t r = proven_quic_stateless_reset(&n->cfg[1], &dcid, *len, (proven_mem_mut_t){ out, sizeof out });
    if (r) { ++g_hook_count; net_push(n, 0, out, r, &n->addr[1], n->now + n->delay); }
    return false;
}

/* The client's first two datagrams that carry a Handshake packet are lost: its Finished, then
 * the probe that carried it again. */
static bool hook_lose_client_handshake_twice(net_t *n, int to, proven_byte_t *b, proven_size_t *len) {
    (void)n;
    if (to != 1 || g_hook_count >= 2) return true;
    proven_size_t at = 0;
    proven_quic_hdr_t h;
    while (at < *len && proven_quic_hdr_parse((proven_mem_view_t){ b + at, *len - at }, 8, &h)) {
        if (h.type == PROVEN_QUIC_PKT_HANDSHAKE) { ++g_hook_count; return false; }   /* coalesced or not */
        if (h.type == PROVEN_QUIC_PKT_SHORT) break;
        at += h.len;
    }
    return true;
}

/* A path that carries no datagram above 1,280 bytes. */
static bool hook_mtu_1280(net_t *n, int to, proven_byte_t *b, proven_size_t *len) {
    (void)n; (void)to; (void)b;
    if (*len <= 1280) return true;
    ++g_hook_count;
    return false;
}

/* Toward the server only: nothing at all (a blackout), or nothing above 1,280 bytes. */
/* Drops every datagram that reaches the client up to g_drop_until, and counts them. */
static proven_u64 g_drop_until;
static bool hook_drop_to_client_until(net_t *n, int to, proven_byte_t *b, proven_size_t *len) {
    (void)b; (void)len;
    if (to != 0 || n->now > g_drop_until) return true;
    ++g_hook_count;
    return false;
}
static bool hook_blackout_to_server(net_t *n, int to, proven_byte_t *b, proven_size_t *len) { (void)n; (void)b; (void)len; return to != 1; }
static bool hook_blackout_to_client(net_t *n, int to, proven_byte_t *b, proven_size_t *len) { (void)n; (void)b; (void)len; return to != 0; }
static bool hook_mtu_1280_to_server(net_t *n, int to, proven_byte_t *b, proven_size_t *len) { (void)n; (void)b; return to != 1 || *len <= 1280; }

/* A payload of frames, from hex. */
static proven_size_t unhex(const char *hex, proven_byte_t *out, proven_size_t cap) {
    proven_size_t k = strlen(hex) / 2;
    PROVEN_TEST_ASSERT(k <= cap, "a payload that fits", "");
    for (proven_size_t i = 0; i < k; ++i) { char c[3] = { hex[2 * i], hex[2 * i + 1], 0 }; out[i] = (proven_byte_t)strtoul(c, NULL, 16); }
    return k;
}

int main(void) {
    PROVEN_TEST_SUITE("QUIC connections",
        "Two QUIC engines over a simulated network: the handshake, streams under loss, flow control and caps, every rule's code, closes and resets, and changed packets refused.",
        "Inspect src/proven/quic_conn.c; the network is this test's, seeded, with a virtual clock.");

    g_alloc = proven_heap_allocator();
    PROVEN_TEST_ASSERT(tp_build(), "the test's certificates and keys are issued", "");
    PROVEN_TEST_ASSERT(proven_cert_store_create(g_alloc, &g_anchors) == PROVEN_OK && proven_cert_store_add_pem(g_anchors, pem(TP_CA), NULL) == PROVEN_OK, "trust anchors", "");
    proven_tls_options_t co = { .alloc = g_alloc, .anchors = g_anchors, .random = test_random, .now = test_now, .now_ctx = &g_now, .alpn = H3, .alpn_count = 1 };
    proven_tls_options_t so = { .alloc = g_alloc, .random = test_random, .now = test_now, .now_ctx = &g_now, .alpn = H3, .alpn_count = 1,
                                .certificate_pem = pem(TP_SERVER_P256), .private_key_pem = pem(TP_SERVER_P256_KEY) };
    PROVEN_TEST_ASSERT(proven_tls_config_create(&co, &g_tls_client) == PROVEN_OK && proven_tls_config_create(&so, &g_tls_server) == PROVEN_OK, "TLS configs", "");

    net_t *n = calloc(1, sizeof *n);

    PROVEN_TEST_SECTION("the handshake", "Client and server complete and confirm; one stream each way carries a megabyte.", "");
    {
        net_init(n, 1);
        net_start(n);
        PROVEN_TEST_ASSERT(run_handshake(n, 5000000), "both sides complete and the client confirms within five seconds", "");
        PROVEN_TEST_ASSERT(stats(n, 1).path_validated, "with no Retry, the server took a Handshake packet as the client's address validated (RFC 9000 section 8.1)", "");
        xfers_t *t = calloc(1, sizeof *t);
        xfer_open(n, t, 0, true, 1 << 20);
        xfer_open(n, t, 1, true, 1 << 20);
        PROVEN_TEST_ASSERT(run_xfers(n, t, 60000000), "a megabyte each way, every byte in order", "");
        printf("  after: sent %llu/%llu datagrams, now %llu us\n", (unsigned long long)n->sent[0], (unsigned long long)n->sent[1], (unsigned long long)n->now);
        free(t);
        net_free(n);
    }

    PROVEN_TEST_SECTION("loss, reordering and duplication", "Four streams, half a megabyte each, both ways, at 1, 5 and 20 percent loss with reordering and duplication: every byte in order.", "");
    {
        const unsigned losses[3] = { 10, 50, 200 };
        for (int li = 0; li < 3; ++li) {
            net_init(n, 10 + (unsigned)li);
            n->loss = losses[li]; n->reorder = 50; n->dup = 20; n->jitter = 4000;
            net_start(n);
            PROVEN_TEST_ASSERT(run_handshake(n, 120000000), "the handshake completes under loss", "");
            xfers_t *t = calloc(1, sizeof *t);
            for (int k = 0; k < 2; ++k) { xfer_open(n, t, 0, true, 512 * 1024); xfer_open(n, t, 1, k == 0, 512 * 1024); }
            const proven_u64 start = n->now;
            PROVEN_TEST_ASSERT(run_xfers(n, t, 900000000), "every byte delivered in order", "");
            proven_quic_stats_t a = stats(n, 0), b = stats(n, 1);
            printf("  loss %u/1000: %.2f s, %llu lost by the network, %llu and %llu declared lost, cwnd %llu and %llu\n", losses[li],
                   (double)(n->now - start) / 1e6, (unsigned long long)n->lost, (unsigned long long)a.packets_lost, (unsigned long long)b.packets_lost,
                   (unsigned long long)a.cwnd, (unsigned long long)b.cwnd);
            free(t);
            net_free(n);
        }
    }

    PROVEN_TEST_SECTION("a lost Finished behind 1-RTT data", "The client's Finished and its first probe are lost while it is already sending a megabyte the server cannot read yet: the Finished goes out again although that data fills the window, and everything arrives.", "");
    {
        net_init(n, 15);
        g_hook_count = 0;
        n->hook = hook_lose_client_handshake_twice;
        net_start(n);
        xfers_t *t = calloc(1, sizeof *t);
        for (int k = 0; k < 200 && !stats(n, 0).established; ++k) net_step(n);
        PROVEN_TEST_ASSERT(stats(n, 0).established && !stats(n, 0).confirmed, "the client has completed the handshake and not yet heard it confirmed", "");
        xfer_open(n, t, 0, true, 1 << 20);
        PROVEN_TEST_ASSERT(run_xfers(n, t, 20000000), "the megabyte arrives: the Finished was not held behind data the server could not acknowledge", "");
        PROVEN_TEST_ASSERT(g_hook_count == 2, "both losses happened", "");
        free(t);
        net_free(n);
    }

    PROVEN_TEST_SECTION("a hundred streams at once, and more when they end", "Each side's 100 streams in use together; the 101st waits (BUSY) and opens once streams end: MAX_STREAMS renewed.", "");
    {
        fresh(n, 20);
        xfers_t *t = calloc(1, sizeof *t);
        for (int k = 0; k < 100; ++k) xfer_open(n, t, 0, true, 8000 + (proven_u64)k * 37);
        for (int k = 0; k < 100; ++k) xfer_open(n, t, 1, false, 5000 + (proven_u64)k * 11);
        proven_u64 id;
        PROVEN_TEST_ASSERT(proven_quic_stream_open(n->ep[0], true, &id) == PROVEN_ERR_BUSY && proven_quic_stream_open(n->ep[1], false, &id) == PROVEN_ERR_BUSY,
            "the 101st stream of a kind waits for the peer's limit", "");
        PROVEN_TEST_ASSERT(run_xfers(n, t, 60000000), "two hundred streams deliver", "");
        run_for(n, 200000);
        PROVEN_TEST_ASSERT(stats(n, 0).streams == 0 && stats(n, 1).streams == 0, "every stream that ended is freed on both sides", "");
        t->n = 0;
        for (int k = 0; k < 100; ++k) xfer_open(n, t, 0, true, 1000);
        PROVEN_TEST_ASSERT(run_xfers(n, t, 60000000), "a hundred more open on renewed credit, and deliver", "");
        free(t);
        net_free(n);
    }

    PROVEN_TEST_SECTION("Retry", "A server that asks for a Retry: one Retry, a token, the address validated, the handshake and data as usual.", "");
    {
        net_init(n, 30);
        n->require_retry = true;
        net_start(n);
        PROVEN_TEST_ASSERT(run_handshake(n, 10000000) && n->replies == 1 && stats(n, 1).path_validated, "one Retry, then the handshake, the address validated by the token", "");
        xfers_t *t = calloc(1, sizeof *t);
        xfer_open(n, t, 0, true, 100000);
        PROVEN_TEST_ASSERT(run_xfers(n, t, 10000000), "and data", "");
        free(t);
        net_free(n);
    }

    PROVEN_TEST_SECTION("0-RTT (RFC-0021)",
        "With a session whose ticket allows early data, a client writes its request before the handshake and the server answers it in its first flight: one round trip. Refused (a ticket used twice), the request goes again under 1-RTT; lost, it is found lost and sent again; a refused client whose early data does not fit the server's own limits ends the connection.",
        "Inspect zr_install, early_after, early_refused, app_ready and the QC_ZRTT paths of build_packet, seal and read_packet.");
    {
        proven_tls_options_t o = { .alloc = g_alloc, .anchors = g_anchors, .random = test_random, .now = test_now, .now_ctx = &g_now, .alpn = H3, .alpn_count = 1, .early_data = true };
        proven_tls_config_t *ce = NULL, *se = NULL;
        PROVEN_TEST_ASSERT(proven_tls_config_create(&o, &ce) == PROVEN_OK, "a client configuration with early_data", "");
        o = (proven_tls_options_t){ .alloc = g_alloc, .random = test_random, .now = test_now, .now_ctx = &g_now, .alpn = H3, .alpn_count = 1, .early_data = true,
                                    .certificate_pem = pem(TP_SERVER_P256), .private_key_pem = pem(TP_SERVER_P256_KEY) };
        PROVEN_TEST_ASSERT(proven_tls_config_create(&o, &se) == PROVEN_OK, "a server configuration with early_data", "");
        static proven_tls_session_t kept, used;

        /* A first connection leaves a ticket that allows early data. */
        net_init(n, 40);
        n->cfg[0].tls = ce; n->cfg[1].tls = se;
        net_start(n);
        proven_u64 id;
        PROVEN_TEST_ASSERT(proven_quic_stream_open(n->ep[0], true, &id) == PROVEN_ERR_INVALID_STATE, "without a session: no stream before the handshake", "");
        PROVEN_TEST_ASSERT(run_handshake(n, 5000000), "a first handshake", "");
        run_for(n, 100000);
        PROVEN_TEST_ASSERT(stats(n, 0).early == 0, "no early data on it", "");
        memcpy(&kept, &n->session, sizeof kept);
        net_free(n);

        /* The next one: the request before the handshake, the answer one round trip later. */
        net_init(n, 41);
        n->cfg[0].tls = ce; n->cfg[1].tls = se;
        memcpy(&n->session, &kept, sizeof kept);
        memcpy(&used, &kept, sizeof used);
        net_start(n);
        proven_quic_event_t ev;
        bool told = false;
        while (proven_quic_conn_poll(n->ep[0], &ev)) if (ev.kind == PROVEN_QUIC_EV_EARLY) told = true;
        PROVEN_TEST_ASSERT(told, "the client is told at once that early data may be written", "");
        xfers_t *t = calloc(1, sizeof *t);
        const proven_u64 start = n->now;
        xfer_open(n, t, 0, true, 1000);
        /* Done is looked for as soon as what arrived has been read: before the clock moves on. */
        bool answered = false;
        pump(n, t);
        for (int i = 0; i < 200 && !answered; ++i) { net_step(n); pump(n, t); answered = xfers_done(t); }
        PROVEN_TEST_ASSERT(answered, "the request is answered", "");
        const proven_u64 took = n->now - start;
        PROVEN_TEST_ASSERT(stats(n, 0).early == 1 && stats(n, 1).early == 1, "both sides say early data was accepted", "");
        printf("  0-RTT: answered after %llu us (round trip 20,000)\n", (unsigned long long)took);
        PROVEN_TEST_ASSERT(took <= 20000, "within one round trip of 20 ms: the answer came with the server's first flight", "");
        PROVEN_TEST_ASSERT(run_handshake(n, 1000000), "the handshake completes as well", "");
        free(t);
        net_free(n);

        /* The same ticket again: refused by the register, the request sent again under 1-RTT. */
        net_init(n, 42);
        n->cfg[0].tls = ce; n->cfg[1].tls = se;
        memcpy(&n->session, &used, sizeof used);
        net_start(n);
        t = calloc(1, sizeof *t);
        const proven_u64 start2 = n->now;
        xfer_open(n, t, 0, true, 3000);
        PROVEN_TEST_ASSERT(run_xfers(n, t, 2000000), "a refused request is answered all the same", "");
        PROVEN_TEST_ASSERT(stats(n, 0).early == 2 && stats(n, 1).early == 2, "both sides say early data was refused", "");
        PROVEN_TEST_ASSERT(n->now - start2 >= 40000, "two round trips: the request went again after the handshake", "");
        free(t);
        net_free(n);

        /* The first datagram, Initial and 0-RTT, lost: the request still arrives. */
        memcpy(&kept, &n->session, sizeof kept);     /* the last connection left a fresh ticket */
        net_init(n, 43);
        n->cfg[0].tls = ce; n->cfg[1].tls = se;
        memcpy(&n->session, &kept, sizeof kept);
        g_hook_count = 0;
        n->hook = hook_server_deaf_first;
        net_start(n);
        t = calloc(1, sizeof *t);
        xfer_open(n, t, 0, true, 5000);
        PROVEN_TEST_ASSERT(run_xfers(n, t, 10000000), "a request whose 0-RTT packets were lost is answered", "");
        n->hook = NULL;
        free(t);
        memcpy(&kept, &n->session, sizeof kept);
        net_free(n);

        /* Refused by a server whose limits are now smaller than what the early data used. */
        net_init(n, 44);
        n->cfg[0].tls = ce; n->cfg[1].tls = se;
        n->cfg[1].stream_window = 4096; n->cfg[1].conn_window = 4096;
        memcpy(&n->session, &kept, sizeof kept);
        net_start(n);
        t = calloc(1, sizeof *t);
        xfer_open(n, t, 0, true, 12000);
        pump(n, t);
        for (int i = 0; i < 400 && !(n->ep[0] && stats(n, 0).closed); ++i) net_step(n);
        PROVEN_TEST_ASSERT(stats(n, 0).early == 2 && stats(n, 0).closed, "the client, refused, could not resend what it sent early within the server's new limits: closed", "");
        bool internal = false;
        while (proven_quic_conn_poll(n->ep[0], &ev)) if (ev.kind == PROVEN_QUIC_EV_CLOSED && !ev.remote && ev.code == 0x01) internal = true;
        PROVEN_TEST_ASSERT(internal, "with INTERNAL_ERROR, its own", "");
        free(t);
        net_free(n);

        /* A server without early_data: its tickets do not allow it, and the client waits. */
        net_init(n, 45);
        n->cfg[0].tls = ce;
        net_start(n);
        PROVEN_TEST_ASSERT(run_handshake(n, 5000000), "a handshake with a server without early_data", "");
        run_for(n, 100000);
        memcpy(&kept, &n->session, sizeof kept);
        net_free(n);
        net_init(n, 46);
        n->cfg[0].tls = ce;
        memcpy(&n->session, &kept, sizeof kept);
        net_start(n);
        PROVEN_TEST_ASSERT(proven_quic_stream_open(n->ep[0], true, &id) == PROVEN_ERR_INVALID_STATE, "its ticket allows no early data: no stream before the handshake", "");
        PROVEN_TEST_ASSERT(run_handshake(n, 5000000) && stats(n, 0).early == 0, "and the connection resumes without it", "");
        net_free(n);

        proven_tls_config_destroy(ce); proven_tls_config_destroy(se);
    }

    PROVEN_TEST_SECTION("NEW_TOKEN (RFC-0021)",
        "A server gives its client a token; the client's next connection carries it, and the server takes its address as validated at once (no three-times limit) and asks for no Retry. A token from another address, or more than a day old, counts as none; a new port on the same address does not matter.",
        "Inspect new_token_make, new_token_ok, token_bound and proven_quic_server_stateless; the client's take and keep in proven_quic_conn_client_create and on_frame.");
    {
        static proven_tls_session_t with_token;
        net_init(n, 50);
        n->addr[0].port_at = 4; n->addr[0].port_len = 2;
        net_start(n);
        PROVEN_TEST_ASSERT(run_handshake(n, 5000000), "a first connection", "");
        run_for(n, 100000);
        const proven_u64 issued = n->now;
        memcpy(&with_token, &n->session, sizeof with_token);
        proven_byte_t tok[255];
        static proven_tls_session_t probe;
        memcpy(&probe, &with_token, sizeof probe);
        PROVEN_TEST_ASSERT(proven_tls_session_token_take_(&probe, tok, sizeof tok) == 1 + 12 + 8 + 16 && tok[0] == 2, "the client kept a token of 37 bytes, of the NEW_TOKEN kind", "");
        net_free(n);

        /* `retry`, `port`, `ip`, `later`: the next connection's conditions. Returns whether the
         * server took the address as validated with the client's first datagram. */
        for (int k = 0; k < 6; ++k) {
            const bool retry = k == 1 || k == 3, other_ip = k == 2 || k == 3, old = k == 4, none = k == 5;
            net_init(n, 51 + (unsigned)k);
            n->now = issued + (old ? 25ull * 3600 * 1000000 : 1000000);
            n->require_retry = retry;
            n->addr[0].port_at = 4; n->addr[0].port_len = 2;
            n->addr[0].b[5] = 0x40;                              /* another port each time */
            if (other_ip) n->addr[0].b[3] = 7;
            if (!none) memcpy(&n->session, &with_token, sizeof with_token);
            net_start(n);
            while (!n->ep[1] && n->now < issued + 30ull * 3600 * 1000000) net_step(n);
            const bool validated_at_once = n->ep[1] && stats(n, 1).path_validated;
            PROVEN_TEST_ASSERT(run_handshake(n, 5000000), "the connection completes", "");
            if (k == 0) PROVEN_TEST_ASSERT(validated_at_once && n->replies == 0, "a token from the last connection, from another port: validated at once", "");
            if (k == 1) PROVEN_TEST_ASSERT(validated_at_once && n->replies == 0, "and a server that asks for Retries asks none of it", "");
            if (k == 2) PROVEN_TEST_ASSERT(!validated_at_once && n->replies == 0, "from another address: as no token (and not dropped)", "");
            if (k == 3) PROVEN_TEST_ASSERT(n->replies == 1 && stats(n, 1).path_validated, "from another address with Retries asked for: a Retry, as without a token", "");
            if (k == 4) PROVEN_TEST_ASSERT(!validated_at_once, "more than a day old: as no token", "");
            if (k == 5) PROVEN_TEST_ASSERT(!validated_at_once, "without a token: not validated at once, as before", "");
            net_free(n);
        }
        static proven_tls_session_t once;
        memcpy(&once, &with_token, sizeof once);
        PROVEN_TEST_ASSERT(proven_tls_session_token_take_(&once, tok, sizeof tok) > 0 && proven_tls_session_token_take_(&once, tok, sizeof tok) == 0,
            "a token is taken out of the session once", "");
    }

    PROVEN_TEST_SECTION("version negotiation", "A server answers another version with version negotiation; a client ignores one that lists version 1 and gives up on one that does not.", "");
    {
        net_init(n, 40);
        g_hook_count = 0;
        n->hook = hook_other_version_first;
        net_start(n);
        PROVEN_TEST_ASSERT(run_handshake(n, 10000000) && n->replies == 1, "the server answered the other version; the client, offered version 1 there, went on", "");
        net_free(n);

        net_init(n, 41);
        n->hook = hook_drop_all;
        n->capture = true;
        net_start(n);
        net_step(n);
        PROVEN_TEST_ASSERT(n->capn >= 1, "the client's first datagram", "");
        proven_quic_hdr_t h;
        PROVEN_TEST_ASSERT(proven_quic_hdr_parse((proven_mem_view_t){ n->cap[0].b, n->cap[0].n }, 8, &h), "its header", "");
        proven_byte_t vn[64];
        proven_size_t k = 0;
        vn[k++] = 0x80; vn[k++] = 0; vn[k++] = 0; vn[k++] = 0; vn[k++] = 0;
        vn[k++] = h.scid.len; memcpy(vn + k, h.scid.id, h.scid.len); k += h.scid.len;
        vn[k++] = h.dcid.len; memcpy(vn + k, h.dcid.id, h.dcid.len); k += h.dcid.len;
        vn[k++] = 0x1a; vn[k++] = 0x2a; vn[k++] = 0x3a; vn[k++] = 0x4a;
        PROVEN_TEST_ASSERT(proven_quic_conn_receive(n->ep[0], (proven_mem_view_t){ vn, k }, &n->addr[1], n->now) == PROVEN_OK, "", "");
        proven_quic_event_t ev;
        PROVEN_TEST_ASSERT(closed_event(n->ep[0], &ev) && ev.remote && ev.code == PROVEN_QUIC_CONNECTION_REFUSED, "a version list without version 1 ends the attempt", "");
        net_free(n);
    }

    PROVEN_TEST_SECTION("closing", "An application's close reaches the peer with its code; both sides end after the closing period; nothing is sent while draining.", "");
    {
        fresh(n, 50);
        proven_quic_conn_close(n->ep[0], true, 0x1234);
        run_for(n, 3000000);
        proven_quic_event_t a, b;
        PROVEN_TEST_ASSERT(closed_event(n->ep[0], &a) && !a.remote && a.app && a.code == 0x1234, "the client was told of its own close", "");
        PROVEN_TEST_ASSERT(closed_event(n->ep[1], &b) && b.remote && b.app && b.code == 0x1234, "the server got the application's code", "");
        PROVEN_TEST_ASSERT(stats(n, 0).closed && stats(n, 1).closed, "both are over after three PTOs", "");
        proven_u64 sent = n->sent[1];
        run_for(n, 1000000);
        PROVEN_TEST_ASSERT(n->sent[1] == sent, "the drained side sends nothing", "");
        net_free(n);
        /* A close asked for long after the engine last saw the clock - the call has no time of its
         * own - whose timeout is looked at before anything is sent: the closing period starts
         * when the close goes out, so the peer is told. (It began at the stale time, was over at
         * once, and the peer heard nothing.) */
        fresh(n, 51);
        n->now += 2000000;                         /* two quiet seconds that no call saw */
        proven_quic_conn_close(n->ep[1], true, 0x99);
        proven_quic_conn_timeout(n->ep[1], n->now);
        PROVEN_TEST_ASSERT(!stats(n, 1).closed, "a close not yet sent does not end with the stale time", "");
        run_for(n, 3000000);
        PROVEN_TEST_ASSERT(closed_event(n->ep[1], &a) && closed_event(n->ep[0], &b) && b.remote && b.app && b.code == 0x99, "the peer gets the code", "");
        PROVEN_TEST_ASSERT(stats(n, 0).closed && stats(n, 1).closed, "and both are over", "");
        net_free(n);
        /* Both close at once, and the peer's close arrives before this side's own has gone out:
         * draining, its period three PTOs - not the idle timeout's thirty seconds. */
        fresh(n, 53);
        n->now += 2000000;
        proven_quic_conn_close(n->ep[1], true, 0x77);
        proven_quic_conn_close(n->ep[0], true, 0x78);
        static proven_byte_t out[1500];
        proven_size_t len = 0;
        proven_quic_addr_t to;
        PROVEN_TEST_ASSERT(proven_quic_conn_send(n->ep[0], n->now, (proven_mem_mut_t){ out, sizeof out }, &len, &to) == PROVEN_OK && len > 0, "the client's close", "");
        PROVEN_TEST_ASSERT(proven_quic_conn_receive(n->ep[1], (proven_mem_view_t){ out, len }, &n->addr[0], n->now) == PROVEN_OK, "reaches the server before its own close went out", "");
        n->hook = hook_drop_all;
        run_for(n, 1000000);
        PROVEN_TEST_ASSERT(stats(n, 1).closed, "the server is over within a second, its draining period", "");
        net_free(n);
    }

    PROVEN_TEST_SECTION("acknowledgements alone", "A packet that holds only ACK frames is not acknowledged (RFC 9000 section 13.2.1): an idle connection between two of these engines sends nothing for a second.", "Check the condition before put_ack in build_packet.");
    {
        fresh(n, 52);
        run_for(n, 200000);                        /* what the handshake owes, acknowledged */
        const proven_u64 before = n->sent[0] + n->sent[1];
        run_for(n, 1000000);
        printf("  datagrams in a quiet second: %llu\n", (unsigned long long)(n->sent[0] + n->sent[1] - before));
        PROVEN_TEST_ASSERT(n->sent[0] + n->sent[1] - before <= 2, "an ACK is not answered with an ACK (they answered each other every round trip)", "");
        net_free(n);
    }

    PROVEN_TEST_SECTION("each rule's code", "Frames that break RFC 9000's rules, read by a server: the connection closes with the code the RFC names, and the client is told it.", "");
    {
        static const struct { const char *frames; proven_u64 code; const char *what; } cases[] = {
            { "0b03" "00" "01" "00",             PROVEN_QUIC_STREAM_STATE_ERROR, "STREAM on the server's own unidirectional stream" },
            { "0b01" "00" "01" "00",             PROVEN_QUIC_STREAM_STATE_ERROR, "STREAM on a server stream not opened" },
            { "0b" "4190" "00" "01" "00",        PROVEN_QUIC_STREAM_LIMIT_ERROR, "STREAM on client stream 400, past the limit of 100" },
            { "0e00" "80040000" "01" "00",       PROVEN_QUIC_FLOW_CONTROL_ERROR, "STREAM past the stream's window (256 KiB)" },
            { "0b00" "02" "6869" "0e00" "10" "01" "00", PROVEN_QUIC_FINAL_SIZE_ERROR, "data past a stream's final size" },
            { "0a00" "04" "61626364" "0400" "00" "02", PROVEN_QUIC_FINAL_SIZE_ERROR, "RESET_STREAM below what was received" },
            { "0502" "00",                       PROVEN_QUIC_STREAM_STATE_ERROR, "STOP_SENDING on a stream the server only receives" },
            { "1102" "4000",                     PROVEN_QUIC_STREAM_STATE_ERROR, "MAX_STREAM_DATA on a stream the server only receives" },
            { "1503" "00",                       PROVEN_QUIC_STREAM_STATE_ERROR, "STREAM_DATA_BLOCKED on a stream the server only sends" },
            { "1e",                              PROVEN_QUIC_PROTOCOL_VIOLATION, "HANDSHAKE_DONE to a server" },
            { "0701" "aa",                       PROVEN_QUIC_PROTOCOL_VIOLATION, "NEW_TOKEN to a server" },
            { "19" "4100",                       PROVEN_QUIC_PROTOCOL_VIOLATION, "RETIRE_CONNECTION_ID of one never issued" },
            { "02" "8000ffff" "00" "00" "00",    PROVEN_QUIC_PROTOCOL_VIOLATION, "ACK of a packet never sent" },
            { "30",                              PROVEN_QUIC_FRAME_ENCODING_ERROR, "an unknown frame type" },
            { "06" "81000000" "01" "00",         PROVEN_QUIC_CRYPTO_BUFFER_EXCEEDED, "CRYPTO past the window" },
            { "180a" "00" "08" "0a0a0a0a0a0a0a0a" "0000000000000000000000000000000a", PROVEN_QUIC_CONNECTION_ID_LIMIT_ERROR,
              "a fifth connection ID while the client's four are active (our limit is 4)" },
            { "1801" "00" "08" "0101010101010101" "00000000000000000000000000000001"
              "1801" "00" "08" "0909090909090909" "00000000000000000000000000000001", PROVEN_QUIC_PROTOCOL_VIOLATION, "one sequence number, two connection IDs" },
            { "0e00" "8003fffe" "01" "00" "0e04" "8003fffe" "01" "00" "0e08" "8003fffe" "01" "00" "0e0c" "8003fffe" "01" "00" "0e10" "8003fffe" "01" "00",
                                                 PROVEN_QUIC_FLOW_CONTROL_ERROR, "five streams' highest offsets past the connection's window" },
        };
        static proven_byte_t payload[512];
        for (proven_size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
            fresh(n, 60 + (unsigned)i);
            proven_size_t k = unhex(cases[i].frames, payload, sizeof payload);
            proven_u64 code = proven_quic_conn_test_frames_(n->ep[1], 2, (proven_mem_view_t){ payload, k }, n->now);
            if (code != cases[i].code) printf("  %s: %llx\n", cases[i].what, (unsigned long long)code);
            PROVEN_TEST_ASSERT(code == cases[i].code, cases[i].what, "");
            run_for(n, 1000000);
            proven_quic_event_t ev;
            PROVEN_TEST_ASSERT(closed_event(n->ep[0], &ev) && ev.remote && !ev.app && ev.code == cases[i].code, "the client is told the code", "");
            net_free(n);
        }
        fresh(n, 90);
        PROVEN_TEST_ASSERT(proven_quic_conn_test_frames_(n->ep[1], 0, (proven_mem_view_t){ (const proven_byte_t *)"\x10\x05", 2 }, n->now) == PROVEN_QUIC_PROTOCOL_VIOLATION,
            "MAX_DATA in an Initial packet: not allowed there", "");
        net_free(n);
        /* 0-RTT packets (RFC 9000 section 12.4): what they may not carry, and what they may. */
        static const struct { const char *frames; proven_u64 code; const char *what; } zr[] = {
            { "02" "00" "00" "00" "00", PROVEN_QUIC_PROTOCOL_VIOLATION, "ACK in a 0-RTT packet" },
            { "06" "00" "01" "00",      PROVEN_QUIC_PROTOCOL_VIOLATION, "CRYPTO in a 0-RTT packet" },
            { "1e",                     PROVEN_QUIC_PROTOCOL_VIOLATION, "HANDSHAKE_DONE in a 0-RTT packet" },
            { "19" "00",                PROVEN_QUIC_PROTOCOL_VIOLATION, "RETIRE_CONNECTION_ID in a 0-RTT packet" },
            { "1b" "0102030405060708",  PROVEN_QUIC_PROTOCOL_VIOLATION, "PATH_RESPONSE in a 0-RTT packet" },
            { "0b00" "01" "61" "01",    0,                              "STREAM and PING in a 0-RTT packet: allowed" },
        };
        for (proven_size_t i = 0; i < sizeof zr / sizeof zr[0]; ++i) {
            fresh(n, 91 + (unsigned)i);
            proven_size_t k = unhex(zr[i].frames, payload, sizeof payload);
            PROVEN_TEST_ASSERT(proven_quic_conn_test_frames_(n->ep[1], 3, (proven_mem_view_t){ payload, k }, n->now) == zr[i].code, zr[i].what, "");
            net_free(n);
        }
    }

    PROVEN_TEST_SECTION("the idle timeout", "A silent network: both sides close without a word after the idle timeout (and not before).", "");
    {
        fresh(n, 100);
        n->hook = hook_drop_all;
        run_for(n, 25000000);
        PROVEN_TEST_ASSERT(!stats(n, 0).closed && !stats(n, 1).closed, "not before 30 seconds", "");
        run_for(n, 10000000);
        proven_quic_event_t a, b;
        PROVEN_TEST_ASSERT(stats(n, 0).closed && stats(n, 1).closed && closed_event(n->ep[0], &a) && closed_event(n->ep[1], &b) && !a.remote && !b.remote && a.code == 0,
            "both closed by the idle timeout", "");
        net_free(n);
        /* A server that says 2 seconds: the client keeps the lesser of the two as well. */
        net_init(n, 101);
        n->cfg[1].idle_timeout_ms = 2000;
        net_start(n);
        PROVEN_TEST_ASSERT(run_handshake(n, 10000000), "", "");
        n->hook = hook_drop_all;
        run_for(n, 1500000);
        PROVEN_TEST_ASSERT(!stats(n, 0).closed, "the client is open after 1.5 s of silence", "");
        run_for(n, 2500000);
        PROVEN_TEST_ASSERT(stats(n, 0).closed && stats(n, 1).closed, "and both have closed by 4 s: the lesser timeout, not the client's 30 s", "");
        net_free(n);
    }

    PROVEN_TEST_SECTION("stateless reset", "A server that lost its state answers with a stateless reset; the client recognises its token and ends.", "");
    {
        fresh(n, 110);
        run_for(n, 500000);
        g_hook_count = 0;
        n->hook = hook_reset_from_server;
        xfers_t *t = calloc(1, sizeof *t);
        xfer_open(n, t, 0, true, 5000);
        for (int k = 0; k < 50 && !stats(n, 0).closed; ++k) { pump(n, t); net_step(n); }
        proven_quic_event_t ev;
        PROVEN_TEST_ASSERT(g_hook_count > 0 && closed_event(n->ep[0], &ev) && ev.remote, "the client ended on the reset", "");
        free(t);
        net_free(n);
    }

    PROVEN_TEST_SECTION("the amplification limit", "A server whose client goes quiet after its first Initial sends at most three times what it received.", "");
    {
        net_init(n, 120);
        g_hook_count = 0;
        n->hook = hook_server_deaf_after_first;
        net_start(n);
        run_for(n, 5000000);
        printf("  the server sent %llu bytes for %d received\n", (unsigned long long)n->bytes[1], 1200);
        PROVEN_TEST_ASSERT(n->bytes[1] > 0 && n->bytes[1] <= 3 * 1200, "no more than three times 1,200 bytes", "");
        n->hook = NULL;
        PROVEN_TEST_ASSERT(run_handshake(n, 20000000), "and when the client is heard again, the handshake completes", "");
        net_free(n);
        /* Such a server closed by its application - an HTTP server's head timeout - can send no
         * close: it owes that address nothing more. It still ends within its closing period, not
         * at the idle timeout. (It was held for thirty seconds and more.) */
        net_init(n, 122);
        g_hook_count = 0;
        n->hook = hook_server_deaf_after_first;
        net_start(n);
        run_for(n, 3000000);
        PROVEN_TEST_ASSERT(n->ep[1] && !stats(n, 1).closed, "a server owed nothing more by its client", "");
        const proven_u64 sent_before = n->bytes[1];
        proven_quic_conn_close(n->ep[1], true, 0x100);
        run_for(n, 5000000);
        PROVEN_TEST_ASSERT(stats(n, 1).closed, "closed by its application, it is over within five seconds", "");
        PROVEN_TEST_ASSERT(n->bytes[1] <= 3 * 1200 && n->bytes[1] >= sent_before, "and still within three times what it received", "");
        net_free(n);
        /* An Initial in a datagram under 1,200 bytes is dropped by a server (RFC 9000 section 14.1).
         * Initial keys come from the connection ID, so anyone can make one: the test does. */
        net_init(n, 121);
        n->capture = true;
        net_start(n);
        for (int k = 0; k < 3 && !n->ep[1]; ++k) net_step(n);
        PROVEN_TEST_ASSERT(n->ep[1] && n->capn >= 1, "the server exists after the client's first datagram", "");
        proven_quic_hdr_t h0;
        PROVEN_TEST_ASSERT(proven_quic_hdr_parse((proven_mem_view_t){ n->cap[0].b, n->cap[0].n }, 8, &h0), "", "");
        proven_byte_t cs[32], ss[32];
        proven_quic_initial_secrets((proven_mem_view_t){ h0.dcid.id, h0.dcid.len }, cs, ss);
        proven_quic_keys_t ck;
        proven_quic_keys_set(&ck, proven_quic_initial_suite(), cs);
        static proven_byte_t dg[1300];
        for (int padded = 0; padded < 2; ++padded) {
            memset(dg, 0, sizeof dg);
            const proven_size_t plen = 30;
            proven_size_t hl = proven_quic_long_header_put(PROVEN_QUIC_PKT_INITIAL, &h0.dcid, &h0.scid, (proven_mem_view_t){ 0 }, 1000 + (proven_u64)padded, 2, plen,
                                                           (proven_mem_mut_t){ dg, sizeof dg });
            dg[hl] = 0x01;                          /* PING, then PADDING */
            const proven_size_t len = proven_quic_protect(&ck, dg, hl, 2, 1000 + (proven_u64)padded, plen);
            const proven_size_t dlen = padded ? 1200 : len;   /* the rest of a padded datagram is zeros: no packet */
            const proven_u64 before = stats(n, 1).packets_received;
            PROVEN_TEST_ASSERT(proven_quic_conn_receive(n->ep[1], (proven_mem_view_t){ dg, dlen }, &n->addr[0], n->now) == PROVEN_OK, "", "");
            if (!padded) PROVEN_TEST_ASSERT(stats(n, 1).packets_received == before, "an Initial in a datagram of 70 bytes is dropped", "");
            else PROVEN_TEST_ASSERT(stats(n, 1).packets_received == before + 1, "the same packet in a datagram of 1,200 bytes is read: the drop was the size", "");
        }
        net_free(n);
    }

    PROVEN_TEST_SECTION("the client's address changes", "A client behind a NAT that rebinds: the server validates the new path and data goes on.", "");
    {
        fresh(n, 130);
        xfers_t *t = calloc(1, sizeof *t);
        xfer_open(n, t, 0, true, 300000);
        xfer_open(n, t, 1, true, 300000);
        for (int k = 0; k < 40; ++k) { pump(n, t); net_step(n); }
        n->addr[0] = (proven_quic_addr_t){ .len = 6, .b = { 10, 0, 0, 9, 0x77, 0x77 } };
        for (int k = 0; k < 400 && stats(n, 1).path_validated; ++k) { pump(n, t); net_step(n); }
        PROVEN_TEST_ASSERT(!stats(n, 1).path_validated, "the server saw the new address and is validating it", "");
        static const proven_byte_t wrong[9] = { 0x1b, 1, 2, 3, 4, 5, 6, 7, 8 };
        PROVEN_TEST_ASSERT(proven_quic_conn_test_frames_(n->ep[1], 2, (proven_mem_view_t){ wrong, 9 }, n->now) == 0 && !stats(n, 1).path_validated,
            "a PATH_RESPONSE that does not echo the challenge validates nothing", "");
        PROVEN_TEST_ASSERT(run_xfers(n, t, 30000000), "the transfer completes across the change", "");
        PROVEN_TEST_ASSERT(stats(n, 1).path_validated && memcmp(n->last_to[1].b, n->addr[0].b, 6) == 0, "the server validated the new address and sends there", "");
        free(t);
        net_free(n);
        /* The same change with the server's challenge lost: everything on its way to the client
         * when the server sees the new address, and the first datagram after it, which carries the
         * PATH_CHALLENGE. The packet is declared lost and the challenge goes again (RFC 9000
         * section 8.2.1): the path is still validated, not given up at the validation timer. */
        fresh(n, 131);
        t = calloc(1, sizeof *t);
        xfer_open(n, t, 0, true, 300000);
        xfer_open(n, t, 1, true, 300000);
        for (int k = 0; k < 40; ++k) { pump(n, t); net_step(n); }
        const proven_quic_addr_t old_addr = n->addr[0];
        n->addr[0] = (proven_quic_addr_t){ .len = 6, .b = { 10, 0, 0, 9, 0x77, 0x78 } };
        for (int k = 0; k < 400 && stats(n, 1).path_validated; ++k) { pump(n, t); net_step(n); }
        PROVEN_TEST_ASSERT(!stats(n, 1).path_validated, "the server saw the new address", "");
        g_hook_count = 0;
        /* What is on its way, and what the server sends in the next three delays: the challenge
         * goes once the client's next packet lifts the amplification limit on the new path. */
        g_drop_until = n->now + 3 * n->delay;
        n->hook = hook_drop_to_client_until;
        const proven_u64 packets_lost = stats(n, 1).packets_lost, seen_at = n->now;
        proven_u64 validated_at = 0;
        bool sent_new = false, went_back = false;
        for (int k = 0; k < 100000 && !validated_at; ++k) {
            pump(n, t); net_step(n);
            if (memcmp(n->last_to[1].b, n->addr[0].b, 6) == 0) sent_new = true;
            else if (sent_new && memcmp(n->last_to[1].b, old_addr.b, 6) == 0) went_back = true;
            if (stats(n, 1).path_validated) validated_at = n->now;
        }
        PROVEN_TEST_ASSERT(run_xfers(n, t, 30000000), "the transfer completes across the change", "");
        printf("  %d datagrams to the client dropped; the server declared %llu packets lost; validated %llu us after it saw the address; %llu validations in all\n", g_hook_count,
               (unsigned long long)(stats(n, 1).packets_lost - packets_lost), (unsigned long long)(validated_at - seen_at), (unsigned long long)stats(n, 1).path_validations);
        PROVEN_TEST_ASSERT(g_hook_count > 0 && stats(n, 1).packets_lost > packets_lost, "the challenge's packet was lost and found lost", "");
        PROVEN_TEST_ASSERT(validated_at && !went_back && stats(n, 1).path_validations == 1, "the challenge went again: the new address validated in one validation (without the second challenge the timer gives it up, the server goes back to the old address and must begin anew)", "");
        PROVEN_TEST_ASSERT(stats(n, 1).path_validated && memcmp(n->last_to[1].b, n->addr[0].b, 6) == 0, "the server sends to the new address", "");
        free(t);
        net_free(n);
    }

    PROVEN_TEST_SECTION("key updates", "Either side starts a key update in the middle of a transfer; the other follows; nothing is lost.", "");
    {
        fresh(n, 140);
        xfers_t *t = calloc(1, sizeof *t);
        xfer_open(n, t, 0, true, 400000);
        xfer_open(n, t, 1, true, 400000);
        for (int k = 0; k < 30; ++k) { pump(n, t); net_step(n); }
        PROVEN_TEST_ASSERT(proven_quic_conn_test_key_update_(n->ep[0]), "the client updates", "");
        for (int k = 0; k < 60; ++k) { pump(n, t); net_step(n); }
        PROVEN_TEST_ASSERT(proven_quic_conn_test_key_update_(n->ep[1]), "then the server", "");
        PROVEN_TEST_ASSERT(run_xfers(n, t, 30000000), "the transfer completes", "");
        PROVEN_TEST_ASSERT(stats(n, 0).key_updates == 2 && stats(n, 1).key_updates == 2, "each side made one update and followed one", "");
        free(t);
        net_free(n);
        /* RFC 9001 section 6.6: each side updates by itself after a number of packets (here 300). */
        net_init(n, 141);
        n->cfg[0].key_update_packets = 300; n->cfg[1].key_update_packets = 300;
        n->loss = 20; n->reorder = 20;
        net_start(n);
        PROVEN_TEST_ASSERT(run_handshake(n, 10000000), "", "");
        t = calloc(1, sizeof *t);
        xfer_open(n, t, 0, true, 2 << 20);
        xfer_open(n, t, 1, true, 2 << 20);
        PROVEN_TEST_ASSERT(run_xfers(n, t, 120000000), "two megabytes each way at 2 percent loss across many updates", "");
        printf("  key updates: %llu and %llu\n", (unsigned long long)stats(n, 0).key_updates, (unsigned long long)stats(n, 1).key_updates);
        PROVEN_TEST_ASSERT(stats(n, 0).key_updates >= 5 && stats(n, 1).key_updates >= 5, "each side's key moved on at least five times", "");
        free(t);
        net_free(n);
        /* Every 4 packets: faster than a round trip, so only "not before a packet under the
         * current key is acknowledged" keeps the two sides on keys the other can still open. */
        net_init(n, 142);
        n->cfg[0].key_update_packets = 4; n->cfg[1].key_update_packets = 4;
        net_start(n);
        PROVEN_TEST_ASSERT(run_handshake(n, 10000000), "", "");
        t = calloc(1, sizeof *t);
        xfer_open(n, t, 0, true, 300000);
        xfer_open(n, t, 1, true, 300000);
        const proven_u64 started = n->now;
        PROVEN_TEST_ASSERT(run_xfers(n, t, 60000000), "with an update due every 4 packets, every byte arrives", "");
        const proven_u64 rtts = (n->now - started) / 20000 + 2;
        PROVEN_TEST_ASSERT(stats(n, 0).packets_dropped == 0 && stats(n, 1).packets_dropped == 0,
            "on this network that loses nothing, no packet was undecryptable: neither side moved two keys ahead of the other", "");
        PROVEN_TEST_ASSERT(stats(n, 0).key_updates <= rtts && stats(n, 1).key_updates <= rtts,
            "and no more updates than round trips: none before a packet under the current key was acknowledged (RFC 9001 section 6.5)", "");
        free(t);
        net_free(n);
    }

    PROVEN_TEST_SECTION("the path's datagram size", "PMTU probing (RFC-0018 S-2): 1,452 reached; a path that carries no more than 1,280 found without a stall and without a congestion signal; a path that stops carrying the size found falls back and finds it again; quiet once found.", "Inspect pm_next, pm_passed, pm_lost and the probe in proven_quic_conn_send, and the fallbacks in detect_lost and on_loss_timeout.");
    {
        fresh(n, 160);
        run_for(n, 2000000);
        proven_quic_stats_t a0 = stats(n, 0), a1 = stats(n, 1);
        printf("  datagram size %zu and %zu after %llu and %llu probes\n", a0.datagram_size, a1.datagram_size, (unsigned long long)a0.pmtu_probes, (unsigned long long)a1.pmtu_probes);
        PROVEN_TEST_ASSERT(a0.datagram_size == 1452 && a1.datagram_size == 1452, "both sides reach 1,452 on a path that carries it", "");
        PROVEN_TEST_ASSERT(a0.pmtu_probes == 1 && a1.pmtu_probes == 1 && a0.pmtu_probes_lost == 0 && a1.pmtu_probes_lost == 0, "with one probe each, none lost", "");
        const proven_u64 before = n->sent[0] + n->sent[1];
        run_for(n, 5000000);
        PROVEN_TEST_ASSERT(n->sent[0] + n->sent[1] == before, "and once it is found, an idle connection sends nothing", "");
        xfers_t *t = calloc(1, sizeof *t);
        xfer_open(n, t, 0, true, 1 << 20);
        PROVEN_TEST_ASSERT(run_xfers(n, t, 30000000), "a megabyte at the size found", "");
        /* The path stops carrying more than 1,280 bytes: full-sized packets vanish, nothing is
         * acknowledged, and after two probe timeouts the size goes back to 1,200 and is found
         * again. */
        n->hook = hook_mtu_1280; g_hook_count = 0;
        t->n = 0;
        xfer_open(n, t, 0, true, 1 << 20);
        PROVEN_TEST_ASSERT(run_xfers(n, t, 60000000), "when the path stops carrying the size, the next megabyte still arrives", "");
        run_for(n, 3000000);
        a0 = stats(n, 0);
        printf("  after the path narrowed: %zu, %llu datagrams dropped by it\n", a0.datagram_size, (unsigned long long)g_hook_count);
        PROVEN_TEST_ASSERT(a0.datagram_size >= 1200 && a0.datagram_size <= 1280, "the sender fell back and found a size the path carries", "");
        free(t);
        net_free(n);

        /* A path that never carried more than 1,280: found by the search, every probe's loss
         * outside congestion control. */
        net_init(n, 161);
        n->hook = hook_mtu_1280; g_hook_count = 0;
        net_start(n);
        PROVEN_TEST_ASSERT(run_handshake(n, 10000000), "the handshake completes (its datagrams are 1,200 bytes)", "");
        const proven_u64 cwnd0 = stats(n, 0).cwnd;
        run_for(n, 10000000);
        a0 = stats(n, 0); a1 = stats(n, 1);
        printf("  narrow path: %zu and %zu, %llu probes lost of %llu, cwnd %llu (after the handshake %llu)\n", a0.datagram_size, a1.datagram_size,
               (unsigned long long)a0.pmtu_probes_lost, (unsigned long long)a0.pmtu_probes, (unsigned long long)a0.cwnd, (unsigned long long)cwnd0);
        PROVEN_TEST_ASSERT(a0.datagram_size >= 1252 && a0.datagram_size <= 1280 && a1.datagram_size >= 1252 && a1.datagram_size <= 1280,
            "each side settles within 32 bytes below 1,280", "");
        PROVEN_TEST_ASSERT(a0.pmtu_probes_lost >= 3 && a0.packets_lost == 0 && a0.cwnd >= cwnd0, "its lost probes were no packet loss and left the window alone", "");
        t = calloc(1, sizeof *t);
        xfer_open(n, t, 0, true, 1 << 20);
        xfer_open(n, t, 1, true, 1 << 20);
        PROVEN_TEST_ASSERT(run_xfers(n, t, 30000000), "a megabyte each way over the narrow path", "");
        PROVEN_TEST_ASSERT(stats(n, 0).packets_lost == 0 && stats(n, 1).packets_lost == 0, "with no packet lost: nothing was sent above what the path carries", "");
        free(t);
        net_free(n);

        /* Thirty seconds in which nothing reaches the server: nothing is acknowledged, no loss
         * can be detected, and only the probe timeouts tell. The size goes back; the probes lost
         * meanwhile say nothing of the size (long enough to fail every size down to 1,200 if
         * they counted), and it is found again once the path is back. */
        net_init(n, 162);
        n->cfg[0].idle_timeout_ms = n->cfg[1].idle_timeout_ms = 120000;
        net_start(n);
        PROVEN_TEST_ASSERT(run_handshake(n, 10000000), "the handshake completes", "");
        run_for(n, 2000000);
        PROVEN_TEST_ASSERT(stats(n, 0).datagram_size == 1452, "1,452 found", "");
        t = calloc(1, sizeof *t);
        xfer_open(n, t, 0, true, 4 << 20);
        for (int k = 0; k < 20; ++k) { pump(n, t); net_step(n); }
        n->hook = hook_blackout_to_server;
        const proven_u64 dark = n->now + 30000000;
        while (n->now < dark) { pump(n, t); net_step(n); }
        n->hook = NULL;
        PROVEN_TEST_ASSERT(run_xfers(n, t, 60000000), "after the blackout the transfer completes", "");
        run_for(n, 10000000);
        printf("  after a blackout: %llu probes, %llu lost, size %zu\n", (unsigned long long)stats(n, 0).pmtu_probes, (unsigned long long)stats(n, 0).pmtu_probes_lost, stats(n, 0).datagram_size);
        PROVEN_TEST_ASSERT(stats(n, 0).pmtu_probes >= 2 && stats(n, 0).datagram_size == 1452,
            "the probe timeouts sent the size back, and it was found again once the path came back: probes lost in the outage did not count", "");
        free(t);
        net_free(n);

        /* The path toward the server narrows while the client writes 3,000 bytes a turn: two
         * full-sized packets and a small one each time. The small ones are acknowledged; the
         * larger ones' losses - or the probe timeouts that follow once the holes they leave hold
         * the sender at its flow-control window - send the size back. */
        fresh(n, 163);
        run_for(n, 2000000);
        PROVEN_TEST_ASSERT(stats(n, 0).datagram_size == 1452, "1,452 found", "");
        n->hook = hook_mtu_1280_to_server;
        t = calloc(1, sizeof *t);
        xfer_t *x = xfer_open(n, t, 0, false, 600000);
        x->fin_written = true;                         /* written here, a turn at a time */
        proven_u64 put = 0;
        const proven_u64 slow_end = n->now + 60000000;
        static proven_byte_t piece[3000];
        while (n->now < slow_end && !xfers_done(t)) {
            if (put < x->total) {
                const proven_size_t len = (proven_size_t)(x->total - put < sizeof piece ? x->total - put : sizeof piece);
                for (proven_size_t k = 0; k < len; ++k) piece[k] = pattern(x->id, put + k);
                proven_size_t taken = 0;
                if (proven_quic_stream_write(n->ep[0], x->id, (proven_mem_view_t){ piece, len }, put + len == x->total, &taken) == PROVEN_OK) put += taken;
            }
            pump(n, t);
            net_step(n);
        }
        printf("  narrowed toward the server, small packets passing: client %zu, server %zu, client pto %llu\n", stats(n, 0).datagram_size, stats(n, 1).datagram_size, (unsigned long long)stats(n, 0).pto_count);
        PROVEN_TEST_ASSERT(xfers_done(t), "the stream arrives whole although only its small packets passed at first", "");
        PROVEN_TEST_ASSERT(stats(n, 0).datagram_size <= 1280 && stats(n, 1).datagram_size == 1452, "the client fell back to what the path carries; the server kept its size", "");
        free(t);
        net_free(n);
    }

    PROVEN_TEST_SECTION("windows that grow", "RFC-0019 section 9: with growth on, a stream read as fast as it arrives gets its window doubled while it is emptied within two round trips, and a transfer over a 20 ms round trip finishes sooner than with fixed windows; a reader that reads nothing holds no more than with fixed windows.", "Inspect rx_grow, rx_credit and tx_room.");
    {
        proven_u64 took[2] = { 0, 0 };
        for (int grow = 0; grow < 2; ++grow) {
            net_init(n, 170);
            for (int i = 0; i < 2; ++i) if (grow) { n->cfg[i].stream_window_max = 16u << 20; n->cfg[i].conn_window_max = 24u << 20; }
            net_start(n);
            PROVEN_TEST_ASSERT(run_handshake(n, 10000000), "the handshake completes", "");
            xfers_t *t = calloc(1, sizeof *t);
            xfer_open(n, t, 1, false, 8u << 20);
            const proven_u64 started = n->now;
            PROVEN_TEST_ASSERT(run_xfers(n, t, 120000000), "8 MiB arrive", "");
            took[grow] = n->now - started;
            free(t);
            net_free(n);
        }
        printf("  8 MiB over a 20 ms round trip: %.2f s with fixed windows, %.2f s with windows that grow\n", (double)took[0] / 1e6, (double)took[1] / 1e6);
        PROVEN_TEST_ASSERT(took[1] * 10 < took[0] * 4, "growing windows - the send buffer following the grant - take less than four tenths of the time", "");
        /* A reader that reads nothing, growth on: the window never moved, so nothing grew. */
        net_init(n, 171);
        for (int i = 0; i < 2; ++i) { n->cfg[i].stream_window_max = 16u << 20; n->cfg[i].conn_window_max = 24u << 20; }
        net_start(n);
        PROVEN_TEST_ASSERT(run_handshake(n, 10000000), "the handshake completes", "");
        n->reading_off[1] = true;
        xfers_t *t = calloc(1, sizeof *t);
        for (int k = 0; k < 6; ++k) xfer_open(n, t, 0, true, 2 << 20);
        for (int k = 0; k < 400; ++k) { pump(n, t); net_step(n); }
        proven_quic_stats_t r = stats(n, 1), w = stats(n, 0);
        printf("  growth on, reader stopped: receiver holds %zu bytes, sender %zu\n", r.memory, w.memory);
        PROVEN_TEST_ASSERT(r.memory < (1u << 20) + (1u << 19) && w.memory < (1u << 20) + (1u << 19), "a reader that reads nothing holds what it held with fixed windows: growth needs reading", "");
        n->reading_off[1] = false;
        PROVEN_TEST_ASSERT(run_xfers(n, t, 120000000), "once read, everything arrives", "");
        free(t);
        net_free(n);
        /* A peer that grants a 16 MiB stream window and 24 MiB for the connection from the start
         * and then acknowledges nothing: the sender, growth on, must not buffer what was granted
         * - its buffer follows twice the congestion window, which stays small. */
        net_init(n, 172);
        n->cfg[0].stream_window_max = 16u << 20; n->cfg[0].conn_window_max = 24u << 20;
        n->cfg[1].stream_window = 16u << 20; n->cfg[1].conn_window = 24u << 20;
        net_start(n);
        PROVEN_TEST_ASSERT(run_handshake(n, 10000000), "the handshake completes", "");
        n->hook = hook_blackout_to_client;
        t = calloc(1, sizeof *t);
        for (int k = 0; k < 6; ++k) xfer_open(n, t, 0, true, 4 << 20);
        for (int k = 0; k < 300; ++k) { pump(n, t); net_step(n); }
        w = stats(n, 0);
        printf("  a 24 MiB grant never acknowledged: the sender holds %zu bytes\n", w.memory);
        PROVEN_TEST_ASSERT(w.memory < (1u << 20) + (1u << 19), "the sender holds no more than its configured buffers: a grant alone does not grow them", "");
        n->hook = NULL;
        free(t);
        net_free(n);
    }

    PROVEN_TEST_SECTION("flow control and memory", "A reader that reads nothing: the sender stops at the windows, and the receiver holds no more than its connection window.", "");
    {
        fresh(n, 150);
        n->reading_off[1] = true;
        xfers_t *t = calloc(1, sizeof *t);
        for (int k = 0; k < 6; ++k) xfer_open(n, t, 0, true, 2 << 20);
        for (int k = 0; k < 400; ++k) { pump(n, t); net_step(n); }
        proven_quic_stats_t r = stats(n, 1), w = stats(n, 0);
        printf("  receiver holds %zu bytes, sender %zu\n", r.memory, w.memory);
        PROVEN_TEST_ASSERT(r.memory < (1u << 20) + (1u << 19), "the receiver: its 1 MiB window and its bookkeeping", "");
        PROVEN_TEST_ASSERT(w.memory < (1u << 20) + (1u << 19), "the sender: its 1 MiB send buffer and its bookkeeping", "");
        n->reading_off[1] = false;
        PROVEN_TEST_ASSERT(run_xfers(n, t, 120000000), "once read, everything arrives", "");
        free(t);
        net_free(n);
        /* Streams the reader never read, reset by the sender: their bytes come back to the connection's window. */
        fresh(n, 151);
        n->reading_off[1] = true;
        t = calloc(1, sizeof *t);
        for (int k = 0; k < 5; ++k) xfer_open(n, t, 0, true, 250000);
        const proven_u64 fill = n->now + 3000000;     /* long enough to fill the 1 MiB window */
        /* The clock in steps no longer than that: once the window is full nothing is sent - an ACK is
         * not answered - and the next deadline is the idle timeout's, thirty seconds on. */
        n->until = fill;
        while (n->now < fill) { pump(n, t); net_step(n); }
        n->until = 0;
        PROVEN_TEST_ASSERT(stats(n, 1).memory > (1u << 20), "the receiver holds a full window it has not read", "");
        for (int k = 0; k < 5; ++k) PROVEN_TEST_ASSERT(proven_quic_stream_reset(n->ep[0], t->x[k].id, 9) == PROVEN_OK, "the sender resets", "");
        n->reading_off[1] = false;
        t->n = 0;
        xfer_open(n, t, 0, true, 2 << 20);
        PROVEN_TEST_ASSERT(run_xfers(n, t, 60000000), "a new stream's 2 MiB arrive: the reset streams' unread bytes were given back", "");
        free(t);
        net_free(n);
    }

    PROVEN_TEST_SECTION("a large fill remembered, then forgotten", "One stream: a burst that fills its send buffer, then small writes, each acknowledged before the next. The first small write gets the burst's capacity in one allocation (RFC-0018 S-1); each later one an eighth less, until it is small enough to be kept.", "Inspect qbuf_drop and ring_drop: `last` is set from `peak` and decays by an eighth at each emptying.");
    {
        net_init(n, 160);
        n->cfg[0].alloc = (proven_allocator_t){ .ctx = NULL, .alloc_fn = rec_alloc, .realloc_fn = rec_realloc, .free_fn = rec_free };
        net_start(n);
        PROVEN_TEST_ASSERT(run_handshake(n, 10000000), "the handshake completes", "");
        xfers_t *t = calloc(1, sizeof *t);
        xfer_t *x = xfer_open(n, t, 0, false, ~0ull);
        x->fin_written = true;                         /* pump only reads it; the writes are below */
        proven_u64 off = 0;
        for (int r = 0; r < 40; ++r) { pump(n, t); run_for(n, 5000); }
        g_rec_biggest = 0;
        write_pattern(n, x, &off, 1u << 20);
        const proven_size_t burst = g_rec_biggest;
        drain_stream(n, t, x, off);
        PROVEN_TEST_ASSERT(x->read == off && !x->bad, "the burst arrives in order", "");
        proven_size_t got[48];
        for (int k = 0; k < 48; ++k) {
            g_rec_biggest = 0;
            write_pattern(n, x, &off, 2048);
            got[k] = g_rec_biggest;                    /* the send buffer's allocation, 0 if it was kept */
            drain_stream(n, t, x, off);
        }
        printf("  burst: one buffer of %zu bytes; small writes then allocated %zu, %zu, %zu ... %zu, %zu\n", burst, got[0], got[1], got[2], got[46], got[47]);
        PROVEN_TEST_ASSERT(x->read == off && !x->bad, "every small write arrives in order", "");
        PROVEN_TEST_ASSERT(burst >= 65536 && got[0] == burst, "the first small write gets the burst's capacity at once", "");
        bool eighths = true; int kept_from = -1;
        for (int k = 1; k < 48; ++k) {
            if (got[k] == 0) { if (kept_from < 0) kept_from = k; continue; }
            if (kept_from >= 0 || got[k] != got[k - 1] - got[k - 1] / 8) eighths = false;
        }
        PROVEN_TEST_ASSERT(eighths, "each emptying forgets exactly an eighth", "");
        PROVEN_TEST_ASSERT(kept_from > 0 && got[kept_from - 1] <= 8192, "once small, the buffer is kept and nothing more is allocated", "");
        free(t);
        net_free(n);
        /* The receiving side's buffer, the same way: a reader that stops lets a burst fill it,
         * then reads it all; small writes after, each read before the next. */
        net_init(n, 161);
        n->cfg[1].alloc = (proven_allocator_t){ .ctx = NULL, .alloc_fn = rec_alloc, .realloc_fn = rec_realloc, .free_fn = rec_free };
        net_start(n);
        PROVEN_TEST_ASSERT(run_handshake(n, 10000000), "the handshake completes", "");
        t = calloc(1, sizeof *t);
        x = xfer_open(n, t, 0, false, ~0ull);
        x->fin_written = true;
        off = 0;
        for (int r = 0; r < 40; ++r) { pump(n, t); run_for(n, 5000); }
        n->reading_off[1] = true;
        g_rec_biggest = 0;
        write_pattern(n, x, &off, 256u << 10);
        for (int r = 0; r < 100; ++r) { pump(n, t); run_for(n, 5000); }     /* half a second: the window fills */
        const proven_size_t rburst = g_rec_biggest;
        n->reading_off[1] = false;
        /* The events that came while it read nothing were passed over: read once by hand. */
        for (;;) {
            static proven_byte_t buf[65536];
            proven_size_t got_now = 0; bool fin = false;
            if (proven_quic_stream_read(n->ep[1], x->id, (proven_mem_mut_t){ buf, sizeof buf }, &got_now, &fin) != PROVEN_OK || got_now == 0) break;
            for (proven_size_t i = 0; i < got_now; ++i) if (buf[i] != pattern(x->id, x->read + i)) x->bad = true;
            x->read += got_now;
        }
        drain_stream(n, t, x, off);
        PROVEN_TEST_ASSERT(x->read == off && !x->bad, "the burst is read in order", "");
        for (int k = 0; k < 48; ++k) {
            write_pattern(n, x, &off, 2048);
            g_rec_biggest = 0;
            drain_stream(n, t, x, off);
            got[k] = g_rec_biggest;
        }
        printf("  receiving: one buffer of %zu bytes; small writes then allocated %zu, %zu, %zu ... %zu, %zu\n", rburst, got[0], got[1], got[2], got[46], got[47]);
        PROVEN_TEST_ASSERT(x->read == off && !x->bad, "every small write is read in order", "");
        PROVEN_TEST_ASSERT(rburst >= 65536 && got[0] == rburst, "the first small write is received into the burst's capacity at once", "");
        eighths = true; kept_from = -1;
        for (int k = 1; k < 48; ++k) {
            if (got[k] == 0) { if (kept_from < 0) kept_from = k; continue; }
            if (kept_from >= 0 || got[k] != got[k - 1] - got[k - 1] / 8) eighths = false;
        }
        PROVEN_TEST_ASSERT(eighths && kept_from > 0 && got[kept_from - 1] <= 8192, "each emptying forgets an eighth, until the buffer is small enough to keep", "");
        free(t);
        net_free(n);
    }

    PROVEN_TEST_SECTION("changed packets", "Every captured datagram of a handshake and a transfer, with a bit changed or cut short, delivered again: dropped, and the connection carries on.", "");
    {
        net_init(n, 160);
        n->capture = true;
        net_start(n);
        PROVEN_TEST_ASSERT(run_handshake(n, 10000000), "", "");
        xfers_t *t = calloc(1, sizeof *t);
        xfer_open(n, t, 0, true, 20000);
        PROVEN_TEST_ASSERT(run_xfers(n, t, 10000000), "", "");
        n->capture = false;
        proven_size_t tried = 0;
        static proven_byte_t b[1500];
        for (proven_size_t i = 0; i < n->capn; ++i) {
            const dg_t *d = &n->cap[i];
            proven_quic_conn_t *to = n->ep[d->to];
            for (proven_size_t bit = 0; bit < d->n * 8; bit += (bit < 64 * 8 ? 1 : 29)) {
                memcpy(b, d->b, d->n);
                b[bit / 8] ^= (proven_byte_t)(1u << (bit % 8));
                PROVEN_TEST_ASSERT(proven_quic_conn_receive(to, (proven_mem_view_t){ b, d->n }, &d->from, n->now) == PROVEN_OK, "", "");
                ++tried;
            }
            for (proven_size_t cut = 0; cut < d->n; cut += (cut < 64 ? 1 : 17)) {
                PROVEN_TEST_ASSERT(proven_quic_conn_receive(to, (proven_mem_view_t){ d->b, cut }, &d->from, n->now) == PROVEN_OK, "", "");
                ++tried;
            }
        }
        printf("  %zu changed or cut datagrams from %zu captured\n", tried, n->capn);
        /* The same datagrams again, unchanged: every packet in them is one already processed. */
        const proven_u64 before0 = stats(n, 0).packets_received, before1 = stats(n, 1).packets_received;
        for (proven_size_t i = 0; i < n->capn; ++i)
            PROVEN_TEST_ASSERT(proven_quic_conn_receive(n->ep[n->cap[i].to], (proven_mem_view_t){ n->cap[i].b, n->cap[i].n }, &n->cap[i].from, n->now) == PROVEN_OK, "", "");
        PROVEN_TEST_ASSERT(stats(n, 0).packets_received == before0 && stats(n, 1).packets_received == before1,
            "a packet number already processed is not processed again (RFC 9000 section 12.3)", "");
        PROVEN_TEST_ASSERT(!stats(n, 0).closed && !stats(n, 1).closed, "neither side closed", "");
        proven_quic_event_t ev;
        PROVEN_TEST_ASSERT(!closed_event(n->ep[0], &ev) && !closed_event(n->ep[1], &ev), "nor was told to", "");
        t->n = 0;
        xfer_open(n, t, 1, true, 50000);
        PROVEN_TEST_ASSERT(run_xfers(n, t, 10000000), "and a new transfer goes through", "");
        free(t);
        net_free(n);
    }

    free(n);
    proven_tls_config_destroy(g_tls_client);
    proven_tls_config_destroy(g_tls_server);
    proven_cert_store_destroy(g_anchors);
    PROVEN_TEST_PASS("QUIC connections hold over a lossy network.");
    return 0;
}

#else

int main(void) {
    PROVEN_TEST_SUITE("QUIC connections", "Built with PROVEN_NO_HTTP3: nothing to test.", "");
    PROVEN_TEST_PASS("skipped: PROVEN_NO_HTTP3");
    return 0;
}

#endif
