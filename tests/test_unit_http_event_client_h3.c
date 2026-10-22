#include "proven.h"
#include "proven_test.h"
#include "test_unit_tls_pki.h"
#include "test_unit_http_coding_vectors.h"
#include "../src/proven/proven_internal_quic_net.h"
#include "../src/proven/proven_internal_h3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The event-driven client speaking HTTP/3, over real UDP on loopback, against the event-driven
 * server on the same loop - this library at both ends - in one thread.
 */

#ifndef PROVEN_NO_HTTP3

static proven_mem_view_t mv(const char *s) { return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }
static proven_u8str_view_t sv(const char *s) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }

static proven_allocator_t g_heap;
static proven_loop_t *g_loop;
static proven_tls_config_t *g_server_tls, *g_client_tls, *g_client_tls_untrusting, *g_client_tls_h1;

static proven_byte_t pattern(proven_size_t i) { return (proven_byte_t)('a' + (i * 7 + i / 251) % 26); }

typedef struct raw raw_t;
static raw_t *g_raw;                               /* the scripted server, when one runs */
static void raw_pump(raw_t *x);

static void spin_until(const bool *flag, proven_u32 ms) {
    proven_net_deadline_t until = proven_net_deadline_in(ms);
    while (!*flag && proven_time_monotonic_now() < until) {
        PROVEN_TEST_ASSERT(proven_loop_poll(g_loop, proven_net_deadline_in(g_raw ? 1 : 10)) == PROVEN_OK, "a round", "");
        if (g_raw) raw_pump(g_raw);
    }
}
static void spin(proven_u32 ms) { bool never = false; spin_until(&never, ms); }

// -----------------------------------------------------------------------------
// The server: an ordinary handler
// -----------------------------------------------------------------------------

typedef struct { proven_http_stream_t *stream; proven_loop_timer_t timer; proven_size_t sent, total, got; proven_byte_t *echo; proven_size_t cap; bool echoing; } served_t;
static int g_served, g_served_done, g_served_h3;
static char g_who_authority[80], g_who_target[80];

static void served_pump(served_t *a) {
    static proven_byte_t piece[8192];
    while (a->sent < a->total) {
        proven_size_t n = a->total - a->sent < sizeof piece ? a->total - a->sent : sizeof piece;
        for (proven_size_t i = 0; i < n; i++) piece[i] = pattern(a->sent + i);
        proven_result_size_t w = proven_http_stream_write(a->stream, (proven_mem_view_t){ piece, n });
        if (w.err != PROVEN_OK) return;
        a->sent += w.value;
        if (w.value < n) return;
    }
    (void)proven_http_stream_end(a->stream);
}

static void served_later(void *ctx) { served_t *a = ctx; (void)proven_http_stream_respond(a->stream, 200, NULL, 0, mv("later")); }
static bool target_is(const proven_http_request_t *req, const char *p) { return req->target.size == strlen(p) && memcmp(req->target.ptr, p, req->target.size) == 0; }
static bool target_starts(const proven_http_request_t *req, const char *p) { return req->target.size >= strlen(p) && memcmp(req->target.ptr, p, strlen(p)) == 0; }

static void srv_request(void *ctx, proven_http_stream_t *s, const proven_http_request_t *req) {
    (void)ctx;
    g_served++;
    if (proven_http_stream_protocol(s) == 3) g_served_h3++;
    served_t *a = calloc(1, sizeof *a);
    a->stream = s;
    proven_http_stream_set_user(s, a);
    char num[24] = { 0 };
    if (target_is(req, "/hello")) (void)proven_http_stream_respond(s, 200, NULL, 0, mv("hello"));
    else if (target_starts(req, "/big/") || target_starts(req, "/zip/")) {
        memcpy(num, req->target.ptr + 5, req->target.size - 5 < 23 ? req->target.size - 5 : 23);
        a->total = (proven_size_t)strtoul(num, NULL, 10);
        if (req->target.ptr[1] == 'z') proven_http_stream_compress(s);
        if (proven_http_stream_begin(s, 200, NULL, 0, a->total) == PROVEN_OK) served_pump(a);
    } else if (target_is(req, "/echo")) {
        a->echoing = true;
        if (proven_http_header_count(req->headers, req->header_count, sv("Content-Length")) == 0 && proven_http_header_count(req->headers, req->header_count, sv("Transfer-Encoding")) == 0)
            (void)proven_http_stream_respond(s, 200, NULL, 0, mv(""));
    } else if (target_starts(req, "/who")) {
        proven_u8str_view_t host = { 0 };
        (void)proven_http_header_find(req->headers, req->header_count, sv("Host"), &host);
        snprintf(g_who_target, sizeof g_who_target, "%.*s", (int)req->target.size, (const char *)req->target.ptr);
        snprintf(g_who_authority, sizeof g_who_authority, "%.*s", (int)host.size, (const char *)host.ptr);
        (void)proven_http_stream_respond(s, 200, NULL, 0, mv("who"));
    } else if (target_is(req, "/later")) proven_loop_timer_set(g_loop, &a->timer, 60, served_later, a);
    else if (target_is(req, "/never")) { /* thinking */ }
    else (void)proven_http_stream_respond(s, 404, NULL, 0, mv("no"));
}

static void srv_body(void *ctx, proven_http_stream_t *s, proven_mem_view_t piece, bool last) {
    (void)ctx;
    served_t *a = proven_http_stream_user(s);
    if (!a || !a->echoing) return;
    if (a->got + piece.size > a->cap) { a->cap = (a->got + piece.size) * 2 + 1024; a->echo = realloc(a->echo, a->cap); }
    if (piece.size) memcpy(a->echo + a->got, piece.ptr, piece.size);
    a->got += piece.size;
    if (last) (void)proven_http_stream_respond(s, 200, NULL, 0, (proven_mem_view_t){ a->echo, a->got });
}

static void srv_writable(void *ctx, proven_http_stream_t *s) { (void)ctx; served_t *a = proven_http_stream_user(s); if (a && a->total) served_pump(a); }
static void srv_done(void *ctx, proven_http_stream_t *s, proven_err_t why) {
    (void)ctx; (void)why;
    served_t *a = proven_http_stream_user(s);
    g_served_done++;
    if (a) { proven_loop_timer_cancel(g_loop, &a->timer); free(a->echo); free(a); }
}

static proven_u16 g_port;

static proven_http_event_server_t *server_on(void) {
    proven_http_event_server_config_t cfg = { .on = { srv_request, srv_body, srv_writable, srv_done }, .tls = g_server_tls, .http2 = true, .http3 = true, .compress_zstd = true };
    proven_http_event_server_t *server = NULL;
    proven_net_addr_t at;
    PROVEN_TEST_ASSERT(proven_http_event_server_create(g_loop, &cfg, &server) == PROVEN_OK &&
                       proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "a server", "");
    g_port = at.port;
    g_served = g_served_done = g_served_h3 = 0;
    return server;
}

// -----------------------------------------------------------------------------
// The client's side
// -----------------------------------------------------------------------------

typedef struct {
    proven_http_event_request_t *request;
    proven_loop_timer_t timer;
    proven_size_t got, to_send, sent;
    int status, protocol, responses, dones, writables;
    proven_err_t why;
    int order;                                     /* when it ended, among the calls of a case */
    bool done, bad, plain, pause_once, paused_now, body_while_paused, last;
    proven_byte_t first[8];
    char coding[8];
} call_t;

static bool g_in_start, g_done_in_start;
static int g_order;
static call_t *g_keep;                             /* the call whose body is kept, whole, in g_kept */
static proven_byte_t g_kept[8192];
static int g_in_write, g_reentered;

static void call_resume(void *ctx) { call_t *c = ctx; c->paused_now = false; proven_http_event_request_resume(c->request); }

static void on_response(void *ctx, proven_http_event_request_t *r, const proven_http_response_t *head) {
    call_t *c = ctx;
    c->responses++;
    c->status = head->status;
    c->protocol = proven_http_event_request_protocol(r);
    proven_u8str_view_t ce;
    c->coding[0] = '\0';
    if (proven_http_header_find(head->headers, head->header_count, sv("Content-Encoding"), &ce) && ce.size < sizeof c->coding) {
        memcpy(c->coding, ce.ptr, ce.size);
        c->coding[ce.size] = '\0';
    }
    if (proven_http_event_request_user(r) != c || head->version_minor != 1) c->bad = true;
    if (c->pause_once) {
        c->pause_once = false;
        c->paused_now = true;
        proven_http_event_request_pause(r);
        proven_loop_timer_set(g_loop, &c->timer, 100, call_resume, c);
    }
}

static void on_body(void *ctx, proven_http_event_request_t *r, proven_mem_view_t piece, bool last) {
    (void)r;
    call_t *c = ctx;
    if (c->paused_now) c->body_while_paused = true;
    for (proven_size_t i = 0; i < piece.size; i++) {
        if (c->got + i < sizeof c->first) c->first[c->got + i] = piece.ptr[i];
        if (!c->plain && piece.ptr[i] != pattern(c->got + i)) c->bad = true;
    }
    if (c == g_keep && piece.size > 0 && c->got + piece.size <= sizeof g_kept) memcpy(g_kept + c->got, piece.ptr, piece.size);
    if (last) c->last = true;
    c->got += piece.size;
}

static void call_pump(call_t *c) {
    static proven_byte_t piece[8192];
    while (c->sent < c->to_send) {
        proven_size_t n = c->to_send - c->sent < sizeof piece ? c->to_send - c->sent : sizeof piece;
        for (proven_size_t i = 0; i < n; i++) piece[i] = pattern(c->sent + i);
        g_in_write++;
        proven_result_size_t w = proven_http_event_request_write(c->request, (proven_mem_view_t){ piece, n });
        g_in_write--;
        if (w.err != PROVEN_OK) return;
        c->sent += w.value;
        if (w.value < n) return;
    }
    (void)proven_http_event_request_end(c->request);
}

static void on_writable(void *ctx, proven_http_event_request_t *r) {
    (void)r;
    call_t *c = ctx;
    c->writables++;
    if (g_in_write > 0) g_reentered++;
    call_pump(c);
}

static void on_done(void *ctx, proven_http_event_request_t *r, proven_err_t why) {
    (void)r;
    call_t *c = ctx;
    if (g_in_start) g_done_in_start = true;
    c->dones++;
    c->done = true;
    c->order = ++g_order;
    c->why = why;
    proven_loop_timer_cancel(g_loop, &c->timer);
}

static char g_url[200];
static proven_u8str_view_t url_to(proven_u16 port, const char *path) {
    int n = snprintf(g_url, sizeof g_url, "https://127.0.0.1:%u%s", (unsigned)port, path);
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)g_url, .size = (proven_size_t)n };
}

static proven_err_t start_to(proven_http_event_client_t *client, call_t *c, proven_u16 port, const char *method, const char *path, proven_mem_view_t body, proven_u64 body_length, bool h3) {
    memset(c, 0, sizeof *c);
    proven_http_event_request_options_t o = { .method = sv(method), .url = url_to(port, path), .body = body, .body_length = body_length, .http3 = h3,
        .on = { .on_response = on_response, .on_body = on_body, .on_writable = on_writable, .on_done = on_done }, .ctx = c };
    g_in_start = true;
    proven_err_t e = proven_http_event_client_start(client, &o, &c->request);
    g_in_start = false;
    if (e == PROVEN_OK) proven_http_event_request_set_user(c->request, c);
    return e;
}
static proven_err_t start(proven_http_event_client_t *client, call_t *c, const char *method, const char *path, proven_mem_view_t body, proven_u64 body_length) {
    return start_to(client, c, g_port, method, path, body, body_length, true);
}

// -----------------------------------------------------------------------------
// A scripted server: QUIC from the engine, HTTP/3 written by hand, so that it can send what the
// event server never would - an interim response, a head that breaks the rules, a body shorter
// or longer than it said, a reset, trailers. One connection at a time; it answers by path.
// -----------------------------------------------------------------------------

struct raw {
    proven_net_udp_t udp;
    proven_u16 port;
    proven_quic_config_t qc;
    proven_quic_conn_t *q;
    proven_net_addr_t peer;
    bool control;                                  /* its control stream is open */
    proven_u64 reset_id, reset_at;                 /* a stream to reset at that time (0: none), once what went before has arrived */
    int requests, rejected, conns;                 /* requests seen; how many /rejected has refused; connections taken */
    struct { proven_u64 id; proven_byte_t buf[2048]; proven_size_t len; bool used; } st[16];
};

static proven_u64 raw_now(void) { return (proven_u64)proven_time_monotonic_now() / 1000u; }

static void raw_start(raw_t *x, const proven_tls_config_t *tls) {
    memset(x, 0, sizeof *x);
    proven_net_addr_t at;
    PROVEN_TEST_ASSERT(proven_net_udp_open(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &x->udp, &at) == PROVEN_OK, "the scripted server's socket", "");
    x->port = at.port;
    x->qc = (proven_quic_config_t){ .alloc = g_heap, .tls = tls };
    g_raw = x;
}

static void raw_stop(raw_t *x) {
    if (x->q) proven_quic_conn_destroy(x->q);
    (void)proven_net_udp_close(&x->udp);
    g_raw = NULL;
}

static void raw_write(raw_t *x, proven_u64 id, const proven_byte_t *p, proven_size_t n, bool fin) {
    proven_size_t taken = 0;
    PROVEN_TEST_ASSERT(proven_quic_stream_write(x->q, id, (proven_mem_view_t){ p, n }, fin, &taken) == PROVEN_OK && taken == n, "the scripted server writes", "");
}

/* A frame: type, length, payload. */
static proven_size_t raw_frame(proven_byte_t *out, proven_u64 type, const proven_byte_t *p, proven_size_t n) {
    proven_size_t k = proven_quic_varint_put(type, out);
    k += proven_quic_varint_put(n, out + k);
    if (n) memcpy(out + k, p, n);
    return k + n;
}

/* A HEADERS frame of `count` fields, each "name", "value" in turn. */
static proven_size_t raw_headers(proven_byte_t *out, const char *const *f, proven_size_t count) {
    proven_http_header_t h[8];
    for (proven_size_t i = 0; i < count; ++i) h[i] = (proven_http_header_t){ sv(f[2 * i]), sv(f[2 * i + 1]) };
    proven_byte_t block[512];
    proven_size_t len = 0;
    PROVEN_TEST_ASSERT(proven_qpack_encode(h, count, NULL, (proven_mem_mut_t){ block, sizeof block }, &len) == PROVEN_OK, "a field section", "");
    return raw_frame(out, 1, block, len);
}

static void raw_answer(raw_t *x, proven_u64 id, const char *path) {
    proven_byte_t out[1024];
    proven_size_t n = 0;
    static const proven_byte_t ten[50] = "0123456789012345678901234567890123456789012345678";
    x->requests++;
    if (!strcmp(path, "/interim")) {
        n += raw_headers(out + n, (const char *const[]){ ":status", "103", "link", "</s.css>; rel=preload" }, 2);
        n += raw_headers(out + n, (const char *const[]){ ":status", "200", "content-length", "5" }, 2);
        n += raw_frame(out + n, 0, (const proven_byte_t *)"hello", 5);
        raw_write(x, id, out, n, true);
    } else if (!strcmp(path, "/interim-only")) {
        n += raw_headers(out + n, (const char *const[]){ ":status", "100" }, 1);
        raw_write(x, id, out, n, true);
    } else if (!strcmp(path, "/upper")) {
        n += raw_headers(out + n, (const char *const[]){ ":status", "200", "Content-Type", "text/plain" }, 2);
        n += raw_frame(out + n, 0, (const proven_byte_t *)"hello", 5);
        raw_write(x, id, out, n, true);
    } else if (!strcmp(path, "/no-status")) {
        n += raw_headers(out + n, (const char *const[]){ "content-type", "text/plain" }, 1);
        raw_write(x, id, out, n, true);
    } else if (!strcmp(path, "/short") || !strcmp(path, "/long")) {
        /* The longer one leaves its stream open: the excess alone must end it, not its end. */
        n += raw_headers(out + n, (const char *const[]){ ":status", "200", "content-length", path[1] == 's' ? "100" : "10" }, 2);
        n += raw_frame(out + n, 0, ten, sizeof ten);
        raw_write(x, id, out, n, path[1] == 's');
    } else if (!strcmp(path, "/reset")) {
        n += raw_headers(out + n, (const char *const[]){ ":status", "200", "content-length", "100" }, 2);
        n += raw_frame(out + n, 0, ten, 10);
        raw_write(x, id, out, n, false);
        x->reset_id = id;                          /* a reset now would take back what is not yet sent */
        x->reset_at = raw_now() + 50000;
    } else if (!strcmp(path, "/rejected") && x->rejected == 0) {
        x->rejected++;
        PROVEN_TEST_ASSERT(proven_quic_stream_reset(x->q, id, PROVEN_H3_REQUEST_REJECTED) == PROVEN_OK, "the scripted server refuses a request", "");
    } else if (!strcmp(path, "/trailers")) {
        n += raw_headers(out + n, (const char *const[]){ ":status", "200" }, 1);
        n += raw_frame(out + n, 0, (const proven_byte_t *)"hello", 5);
        n += raw_headers(out + n, (const char *const[]){ "x-checksum", "1234" }, 1);
        raw_write(x, id, out, n, true);
    } else if (!strcmp(path, "/br")) {
        const coding_vector_t *v = &coding_vectors[36];
        char len[16];
        snprintf(len, sizeof len, "%lu", v->size);
        n += raw_headers(out + n, (const char *const[]){ ":status", "200", "content-encoding", "br", "content-length", len }, 3);
        n += raw_frame(out + n, 0, v->bytes, v->size);
        raw_write(x, id, out, n, true);
    } else {
        n += raw_headers(out + n, (const char *const[]){ ":status", "200" }, 1);
        n += raw_frame(out + n, 0, (const proven_byte_t *)"hello", 5);
        raw_write(x, id, out, n, true);
    }
}

/* A request stream's bytes so far: at its end, the path of its HEADERS frame, answered. */
static void raw_request(raw_t *x, proven_u64 id) {
    int k = -1;
    for (int i = 0; i < 16; ++i) if (x->st[i].used && x->st[i].id == id) k = i;
    for (int i = 0; i < 16 && k < 0; ++i) if (!x->st[i].used) { k = i; x->st[i].used = true; x->st[i].id = id; x->st[i].len = 0; }
    PROVEN_TEST_ASSERT(k >= 0, "a place for the request", "");
    proven_size_t got = 0;
    bool fin = false;
    (void)proven_quic_stream_read(x->q, id, (proven_mem_mut_t){ x->st[k].buf + x->st[k].len, sizeof x->st[k].buf - x->st[k].len }, &got, &fin);
    x->st[k].len += got;
    if (!fin) return;
    x->st[k].used = false;
    proven_quic_rd_t r = { .p = x->st[k].buf, .n = x->st[k].len, .ok = true };
    const proven_u64 type = proven_quic_rd_varint(&r), len = proven_quic_rd_varint(&r);
    proven_quic_rd_t block = proven_quic_rd_take(&r, (proven_size_t)len);
    proven_http_header_t h[16];
    proven_byte_t scratch[512];
    proven_size_t count = 0;
    PROVEN_TEST_ASSERT(r.ok && type == 1 && proven_qpack_decode((proven_mem_view_t){ block.p, block.n }, h, NULL, 16, (proven_mem_mut_t){ scratch, sizeof scratch }, 0, &count) == PROVEN_OK,
        "the scripted server reads a request's head", "");
    char path[64] = "";
    for (proven_size_t i = 0; i < count; ++i)
        if (h[i].name.size == 5 && !memcmp(h[i].name.ptr, ":path", 5) && h[i].value.size < sizeof path) { memcpy(path, h[i].value.ptr, h[i].value.size); path[h[i].value.size] = '\0'; }
    raw_answer(x, id, path);
}

static void raw_pump(raw_t *x) {
    static proven_byte_t dg[2048];
    for (;;) {
        proven_net_addr_t from;
        proven_result_size_t r = proven_net_udp_recv_from(&x->udp, (proven_mem_mut_t){ dg, sizeof dg }, &from, proven_net_deadline_in(0));
        if (r.err != PROVEN_OK) break;
        const proven_quic_addr_t qa = proven_quic_addr_of_(&from);
        if (x->q) { (void)proven_quic_conn_receive(x->q, (proven_mem_view_t){ dg, r.value }, &qa, raw_now()); continue; }
        proven_quic_accept_t acc;
        proven_byte_t reply[1500];
        proven_size_t n = 0;
        if (proven_quic_server_stateless(&x->qc, (proven_mem_view_t){ dg, r.value }, &qa, raw_now(), false, (proven_mem_mut_t){ reply, sizeof reply }, &n, &acc) != PROVEN_QUIC_STATELESS_ACCEPT) continue;
        PROVEN_TEST_ASSERT(proven_quic_conn_server_create(&x->qc, &acc, (proven_mem_view_t){ dg, r.value }, &qa, raw_now(), &x->q) == PROVEN_OK, "the scripted server takes a connection", "");
        x->peer = from;
        x->control = false;
        x->conns++;
    }
    if (!x->q) return;
    if (x->reset_at && raw_now() >= x->reset_at) {
        PROVEN_TEST_ASSERT(proven_quic_stream_reset(x->q, x->reset_id, PROVEN_H3_INTERNAL_ERROR) == PROVEN_OK, "the scripted server resets a stream", "");
        x->reset_at = 0;
    }
    if (raw_now() >= proven_quic_conn_deadline(x->q)) proven_quic_conn_timeout(x->q, raw_now());
    proven_quic_event_t ev;
    bool closed = false;
    while (proven_quic_conn_poll(x->q, &ev)) {
        if (ev.kind == PROVEN_QUIC_EV_ESTABLISHED && !x->control) {
            /* The control stream and its SETTINGS, empty: every default. */
            proven_u64 id;
            static const proven_byte_t settings[] = { 0x00, 0x04, 0x00 };
            PROVEN_TEST_ASSERT(proven_quic_stream_open(x->q, false, &id) == PROVEN_OK, "the scripted server's control stream", "");
            raw_write(x, id, settings, sizeof settings, false);
            x->control = true;
        } else if (ev.kind == PROVEN_QUIC_EV_STREAM_READABLE) {
            if ((ev.stream_id & 2) == 0) raw_request(x, ev.stream_id);
            else {                                 /* the client's control and QPACK streams: read and let go */
                proven_byte_t sink[256];
                proven_size_t got = 0;
                bool fin = false;
                (void)proven_quic_stream_read(x->q, ev.stream_id, (proven_mem_mut_t){ sink, sizeof sink }, &got, &fin);
            }
        } else if (ev.kind == PROVEN_QUIC_EV_CLOSED) closed = true;
    }
    for (;;) {
        proven_byte_t out[1500];
        proven_size_t len = 0;
        proven_quic_addr_t to;
        if (proven_quic_conn_send(x->q, raw_now(), (proven_mem_mut_t){ out, sizeof out }, &len, &to) != PROVEN_OK || len == 0) break;
        (void)proven_net_udp_send_to(&x->udp, x->peer, (proven_mem_view_t){ out, len }, proven_net_deadline_in(100));
    }
    if (closed) { proven_quic_conn_destroy(x->q); x->q = NULL; }
}

// -----------------------------------------------------------------------------
// Cases
// -----------------------------------------------------------------------------

static void cases(void) {
    proven_http_event_server_t *server = server_on();
    proven_http_event_client_config_t ccfg = { .alloc = g_heap, .tls = g_client_tls, .http2 = true, .http3 = true, .decompress = true, .decompress_zstd = true, .idle_timeout_ms = 400 };
    proven_http_event_client_t *client = NULL;
    PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &ccfg, &client) == PROVEN_OK, "a client that may speak HTTP/3", "");
    static call_t c, many[40];

    PROVEN_TEST_ASSERT(start(client, &c, "GET", "/hello", mv(""), 0) == PROVEN_OK, "a request asked over HTTP/3 is started", "");
    c.plain = true;
    PROVEN_TEST_ASSERT(proven_http_event_request_protocol(c.request) == 3, "and says HTTP/3 from the start: it was asked for", "");
    spin_until(&c.done, 5000);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.status == 200 && c.got == 5 && memcmp(c.first, "hello", 5) == 0 && c.protocol == 3 && c.responses == 1 && !c.bad,
        "is answered over HTTP/3, with the user pointer it was given", "");
    PROVEN_TEST_ASSERT(g_served_h3 == 1 && proven_http_event_server_connections(server) == 1 && proven_http_event_client_requests(client) == 0, "the server agrees: one connection, HTTP/3", "");

    /* The same client, without asking: HTTP/2 over TCP, as before. */
    PROVEN_TEST_ASSERT(start_to(client, &c, g_port, "GET", "/hello", mv(""), 0, false) == PROVEN_OK, "a request that does not ask", "");
    c.plain = true;
    spin_until(&c.done, 5000);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.protocol == 2, "goes over HTTP/2: HTTP/3 only when asked, never by following Alt-Svc", "");

    for (int i = 0; i < 40; i++) { PROVEN_TEST_ASSERT(start(client, &many[i], "GET", i % 2 ? "/big/20000" : "/later", mv(""), 0) == PROVEN_OK, "a request is started", ""); many[i].plain = i % 2 == 0; }
    for (int i = 0; i < 40; i++) spin_until(&many[i].done, 8000);
    bool all = true;
    for (int i = 0; i < 40; i++) all = all && many[i].done && many[i].why == PROVEN_OK && many[i].status == 200 && many[i].dones == 1 && !many[i].bad && many[i].got == (proven_size_t)(i % 2 ? 20000 : 5) && many[i].protocol == 3;
    PROVEN_TEST_ASSERT(all, "forty at once, each answered whole and ended once, on the one QUIC connection", "");

    PROVEN_TEST_ASSERT(start(client, &c, "GET", "/big/3000000", mv(""), 0) == PROVEN_OK, "a download of 3 MB", "");
    spin_until(&c.done, 15000);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.got == 3000000 && !c.bad, "arrives whole", "");
    PROVEN_TEST_ASSERT(start(client, &c, "GET", "/zip/300000", mv(""), 0) == PROVEN_OK, "a compressed download", "");
    spin_until(&c.done, 8000);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.got == 300000 && !c.bad && strcmp(c.coding, "zstd") == 0, "is delivered decoded (zstd, as the client asked)", "");
    PROVEN_TEST_ASSERT(start(client, &c, "HEAD", "/big/5000", mv(""), 0) == PROVEN_OK, "HEAD", "");
    spin_until(&c.done, 5000);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.status == 200 && c.got == 0, "has a head and no body", "");
    PROVEN_TEST_ASSERT(start(client, &c, "GET", "/nothing", mv(""), 0) == PROVEN_OK, "an unknown path", "");
    c.plain = true;
    spin_until(&c.done, 5000);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.status == 404, "is a 404, delivered as any response", "");
    static proven_byte_t up[600000];
    for (proven_size_t i = 0; i < sizeof up; i++) up[i] = pattern(i);
    PROVEN_TEST_ASSERT(start(client, &c, "POST", "/echo", (proven_mem_view_t){ up, sizeof up }, 0) == PROVEN_OK, "a body held in memory, larger than a stream's send buffer", "");
    spin_until(&c.done, 8000);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.got == sizeof up && !c.bad, "is sent whole and echoed whole", "");
    for (int unknown = 0; unknown < 2; unknown++) {
        PROVEN_TEST_ASSERT(start(client, &c, "POST", "/echo", mv(""), unknown ? PROVEN_HTTP_EVENT_LENGTH_UNKNOWN : 400000) == PROVEN_OK, "a body to be written in pieces", "");
        c.to_send = 400000;
        call_pump(&c);
        spin_until(&c.done, 8000);
        PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.got == 400000 && !c.bad && c.writables > 0, unknown ? "of unknown length: written as room comes, echoed whole" : "of known length: written as room comes, echoed whole", "");
    }
    PROVEN_TEST_ASSERT(g_reentered == 0, "on_writable is never called from inside the caller's own write", "");
    for (int zip = 0; zip < 2; zip++) {
        PROVEN_TEST_ASSERT(start(client, &c, "GET", zip ? "/zip/200000" : "/big/600000", mv(""), 0) == PROVEN_OK, "a download that will be paused at its head", "");
        c.pause_once = true;
        spin_until(&c.done, 8000);
        PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.got == (proven_size_t)(zip ? 200000 : 600000) && !c.bad && !c.body_while_paused,
                           zip ? "a paused compressed download is held back, then delivered decoded" : "a paused download is held back by flow control, then delivered whole", "");
    }
    PROVEN_TEST_ASSERT(start(client, &c, "GET", "/who?a=1", mv(""), 0) == PROVEN_OK, "what the server is told", "");
    spin_until(&c.done, 5000);
    char want[40];
    snprintf(want, sizeof want, "127.0.0.1:%u", (unsigned)g_port);
    PROVEN_TEST_ASSERT(c.done && strcmp(g_who_target, "/who?a=1") == 0 && strcmp(g_who_authority, want) == 0, "the path and query, and the authority with its port", g_who_authority);

    PROVEN_TEST_ASSERT(start(client, &c, "GET", "/never", mv(""), 0) == PROVEN_OK, "a request that will be given up", "");
    spin(50);
    proven_http_event_request_abort(c.request);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_ERR_RESET && c.dones == 1, "an aborted request ends at once, with PROVEN_ERR_RESET", "");
    PROVEN_TEST_ASSERT(start(client, &c, "GET", "/hello", mv(""), 0) == PROVEN_OK, "the next request", "");
    c.plain = true;
    spin_until(&c.done, 5000);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.got == 5, "goes over the same connection", "");
    PROVEN_TEST_ASSERT(!g_done_in_start, "on_done was never called from inside proven_http_event_client_start", "");

    /* Idle: the client says goodbye, and the next request makes a new connection. */
    spin(1500);
    PROVEN_TEST_ASSERT(proven_http_event_server_connections(server) == 0, "left idle past the idle timeout, both connections are closed", "");
    PROVEN_TEST_ASSERT(start(client, &c, "GET", "/hello", mv(""), 0) == PROVEN_OK, "a request after that", "");
    c.plain = true;
    spin_until(&c.done, 5000);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.protocol == 3 && proven_http_event_server_connections(server) == 1, "makes a new connection and is answered", "");

    for (int i = 0; i < 5; i++) PROVEN_TEST_ASSERT(start(client, &many[i], "GET", "/never", mv(""), 0) == PROVEN_OK, "a request that will not be answered", "");
    spin(100);
    proven_http_event_client_destroy(client);
    bool cut = true;
    for (int i = 0; i < 5; i++) cut = cut && many[i].done && many[i].dones == 1 && many[i].why == PROVEN_ERR_RESET;
    PROVEN_TEST_ASSERT(cut, "a client destroyed with requests open tells each, once, that it was cut off", "");
    spin(300);
    proven_http_event_server_destroy(server);
    PROVEN_TEST_ASSERT(g_served == g_served_done, "and the server's handlers each saw their end", "");
}

int main(void) {
    PROVEN_TEST_SUITE("the event-driven HTTP client, speaking HTTP/3",
        "The event-driven client asked for HTTP/3, against the event-driven server on the same loop over real UDP: requests and bodies both ways, compression, pause, abort, idle, destroy; the client's checks; no fallback to TCP; an untrusted server; time. Against a scripted server: responses that break the rules, and a server that refuses h3. max_connections across protocols.",
        "Inspect src/proven/http_event_client_h3.c: r_submit and r_push for what leaves, c_event for what arrives, c_round for a round, c_check_time and c_arm for time, c_open for a connection.");

    g_heap = proven_heap_allocator();
    PROVEN_TEST_ASSERT(proven_loop_create(g_heap, &g_loop) == PROVEN_OK, "a loop", "");
    PROVEN_TEST_ASSERT(tp_build(), "the test's certificates and keys are issued", "");
    proven_cert_store_t *anchors = NULL, *strangers = NULL;
    PROVEN_TEST_ASSERT(proven_cert_store_create(g_heap, &anchors) == PROVEN_OK &&
                       proven_cert_store_add_pem(anchors, (proven_mem_view_t){ (const proven_byte_t *)TP_CA, strlen(TP_CA) }, NULL) == PROVEN_OK, "anchors", "");
    PROVEN_TEST_ASSERT(proven_cert_store_create(g_heap, &strangers) == PROVEN_OK, "an empty store", "");
    static const proven_u8str_view_t all[] = { { (const proven_byte_t *)"h2", 2 }, { (const proven_byte_t *)"http/1.1", 8 }, { (const proven_byte_t *)"h3", 2 } };
    static const proven_u8str_view_t h2h3[] = { { (const proven_byte_t *)"h2", 2 }, { (const proven_byte_t *)"h3", 2 } };
    proven_tls_options_t so = { .alloc = g_heap, .certificate_pem = { (const proven_byte_t *)TP_SERVER_P256, strlen(TP_SERVER_P256) },
                                .private_key_pem = { (const proven_byte_t *)TP_SERVER_P256_KEY, strlen(TP_SERVER_P256_KEY) }, .alpn = all, .alpn_count = 3 };
    proven_tls_options_t co = { .alloc = g_heap, .anchors = anchors, .alpn = h2h3, .alpn_count = 2 };
    proven_tls_options_t co_none = co; co_none.anchors = strangers;
    proven_tls_options_t co_h1 = { .alloc = g_heap, .anchors = anchors };
    PROVEN_TEST_ASSERT(proven_tls_config_create(&so, &g_server_tls) == PROVEN_OK && proven_tls_config_create(&co, &g_client_tls) == PROVEN_OK &&
                       proven_tls_config_create(&co_none, &g_client_tls_untrusting) == PROVEN_OK && proven_tls_config_create(&co_h1, &g_client_tls_h1) == PROVEN_OK, "TLS configurations", "");

    PROVEN_TEST_SECTION("making a client, and starting a request", "HTTP/3 needs TLS that offers h3, and a request asks for it on an https URL of a client that may speak it.", "Check proven_http_event_client_create and the http3 branch of proven_http_event_client_start_h1_.");
    {
        proven_http_event_client_t *cl = NULL;
        proven_http_event_client_config_t cfg = { .alloc = g_heap, .http3 = true };
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &cfg, &cl) == PROVEN_ERR_INVALID_ARG && !cl, "http3 without TLS is refused", "");
        cfg.tls = g_client_tls_h1;
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &cfg, &cl) == PROVEN_ERR_INVALID_ARG && !cl, "and over a TLS configuration that does not offer h3", "");
        cfg.tls = g_client_tls; cfg.http3 = false; cfg.http2 = true;
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &cfg, &cl) == PROVEN_ERR_INVALID_ARG && !cl, "and a configuration that offers h3 to a client without http3", "");
        cfg.http3 = true; cfg.http3_window_bytes = (proven_size_t)1 << 31;
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &cfg, &cl) == PROVEN_ERR_INVALID_ARG && !cl, "and a window above 2^31-1", "");
        cfg.http3_window_bytes = 0;
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &cfg, &cl) == PROVEN_OK && cl, "together they are a client", "");
        call_t c;
        memset(&c, 0, sizeof c);
        proven_http_event_request_options_t o = { .url = sv("http://127.0.0.1:1/x"), .http3 = true, .on = { .on_done = on_done }, .ctx = &c };
        PROVEN_TEST_ASSERT(proven_http_event_client_start(cl, &o, NULL) == PROVEN_ERR_INVALID_ARG, "an http URL asked over HTTP/3 is refused", "");
        proven_http_event_client_destroy(cl);
        proven_http_event_client_config_t plain = { .alloc = g_heap, .tls = g_client_tls_h1 };
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &plain, &cl) == PROVEN_OK, "a client without http3", "");
        o.url = sv("https://127.0.0.1:1/x");
        PROVEN_TEST_ASSERT(proven_http_event_client_start(cl, &o, NULL) == PROVEN_ERR_INVALID_ARG, "is refused a request that asks for HTTP/3", "");
        proven_http_event_client_destroy(cl);
    }

    PROVEN_TEST_SECTION("requests and responses", "Every case of the HTTP/2 client test that the event server can answer, asked over HTTP/3.", "Check r_submit, r_push, c_event and c_round.");
    cases();

    PROVEN_TEST_SECTION("no fallback", "A port where nothing answers over UDP, though TCP listens there: the request ends with PROVEN_ERR_TIMEOUT after connect_timeout_ms, and no TCP connection is tried. A server the client does not trust: PROVEN_ERR_UNTRUSTED.", "Check c_check_time for the connect timeout, and c_close_reason for the handshake's alerts.");
    {
        proven_net_listener_t tcp;
        proven_net_addr_t at;
        PROVEN_TEST_ASSERT(proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 16, &tcp, &at) == PROVEN_OK, "a TCP listener nobody serves", "");
        proven_http_event_client_config_t cfg = { .alloc = g_heap, .tls = g_client_tls, .http2 = true, .http3 = true, .connect_timeout_ms = 400 };
        proven_http_event_client_t *cl = NULL;
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &cfg, &cl) == PROVEN_OK, "a client", "");
        static call_t c;
        const proven_time_t t0 = proven_time_monotonic_now();
        PROVEN_TEST_ASSERT(start_to(cl, &c, at.port, "GET", "/hello", mv(""), 0, true) == PROVEN_OK, "a request asked over HTTP/3 to that port", "");
        spin_until(&c.done, 3000);
        const proven_time_t took = (proven_time_monotonic_now() - t0) / 1000000;
        PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_ERR_TIMEOUT && c.responses == 0 && took >= 350 && took < 2000, "ends with PROVEN_ERR_TIMEOUT after connect_timeout_ms", "");
        proven_net_conn_t got;
        proven_net_addr_t peer;
        PROVEN_TEST_ASSERT(proven_net_accept(&tcp, PROVEN_NET_DONT_WAIT, &got, &peer) != PROVEN_OK, "and no TCP connection was tried", "");
        (void)proven_net_listener_close(&tcp);
        proven_http_event_client_destroy(cl);

        proven_http_event_server_t *server = server_on();
        proven_http_event_client_config_t ucfg = { .alloc = g_heap, .tls = g_client_tls_untrusting, .http2 = true, .http3 = true };
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &ucfg, &cl) == PROVEN_OK, "a client that trusts no one", "");
        PROVEN_TEST_ASSERT(start(cl, &c, "GET", "/hello", mv(""), 0) == PROVEN_OK, "a request to the server", "");
        spin_until(&c.done, 5000);
        PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_ERR_UNTRUSTED && c.responses == 0 && g_served == 0, "ends with PROVEN_ERR_UNTRUSTED, and the request was never sent", "");
        proven_http_event_client_destroy(cl);
        spin(200);
        proven_http_event_server_destroy(server);
    }

    PROVEN_TEST_SECTION("a server that says goodbye", "Two requests on the server's only two streams, three waiting behind them; the server stops listening and listens again on the same port. Its GOAWAY sends the three waiting through retry_unsent to a new connection, where they are answered; without retry_unsent they end with PROVEN_ERR_AGAIN.", "Check the GOAWAY case of c_event, r_unsent and r_place.");
    for (int retry = 1; retry >= 0; --retry) {
        proven_http_event_server_config_t scfg = { .on = { srv_request, srv_body, srv_writable, srv_done }, .tls = g_server_tls, .http2 = true, .http3 = true, .max_concurrent_streams = 2 };
        proven_http_event_server_t *server = NULL;
        proven_net_addr_t at;
        PROVEN_TEST_ASSERT(proven_http_event_server_create(g_loop, &scfg, &server) == PROVEN_OK &&
                           proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "a server with two streams to a connection", "");
        g_port = at.port;
        g_served = g_served_done = g_served_h3 = 0;
        proven_http_event_client_config_t cfg = { .alloc = g_heap, .tls = g_client_tls, .http2 = true, .http3 = true, .retry_unsent = (proven_u32)retry };
        proven_http_event_client_t *cl = NULL;
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &cfg, &cl) == PROVEN_OK, "a client", "");
        static call_t held[2], waiting[3];
        for (int i = 0; i < 2; ++i) PROVEN_TEST_ASSERT(start(cl, &held[i], "GET", "/never", mv(""), 0) == PROVEN_OK, "a request the server holds", "");
        for (int i = 0; i < 3; ++i) { PROVEN_TEST_ASSERT(start(cl, &waiting[i], "GET", "/hello", mv(""), 0) == PROVEN_OK, "a request that waits for a stream", ""); waiting[i].plain = true; }
        for (int i = 0; i < 300 && g_served < 2; ++i) spin(5);
        spin(100);
        PROVEN_TEST_ASSERT(g_served == 2 && !waiting[0].done && !waiting[2].done, "two on the server's streams, three waiting", "");
        proven_http_event_server_stop_listening(server);
        PROVEN_TEST_ASSERT(proven_http_event_server_listen(server, at, NULL) == PROVEN_OK, "the server stops listening and listens again on the same port", "");
        for (int i = 0; i < 3; ++i) spin_until(&waiting[i].done, 5000);
        bool all = true;
        for (int i = 0; i < 3; ++i)
            all = all && waiting[i].done && waiting[i].dones == 1 && (retry ? waiting[i].why == PROVEN_OK && waiting[i].status == 200 && waiting[i].protocol == 3 && waiting[i].got == 5
                                                                             : waiting[i].why == PROVEN_ERR_AGAIN && waiting[i].responses == 0);
        PROVEN_TEST_ASSERT(all, retry ? "the GOAWAY sends the three waiting to a new connection, where they are answered" : "without retry_unsent the three waiting end with PROVEN_ERR_AGAIN, never sent", "");
        if (retry) PROVEN_TEST_ASSERT(g_served == 5 && proven_http_event_server_connections(server) == 2, "the server saw five requests, on two connections", "");
        PROVEN_TEST_ASSERT(!held[0].done && !held[1].done, "the two it held go on waiting for their answers", "");
        proven_http_event_client_destroy(cl);
        PROVEN_TEST_ASSERT(held[0].done && held[1].done && held[0].why == PROVEN_ERR_RESET, "and end with the client", "");
        spin(300);
        proven_http_event_server_destroy(server);
    }

    PROVEN_TEST_SECTION("time", "A request that is not answered: response_timeout_ms.", "Check r_expect and c_check_time.");
    {
        proven_http_event_server_t *server = server_on();
        proven_http_event_client_config_t cfg = { .alloc = g_heap, .tls = g_client_tls, .http2 = true, .http3 = true, .response_timeout_ms = 300 };
        proven_http_event_client_t *cl = NULL;
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &cfg, &cl) == PROVEN_OK, "a client", "");
        static call_t c, d;
        PROVEN_TEST_ASSERT(start(cl, &c, "GET", "/never", mv(""), 0) == PROVEN_OK, "a request nobody answers", "");
        spin_until(&c.done, 3000);
        PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_ERR_TIMEOUT, "ends with PROVEN_ERR_TIMEOUT after response_timeout_ms", "");
        PROVEN_TEST_ASSERT(start(cl, &d, "GET", "/hello", mv(""), 0) == PROVEN_OK, "the next request", "");
        d.plain = true;
        spin_until(&d.done, 3000);
        PROVEN_TEST_ASSERT(d.done && d.why == PROVEN_OK && d.got == 5, "is answered on the same connection", "");
        proven_http_event_client_destroy(cl);
        spin(200);
        proven_http_event_server_destroy(server);
    }

    PROVEN_TEST_SECTION("a server that breaks the rules", "A scripted server writes HTTP/3 by hand: an interim response before the answer, and one with nothing after it; a field name in capitals; no :status; a body shorter and longer than its Content-Length; a reset after part of a body; a request refused as not acted on, sent again; trailers; a br body. Each request ends as RFC 9114 section 4.1.2 asks, and the connection serves the next.", "Check c_response, the STREAM_RESET case of c_event and r_deliver in src/proven/http_event_client_h3.c, and the response checks of h3_conn.c (st_fault).");
    {
        static raw_t x;
        raw_start(&x, g_server_tls);
        proven_http_event_client_config_t cfg = { .alloc = g_heap, .tls = g_client_tls, .http2 = true, .http3 = true, .retry_unsent = 1, .decompress = true,
#ifndef PROVEN_NO_BROTLI
                                                  .decompress_brotli = true,
#endif
        };
        proven_http_event_client_t *cl = NULL;
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &cfg, &cl) == PROVEN_OK, "a client", "");
        static call_t c;
#define RAW_GET(path) do { PROVEN_TEST_ASSERT(start_to(cl, &c, x.port, "GET", path, mv(""), 0, true) == PROVEN_OK, "a request to the scripted server", ""); c.plain = true; spin_until(&c.done, 3000); } while (0)
        RAW_GET("/interim");
        PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.responses == 1 && c.status == 200 && c.got == 5 && c.last && memcmp(c.first, "hello", 5) == 0,
            "a 103 before the answer is passed over: one response, 200, and its body", "");
        RAW_GET("/interim-only");
        PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_ERR_RESET && c.responses == 0, "a stream that ends after an interim response: PROVEN_ERR_RESET, no response", "");
        RAW_GET("/upper");
        PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_ERR_RESET && c.responses == 0, "a field name in capitals: malformed, PROVEN_ERR_RESET", "");
        RAW_GET("/no-status");
        PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_ERR_RESET && c.responses == 0, "a head without :status: the same", "");
        RAW_GET("/short");
        PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_ERR_RESET && c.responses == 1 && c.got <= 50 && !c.last, "a body of 50 bytes under a Content-Length of 100: the head is delivered, the body never as ended, and the request ends PROVEN_ERR_RESET", "");
        RAW_GET("/long");
        PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_ERR_RESET && c.got <= 10 && !c.last, "a body of 50 under a Content-Length of 10, the stream left open: no more than 10 bytes delivered, PROVEN_ERR_RESET", "");
        RAW_GET("/reset");
        PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_ERR_RESET && c.responses == 1 && c.got == 10 && !c.last, "a reset 50 ms after the head and 10 bytes of the body: both delivered, then PROVEN_ERR_RESET", "");
        const int before = x.requests;
        RAW_GET("/rejected");
        PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.status == 200 && c.got == 5 && x.rejected == 1 && x.requests == before + 2,
            "H3_REQUEST_REJECTED: not acted on, so sent again (retry_unsent) and answered", "");
        RAW_GET("/trailers");
        PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.got == 5 && c.last, "trailers end the body and are not passed on", "");
#ifndef PROVEN_NO_BROTLI
        g_keep = &c;
        RAW_GET("/br");
        g_keep = NULL;
        static proven_byte_t text[3000];
        proven_size_t tn = 0;
        for (unsigned k = 0; tn < sizeof text; ++k) {
            char line[64];
            int len = snprintf(line, sizeof line, "line %u of the body, which repeats itself\n", k);
            for (int i = 0; i < len && tn < sizeof text; ++i) text[tn++] = (proven_byte_t)line[i];
        }
        PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && strcmp(c.coding, "br") == 0 && c.got == 3000 && memcmp(g_kept, text, 3000) == 0, "a br body is decoded: the 3,000 bytes libbrotli was given", "");
#endif
        RAW_GET("/hello");
        PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.got == 5 && x.conns == 1, "and every request went on one connection: a stream's error is the stream's", "");
#undef RAW_GET
        proven_http_event_client_destroy(cl);
        spin(100);
        raw_stop(&x);
    }

    PROVEN_TEST_SECTION("a server that refuses h3", "A QUIC server whose TLS offers only h2 ends the handshake with no_application_protocol (alert 120): the request ends with PROVEN_ERR_UNSUPPORTED, never sent.", "Check c_close_reason in src/proven/http_event_client_h3.c.");
    {
        static const proven_u8str_view_t h2only[] = { { (const proven_byte_t *)"h2", 2 } };
        proven_tls_options_t o = { .alloc = g_heap, .certificate_pem = { (const proven_byte_t *)TP_SERVER_P256, strlen(TP_SERVER_P256) },
                                   .private_key_pem = { (const proven_byte_t *)TP_SERVER_P256_KEY, strlen(TP_SERVER_P256_KEY) }, .alpn = h2only, .alpn_count = 1 };
        proven_tls_config_t *tls = NULL;
        PROVEN_TEST_ASSERT(proven_tls_config_create(&o, &tls) == PROVEN_OK, "a server TLS configuration that offers only h2", "");
        static raw_t x;
        raw_start(&x, tls);
        proven_http_event_client_config_t cfg = { .alloc = g_heap, .tls = g_client_tls, .http2 = true, .http3 = true };
        proven_http_event_client_t *cl = NULL;
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &cfg, &cl) == PROVEN_OK, "a client", "");
        static call_t c;
        PROVEN_TEST_ASSERT(start_to(cl, &c, x.port, "GET", "/hello", mv(""), 0, true) == PROVEN_OK, "a request over HTTP/3", "");
        spin_until(&c.done, 3000);
        PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_ERR_UNSUPPORTED && c.responses == 0 && x.requests == 0, "ends with PROVEN_ERR_UNSUPPORTED, never sent", "");
        proven_http_event_client_destroy(cl);
        spin(100);
        raw_stop(&x);
        proven_tls_config_destroy(tls);
    }

    PROVEN_TEST_SECTION("max_connections across protocols", "A client of one connection: an HTTP/2 request waits while an HTTP/3 request is on the only one, and has it when that is done; the same the other way round.", "Check proven_http_event_client_room_ and proven_http_event_client_freed_ in src/proven/http_event_client.c, r_place and c_round's call to proven_http_event_client_freed_ in http_event_client_h3.c.");
    for (int first3 = 1; first3 >= 0; --first3) {
        proven_http_event_server_t *server = server_on();
        proven_http_event_client_config_t cfg = { .alloc = g_heap, .tls = g_client_tls, .http2 = true, .http3 = true, .max_connections = 1 };
        proven_http_event_client_t *cl = NULL;
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &cfg, &cl) == PROVEN_OK, "a client of one connection", "");
        static call_t busy, next;
        g_order = 0;
        PROVEN_TEST_ASSERT(start_to(cl, &busy, g_port, "GET", "/later", mv(""), 0, first3 == 1) == PROVEN_OK, "a request answered after 60 ms", "");
        busy.plain = true;
        for (int i = 0; i < 300 && g_served < 1; ++i) spin(5);
        PROVEN_TEST_ASSERT(start_to(cl, &next, g_port, "GET", "/hello", mv(""), 0, first3 == 0) == PROVEN_OK, "a request over the other protocol", "");
        next.plain = true;
        spin_until(&next.done, 5000);
        PROVEN_TEST_ASSERT(busy.done && busy.why == PROVEN_OK && busy.protocol == (first3 ? 3 : 2) && next.done && next.why == PROVEN_OK && next.protocol == (first3 ? 2 : 3) &&
                           next.got == 5 && busy.order == 1 && next.order == 2 && g_served == 2,
            first3 ? "an HTTP/2 request waits for the HTTP/3 connection to be done, then has the place" : "an HTTP/3 request waits for the HTTP/2 connection to be done, then has the place", "");
        proven_http_event_client_destroy(cl);
        spin(200);
        proven_http_event_server_destroy(server);
    }

    proven_tls_config_destroy(g_server_tls); proven_tls_config_destroy(g_client_tls); proven_tls_config_destroy(g_client_tls_untrusting); proven_tls_config_destroy(g_client_tls_h1);
    proven_cert_store_destroy(anchors); proven_cert_store_destroy(strangers);
    proven_loop_destroy(g_loop);
    PROVEN_TEST_PASS("the event-driven client speaks HTTP/3 when asked, to the event-driven server, with no fallback.");
    return 0;
}

#else

int main(void) {
    PROVEN_TEST_SUITE("the event-driven HTTP client, speaking HTTP/3", "Built with PROVEN_NO_HTTP3: nothing to test.", "");
    PROVEN_TEST_PASS("skipped: PROVEN_NO_HTTP3");
    return 0;
}

#endif
