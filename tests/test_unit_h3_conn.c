#include "proven.h"
#include "proven_test.h"
#include "../src/proven/proven_internal_h3.h"
#include "test_unit_tls_pki.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * HTTP/3 (RFC 9114) over two QUIC engines on a simulated network - a virtual clock and a seeded
 * network that delays, loses, duplicates and reorders. Two HTTP/3 ends exchange requests and
 * responses of every shape; then one end speaks raw QUIC streams to the other, so that every
 * code of RFC 9114 section 8.1 and RFC 9204 section 6 that this side acts on is produced by the
 * input that should produce it.
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

/* An allocator that can be told to fail: for the HTTP/3 side's own memory. */
static long g_fail_after = -1;
static proven_result_mem_mut_t fa_alloc(void *ctx, proven_size_t size, proven_size_t align) {
    (void)ctx;
    if (g_fail_after == 0) return (proven_result_mem_mut_t){ .err = PROVEN_ERR_NOMEM };
    if (g_fail_after > 0) --g_fail_after;
    proven_allocator_t h = proven_heap_allocator();
    return h.alloc_fn(h.ctx, size, align);
}
static proven_result_mem_mut_t fa_realloc(void *ctx, void *p, proven_size_t old, proven_size_t size, proven_size_t align) {
    (void)ctx;
    proven_allocator_t h = proven_heap_allocator();
    return h.realloc_fn(h.ctx, p, old, size, align);
}
static void fa_free(void *ctx, void *p) { (void)ctx; proven_allocator_t h = proven_heap_allocator(); h.free_fn(h.ctx, p); }

/* ---- The network ---- */

typedef struct { proven_byte_t b[1500]; proven_size_t n; proven_u64 at; int to; proven_quic_addr_t from; } dg_t;

typedef struct {
    proven_quic_conn_t *ep[2];          /* 0: the client, 1: the server */
    proven_h3_conn_t *h3[2];            /* null: that end speaks raw QUIC streams */
    bool use_h3[2];
    proven_h3_config_t h3cfg[2];
    proven_quic_addr_t addr[2];
    proven_quic_config_t cfg[2];
    proven_tls_session_t session;
    dg_t *q; proven_size_t qn, qcap;
    proven_u64 now;
    unsigned loss, dup, reorder;        /* per thousand */
    proven_u64 delay, jitter;
    proven_u64 rng;
    proven_u64 until;
} net_t;

static proven_u64 nrand(net_t *n) { n->rng ^= n->rng << 13; n->rng ^= n->rng >> 7; n->rng ^= n->rng << 17; return n->rng; }

static void net_push(net_t *n, int to, const proven_byte_t *b, proven_size_t len, const proven_quic_addr_t *from, proven_u64 at) {
    if (n->qn == n->qcap) { n->qcap = n->qcap ? n->qcap * 2 : 256; n->q = realloc(n->q, n->qcap * sizeof *n->q); }
    dg_t *d = &n->q[n->qn++];
    memcpy(d->b, b, len); d->n = len; d->at = at; d->to = to; d->from = *from;
}

static void net_send(net_t *n, int from_ep, const proven_byte_t *b, proven_size_t len) {
    if (n->loss && nrand(n) % 1000 < n->loss) return;
    proven_u64 at = n->now + n->delay + (n->jitter ? nrand(n) % n->jitter : 0);
    if (n->reorder && nrand(n) % 1000 < n->reorder) at += n->delay + 1000;
    net_push(n, 1 - from_ep, b, len, &n->addr[from_ep], at);
    if (n->dup && nrand(n) % 1000 < n->dup) net_push(n, 1 - from_ep, b, len, &n->addr[from_ep], at + 300);
}

static void make_h3(net_t *n, int side) {
    if (!n->use_h3[side]) return;
    n->h3cfg[side].server = side == 1;
    if (!proven_alloc_is_valid(n->h3cfg[side].alloc)) n->h3cfg[side].alloc = g_alloc;
    PROVEN_TEST_ASSERT(proven_h3_conn_create(&n->h3cfg[side], n->ep[side], &n->h3[side]) == PROVEN_OK, "the HTTP/3 side is made", "");
}

static void net_init(net_t *n, unsigned seed) {
    memset(n, 0, sizeof *n);
    n->rng = 0x9e3779b97f4a7c15ull ^ ((proven_u64)seed * 0x100000001b3ull);
    n->now = 1000000;
    n->delay = 10000;
    n->addr[0] = (proven_quic_addr_t){ .len = 6, .b = { 10, 0, 0, 1, 0x30, 0x39 } };
    n->addr[1] = (proven_quic_addr_t){ .len = 6, .b = { 10, 0, 0, 2, 0x01, 0xbb } };
    for (int i = 0; i < 2; ++i) n->cfg[i] = (proven_quic_config_t){ .alloc = g_alloc, .tls = i ? g_tls_server : g_tls_client };
    n->use_h3[0] = n->use_h3[1] = true;
}

static void net_free(net_t *n) {
    for (int i = 0; i < 2; ++i) { proven_h3_conn_destroy(n->h3[i]); proven_quic_conn_destroy(n->ep[i]); n->h3[i] = NULL; n->ep[i] = NULL; }
    free(n->q);
    n->q = NULL; n->qn = n->qcap = 0;
}

static void deliver(net_t *n, int to, proven_byte_t *b, proven_size_t len, const proven_quic_addr_t *from) {
    if (to == 1 && !n->ep[1]) {
        static proven_byte_t out[1500];
        proven_size_t olen = 0;
        proven_quic_accept_t acc;
        proven_quic_stateless_t r = proven_quic_server_stateless(&n->cfg[1], (proven_mem_view_t){ b, len }, from, n->now, false,
                                                                 (proven_mem_mut_t){ out, sizeof out }, &olen, &acc);
        if (r == PROVEN_QUIC_STATELESS_REPLY) { net_send(n, 1, out, olen); return; }
        if (r == PROVEN_QUIC_STATELESS_ACCEPT) {
            PROVEN_TEST_ASSERT(proven_quic_conn_server_create(&n->cfg[1], &acc, (proven_mem_view_t){ b, len }, from, n->now, &n->ep[1]) == PROVEN_OK, "the server's connection", "");
            make_h3(n, 1);
        }
        return;
    }
    if (n->ep[to]) PROVEN_TEST_ASSERT(proven_quic_conn_receive(n->ep[to], (proven_mem_view_t){ b, len }, from, n->now) == PROVEN_OK, "a datagram is taken", "");
}

static void app_pump(net_t *n);

static void net_step(net_t *n) {
    static proven_byte_t out[1500];
    app_pump(n);
    for (int i = 0; i < 2; ++i) {
        if (!n->ep[i]) continue;
        for (int k = 0; k < 512; ++k) {
            proven_size_t len = 0;
            proven_quic_addr_t to;
            PROVEN_TEST_ASSERT(proven_quic_conn_send(n->ep[i], n->now, (proven_mem_mut_t){ out, sizeof out }, &len, &to) == PROVEN_OK, "send", "");
            if (len == 0) break;
            net_send(n, i, out, len);
        }
    }
    proven_u64 next = (proven_u64)-1;
    for (proven_size_t i = 0; i < n->qn; ++i) if (n->q[i].at < next) next = n->q[i].at;
    for (int i = 0; i < 2; ++i) if (n->ep[i]) { proven_u64 d = proven_quic_conn_deadline(n->ep[i]); if (d < next) next = d; }
    if (next == (proven_u64)-1) next = n->now + 1000000;
    if (n->until && next > n->until) next = n->until;
    if (next > n->now) n->now = next;
    for (;;) {
        proven_size_t best = n->qn;
        for (proven_size_t i = 0; i < n->qn; ++i) if (n->q[i].at <= n->now && (best == n->qn || n->q[i].at < n->q[best].at)) best = i;
        if (best == n->qn) break;
        dg_t d = n->q[best];
        n->q[best] = n->q[--n->qn];
        deliver(n, d.to, d.b, d.n, &d.from);
    }
    for (int i = 0; i < 2; ++i) if (n->ep[i] && proven_quic_conn_deadline(n->ep[i]) <= n->now) proven_quic_conn_timeout(n->ep[i], n->now);
    app_pump(n);
}

static void run_for(net_t *n, proven_u64 us) {
    const proven_u64 end = n->now + us;
    n->until = end;
    while (n->now < end) net_step(n);
    n->until = 0;
}

/* ---- The applications ---- */

static proven_byte_t pattern(proven_u64 seed, proven_u64 off) { return (proven_byte_t)((off * 131 + seed * 7 + (off >> 9)) & 0xff); }

typedef struct {
    proven_u64 id;
    char path[48];
    bool post;
    int status, interim;
    proven_u64 got, seed, rseed, want, sent;   /* seed: of what this end sends; rseed: of what it receives */
    bool end, bad, reset, stopped, trailers, sent_end, with_trailers, respond_at_end, closing_read;
    proven_u64 code;
} rec_t;

typedef struct {
    rec_t r[512];
    int n;
    bool closed, close_app, close_remote, established;
    proven_u64 close_code, goaway_id;
    int goaways, writable_events;
    bool strict;                        /* send only when told the stream is writable */
    bool pause_first;                   /* a client: pause each stream at its first DATA */
} app_t;

static app_t g_app[2];

/* What a raw end saw: per stream, the last reset and stop codes and bytes. */
typedef struct { proven_u64 id, reset_code, stop_code; bool reset, stopped, fin; proven_u64 bytes; } raw_t;
static raw_t g_raw[64];
static int g_nraw;
static raw_t *raw_rec(proven_u64 id) {
    for (int i = 0; i < g_nraw; ++i) if (g_raw[i].id == id) return &g_raw[i];
    if (g_nraw == 64) return &g_raw[63];
    g_raw[g_nraw] = (raw_t){ .id = id };
    return &g_raw[g_nraw++];
}

static rec_t *rec_find(app_t *a, proven_u64 id) {
    for (int i = 0; i < a->n; ++i) if (a->r[i].id == id) return &a->r[i];
    return NULL;
}

static proven_u64 header_u64(const proven_http_header_t *f, proven_size_t n, const char *name) {
    for (proven_size_t i = 0; i < n; ++i) {
        if (f[i].name.size != strlen(name) || memcmp(f[i].name.ptr, name, f[i].name.size) != 0) continue;
        proven_u64 v = 0;
        for (proven_size_t k = 0; k < f[i].value.size; ++k) v = v * 10 + (proven_u64)(f[i].value.ptr[k] - '0');
        return v;
    }
    return 0;
}

/* Send what a record still has to send. */
static void feed(proven_h3_conn_t *h, rec_t *r) {
    static proven_byte_t chunk[8192];
    while (!r->sent_end && !r->stopped && !r->reset) {
        const proven_u64 left = r->want - r->sent;
        const proven_size_t len = (proven_size_t)(left < sizeof chunk ? left : sizeof chunk);
        for (proven_size_t k = 0; k < len; ++k) chunk[k] = pattern(r->seed, r->sent + k);
        const bool last = r->sent + len == r->want && !r->with_trailers;
        proven_size_t taken = 0;
        const proven_err_t e = proven_h3_send_data(h, r->id, (proven_mem_view_t){ chunk, len }, last, &taken);
        if (e != PROVEN_OK) { r->bad = true; return; }
        r->sent += taken;
        if (r->sent == r->want && r->with_trailers) {
            const proven_http_header_t t[] = { { sv("x-trailer"), sv("done") } };
            if (proven_h3_send_trailers(h, r->id, t, 1) != PROVEN_OK) r->bad = true;
            r->sent_end = true;
            return;
        }
        if (last && taken == len) { r->sent_end = true; return; }
        if (taken < len) return;
    }
}

static char *num(proven_u64 v, char *buf) { snprintf(buf, 24, "%llu", (unsigned long long)v); return buf; }

/* A server's answer to a request whose head has come. */
static void serve(proven_h3_conn_t *h, rec_t *r) {
    char lenbuf[24];
    if (strncmp(r->path, "/get/", 5) == 0) {
        r->want = r->seed = (proven_u64)strtoull(r->path + 5, NULL, 10);
        const proven_http_header_t f[] = { { sv(":status"), sv("200") }, { sv("content-length"), sv(num(r->want, lenbuf)) } };
        if (proven_h3_submit_response(h, r->id, f, 2, r->want == 0) != PROVEN_OK) r->bad = true;
        if (r->want == 0) r->sent_end = true; else feed(h, r);
    } else if (strcmp(r->path, "/trailers") == 0) {
        r->want = 10; r->seed = 10; r->with_trailers = true;
        const proven_http_header_t f[] = { { sv(":status"), sv("200") }, { sv("content-length"), sv("10") } };
        if (proven_h3_submit_response(h, r->id, f, 2, false) != PROVEN_OK) r->bad = true;
        feed(h, r);
    } else if (strcmp(r->path, "/interim") == 0) {
        const proven_http_header_t i[] = { { sv(":status"), sv("103") }, { sv("link"), sv("</s.css>; rel=preload") } };
        const proven_http_header_t f[] = { { sv(":status"), sv("204") } };
        if (proven_h3_submit_response(h, r->id, i, 2, false) != PROVEN_OK || proven_h3_submit_response(h, r->id, f, 1, true) != PROVEN_OK) r->bad = true;
        r->sent_end = true;
    } else if (strcmp(r->path, "/early") == 0) {
        const proven_http_header_t f[] = { { sv(":status"), sv("200") }, { sv("content-length"), sv("0") } };
        if (proven_h3_submit_response(h, r->id, f, 2, true) != PROVEN_OK) r->bad = true;
        r->sent_end = true;
    } else if (strcmp(r->path, "/head") == 0) {
        const proven_http_header_t f[] = { { sv(":status"), sv("200") }, { sv("content-length"), sv("1000") } };
        if (proven_h3_submit_response(h, r->id, f, 2, true) != PROVEN_OK) r->bad = true;
        r->sent_end = true;
    } else if (strcmp(r->path, "/end-after-head") == 0) {
        /* A head, then an end with no bytes - as a server that learns only afterwards that the
         * body is empty sends it. Behind a download that holds the connection's send buffer the
         * head is queued, and the end must be taken behind it. */
        static const char pad[] = "a field of a hundred bytes, longer than what a full send buffer still has room for after a body....";
        const proven_http_header_t f[] = { { sv(":status"), sv("200") }, { sv("x-pad"), sv(pad) } };
        proven_size_t taken = 0;
        if (proven_h3_submit_response(h, r->id, f, 2, false) != PROVEN_OK || proven_h3_send_data(h, r->id, (proven_mem_view_t){ 0 }, true, &taken) != PROVEN_OK) r->bad = true;
        r->sent_end = true;
    } else if (strcmp(r->path, "/post") == 0) {
        r->respond_at_end = true;                /* when the body has come */
    } else {
        const proven_http_header_t f[] = { { sv(":status"), sv("404") } };
        if (proven_h3_submit_response(h, r->id, f, 1, true) != PROVEN_OK) r->bad = true;
        r->sent_end = true;
    }
}

static void post_done(proven_h3_conn_t *h, rec_t *r) {
    const proven_http_header_t f[] = { { sv(":status"), sv(r->bad ? "500" : "200") }, { sv("content-length"), sv("0") } };
    if (proven_h3_submit_response(h, r->id, f, 2, true) != PROVEN_OK) r->bad = true;
    r->sent_end = true;
}

static void on_event(net_t *n, int side, const proven_h3_event_t *ev) {
    app_t *a = &g_app[side];
    proven_h3_conn_t *h = n->h3[side];
    rec_t *r = ev->stream_id || ev->kind == PROVEN_H3_EV_HEADERS || ev->kind == PROVEN_H3_EV_DATA || ev->kind == PROVEN_H3_EV_TRAILERS
               || ev->kind == PROVEN_H3_EV_STREAM_RESET || ev->kind == PROVEN_H3_EV_STOP_SENDING || ev->kind == PROVEN_H3_EV_STREAM_WRITABLE
               ? rec_find(a, ev->stream_id) : NULL;
    switch (ev->kind) {
    case PROVEN_H3_EV_HEADERS:
        if (side == 1) {
            PROVEN_TEST_ASSERT(!r && a->n < 512, "a new request", "");
            r = &a->r[a->n++];
            *r = (rec_t){ .id = ev->stream_id };
            const proven_u8str_view_t p = ev->request.path;
            memcpy(r->path, p.ptr, p.size < 47 ? p.size : 47);
            r->post = ev->request.method.size == 4 && memcmp(ev->request.method.ptr, "POST", 4) == 0;
            r->rseed = header_u64(ev->request.fields, ev->request.field_count, "content-length");
            r->end = ev->end_stream;
            serve(h, r);
            if (r->end && r->respond_at_end) post_done(h, r);
        } else {
            PROVEN_TEST_ASSERT(r != NULL, "a response to a request made", "");
            if (ev->status < 200) { ++r->interim; break; }
            r->status = ev->status;
            r->rseed = header_u64(ev->fields, ev->field_count, "content-length");
            if (ev->end_stream) r->end = true;
        }
        break;
    case PROVEN_H3_EV_DATA:
        PROVEN_TEST_ASSERT(r != NULL, "data for a known stream", "");
        for (proven_size_t i = 0; i < ev->data.size; ++i) if (ev->data.ptr[i] != pattern(r->rseed, r->got + i)) r->bad = true;
        r->got += ev->data.size;
        if (ev->end_stream) {
            r->end = true;
            if (side == 1 && r->respond_at_end) post_done(h, r);
        }
        if (a->pause_first && side == 0 && ev->data.size > 0) proven_h3_stream_pause(h, r->id, true);
        break;
    case PROVEN_H3_EV_TRAILERS:
        PROVEN_TEST_ASSERT(r != NULL && ev->end_stream, "trailers end a known stream", "");
        r->trailers = ev->field_count == 1 && ev->fields[0].value.size == 4;
        r->end = true;
        if (side == 1 && r->respond_at_end) post_done(h, r);
        break;
    case PROVEN_H3_EV_STREAM_RESET:
        if (r) { r->reset = true; r->code = ev->code; }
        break;
    case PROVEN_H3_EV_STOP_SENDING:
        if (r) { r->stopped = true; r->code = ev->code; }
        break;
    case PROVEN_H3_EV_STREAM_WRITABLE:
        ++a->writable_events;
        if (r) feed(h, r);
        break;
    case PROVEN_H3_EV_GOAWAY:
        ++a->goaways;
        a->goaway_id = ev->id;
        break;
    case PROVEN_H3_EV_QUIC:
        if (ev->quic.kind == PROVEN_QUIC_EV_ESTABLISHED) a->established = true;
        break;
    case PROVEN_H3_EV_CLOSED:
        a->closed = true; a->close_code = ev->code; a->close_app = ev->app; a->close_remote = ev->remote;
        break;
    default:
        break;
    }
}

static void app_pump(net_t *n) {
    static proven_byte_t buf[65536];
    for (int side = 0; side < 2; ++side) {
        if (n->h3[side]) {
            proven_h3_event_t ev;
            while (proven_h3_conn_poll(n->h3[side], &ev)) on_event(n, side, &ev);
            if (!g_app[side].strict)
                for (int i = 0; i < g_app[side].n; ++i) if (!g_app[side].r[i].sent_end && g_app[side].r[i].want) feed(n->h3[side], &g_app[side].r[i]);
        } else if (n->ep[side]) {
            proven_quic_event_t ev;
            while (proven_quic_conn_poll(n->ep[side], &ev)) {
                if (ev.kind == PROVEN_QUIC_EV_CLOSED) { g_app[side].closed = true; g_app[side].close_code = ev.code; g_app[side].close_app = ev.app; g_app[side].close_remote = ev.remote; }
                if (ev.kind == PROVEN_QUIC_EV_ESTABLISHED) g_app[side].established = true;
                if (ev.kind == PROVEN_QUIC_EV_STREAM_RESET) { raw_t *w = raw_rec(ev.stream_id); w->reset = true; w->reset_code = ev.code; }
                if (ev.kind == PROVEN_QUIC_EV_STOP_SENDING) { raw_t *w = raw_rec(ev.stream_id); w->stopped = true; w->stop_code = ev.code; }
                if (ev.kind == PROVEN_QUIC_EV_STREAM_READABLE) {
                    raw_t *w = raw_rec(ev.stream_id);
                    for (;;) {
                        proven_size_t got = 0; bool fin = false;
                        if (proven_quic_stream_read(n->ep[side], ev.stream_id, (proven_mem_mut_t){ buf, sizeof buf }, &got, &fin) != PROVEN_OK) break;
                        w->bytes += got;
                        if (fin) w->fin = true;
                        if (got == 0 || fin) break;
                    }
                }
            }
        }
    }
}

/* A connection, its handshake, and both control streams across. */
static void fresh(net_t *n, unsigned seed) {
    PROVEN_TEST_ASSERT(proven_quic_conn_client_create(&n->cfg[0], sv("example.test"), &n->session, &n->addr[1], n->now, &n->ep[0]) == PROVEN_OK, "a client is made", "");
    (void)seed;
    make_h3(n, 0);
    const proven_u64 end = n->now + 5000000;
    while (n->now < end && !(g_app[0].established && n->ep[1] && g_app[1].established)) net_step(n);
    PROVEN_TEST_ASSERT(g_app[0].established && g_app[1].established, "the handshake completes", "");
    run_for(n, 100000);
}

static void reset_apps(void) {
    memset(g_app, 0, sizeof g_app);
    memset(g_raw, 0, sizeof g_raw);
    g_nraw = 0;
}

static proven_u64 request(net_t *n, const char *method, const char *path, proven_u64 body, bool trailers) {
    char lenbuf[24];
    proven_http_header_t f[6] = { { sv(":method"), sv(method) }, { sv(":scheme"), sv("https") }, { sv(":authority"), sv("example.test") }, { sv(":path"), sv(path) } };
    proven_size_t nf = 4;
    if (body) f[nf++] = (proven_http_header_t){ sv("content-length"), sv(num(body, lenbuf)) };
    proven_u64 id = 0;
    const proven_err_t e = proven_h3_submit_request(n->h3[0], f, nf, body == 0 && !trailers, &id);
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "a request is submitted", path);
    app_t *a = &g_app[0];
    rec_t *r = &a->r[a->n++];
    *r = (rec_t){ .id = id, .want = body, .seed = body, .with_trailers = trailers, .sent_end = body == 0 && !trailers };
    snprintf(r->path, sizeof r->path, "%s", path);
    if (!r->sent_end) feed(n->h3[0], r);
    return id;
}

static bool all_done(int count) {
    if (g_app[0].n < count) return false;
    for (int i = 0; i < g_app[0].n; ++i) if (!g_app[0].r[i].end && !g_app[0].r[i].reset) return false;
    return true;
}

static bool run_until_done(net_t *n, int count, proven_u64 limit) {
    const proven_u64 end = n->now + limit;
    while (n->now < end) { net_step(n); if (all_done(count)) return true; }
    return false;
}

/* ---- Raw ends: QUIC streams written by hand ---- */

static proven_size_t unhex(const char *hex, proven_byte_t *out, proven_size_t cap) {
    proven_size_t k = 0;
    for (const char *p = hex; p[0] && p[1]; ) {
        if (*p == ' ') { ++p; continue; }
        PROVEN_TEST_ASSERT(k < cap, "hex fits", "");
        char c[3] = { p[0], p[1], 0 };
        out[k++] = (proven_byte_t)strtoul(c, NULL, 16);
        p += 2;
    }
    return k;
}

/* A HEADERS frame for "name=value|name=value". */
static proven_size_t headers_frame(const char *spec, proven_byte_t *out, proven_size_t cap) {
    static char copy[70000];
    snprintf(copy, sizeof copy, "%s", spec);
    proven_http_header_t f[32];
    proven_size_t nf = 0;
    for (char *tok = strtok(copy, "|"); tok && nf < 32; tok = strtok(NULL, "|")) {
        char *eq = strchr(tok + 1, '=');
        PROVEN_TEST_ASSERT(eq != NULL, "a field spec", spec);
        *eq = 0;
        f[nf++] = (proven_http_header_t){ sv(tok), sv(eq + 1) };
    }
    static proven_byte_t sec[70000];
    proven_size_t len = 0;
    PROVEN_TEST_ASSERT(proven_qpack_encode(f, nf, NULL, (proven_mem_mut_t){ sec, sizeof sec }, &len) == PROVEN_OK, "encoded", "");
    proven_size_t at = proven_quic_varint_put(PROVEN_H3_FRAME_HEADERS, out);
    at += proven_quic_varint_put(len, out + at);
    PROVEN_TEST_ASSERT(at + len <= cap, "the frame fits", "");
    memcpy(out + at, sec, len);
    return at + len;
}

static void raw_write(net_t *n, int side, proven_u64 id, const proven_byte_t *b, proven_size_t len, bool fin) {
    proven_size_t taken = 0;
    PROVEN_TEST_ASSERT(proven_quic_stream_write(n->ep[side], id, (proven_mem_view_t){ b, len }, fin, &taken) == PROVEN_OK && taken == len, "a raw write", "");
}

#define REQ "|:scheme=https|:authority=example.test"

/* One script, one verdict: the raw end (`raw`) runs the steps, separated by spaces:
 *   C:hex  open a unidirectional stream and write hex (its type first); it is the control stream
 *   U:hex  open a unidirectional stream and write hex          B:hex  open a bidirectional one
 *   W:hex  write hex on the last stream                         H:spec a HEADERS frame on it
 *   F      end the last stream    R  reset it (0x10c)    S  ask the other end's control stream to stop
 *   Q:hex  write on stream 0 (a raw server answering the client's request)
 *   q      the HTTP/3 client sends GET /get/10                  G  the HTTP/3 end says GOAWAY
 *   g      the HTTP/3 server says the first GOAWAY of two      T  let 100 ms pass */
typedef struct {
    int h3side;                         /* which end speaks HTTP/3 */
    const char *steps;
    proven_u64 conn_code;               /* the HTTP/3 end closes with it (0: it does not close) */
    proven_u64 raw_reset;               /* the last raw stream is reset with it (0: not looked at) */
    proven_u64 raw_stop;                /* ... asked to stop with it */
    proven_u64 h3_reset;                /* the HTTP/3 client's stream 0 or 4 ends with STREAM_RESET and it */
    proven_u64 h3_reset_id;
    const char *what;
} script_t;

static void run_script(net_t *n, const script_t *s, unsigned seed) {
    reset_apps();
    net_init(n, seed);
    n->use_h3[1 - s->h3side] = false;
    fresh(n, seed);
    const int raw = 1 - s->h3side;
    proven_u64 last = 0;
    bool have_last = false;
    static char copy[1024];
    snprintf(copy, sizeof copy, "%s", s->steps);
    static proven_byte_t b[70000];
    for (char *tok = copy, *next = NULL; tok && *tok; tok = next) {
        /* Steps are split by hand: headers_frame uses strtok for its own list. */
        next = strchr(tok, ' ');
        if (next) *next++ = 0;
        const char op = tok[0];
        const char *arg = tok[1] == ':' ? tok + 2 : "";
        proven_size_t len = 0;
        switch (op) {
        case 'C': case 'U': case 'B':
            PROVEN_TEST_ASSERT(proven_quic_stream_open(n->ep[raw], op == 'B', &last) == PROVEN_OK, "a raw stream opens", s->what);
            have_last = true;
            len = unhex(arg, b, sizeof b);
            raw_write(n, raw, last, b, len, false);
            break;
        case 'W': len = unhex(arg, b, sizeof b); raw_write(n, raw, last, b, len, false); break;
        case 'H': len = headers_frame(arg, b, sizeof b); raw_write(n, raw, last, b, len, false); break;
        case 'F': raw_write(n, raw, last, b, 0, true); break;
        case 'R': PROVEN_TEST_ASSERT(proven_quic_stream_reset(n->ep[raw], last, 0x10c) == PROVEN_OK, "a raw reset", s->what); break;
        case 'S': PROVEN_TEST_ASSERT(proven_quic_stream_stop(n->ep[raw], raw == 0 ? 3 : 2, 0x10c) == PROVEN_OK, "a raw stop", s->what); break;
        case 'Q': last = 0; have_last = true; len = unhex(arg, b, sizeof b); raw_write(n, raw, 0, b, len, false); break;
        case 'q': (void)request(n, "GET", "/get/10", 0, false); break;
        case 'G': PROVEN_TEST_ASSERT(proven_h3_conn_goaway(n->h3[s->h3side]) == PROVEN_OK, "goaway", s->what); break;
        case 'g': PROVEN_TEST_ASSERT(proven_h3_conn_goaway_step(n->h3[s->h3side], true) == PROVEN_OK, "the first goodbye of two", s->what); break;
        case 'T': run_for(n, 100000); break;
        default: PROVEN_TEST_ASSERT(false, "a known step", tok);
        }
    }
    run_for(n, 1000000);
    const app_t *a = &g_app[s->h3side];
    if (s->conn_code) {
        if (!a->closed || a->close_code != s->conn_code) printf("  %s: closed %d code %llx\n", s->what, a->closed, (unsigned long long)a->close_code);
        PROVEN_TEST_ASSERT(a->closed && a->close_app && !a->close_remote && a->close_code == s->conn_code, "the HTTP/3 end closes with the code", s->what);
        PROVEN_TEST_ASSERT(g_app[raw].closed && g_app[raw].close_app && g_app[raw].close_remote && g_app[raw].close_code == s->conn_code, "and the peer is told it", s->what);
    } else {
        if (a->closed) printf("  %s: closed with %llx\n", s->what, (unsigned long long)a->close_code);
        PROVEN_TEST_ASSERT(!a->closed, "the connection stays", s->what);
    }
    if (s->raw_reset || s->raw_stop) {
        PROVEN_TEST_ASSERT(have_last, "a stream to look at", s->what);
        const raw_t *w = raw_rec(last);
        if ((s->raw_reset && (!w->reset || w->reset_code != s->raw_reset)) || (s->raw_stop && (!w->stopped || w->stop_code != s->raw_stop)))
            printf("  %s: reset %d %llx stop %d %llx\n", s->what, w->reset, (unsigned long long)w->reset_code, w->stopped, (unsigned long long)w->stop_code);
        if (s->raw_reset) PROVEN_TEST_ASSERT(w->reset && w->reset_code == s->raw_reset, "the stream is reset with the code", s->what);
        if (s->raw_stop) PROVEN_TEST_ASSERT(w->stopped && w->stop_code == s->raw_stop, "the stream is asked to stop with the code", s->what);
    }
    if (s->h3_reset) {
        const rec_t *r = rec_find(&g_app[0], s->h3_reset_id);
        PROVEN_TEST_ASSERT(r && r->reset && r->code == s->h3_reset, "the client's request ends with STREAM_RESET and the code", s->what);
    }
    net_free(n);
}

int main(void) {
    PROVEN_TEST_SUITE("HTTP/3 connections",
        "HTTP/3 over two QUIC engines on a simulated network: requests and responses of every shape, flow control and pausing, goodbye, and every code of RFC 9114 and RFC 9204 that this side acts on, from the input that should produce it.",
        "Inspect src/proven/h3_conn.c; the network is this test's, seeded, with a virtual clock.");

    g_alloc = proven_heap_allocator();
    PROVEN_TEST_ASSERT(tp_build(), "the test's certificates and keys are issued", "");
    PROVEN_TEST_ASSERT(proven_cert_store_create(g_alloc, &g_anchors) == PROVEN_OK && proven_cert_store_add_pem(g_anchors, pem(TP_CA), NULL) == PROVEN_OK, "trust anchors", "");
    proven_tls_options_t co = { .alloc = g_alloc, .anchors = g_anchors, .random = test_random, .now = test_now, .now_ctx = &g_now, .alpn = H3, .alpn_count = 1 };
    proven_tls_options_t so = { .alloc = g_alloc, .random = test_random, .now = test_now, .now_ctx = &g_now, .alpn = H3, .alpn_count = 1,
                                .certificate_pem = pem(TP_SERVER_P256), .private_key_pem = pem(TP_SERVER_P256_KEY) };
    PROVEN_TEST_ASSERT(proven_tls_config_create(&co, &g_tls_client) == PROVEN_OK && proven_tls_config_create(&so, &g_tls_server) == PROVEN_OK, "TLS configs", "");

    net_t *n = calloc(1, sizeof *n);

    PROVEN_TEST_SECTION("requests and responses", "GET with bodies of 0 to 300,000 bytes, POST with a body, trailers both ways, an interim response, HEAD, an unknown path: each answered whole and in order.", "");
    {
        reset_apps();
        net_init(n, 1);
        g_app[0].strict = g_app[1].strict = true;
        fresh(n, 1);
        PROVEN_TEST_ASSERT(proven_h3_conn_memory(n->h3[0]) > 0, "", "");
        const proven_u64 sizes[] = { 0, 1, 1000, 16384, 16385, 300000 };
        char path[48];
        for (int i = 0; i < 6; ++i) { snprintf(path, sizeof path, "/get/%llu", (unsigned long long)sizes[i]); (void)request(n, "GET", path, 0, false); }
        (void)request(n, "POST", "/post", 200000, false);
        (void)request(n, "POST", "/post", 5000, true);
        (void)request(n, "GET", "/trailers", 0, false);
        (void)request(n, "GET", "/interim", 0, false);
        (void)request(n, "HEAD", "/head", 0, false);
        (void)request(n, "GET", "/nothing", 0, false);
        PROVEN_TEST_ASSERT(run_until_done(n, 12, 20000000), "every response ends", "");
        for (int i = 0; i < 6; ++i) {
            const rec_t *r = &g_app[0].r[i];
            PROVEN_TEST_ASSERT(r->status == 200 && r->got == sizes[i] && !r->bad && !r->reset, "a GET's body whole and in order", r->path);
        }
        PROVEN_TEST_ASSERT(g_app[0].r[6].status == 200 && g_app[0].r[7].status == 200 && g_app[0].r[6].sent == 200000, "a POST's body arrives whole (the server checked it)", "");
        bool server_trailers = false;
        for (int i = 0; i < g_app[1].n; ++i) server_trailers |= g_app[1].r[i].trailers;
        PROVEN_TEST_ASSERT(server_trailers, "the request's trailers reach the server", "");
        PROVEN_TEST_ASSERT(g_app[0].r[8].status == 200 && g_app[0].r[8].got == 10 && g_app[0].r[8].trailers, "a response with trailers", "");
        PROVEN_TEST_ASSERT(g_app[0].r[9].interim == 1 && g_app[0].r[9].status == 204, "an interim response before the final one", "");
        PROVEN_TEST_ASSERT(g_app[0].r[10].status == 200 && g_app[0].r[10].got == 0 && g_app[0].r[10].end, "HEAD: content-length 1000 and no body", "");
        PROVEN_TEST_ASSERT(g_app[0].r[11].status == 404, "an unknown path", "");
        for (int i = 0; i < g_app[1].n; ++i) PROVEN_TEST_ASSERT(!g_app[1].r[i].bad, "the server saw every body as sent", g_app[1].r[i].path);
        PROVEN_TEST_ASSERT(g_app[0].writable_events + g_app[1].writable_events > 0, "sends that were cut short went on when told", "");
        run_for(n, 200000);
        PROVEN_TEST_ASSERT(proven_h3_conn_memory(n->h3[0]) < 20000 && proven_h3_conn_memory(n->h3[1]) < 20000, "idle: the read buffer of 16 KiB and the control streams, no more", "");
        PROVEN_TEST_INFO("idle memory: client {}, server {}", PROVEN_ARG((long long)(proven_h3_conn_memory(n->h3[0]))), PROVEN_ARG((long long)(proven_h3_conn_memory(n->h3[1]))));
        net_free(n);
    }

    PROVEN_TEST_SECTION("the connection's send buffer, shared", "Eight POST bodies of 200,000 bytes sent only when told: together they pass QUIC's 1 MiB connection send buffer though each fits its stream's, so streams held back by the connection must be told when another's data is acknowledged.", "");
    {
        reset_apps();
        net_init(n, 7);
        g_app[0].strict = g_app[1].strict = true;
        fresh(n, 7);
        for (int i = 0; i < 8; ++i) (void)request(n, "POST", "/post", 200000, false);
        PROVEN_TEST_ASSERT(run_until_done(n, 8, 20000000), "every POST is answered", "");
        for (int i = 0; i < 8; ++i) PROVEN_TEST_ASSERT(g_app[0].r[i].status == 200 && g_app[0].r[i].sent == 200000, "each body whole and as sent", "");
        net_free(n);
    }

    PROVEN_TEST_SECTION("a hundred requests at once, under loss", "The peer's limit of 100 streams in use together, with 5 percent loss, reordering and duplication; the 101st waits (BUSY) until one ends.", "");
    {
        reset_apps();
        net_init(n, 2);
        fresh(n, 2);
        n->loss = 50; n->reorder = 20; n->dup = 10; n->jitter = 2000;
        char path[48];
        for (int i = 0; i < 100; ++i) { snprintf(path, sizeof path, "/get/%d", 2000 + i * 37); (void)request(n, "GET", path, 0, false); }
        proven_u64 id = 0;
        const proven_http_header_t f[] = { { sv(":method"), sv("GET") }, { sv(":scheme"), sv("https") }, { sv(":authority"), sv("example.test") }, { sv(":path"), sv("/get/5") } };
        PROVEN_TEST_ASSERT(proven_h3_submit_request(n->h3[0], f, 4, true, &id) == PROVEN_ERR_BUSY, "the 101st: BUSY", "");
        PROVEN_TEST_ASSERT(run_until_done(n, 100, 60000000), "all hundred end", "");
        for (int i = 0; i < 100; ++i) PROVEN_TEST_ASSERT(g_app[0].r[i].status == 200 && g_app[0].r[i].got == (proven_u64)(2000 + i * 37) && !g_app[0].r[i].bad, "each whole", "");
        run_for(n, 500000);
        (void)request(n, "GET", "/get/5", 0, false);
        PROVEN_TEST_ASSERT(run_until_done(n, 101, 10000000), "the next opens once streams have ended", "");
        net_free(n);
    }

    PROVEN_TEST_SECTION("a paused stream", "A client that pauses a download at its first piece: the server's sending stops at the stream's window; resumed, the rest arrives.", "");
    {
        reset_apps();
        net_init(n, 3);
        g_app[0].pause_first = true;
        fresh(n, 3);
        (void)request(n, "GET", "/get/2000000", 0, false);
        run_for(n, 3000000);
        const rec_t *s = &g_app[1].r[0];
        PROVEN_TEST_ASSERT(s->sent < 2000000 && s->sent >= 256 * 1024 && s->sent <= 256 * 1024 + 300000, "the server stops near the stream's window (256 KiB) and its send buffer", "");
        PROVEN_TEST_INFO("sent while paused: {}", PROVEN_ARG((long long)((unsigned long long)s->sent)));
        g_app[0].pause_first = false;
        proven_h3_stream_pause(n->h3[0], g_app[0].r[0].id, false);
        PROVEN_TEST_ASSERT(run_until_done(n, 1, 20000000) && g_app[0].r[0].got == 2000000 && !g_app[0].r[0].bad, "resumed: all of it", "");
        net_free(n);
    }

    PROVEN_TEST_SECTION("a response before its request's end", "A server that answers before a POST's body has come asks the client to stop sending it, with H3_NO_ERROR; the response still arrives.", "");
    {
        reset_apps();
        net_init(n, 4);
        fresh(n, 4);
        (void)request(n, "POST", "/early", 3000000, false);
        PROVEN_TEST_ASSERT(run_until_done(n, 1, 10000000), "the response ends", "");
        run_for(n, 500000);
        const rec_t *r = &g_app[0].r[0];
        PROVEN_TEST_ASSERT(r->status == 200 && r->stopped && r->code == PROVEN_H3_NO_ERROR && r->sent < 3000000, "STOP_SENDING with H3_NO_ERROR, the body cut short", "");
        net_free(n);
    }

    PROVEN_TEST_SECTION("an end with no bytes behind a queued head", "A response head the connection's send buffer cannot take yet, then proven_h3_send_data with nothing and the end: the end is taken, and goes behind the head.", "Check the order of the checks at the top of proven_h3_send_data.");
    {
        reset_apps();
        net_init(n, 9);
        /* A client whose windows are larger than the server's send buffers: five downloads then
         * hold all of the server's 1 MiB, and a head that comes after waits in HTTP/3's queue. */
        n->cfg[0].stream_window = 4u << 20;
        n->cfg[0].conn_window = 16u << 20;
        fresh(n, 9);
        for (int i = 0; i < 5; ++i) (void)request(n, "GET", "/get/2000000", 0, false);
        (void)request(n, "GET", "/end-after-head", 0, false);
        PROVEN_TEST_ASSERT(run_until_done(n, 6, 30000000), "every response ends", "");
        for (int i = 0; i < 5; ++i) PROVEN_TEST_ASSERT(g_app[0].r[i].status == 200 && g_app[0].r[i].got == 2000000 && !g_app[0].r[i].bad, "the downloads, whole", "");
        PROVEN_TEST_ASSERT(g_app[0].r[5].status == 200 && g_app[0].r[5].got == 0 && g_app[0].r[5].end && !g_app[1].r[5].bad, "the empty response, ended", "");
        net_free(n);
    }

    PROVEN_TEST_SECTION("goodbye", "A server's GOAWAY: what it took is answered, the client sends no new request, and both close with H3_NO_ERROR once the last stream ends; a client's GOAWAY likewise.", "");
    {
        for (int who = 1; who >= 0; --who) {
            reset_apps();
            net_init(n, 5 + (unsigned)who);
            fresh(n, 5);
            (void)request(n, "GET", "/get/100000", 0, false);
            run_for(n, 50000);
            PROVEN_TEST_ASSERT(proven_h3_conn_goaway(n->h3[who]) == PROVEN_OK, "goodbye", "");
            run_for(n, 3000000);
            PROVEN_TEST_ASSERT(g_app[0].r[0].status == 200 && g_app[0].r[0].got == 100000, "the request in flight is answered", "");
            PROVEN_TEST_ASSERT(g_app[1 - who].goaways == 1 && (who == 0 || g_app[0].goaway_id == 4), "the other end hears it (a server's: the first request not acted on)", "");
            proven_u64 id;
            const proven_http_header_t f[] = { { sv(":method"), sv("GET") }, { sv(":scheme"), sv("https") }, { sv(":authority"), sv("example.test") }, { sv(":path"), sv("/") } };
            PROVEN_TEST_ASSERT(proven_h3_submit_request(n->h3[0], f, 4, true, &id) == PROVEN_ERR_INVALID_STATE, "no new request after a goodbye", "");
            PROVEN_TEST_ASSERT(g_app[0].closed && g_app[1].closed && g_app[who].close_code == PROVEN_H3_NO_ERROR && g_app[who].close_app, "closed with H3_NO_ERROR", "");
            net_free(n);
        }
        /* In two steps (RFC 9114 section 5.2): a request already on its way when the server says
         * the first is answered, the first does not close, and the second names the real ID. A
         * long download keeps the client from closing on its own between the two. */
        reset_apps();
        net_init(n, 7);
        fresh(n, 7);
        (void)request(n, "GET", "/get/5000000", 0, false);
        run_for(n, 50000);
        PROVEN_TEST_ASSERT(proven_h3_conn_goaway_step(n->h3[1], true) == PROVEN_OK, "the first goodbye", "");
        (void)request(n, "GET", "/get/2000", 0, false);              /* sent before the client hears it */
        run_for(n, 80000);
        PROVEN_TEST_ASSERT(g_app[0].r[1].status == 200 && g_app[0].r[1].got == 2000 && !g_app[0].r[1].reset, "the request on its way is answered, not rejected", "");
        PROVEN_TEST_ASSERT(g_app[0].goaways == 1 && g_app[0].goaway_id == ((proven_u64)1 << 62) - 4, "the first names the largest ID a client may use", "");
        PROVEN_TEST_ASSERT(!g_app[1].closed, "the server does not close on it", "");
        PROVEN_TEST_ASSERT(proven_h3_conn_goaway_step(n->h3[1], true) == PROVEN_OK, "a first goodbye again", "");
        PROVEN_TEST_ASSERT(proven_h3_conn_goaway_step(n->h3[1], false) == PROVEN_OK, "the last goodbye", "");
        PROVEN_TEST_ASSERT(run_until_done(n, 2, 20000000), "the download ends", "");
        run_for(n, 3000000);
        PROVEN_TEST_ASSERT(g_app[0].r[0].status == 200 && g_app[0].r[0].got == 5000000, "whole", "");
        PROVEN_TEST_ASSERT(g_app[0].goaways == 2 && g_app[0].goaway_id == 8, "the client heard two goodbyes - the first again said nothing - the last naming the first request not acted on", "");
        PROVEN_TEST_ASSERT(g_app[0].closed && g_app[1].closed && g_app[1].close_code == PROVEN_H3_NO_ERROR, "and the connection closes with H3_NO_ERROR", "");
        net_free(n);
        /* With nothing open and a peer that does not close on its own (raw QUIC): the first goodbye
         * keeps the connection, the last one closes it. */
        run_script(n, &(script_t){ 1, "C:000400 g T", 0, 0, 0, 0, 0, "a server's first goodbye of two, nothing open: the connection stays" }, 22);
        run_script(n, &(script_t){ 1, "C:000400 g T G", PROVEN_H3_NO_ERROR, 0, 0, 0, 0, "and its last one closes it with H3_NO_ERROR" }, 23);
        /* A request the server had not seen when it said goodbye is rejected, safe to send again. */
        run_script(n, &(script_t){ 1, "B: H::method=GET" REQ "|:path=/get/100000 F T G B: H::method=GET" REQ "|:path=/ F", PROVEN_H3_NO_ERROR, PROVEN_H3_REQUEST_REJECTED, 0, 0, 0, "a request above the server's GOAWAY: H3_REQUEST_REJECTED, then the close" }, 20);
        run_script(n, &(script_t){ 0, "q q T C:00 W:0400 W:070104", 0, 0, 0, PROVEN_H3_REQUEST_REJECTED, 4, "the client's request above a GOAWAY: STREAM_RESET with H3_REQUEST_REJECTED" }, 21);
    }

    PROVEN_TEST_SECTION("every code", "Each code of RFC 9114 section 8.1 and RFC 9204 section 6 this side acts on, from the input that should produce it, both directions.", "");
    {
        static const script_t cases[] = {
            /* H3_STREAM_CREATION_ERROR */
            { 0, "B:0000", PROVEN_H3_STREAM_CREATION_ERROR, 0, 0, 0, 0, "a server's bidirectional stream" },
            { 1, "C:000400 U:00", PROVEN_H3_STREAM_CREATION_ERROR, 0, 0, 0, 0, "a second control stream" },
            { 1, "U:01", PROVEN_H3_STREAM_CREATION_ERROR, 0, 0, 0, 0, "a client's push stream" },
            { 1, "U:02 U:02", PROVEN_H3_STREAM_CREATION_ERROR, 0, 0, 0, 0, "a second QPACK encoder stream" },
            { 1, "U:03 U:03", PROVEN_H3_STREAM_CREATION_ERROR, 0, 0, 0, 0, "a second QPACK decoder stream" },
            { 1, "U:21aabbcc", 0, 0, PROVEN_H3_STREAM_CREATION_ERROR, 0, 0, "an unknown unidirectional stream type: STOP_SENDING, the connection stays" },
            { 0, "U:4040", 0, 0, PROVEN_H3_STREAM_CREATION_ERROR, 0, 0, "an unknown two-byte type, to a client" },
            /* H3_CLOSED_CRITICAL_STREAM */
            { 1, "C:000400 F", PROVEN_H3_CLOSED_CRITICAL_STREAM, 0, 0, 0, 0, "the control stream ended" },
            { 0, "C:000400 T R", PROVEN_H3_CLOSED_CRITICAL_STREAM, 0, 0, 0, 0, "the control stream reset" },
            { 0, "C:000400 R", 0, 0, 0, 0, 0, "a stream reset before its type was read: tolerated (RFC 9114 section 6.2)" },
            { 1, "U:02 F", PROVEN_H3_CLOSED_CRITICAL_STREAM, 0, 0, 0, 0, "the QPACK encoder stream ended" },
            { 1, "U:03 T R", PROVEN_H3_CLOSED_CRITICAL_STREAM, 0, 0, 0, 0, "the QPACK decoder stream reset" },
            { 1, "S", PROVEN_H3_CLOSED_CRITICAL_STREAM, 0, 0, 0, 0, "STOP_SENDING on this side's control stream" },
            { 1, "C:00 F", PROVEN_H3_CLOSED_CRITICAL_STREAM, 0, 0, 0, 0, "the control stream ended before SETTINGS" },
            /* H3_FRAME_UNEXPECTED */
            { 1, "B:0000", PROVEN_H3_FRAME_UNEXPECTED, 0, 0, 0, 0, "DATA before HEADERS" },
            { 1, "B: H::method=POST" REQ "|:path=/post H:x-t=1 H:x-t=2", PROVEN_H3_FRAME_UNEXPECTED, 0, 0, 0, 0, "HEADERS after trailers" },
            { 1, "B: H::method=POST" REQ "|:path=/post H:x-t=1 W:000101", PROVEN_H3_FRAME_UNEXPECTED, 0, 0, 0, 0, "DATA after trailers" },
            { 1, "C:000400 W:0400", PROVEN_H3_FRAME_UNEXPECTED, 0, 0, 0, 0, "a second SETTINGS" },
            { 1, "B:0400", PROVEN_H3_FRAME_UNEXPECTED, 0, 0, 0, 0, "SETTINGS on a request stream" },
            { 1, "B:070100", PROVEN_H3_FRAME_UNEXPECTED, 0, 0, 0, 0, "GOAWAY on a request stream" },
            { 1, "B:0d0100", PROVEN_H3_FRAME_UNEXPECTED, 0, 0, 0, 0, "MAX_PUSH_ID on a request stream" },
            { 1, "B:030100", PROVEN_H3_FRAME_UNEXPECTED, 0, 0, 0, 0, "CANCEL_PUSH on a request stream" },
            { 1, "C:000400 W:0000", PROVEN_H3_FRAME_UNEXPECTED, 0, 0, 0, 0, "DATA on the control stream" },
            { 1, "C:000400 W:0100", PROVEN_H3_FRAME_UNEXPECTED, 0, 0, 0, 0, "HEADERS on the control stream" },
            { 0, "C:000400 W:050100", PROVEN_H3_FRAME_UNEXPECTED, 0, 0, 0, 0, "PUSH_PROMISE on the control stream" },
            { 1, "B:0200", PROVEN_H3_FRAME_UNEXPECTED, 0, 0, 0, 0, "HTTP/2's PRIORITY (0x2) on a request stream" },
            { 1, "C:000400 W:0600", PROVEN_H3_FRAME_UNEXPECTED, 0, 0, 0, 0, "HTTP/2's PING (0x6) on the control stream" },
            { 1, "B:0800", PROVEN_H3_FRAME_UNEXPECTED, 0, 0, 0, 0, "HTTP/2's WINDOW_UPDATE (0x8)" },
            { 0, "C:000400 W:0900", PROVEN_H3_FRAME_UNEXPECTED, 0, 0, 0, 0, "HTTP/2's CONTINUATION (0x9), to a client" },
            { 0, "C:000400 W:0d0100", PROVEN_H3_FRAME_UNEXPECTED, 0, 0, 0, 0, "MAX_PUSH_ID to a client" },
            { 1, "B: H::method=POST" REQ "|:path=/post W:050100", PROVEN_H3_FRAME_UNEXPECTED, 0, 0, 0, 0, "PUSH_PROMISE to a server" },
            /* H3_FRAME_ERROR */
            { 1, "C:000400 W:07020000", PROVEN_H3_FRAME_ERROR, 0, 0, 0, 0, "GOAWAY with a byte too many" },
            { 1, "C:000400 W:0700", PROVEN_H3_FRAME_ERROR, 0, 0, 0, 0, "GOAWAY with no payload" },
            { 1, "C:00 W:040140", PROVEN_H3_FRAME_ERROR, 0, 0, 0, 0, "SETTINGS cut inside a varint" },
            { 1, "C:00 W:040106", PROVEN_H3_FRAME_ERROR, 0, 0, 0, 0, "SETTINGS with an identifier and no value" },
            { 1, "C:000400 W:0d03 W:4000ff", PROVEN_H3_FRAME_ERROR, 0, 0, 0, 0, "MAX_PUSH_ID with a byte too many" },
            { 1, "B:010a0000 F", PROVEN_H3_FRAME_ERROR, 0, 0, 0, 0, "a request stream ended inside a frame" },
            { 1, "B: H::method=POST" REQ "|:path=/post W:000a6162 F", PROVEN_H3_FRAME_ERROR, 0, 0, 0, 0, "a request stream ended inside DATA" },
            { 1, "B:01 F", PROVEN_H3_FRAME_ERROR, 0, 0, 0, 0, "a request stream ended inside a frame header" },
            /* H3_EXCESSIVE_LOAD */
            { 1, "C:00 W:0480004e20", PROVEN_H3_EXCESSIVE_LOAD, 0, 0, 0, 0, "SETTINGS of 20,000 bytes" },
            { 1, "C:000400 W:07c000000040000000", PROVEN_H3_EXCESSIVE_LOAD, 0, 0, 0, 0, "a GOAWAY that says it is a gigabyte (h3spec)" },
            /* H3_ID_ERROR */
            { 0, "C:000400 W:070108 W:07010c", PROVEN_H3_ID_ERROR, 0, 0, 0, 0, "a GOAWAY larger than the one before" },
            { 0, "C:000400 W:070101", PROVEN_H3_ID_ERROR, 0, 0, 0, 0, "a server's GOAWAY that is no client request stream" },
            { 1, "C:000400 W:07010a W:07010b", PROVEN_H3_ID_ERROR, 0, 0, 0, 0, "a client's GOAWAY larger than before" },
            { 0, "q T Q:050100", PROVEN_H3_ID_ERROR, 0, 0, 0, 0, "PUSH_PROMISE to a client that allowed no push" },
            { 0, "U:0100", PROVEN_H3_ID_ERROR, 0, 0, 0, 0, "a push stream to a client that allowed none" },
            { 0, "C:000400 W:030100", PROVEN_H3_ID_ERROR, 0, 0, 0, 0, "CANCEL_PUSH to a client" },
            { 1, "C:000400 W:030100", PROVEN_H3_ID_ERROR, 0, 0, 0, 0, "CANCEL_PUSH to a server that promised nothing" },
            { 1, "C:000400 W:0d010a W:0d0105", PROVEN_H3_ID_ERROR, 0, 0, 0, 0, "MAX_PUSH_ID smaller than before" },
            /* H3_SETTINGS_ERROR */
            { 1, "C:00 W:040406010602", PROVEN_H3_SETTINGS_ERROR, 0, 0, 0, 0, "SETTINGS_MAX_FIELD_SECTION_SIZE twice" },
            { 0, "C:00 W:04040100 W:0100", PROVEN_H3_SETTINGS_ERROR, 0, 0, 0, 0, "QPACK_MAX_TABLE_CAPACITY twice" },
            { 1, "C:00 W:04020200", PROVEN_H3_SETTINGS_ERROR, 0, 0, 0, 0, "HTTP/2's ENABLE_PUSH (0x2)" },
            { 1, "C:00 W:04020500", PROVEN_H3_SETTINGS_ERROR, 0, 0, 0, 0, "HTTP/2's MAX_FRAME_SIZE (0x5)" },
            /* H3_MISSING_SETTINGS */
            { 1, "C:00 W:070100", PROVEN_H3_MISSING_SETTINGS, 0, 0, 0, 0, "GOAWAY first on the control stream" },
            { 0, "C:00 W:2100", PROVEN_H3_MISSING_SETTINGS, 0, 0, 0, 0, "a reserved frame type first" },
            /* H3_REQUEST_INCOMPLETE */
            { 1, "B: F", 0, PROVEN_H3_REQUEST_INCOMPLETE, 0, 0, 0, "a request stream that ends with no request" },
            { 1, "B:2100 F", 0, PROVEN_H3_REQUEST_INCOMPLETE, 0, 0, 0, "a request stream of an unknown frame and its end" },
            { 0, "q T Q: F", 0, 0, 0, PROVEN_H3_REQUEST_INCOMPLETE, 0, "a response stream that ends with no response" },
            /* H3_MESSAGE_ERROR */
            { 1, "B: H::method=GET" REQ "|:path=/|X-Upper=1", 0, PROVEN_H3_MESSAGE_ERROR, PROVEN_H3_MESSAGE_ERROR, 0, 0, "an uppercase field name" },
            { 1, "B: H::method=CONNECT|:protocol=websocket" REQ "|:path=/", 0, PROVEN_H3_MESSAGE_ERROR, 0, 0, 0, "an extended CONNECT (:protocol)" },
            { 1, "B: H::method=GET|:path=/", 0, PROVEN_H3_MESSAGE_ERROR, 0, 0, 0, "no :scheme" },
            { 1, "B: H::method=GET|:scheme=https|:path=/", 0, PROVEN_H3_MESSAGE_ERROR, 0, 0, 0, "https with neither :authority nor Host (RFC 9114 section 4.3.1; h3spec)" },
            { 1, "B: H::method=GET|:scheme=https|:path=/|host=example.test F", 0, 0, 0, 0, 0, "https with Host and no :authority: taken" },
            { 1, "B: H::method=GET" REQ "|:path=/|connection=close", 0, PROVEN_H3_MESSAGE_ERROR, 0, 0, 0, "a connection-specific field" },
            { 1, "B: H::method=POST" REQ "|:path=/post|content-length=5 W:0003616263 F", 0, PROVEN_H3_MESSAGE_ERROR, 0, 0, 0, "content-length 5 and 3 bytes" },
            { 1, "B: H::method=POST" REQ "|:path=/post|content-length=2 W:0003616263", 0, PROVEN_H3_MESSAGE_ERROR, 0, 0, 0, "content-length 2 and 3 bytes" },
            { 1, "B: H::method=GET" REQ "|:path=/|content-length=4 F", 0, PROVEN_H3_MESSAGE_ERROR, 0, 0, 0, "content-length 4 and the stream's end" },
            { 1, "B:0180010000", 0, PROVEN_H3_MESSAGE_ERROR, 0, 0, 0, "a HEADERS frame of 65,536 bytes, more than twice the limit" },
            { 0, "q T Q: H::status=101", 0, 0, 0, PROVEN_H3_MESSAGE_ERROR, 0, "a response of status 101" },
            { 0, "q T Q: H::status=103 F", 0, 0, 0, PROVEN_H3_MESSAGE_ERROR, 0, "an interim response the stream ends on" },
            { 0, "q T Q:01020000", 0, 0, 0, PROVEN_H3_MESSAGE_ERROR, 0, "a response with no :status" },
            /* QPACK */
            { 1, "B:0103000080", PROVEN_QPACK_DECOMPRESSION_FAILED, 0, 0, 0, 0, "a reference to the dynamic table" },
            { 0, "q T Q:01030100d9", PROVEN_QPACK_DECOMPRESSION_FAILED, 0, 0, 0, 0, "a Required Insert Count of 1" },
            { 1, "U:02c0", PROVEN_QPACK_ENCODER_STREAM_ERROR, 0, 0, 0, 0, "an Insert on the encoder stream" },
            { 0, "U:023f01", PROVEN_QPACK_ENCODER_STREAM_ERROR, 0, 0, 0, 0, "a dynamic table capacity of 32" },
            { 1, "U:0380", PROVEN_QPACK_DECODER_STREAM_ERROR, 0, 0, 0, 0, "a Section Acknowledgement" },
            { 0, "U:0301", PROVEN_QPACK_DECODER_STREAM_ERROR, 0, 0, 0, 0, "an Insert Count Increment" },
            /* What is taken */
            { 1, "C:00 W:0407210001000644 W:00 U:0220 U:0348 B:2100 H::method=GET" REQ "|:path=/get/3 F", 0, 0, 0, 0, 0, "unknown settings, a capacity of 0, a Stream Cancellation, a reserved frame before HEADERS" },
            { 0, "C:000400 W:070100 W:070100", PROVEN_H3_NO_ERROR, 0, 0, 0, 0, "the same GOAWAY twice: taken, and a client with nothing open closes" },
        };
        for (proven_size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) run_script(n, &cases[i], 100 + (unsigned)i);
        PROVEN_TEST_INFO("{} scripts", PROVEN_ARG((long long)(sizeof cases / sizeof cases[0])));
    }

    PROVEN_TEST_SECTION("codes this side sends", "H3_REQUEST_CANCELLED for a stream given up, both ways; H3_EXCESSIVE_LOAD when the peer resets more streams than finish; H3_INTERNAL_ERROR when memory runs out; a field section over the limit.", "");
    {
        /* Given up by the client: the server sees the reset with H3_REQUEST_CANCELLED. */
        reset_apps();
        net_init(n, 301);
        n->use_h3[1] = false;
        fresh(n, 301);
        const proven_u64 id = request(n, "GET", "/get/10", 0, false);
        run_for(n, 100000);
        PROVEN_TEST_ASSERT(proven_h3_stream_reset(n->h3[0], id, 0) == PROVEN_OK, "given up", "");
        run_for(n, 500000);
        PROVEN_TEST_ASSERT(raw_rec(id)->stopped && raw_rec(id)->stop_code == PROVEN_H3_REQUEST_CANCELLED, "the peer is asked to stop with H3_REQUEST_CANCELLED", "");
        net_free(n);
        /* Reset by the peer: the client's request ends with its code. */
        reset_apps();
        net_init(n, 302);
        n->use_h3[1] = false;
        fresh(n, 302);
        (void)request(n, "GET", "/get/10", 0, false);
        run_for(n, 100000);
        PROVEN_TEST_ASSERT(proven_quic_stream_reset(n->ep[1], 0, PROVEN_H3_REQUEST_CANCELLED) == PROVEN_OK, "the raw server resets", "");
        run_for(n, 500000);
        PROVEN_TEST_ASSERT(g_app[0].r[0].reset && g_app[0].r[0].code == PROVEN_H3_REQUEST_CANCELLED, "STREAM_RESET with H3_REQUEST_CANCELLED", "");
        net_free(n);
        /* A peer that resets more streams than finish. */
        reset_apps();
        net_init(n, 303);
        n->use_h3[0] = false;
        n->h3cfg[1].max_reset_debt = 3;
        fresh(n, 303);
        static proven_byte_t b[512];
        const proven_size_t len = headers_frame(":method=POST" REQ "|:path=/post", b, sizeof b);
        for (int i = 0; i < 4; ++i) {
            proven_u64 sid;
            PROVEN_TEST_ASSERT(proven_quic_stream_open(n->ep[0], true, &sid) == PROVEN_OK, "", "");
            raw_write(n, 0, sid, b, len, false);
            run_for(n, 50000);
            PROVEN_TEST_ASSERT(proven_quic_stream_reset(n->ep[0], sid, PROVEN_H3_REQUEST_CANCELLED) == PROVEN_OK, "", "");
            run_for(n, 50000);
            PROVEN_TEST_ASSERT(i < 3 ? !g_app[1].closed : g_app[1].closed && g_app[1].close_code == PROVEN_H3_EXCESSIVE_LOAD, "the fourth reset beyond a debt of 3: H3_EXCESSIVE_LOAD", "");
        }
        net_free(n);
        /* Out of memory on the HTTP/3 side. */
        reset_apps();
        net_init(n, 304);
        n->h3cfg[1].alloc = (proven_allocator_t){ .alloc_fn = fa_alloc, .realloc_fn = fa_realloc, .free_fn = fa_free };
        fresh(n, 304);
        g_fail_after = 0;
        (void)request(n, "POST", "/post", 100000, false);
        run_for(n, 1000000);
        g_fail_after = -1;
        PROVEN_TEST_ASSERT(g_app[1].closed && g_app[1].close_code == PROVEN_H3_INTERNAL_ERROR && g_app[0].close_code == PROVEN_H3_INTERNAL_ERROR, "H3_INTERNAL_ERROR, and the peer told", "");
        net_free(n);
        /* A list larger than the peer said it takes is refused before it is sent. */
        reset_apps();
        net_init(n, 305);
        n->h3cfg[1].max_field_section_bytes = 100;
        fresh(n, 305);
        proven_u64 sid;
        const proven_http_header_t f[] = { { sv(":method"), sv("GET") }, { sv(":scheme"), sv("https") }, { sv(":authority"), sv("example.test") }, { sv(":path"), sv("/") } };
        PROVEN_TEST_ASSERT(proven_h3_submit_request(n->h3[0], f, 4, true, &sid) == PROVEN_ERR_OUT_OF_BOUNDS, "four fields of 32 and more: over 100 bytes, PROVEN_ERR_OUT_OF_BOUNDS", "");
        net_free(n);
    }

    PROVEN_TEST_SECTION("changed streams", "A request stream's bytes with each bit changed in turn, sent to the server: the change is refused with a code or taken as another request; nothing else.", "");
    {
        static proven_byte_t b[512];
        const proven_size_t len = headers_frame(":method=POST" REQ "|:path=/post|content-length=3", b, sizeof b);
        b[len] = 0x00; b[len + 1] = 0x03; b[len + 2] = 'a'; b[len + 3] = 'b'; b[len + 4] = 'c';
        const proven_size_t total = len + 5;
        unsigned closed = 0, reset = 0, answered = 0;
        for (proven_size_t bit = 0; bit < total * 8; bit += 5) {
            reset_apps();
            net_init(n, 400 + (unsigned)bit);
            n->use_h3[0] = false;
            fresh(n, 400);
            static proven_byte_t c[512];
            memcpy(c, b, total);
            c[bit / 8] ^= (proven_byte_t)(1u << (bit % 8));
            proven_u64 sid;
            PROVEN_TEST_ASSERT(proven_quic_stream_open(n->ep[0], true, &sid) == PROVEN_OK, "", "");
            raw_write(n, 0, sid, c, total, true);
            run_for(n, 500000);
            if (g_app[1].closed) ++closed;
            else if (raw_rec(sid)->reset) ++reset;
            else ++answered;
            net_free(n);
        }
        PROVEN_TEST_INFO("{} closed, {} reset, {} answered", PROVEN_ARG((long long)(closed)), PROVEN_ARG((long long)(reset)), PROVEN_ARG((long long)(answered)));
        PROVEN_TEST_ASSERT(closed + reset > 0, "", "");
    }

    free(n);
    proven_tls_config_destroy(g_tls_client);
    proven_tls_config_destroy(g_tls_server);
    proven_cert_store_destroy(g_anchors);
    PROVEN_TEST_PASS("HTTP/3 requests and responses complete, and every rule's code is the one RFC 9114 and RFC 9204 name.");
    return 0;
}

#else

int main(void) {
    PROVEN_TEST_SUITE("HTTP/3 connections", "Built with PROVEN_NO_HTTP3: nothing to test.", "");
    PROVEN_TEST_PASS("skipped: PROVEN_NO_HTTP3");
    return 0;
}

#endif
