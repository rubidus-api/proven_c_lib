#include "proven.h"
#include "proven_test.h"
#include "../src/proven/proven_internal_h3.h"
#include "test_unit_tls_pki.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The event-driven server speaking HTTP/3, over real UDP on loopback.
 *
 * The server runs on a loop of its own, on a thread; the handlers are an HTTP/1.1 server's,
 * unchanged. The client is in the test's thread: the QUIC and HTTP/3 engines of this library
 * (quic_conn.c, h3_conn.c) on a UDP socket of its own, pumped by hand.
 */

#ifndef PROVEN_NO_HTTP3

#define H(n, v) { .name = { (const proven_byte_t *)(n), sizeof(n) - 1 }, .value = { (const proven_byte_t *)(v), sizeof(v) - 1 } }
static proven_u8str_view_t sv(const char *s) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }

static proven_allocator_t g_heap;
static proven_loop_t *g_loop;
static proven_http_event_server_t *g_server;
static proven_job_sys_t *g_threads;
static proven_job_group_t g_running;
static proven_tls_config_t *g_server_tls, *g_client_tls, *g_client_tls_h2h3, *g_tcp_tls_h1, *g_tcp_tls_h2;
static atomic_int g_in_write, g_reentered;
static atomic_int g_requests, g_done_ok, g_done_reset, g_done_timeout, g_done_other, g_writable, g_bodies_while_paused, g_dispatch_bad, g_protocol;

/* A loopback port where both a TCP listener and a UDP socket can be had, the UDP one kept open in
 * `held`. Chosen below every system's ephemeral range (Linux 32768-60999, FreeBSD 10000-65535,
 * Windows and macOS 49152-65535), so no test that binds port 0 or connects can be given it while
 * this one closes and reopens the TCP side. */
static bool udp_port_held(proven_net_udp_t *held, proven_net_addr_t *at) {
    for (proven_u16 port = 7100; port < 9900; ++port) {
        const proven_net_addr_t a = proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, port);
        if (proven_net_udp_open(a, held, NULL) != PROVEN_OK) continue;
        proven_net_listener_t l;
        if (proven_net_listen(a, 16, &l, NULL) == PROVEN_OK) {
            (void)proven_net_listener_close(&l);
            *at = a;
            return true;
        }
        (void)proven_net_udp_close(held);
    }
    return false;
}

static proven_byte_t pat(proven_size_t i) { return (proven_byte_t)('a' + (i * 7 + i / 251) % 26); }

// -----------------------------------------------------------------------------
// The application: an ordinary handler, written as for HTTP/1.1
// -----------------------------------------------------------------------------

typedef struct {
    proven_http_stream_t *stream;
    proven_loop_timer_t timer;
    proven_size_t sent, total, received;
    proven_byte_t *echo; proven_size_t echo_cap;
    bool paused, sink;
    char path[48];
} app_t;

static void app_pump(app_t *a) {
    static proven_byte_t piece[8192];
    while (a->sent < a->total) {
        proven_size_t n = a->total - a->sent < sizeof piece ? a->total - a->sent : sizeof piece;
        for (proven_size_t i = 0; i < n; i++) piece[i] = pat(a->sent + i);
        atomic_fetch_add(&g_in_write, 1);
        proven_result_size_t w = proven_http_stream_write(a->stream, (proven_mem_view_t){ piece, n });
        atomic_fetch_sub(&g_in_write, 1);
        if (w.err != PROVEN_OK) return;
        a->sent += w.value;
        if (w.value < n) return;                   /* refused: on_writable says when */
    }
    (void)proven_http_stream_end(a->stream);
}

static void later_fire(void *ctx) {
    app_t *a = ctx;
    static const proven_http_header_t plain[] = { H("Content-Type", "text/plain") };
    if (proven_http_stream_begin(a->stream, 200, plain, 1, a->total) == PROVEN_OK) app_pump(a);
}
static void resume_fire(void *ctx) { app_t *a = ctx; a->paused = false; proven_http_stream_resume(a->stream); }


static bool path_is(const proven_http_request_t *req, const char *p) { return req->target.size == strlen(p) && memcmp(req->target.ptr, p, req->target.size) == 0; }
static bool path_starts(const proven_http_request_t *req, const char *p) { return req->target.size >= strlen(p) && memcmp(req->target.ptr, p, strlen(p)) == 0; }
static void ws_never(void *ctx, proven_ws_stream_t *ws, bool text, proven_mem_view_t piece, bool first, bool last) { (void)ctx; (void)ws; (void)text; (void)piece; (void)first; (void)last; }

static void on_request(void *ctx, proven_http_stream_t *s, const proven_http_request_t *req) {
    (void)ctx;
    atomic_fetch_add(&g_requests, 1);
    app_t *a = calloc(1, sizeof *a);
    a->stream = s;
    snprintf(a->path, sizeof a->path, "%.*s", (int)(req->target.size < 47 ? req->target.size : 47), (const char *)req->target.ptr);
    proven_http_stream_set_user(s, a);
    if (proven_http_stream_user(s) != a || req->version_minor != 1 || proven_http_stream_peer(s).family != PROVEN_NET_FAMILY_IPV4) atomic_fetch_add(&g_dispatch_bad, 1);
    atomic_store(&g_protocol, proven_http_stream_protocol(s));
    static const proven_http_header_t plain[] = { H("Content-Type", "text/plain"), H("Keep-Alive", "timeout=5") };

    if (path_is(req, "/hello")) {
        (void)proven_http_stream_respond(s, 200, plain, 2, (proven_mem_view_t){ (const proven_byte_t *)"hello", 5 });
    } else if (path_is(req, "/proto")) {
        char p[2] = { (char)('0' + proven_http_stream_protocol(s)), 0 };
        if (proven_http_header_count(req->headers, req->header_count, sv("Host")) != 1) atomic_fetch_add(&g_dispatch_bad, 1);
        (void)proven_http_stream_respond(s, 200, plain, 1, (proven_mem_view_t){ (const proven_byte_t *)p, 1 });
    } else if (path_is(req, "/own-alt")) {
        static const proven_http_header_t own[] = { H("Alt-Svc", "clear") };
        (void)proven_http_stream_respond(s, 200, own, 1, (proven_mem_view_t){ (const proven_byte_t *)"own", 3 });
    } else if (path_starts(req, "/big/")) {
        a->total = (proven_size_t)strtoul(a->path + 5, NULL, 10);
        if (proven_http_stream_begin(s, 200, plain, 1, a->total) == PROVEN_OK) app_pump(a);
    } else if (path_starts(req, "/whole/")) {
        static proven_byte_t body[400000];
        if (body[1] == 0) for (proven_size_t i = 0; i < sizeof body; i++) body[i] = pat(i);
        const proven_size_t n = (proven_size_t)strtoul(a->path + 7, NULL, 10);
        (void)proven_http_stream_respond(s, 200, plain, 1, (proven_mem_view_t){ body, n < sizeof body ? n : sizeof body });
    } else if (path_starts(req, "/later/")) {
        a->total = (proven_size_t)strtoul(a->path + 7, NULL, 10);
        proven_loop_timer_set(g_loop, &a->timer, 200, later_fire, a);
    } else if (path_starts(req, "/stream/")) {
        a->total = (proven_size_t)strtoul(a->path + 8, NULL, 10);
        if (proven_http_stream_begin(s, 200, plain, 1, PROVEN_HTTP_EVENT_LENGTH_UNKNOWN) == PROVEN_OK) app_pump(a);
    } else if (path_starts(req, "/zip/")) {
        a->total = (proven_size_t)strtoul(a->path + 5, NULL, 10);
        proven_http_stream_compress(s);
        if (proven_http_stream_begin(s, 200, plain, 1, a->total) == PROVEN_OK) app_pump(a);
    } else if (path_is(req, "/zip-whole")) {
        static proven_byte_t body[20000];
        for (proven_size_t i = 0; i < sizeof body; i++) body[i] = pat(i);
        proven_http_stream_compress(s);
        (void)proven_http_stream_respond(s, 200, plain, 1, (proven_mem_view_t){ body, sizeof body });
    } else if (path_is(req, "/body-sign")) {
        const bool chunked = proven_http_header_has_token(req->headers, req->header_count, sv("Transfer-Encoding"), sv("chunked"));
        const bool length = proven_http_header_count(req->headers, req->header_count, sv("Content-Length")) > 0;
        const char *sign = chunked ? "chunked" : length ? "length" : "none";
        if (chunked || length) { a->sink = true; snprintf(a->path, sizeof a->path, "%s", sign); }
        else (void)proven_http_stream_respond(s, 200, NULL, 0, (proven_mem_view_t){ (const proven_byte_t *)sign, strlen(sign) });
    } else if (path_is(req, "/echo") || path_is(req, "/pause")) {
        if (path_is(req, "/pause")) {
            a->paused = true;
            proven_http_stream_pause(s);
            proven_loop_timer_set(g_loop, &a->timer, 150, resume_fire, a);
        }
    } else if (path_is(req, "/sink")) {
        a->sink = true;

    } else if (path_is(req, "/ws")) {
        proven_ws_event_config_t wcfg = { .on = { .on_message = ws_never } };
        proven_ws_stream_t *ws = NULL;
        if (proven_ws_event_accept(s, req, &wcfg, &ws) == PROVEN_OK || ws != NULL) atomic_fetch_add(&g_dispatch_bad, 1);
        (void)proven_http_stream_respond(s, 400, NULL, 0, (proven_mem_view_t){ (const proven_byte_t *)"no", 2 });
    } else if (path_is(req, "/never")) {
        /* The handler is thinking, for ever. */
    } else if (path_is(req, "/abort")) {
        proven_http_stream_abort(s);
    } else if (path_is(req, "/bad-header")) {
        static const proven_http_header_t bad[] = { H("Content-Length", "5") };
        if (proven_http_stream_begin(s, 200, bad, 1, 5) != PROVEN_ERR_INVALID_ARG) atomic_fetch_add(&g_dispatch_bad, 1);
        (void)proven_http_stream_respond(s, 200, NULL, 0, (proven_mem_view_t){ (const proven_byte_t *)"ok", 2 });
    } else if (path_is(req, "/short")) {
        if (proven_http_stream_begin(s, 200, plain, 1, 10) == PROVEN_OK) {
            (void)proven_http_stream_write(s, (proven_mem_view_t){ (const proven_byte_t *)"abc", 3 });
            if (proven_http_stream_end(s) != PROVEN_ERR_INVALID_FORMAT) atomic_fetch_add(&g_dispatch_bad, 1);
        }
    } else {
        (void)proven_http_stream_respond(s, 404, NULL, 0, (proven_mem_view_t){ (const proven_byte_t *)"no", 2 });
    }
}

static void on_body(void *ctx, proven_http_stream_t *s, proven_mem_view_t piece, bool last) {
    (void)ctx;
    app_t *a = proven_http_stream_user(s);
    if (!a) return;
    if (a->paused) atomic_fetch_add(&g_bodies_while_paused, 1);
    if (!a->sink) {
        if (a->received + piece.size > a->echo_cap) { a->echo_cap = (a->received + piece.size) * 2 + 1024; a->echo = realloc(a->echo, a->echo_cap); }
        if (piece.size) memcpy(a->echo + a->received, piece.ptr, piece.size);
    }
    a->received += piece.size;
    if (!last) return;
    if (a->sink) {
        char text[32];
        int n = strcmp(a->path, "chunked") == 0 || strcmp(a->path, "length") == 0 ? snprintf(text, sizeof text, "%s", a->path) : snprintf(text, sizeof text, "got %zu", a->received);
        (void)proven_http_stream_respond(s, 200, NULL, 0, (proven_mem_view_t){ (const proven_byte_t *)text, (proven_size_t)n });
    } else {
        (void)proven_http_stream_respond(s, 200, NULL, 0, (proven_mem_view_t){ a->echo, a->received });
    }
}

static void on_writable(void *ctx, proven_http_stream_t *s) {
    (void)ctx;
    atomic_fetch_add(&g_writable, 1);
    if (atomic_load(&g_in_write) > 0) atomic_fetch_add(&g_reentered, 1);
    app_t *a = proven_http_stream_user(s);
    if (a) app_pump(a);
}

static void on_done(void *ctx, proven_http_stream_t *s, proven_err_t why) {
    (void)ctx;
    app_t *a = proven_http_stream_user(s);
    if (why == PROVEN_OK) atomic_fetch_add(&g_done_ok, 1);
    else if (why == PROVEN_ERR_RESET || why == PROVEN_ERR_EOF) atomic_fetch_add(&g_done_reset, 1);
    else if (why == PROVEN_ERR_TIMEOUT) atomic_fetch_add(&g_done_timeout, 1);
    else atomic_fetch_add(&g_done_other, 1);
    if (a) { proven_loop_timer_cancel(g_loop, &a->timer); free(a->echo); free(a); }
}

// -----------------------------------------------------------------------------
// The server, on a thread of its own
// -----------------------------------------------------------------------------

static void run_loop(void *arg) { (void)arg; (void)proven_loop_run(g_loop); }

static atomic_int g_ran;
static atomic_ulong g_conn_count;
static atomic_int g_drained_calls, g_drained_why;
static atomic_uint g_drain_grace;
static void do_count(void *ctx) { (void)ctx; atomic_store(&g_conn_count, (unsigned long)proven_http_event_server_connections(g_server)); atomic_fetch_add(&g_ran, 1); }
static void do_destroy(void *ctx) { (void)ctx; proven_http_event_server_destroy(g_server); g_server = NULL; atomic_fetch_add(&g_ran, 1); }
static void do_stop_listening(void *ctx) { (void)ctx; proven_http_event_server_stop_listening(g_server); atomic_fetch_add(&g_ran, 1); }
static void drained(void *ctx, proven_err_t why) { (void)ctx; atomic_store(&g_drained_why, (int)why); atomic_fetch_add(&g_drained_calls, 1); }
static void do_shutdown(void *ctx) {
    (void)ctx;
    if (proven_http_event_server_shutdown(g_server, atomic_load(&g_drain_grace), drained, NULL) != PROVEN_OK) atomic_store(&g_drained_why, -1);
    atomic_fetch_add(&g_ran, 1);
}
static proven_u16 g_relisten_port;
static atomic_int g_relisten_err;
static void do_relisten(void *ctx) {
    (void)ctx;
    atomic_store(&g_relisten_err, (int)proven_http_event_server_listen(g_server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, g_relisten_port), NULL));
    atomic_fetch_add(&g_ran, 1);
}
static proven_size_t g_set_n;
static atomic_int g_set_err;
static void do_set_streams(void *ctx) { (void)ctx; atomic_store(&g_set_err, (int)proven_http_event_server_set_max_streams(g_server, g_set_n)); atomic_fetch_add(&g_ran, 1); }
static void on_loop(proven_loop_fn fn) {
    int before = atomic_load(&g_ran);
    PROVEN_TEST_ASSERT(proven_loop_post(g_loop, fn, NULL) == PROVEN_OK, "posted to the loop", "");
    for (int i = 0; i < 4000 && atomic_load(&g_ran) == before; ++i) proven_time_sleep(1);
}
static unsigned long connections(void) { on_loop(do_count); return atomic_load(&g_conn_count); }
/* A closed QUIC connection is held for its closing period (three probe timeouts) before it goes. */
static bool connections_fall_to(unsigned long n) {
    for (int i = 0; i < 200; ++i) { if (connections() == n) return true; proven_time_sleep(10); }
    return false;
}
static int done_total(void) { return atomic_load(&g_done_ok) + atomic_load(&g_done_reset) + atomic_load(&g_done_timeout) + atomic_load(&g_done_other); }
static void reset_counts(void) {
    atomic_store(&g_requests, 0); atomic_store(&g_done_ok, 0); atomic_store(&g_done_reset, 0); atomic_store(&g_done_timeout, 0); atomic_store(&g_done_other, 0);
    atomic_store(&g_writable, 0);
}

static proven_u16 server_start(proven_http_event_server_config_t cfg) {
    PROVEN_TEST_ASSERT(proven_loop_create(g_heap, &g_loop) == PROVEN_OK, "a loop", "");
    cfg.on = (proven_http_event_callbacks_t){ .on_request = on_request, .on_body = on_body, .on_writable = on_writable, .on_done = on_done };
    cfg.http2 = true;
    cfg.http3 = true;
    cfg.tls = g_server_tls;
    PROVEN_TEST_ASSERT(proven_http_event_server_create(g_loop, &cfg, &g_server) == PROVEN_OK, "a server that speaks all three", "");
    proven_net_addr_t bound;
    PROVEN_TEST_ASSERT(proven_http_event_server_listen(g_server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &bound) == PROVEN_OK, "listening", "");
    proven_job_group_init(&g_running);
    PROVEN_TEST_ASSERT(proven_job_group_submit(g_threads, &g_running, run_loop, NULL) == PROVEN_OK, "the loop's thread", "");
    reset_counts();
    return bound.port;
}

static void server_stop(void) {
    if (g_server) on_loop(do_destroy);
    proven_loop_stop(g_loop);
    proven_job_group_wait(g_threads, &g_running);
    proven_loop_destroy(g_loop);
    g_loop = NULL;
}

// -----------------------------------------------------------------------------
// The client: QUIC and HTTP/3 engines on a UDP socket
// -----------------------------------------------------------------------------

#define CL_REQS 512
typedef struct {
    proven_u64 id;
    int status, interim;
    proven_size_t got, up, up_sent;
    bool end, reset, bad, zipped, has_length, has_date, alt, up_done, hop;
    proven_u64 code;
    char coding[16], alt_value[32];
    proven_byte_t first[16];
} rq_t;

typedef struct {
    proven_net_udp_t udp;
    proven_net_addr_t server;
    proven_quic_conn_t *q;
    proven_h3_conn_t *h;
    proven_tls_session_t session;
    bool established, closed, remote_close, mute;
    int first_type;                    /* the first datagram's first byte, high nibble: 0xf for a Retry; -1 before one has come */
    proven_u64 close_code;
    int goaways;
    proven_u64 goaway_id;
    rq_t r[CL_REQS];
    int n;
} cl_t;

static proven_u64 us_now(void) { return (proven_u64)proven_time_monotonic_now() / 1000u; }

static proven_quic_addr_t qaddr(const proven_net_addr_t *a) {
    proven_quic_addr_t q;
    memset(&q, 0, sizeof q);
    q.len = 7;
    q.b[0] = (proven_byte_t)a->family; q.b[1] = (proven_byte_t)(a->port >> 8); q.b[2] = (proven_byte_t)a->port;
    memcpy(q.b + 3, a->ip, 4);
    return q;
}

static bool cl_open(cl_t *c, proven_u16 port) {
    memset(c, 0, sizeof *c);
    c->server = proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, port);
    c->first_type = -1;
    if (proven_net_udp_open(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &c->udp, NULL) != PROVEN_OK) return false;
    const proven_quic_config_t qc = { .alloc = g_heap, .tls = g_client_tls };
    const proven_quic_addr_t sa = qaddr(&c->server);
    if (proven_quic_conn_client_create(&qc, sv("example.test"), &c->session, &sa, us_now(), &c->q) != PROVEN_OK) return false;
    const proven_h3_config_t hc = { .alloc = g_heap, .server = false, .max_field_section_bytes = 65536 };
    return proven_h3_conn_create(&hc, c->q, &c->h) == PROVEN_OK;
}

static void cl_pump(cl_t *c, proven_u32 ms);

static void cl_close(cl_t *c) {
    if (c->closed) cl_pump(c, 0);                  /* what the close owes the peer */
    proven_h3_conn_destroy(c->h);
    proven_quic_conn_destroy(c->q);
    (void)proven_net_udp_close(&c->udp);
}

static rq_t *cl_find(cl_t *c, proven_u64 id) {
    for (int i = 0; i < c->n; ++i) if (c->r[i].id == id && !c->r[i].end) return &c->r[i];
    return NULL;
}

static void cl_upload(cl_t *c, rq_t *r) {
    static proven_byte_t piece[16384];
    while (!r->up_done && !r->end) {
        proven_size_t n = r->up - r->up_sent < sizeof piece ? r->up - r->up_sent : sizeof piece;
        for (proven_size_t i = 0; i < n; ++i) piece[i] = pat(r->up_sent + i);
        proven_size_t taken = 0;
        if (proven_h3_send_data(c->h, r->id, (proven_mem_view_t){ piece, n }, r->up_sent + n == r->up, &taken) != PROVEN_OK) { r->up_done = true; return; }
        r->up_sent += taken;
        if (r->up_sent == r->up) r->up_done = true;
        if (taken < n) return;
    }
}

static void cl_event(cl_t *c, const proven_h3_event_t *ev) {
    rq_t *r = ev->kind == PROVEN_H3_EV_HEADERS || ev->kind == PROVEN_H3_EV_DATA || ev->kind == PROVEN_H3_EV_TRAILERS || ev->kind == PROVEN_H3_EV_STREAM_RESET ||
              ev->kind == PROVEN_H3_EV_STREAM_WRITABLE || ev->kind == PROVEN_H3_EV_STOP_SENDING ? cl_find(c, ev->stream_id) : NULL;
    switch (ev->kind) {
    case PROVEN_H3_EV_HEADERS:
        if (!r) break;
        if (ev->status < 200) { r->interim++; break; }
        r->status = ev->status;
        for (proven_size_t i = 0; i < ev->field_count; ++i) {
            const proven_http_header_t *f = &ev->fields[i];
            if (f->name.size == 14 && memcmp(f->name.ptr, "content-length", 14) == 0) r->has_length = true;
            if (f->name.size == 4 && memcmp(f->name.ptr, "date", 4) == 0) r->has_date = true;
            if (f->name.size == 7 && memcmp(f->name.ptr, "alt-svc", 7) == 0) r->alt = true;
            if (f->name.size == 10 && memcmp(f->name.ptr, "keep-alive", 10) == 0) r->hop = true;      /* a field of HTTP/1.1's, not to be sent */
            if (f->name.size == 16 && memcmp(f->name.ptr, "content-encoding", 16) == 0) {
                r->zipped = true;
                snprintf(r->coding, sizeof r->coding, "%.*s", (int)f->value.size, (const char *)f->value.ptr);
            }
        }
        if (ev->end_stream) r->end = true;
        break;
    case PROVEN_H3_EV_DATA:
        if (!r) break;
        for (proven_size_t i = 0; i < ev->data.size; ++i) {
            if (r->got + i < sizeof r->first) r->first[r->got + i] = ev->data.ptr[i];
            if (!r->zipped && ev->data.ptr[i] != pat(r->got + i)) r->bad = true;
        }
        r->got += ev->data.size;
        if (ev->end_stream) r->end = true;
        break;
    case PROVEN_H3_EV_TRAILERS: if (r) r->end = true; break;
    case PROVEN_H3_EV_STREAM_RESET: if (r) { r->reset = true; r->code = ev->code; r->end = true; } break;
    case PROVEN_H3_EV_STREAM_WRITABLE: if (r) cl_upload(c, r); break;
    case PROVEN_H3_EV_GOAWAY: c->goaways++; c->goaway_id = ev->id; break;
    case PROVEN_H3_EV_QUIC: if (ev->quic.kind == PROVEN_QUIC_EV_ESTABLISHED) c->established = true; break;
    case PROVEN_H3_EV_CLOSED: c->closed = true; c->close_code = ev->code; c->remote_close = ev->remote; break;
    default: break;
    }
}

/* One round: send what is queued, wait at most `ms` for datagrams, take every event. */
static void cl_pump(cl_t *c, proven_u32 ms) {
    static proven_byte_t dg[65536];
    for (int k = 0; k < 256; ++k) {                /* after the close too: its CONNECTION_CLOSE goes out */
        proven_size_t len = 0;
        proven_quic_addr_t to;
        if (proven_quic_conn_send(c->q, us_now(), (proven_mem_mut_t){ dg, sizeof dg }, &len, &to) != PROVEN_OK || len == 0) break;
        if (!c->mute) (void)proven_net_udp_send_to(&c->udp, c->server, (proven_mem_view_t){ dg, len }, proven_net_deadline_in(100));
    }
    proven_u64 now = us_now(), until = now + (proven_u64)ms * 1000;
    const proven_u64 d = proven_quic_conn_deadline(c->q);
    if (d < until) until = d;
    const proven_net_deadline_t wait = until <= now ? PROVEN_NET_DONT_WAIT : proven_time_monotonic_now() + (proven_time_t)(until - now) * 1000;
    proven_net_addr_t from;
    const proven_quic_addr_t sa = qaddr(&c->server);
    proven_result_size_t r = proven_net_udp_recv_from(&c->udp, (proven_mem_mut_t){ dg, sizeof dg }, &from, wait);
    while (r.err == PROVEN_OK) {
        if (c->first_type < 0 && r.value > 0) c->first_type = dg[0] >> 4;
        (void)proven_quic_conn_receive(c->q, (proven_mem_view_t){ dg, r.value }, &sa, us_now());
        r = proven_net_udp_recv_from(&c->udp, (proven_mem_mut_t){ dg, sizeof dg }, &from, PROVEN_NET_DONT_WAIT);
    }
    now = us_now();
    if (proven_quic_conn_deadline(c->q) <= now) proven_quic_conn_timeout(c->q, now);
    proven_h3_event_t ev;
    while (proven_h3_conn_poll(c->h, &ev)) cl_event(c, &ev);
}

static proven_u64 ms_now(void) { return (proven_u64)proven_time_monotonic_now() / 1000000u; }

static bool cl_connect(cl_t *c, proven_u16 port) {
    if (!cl_open(c, port)) return false;
    for (const proven_u64 end = ms_now() + 4000; ms_now() < end && !c->established && !c->closed;) cl_pump(c, 10);
    return c->established;
}

/* A request; its body, if any, is sent as the engine takes it. -1 when it could not be made. */
static int cl_request(cl_t *c, const char *method, const char *path, proven_size_t body, const proven_http_header_t *extra, proven_size_t extra_count, bool length) {
    if (c->n == CL_REQS) return -1;
    char lb[24];
    snprintf(lb, sizeof lb, "%zu", body);
    proven_http_header_t f[12] = { { sv(":method"), sv(method) }, { sv(":scheme"), sv("https") }, { sv(":authority"), sv("example.test") }, { sv(":path"), sv(path) } };
    proven_size_t nf = 4;
    if (body && length) f[nf++] = (proven_http_header_t){ sv("content-length"), sv(lb) };
    for (proven_size_t i = 0; i < extra_count && nf < 12; ++i) f[nf++] = extra[i];
    proven_u64 id = 0;
    if (proven_h3_submit_request(c->h, f, nf, body == 0, &id) != PROVEN_OK) return -1;
    rq_t *r = &c->r[c->n];
    memset(r, 0, sizeof *r);
    r->id = id;
    r->up = body;
    r->up_done = body == 0;
    cl_upload(c, r);
    return c->n++;
}

static bool cl_wait(cl_t *c, int from, int to, int ms) {
    for (const proven_u64 end = ms_now() + (proven_u64)ms; ms_now() < end;) {
        bool all = true;
        for (int i = from; i < to; ++i) all = all && c->r[i].end;
        if (all) return true;
        if (c->closed) return false;
        cl_pump(c, 5);
    }
    return false;
}

static bool cl_wait_closed(cl_t *c, int ms) {
    for (const proven_u64 end = ms_now() + (proven_u64)ms; ms_now() < end && !c->closed;) cl_pump(c, 5);
    if (c->closed) cl_pump(c, 0);                  /* a close of this side's own: its CONNECTION_CLOSE goes out */
    return c->closed;
}

/* Pump for `ms` of the clock's time. */
static void cl_run(cl_t *c, int ms) {
    for (const proven_u64 end = ms_now() + (proven_u64)ms; ms_now() < end && !c->closed;) cl_pump(c, 5);
}

static bool settled(int done) {
    for (int i = 0; i < 6000; ++i) { if (done_total() >= done) return true; proven_time_sleep(1); }
    return false;
}

static bool body_is(const rq_t *r, const char *text) { return r->got == strlen(text) && memcmp(r->first, text, r->got) == 0; }

// -----------------------------------------------------------------------------
// A TCP client, for what HTTP/1.1 and HTTP/2 responses say about HTTP/3
// -----------------------------------------------------------------------------

/* One request over TLS: HTTP/1.1 as text, or HTTP/2 through h2.h's engine. The response's
 * Alt-Svc value into `alt` (empty for none). */
static bool tcp_alt_svc(proven_u16 port, bool h2, const char *path, char alt[64]) {
    alt[0] = 0;
    proven_net_conn_t sock;
    if (proven_net_connect(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, port), proven_net_deadline_in(5000), &sock) != PROVEN_OK) return false;
    proven_transport_t t = proven_net_conn_transport(&sock);
    if (proven_tls_transport_client(t, h2 ? g_tcp_tls_h2 : g_tcp_tls_h1, sv("example.test"), NULL, proven_net_deadline_in(5000), &t) != PROVEN_OK) return false;
    bool ok = false;
    static proven_byte_t in[65536];
    if (!h2) {
        char req[128];
        const int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n", path);
        if (proven_transport_write_all(t, (proven_mem_view_t){ (const proven_byte_t *)req, (proven_size_t)n }, proven_net_deadline_in(5000)).err != PROVEN_OK) goto out;
        proven_size_t len = 0;
        for (;;) {
            proven_result_size_t r = proven_transport_read(t, (proven_mem_mut_t){ in + len, sizeof in - 1 - len }, proven_net_deadline_in(5000));
            if (r.err != PROVEN_OK || r.value == 0) break;
            len += r.value;
        }
        in[len] = 0;
        ok = len > 0;
        const char *a = strstr((const char *)in, "\r\nAlt-Svc: ");
        if (a) { a += 11; const char *e = strstr(a, "\r\n"); snprintf(alt, 64, "%.*s", e ? (int)(e - a) : 0, a); }
    } else {
        proven_h2_config_t cfg = { .alloc = g_heap, .max_header_list_bytes = 65536 };
        proven_h2_conn_t *e = NULL;
        if (proven_h2_conn_create(&cfg, &e) != PROVEN_OK) goto out;
        const proven_http_header_t f[] = { { sv(":method"), sv("GET") }, { sv(":scheme"), sv("https") }, { sv(":authority"), sv("example.test") }, { sv(":path"), sv(path) } };
        proven_u32 id = 0;
        if (proven_h2_conn_submit_request(e, f, 4, true, &id) != PROVEN_OK) { proven_h2_conn_destroy(e); goto out; }
        for (int round = 0; round < 200 && !ok; ++round) {
            proven_mem_view_t pending = proven_h2_conn_pending_output(e);
            if (pending.size) { if (proven_transport_write_all(t, pending, proven_net_deadline_in(5000)).err != PROVEN_OK) break; proven_h2_conn_output_sent(e, pending.size); }
            proven_result_size_t r = proven_transport_read(t, (proven_mem_mut_t){ in, sizeof in }, proven_net_deadline_in(2000));
            if (r.err != PROVEN_OK || r.value == 0) break;
            proven_size_t at = 0;
            while (at < r.value) {
                proven_size_t used = 0;
                proven_h2_event_t ev;
                if (proven_h2_conn_feed(e, (proven_mem_view_t){ in + at, r.value - at }, &used, &ev) != PROVEN_OK) { at = r.value; break; }
                at += used;
                if (ev.kind == PROVEN_H2_EVENT_HEADERS && ev.stream_id == id) {
                    ok = true;
                    for (proven_size_t i = 0; i < ev.field_count; ++i)
                        if (ev.fields[i].name.size == 7 && memcmp(ev.fields[i].name.ptr, "alt-svc", 7) == 0)
                            snprintf(alt, 64, "%.*s", (int)ev.fields[i].value.size, (const char *)ev.fields[i].value.ptr);
                }
                if (ev.kind == PROVEN_H2_EVENT_NONE && used == 0) break;
            }
        }
        proven_h2_conn_destroy(e);
    }
out:
    (void)proven_transport_close(t);
    return ok;
}

// -----------------------------------------------------------------------------
// Cases
// -----------------------------------------------------------------------------

static void case_requests(proven_u16 port) {
    cl_t *c = calloc(1, sizeof *c);
    PROVEN_TEST_ASSERT(cl_connect(c, port), "a QUIC connection, with h3", "");
    PROVEN_TEST_ASSERT(c->first_type == 0xc, "without http3_retry the server's first answer is its Initial", "");
    static const proven_http_header_t gzip[] = { H("accept-encoding", "gzip") }, zstd[] = { H("accept-encoding", "gzip, zstd") };
    const int hello = cl_request(c, "GET", "/hello", 0, NULL, 0, true);
    const int proto = cl_request(c, "GET", "/proto", 0, NULL, 0, true);
    const proven_size_t sizes[] = { 0, 1, 1000, 16384, 100000, 3000000 };
    static const proven_size_t whole_sizes[3] = { 5, 70000, 400000 };
    int big[6], whole[3], stream[2];
    char path[48];
    for (int i = 0; i < 6; ++i) { snprintf(path, sizeof path, "/big/%zu", sizes[i]); big[i] = cl_request(c, "GET", path, 0, NULL, 0, true); }
    for (int i = 0; i < 3; ++i) { snprintf(path, sizeof path, "/whole/%zu", whole_sizes[i]); whole[i] = cl_request(c, "GET", path, 0, NULL, 0, true); }
    stream[0] = cl_request(c, "GET", "/stream/0", 0, NULL, 0, true);
    stream[1] = cl_request(c, "GET", "/stream/250000", 0, NULL, 0, true);
    const int zg = cl_request(c, "GET", "/zip/200000", 0, gzip, 1, true);
    const int zz = cl_request(c, "GET", "/zip/200000", 0, zstd, 1, true);
    const int zw = cl_request(c, "GET", "/zip-whole", 0, gzip, 1, true);
    const int zn = cl_request(c, "GET", "/zip/5000", 0, NULL, 0, true);
    const int echo = cl_request(c, "POST", "/echo", 200000, NULL, 0, true);
    const int sink = cl_request(c, "POST", "/sink", 3000000, NULL, 0, true);
    const int sign_len = cl_request(c, "POST", "/body-sign", 10, NULL, 0, true);
    const int sign_none = cl_request(c, "POST", "/body-sign", 10, NULL, 0, false);
    const int sign_empty = cl_request(c, "GET", "/body-sign", 0, NULL, 0, true);
    const int later = cl_request(c, "GET", "/later/5000", 0, NULL, 0, true);
    const int head = cl_request(c, "HEAD", "/big/1000", 0, NULL, 0, true);
    const int nothing = cl_request(c, "GET", "/nothing", 0, NULL, 0, true);
    const int abort_ = cl_request(c, "GET", "/abort", 0, NULL, 0, true);
    const int short_ = cl_request(c, "GET", "/short", 0, NULL, 0, true);
    const int badh = cl_request(c, "GET", "/bad-header", 0, NULL, 0, true);
    const int ws = cl_request(c, "GET", "/ws", 0, NULL, 0, true);
    const int last = c->n;
    PROVEN_TEST_ASSERT(last == 29, "every request was taken by the client's engine", "");
    PROVEN_TEST_ASSERT(cl_wait(c, 0, last, 20000), "every response ends", "");
    PROVEN_TEST_ASSERT(c->r[hello].status == 200 && body_is(&c->r[hello], "hello") && c->r[hello].has_date && c->r[hello].has_length, "a whole response, with Date and its length", "");
    PROVEN_TEST_ASSERT(!c->r[hello].hop, "the handler's Keep-Alive is not sent over HTTP/3", "");
    PROVEN_TEST_ASSERT(!c->r[hello].alt, "and no Alt-Svc on HTTP/3 itself", "");
    PROVEN_TEST_ASSERT(c->r[proto].status == 200 && body_is(&c->r[proto], "3"), "the handler is told the protocol is 3, and sees a Host field", "");
    for (int i = 0; i < 6; ++i) PROVEN_TEST_ASSERT(c->r[big[i]].status == 200 && c->r[big[i]].got == sizes[i] && !c->r[big[i]].bad && !c->r[big[i]].reset, "a body written in pieces under backpressure, whole and in order", "");
    for (int i = 0; i < 3; ++i) PROVEN_TEST_ASSERT(c->r[whole[i]].status == 200 && c->r[whole[i]].got == whole_sizes[i] && !c->r[whole[i]].bad, "a body given whole", "");
    PROVEN_TEST_ASSERT(c->r[stream[0]].status == 200 && c->r[stream[0]].got == 0 && !c->r[stream[0]].has_length, "a body of unknown length: none", "");
    PROVEN_TEST_ASSERT(c->r[stream[1]].status == 200 && c->r[stream[1]].got == 250000 && !c->r[stream[1]].bad && !c->r[stream[1]].has_length, "and 250,000 bytes of it, with no length said", "");
    PROVEN_TEST_ASSERT(c->r[zg].status == 200 && strcmp(c->r[zg].coding, "gzip") == 0 && c->r[zg].got > 0 && c->r[zg].got < 200000, "a streamed body compressed with gzip", "");
    PROVEN_TEST_ASSERT(c->r[zz].status == 200 && strcmp(c->r[zz].coding, "zstd") == 0 && c->r[zz].got > 0 && c->r[zz].got < 200000, "and with zstd, when the client weighs it as high", "");
    PROVEN_TEST_ASSERT(c->r[zw].status == 200 && strcmp(c->r[zw].coding, "gzip") == 0 && c->r[zw].got < 20000, "a body given whole, compressed", "");
    PROVEN_TEST_ASSERT(c->r[zn].status == 200 && !c->r[zn].zipped && c->r[zn].got == 5000 && !c->r[zn].bad, "and sent as it is to a client that accepts no coding", "");
    PROVEN_TEST_ASSERT(c->r[echo].status == 200 && c->r[echo].got == 200000 && !c->r[echo].bad, "a request body of 200,000 bytes, echoed whole", "");
    char got[32];
    snprintf(got, sizeof got, "got %u", 3000000u);
    PROVEN_TEST_ASSERT(c->r[sink].status == 200 && body_is(&c->r[sink], got), "a request body of 3 MB, through the stream's window, counted whole", "");
    PROVEN_TEST_ASSERT(body_is(&c->r[sign_len], "length") && body_is(&c->r[sign_none], "chunked") && body_is(&c->r[sign_empty], "none"),
                       "a handler learns whether a body follows as over HTTP/1.1: a length, chunked for one of unsaid length, or neither", "");
    PROVEN_TEST_ASSERT(c->r[later].status == 200 && c->r[later].got == 5000 && !c->r[later].bad, "an answer made later, from a timer", "");
    PROVEN_TEST_ASSERT(c->r[head].status == 200 && c->r[head].got == 0 && c->r[head].has_length && !c->r[head].reset, "HEAD: the length of the body, and no body", "");
    PROVEN_TEST_ASSERT(c->r[nothing].status == 404, "an unknown path", "");
    PROVEN_TEST_ASSERT(c->r[abort_].reset && c->r[abort_].status == 0, "a handler's abort resets the stream", "");
    PROVEN_TEST_ASSERT(c->r[short_].reset, "a body shorter than its length is not given an end: the stream is reset", "");
    PROVEN_TEST_ASSERT(c->r[badh].status == 200 && body_is(&c->r[badh], "ok"), "a field the server writes itself is refused, and the handler may answer again", "");
    PROVEN_TEST_ASSERT(c->r[ws].status == 400, "WebSocket is not taken over HTTP/3, and the stream is not mistaken for a connection", "");
    PROVEN_TEST_ASSERT(settled(last), "every exchange is over for the server too", "");
    PROVEN_TEST_ASSERT(atomic_load(&g_dispatch_bad) == 0, "every call a handler makes is sent to HTTP/3's", "");
    PROVEN_TEST_ASSERT(atomic_load(&g_reentered) == 0, "on_writable is never called from inside the handler's own write", "");
    PROVEN_TEST_ASSERT(atomic_load(&g_writable) > 0, "and it is called when output that was refused can go on", "");

    /* A client that gives a request up: the handler is told PROVEN_ERR_RESET, the connection goes on. */
    {
        const int before = atomic_load(&g_done_reset);
        const int given = cl_request(c, "GET", "/never", 0, NULL, 0, true);
        cl_run(c, 50);
        PROVEN_TEST_ASSERT(proven_h3_stream_reset(c->h, c->r[given].id, 0) == PROVEN_OK, "the client resets its request", "");
        c->r[given].end = true;
        for (int i = 0; i < 400 && atomic_load(&g_done_reset) == before; ++i) cl_pump(c, 5);
        PROVEN_TEST_ASSERT(atomic_load(&g_done_reset) == before + 1, "the handler is told PROVEN_ERR_RESET", "");
        const int next = cl_request(c, "GET", "/hello", 0, NULL, 0, true);
        PROVEN_TEST_ASSERT(cl_wait(c, next, next + 1, 3000) && c->r[next].status == 200, "and the next request is answered", "");
    }
    /* A client that abandons an upload, resetting only its sending side (RESET_STREAM, no
     * STOP_SENDING): the handler is told PROVEN_ERR_RESET at once, not at the body timeout. */
    {
        const int before = atomic_load(&g_done_reset);
        const proven_http_header_t len[] = { H("content-length", "100000") };
        proven_u64 id = 0;
        const proven_http_header_t f[] = { { sv(":method"), sv("POST") }, { sv(":scheme"), sv("https") }, { sv(":authority"), sv("example.test") }, { sv(":path"), sv("/sink") }, len[0] };
        proven_size_t taken = 0;
        PROVEN_TEST_ASSERT(proven_h3_submit_request(c->h, f, 5, false, &id) == PROVEN_OK && proven_h3_send_data(c->h, id, (proven_mem_view_t){ (const proven_byte_t *)"0123456789", 10 }, false, &taken) == PROVEN_OK,
                           "an upload of 100,000 bytes begun", "");
        cl_run(c, 50);
        PROVEN_TEST_ASSERT(proven_quic_stream_reset(c->q, id, PROVEN_H3_REQUEST_CANCELLED) == PROVEN_OK, "the client resets its sending side only", "");
        for (int i = 0; i < 400 && atomic_load(&g_done_reset) == before; ++i) cl_pump(c, 5);
        PROVEN_TEST_ASSERT(atomic_load(&g_done_reset) == before + 1, "the handler is told PROVEN_ERR_RESET, at once", "");
    }
    /* Many at once: more than the server lets be open, so the client waits for room. */
    const int first = c->n;
    int made = 0, busy = 0;
    for (int i = 0; i < 300 && c->n < CL_REQS; ++i) {
        if (cl_request(c, "GET", "/big/20000", 0, NULL, 0, true) >= 0) made++;
        else { busy++; cl_pump(c, 1); i--; if (busy > 20000) break; }
    }
    PROVEN_TEST_ASSERT(made == 300 && busy > 0, "300 requests, more than the server's 100 streams at once: the client is held to them", "");
    PROVEN_TEST_ASSERT(cl_wait(c, first, c->n, 30000), "and all are answered", "");
    bool all = true;
    for (int i = first; i < c->n; ++i) all = all && c->r[i].status == 200 && c->r[i].got == 20000 && !c->r[i].bad;
    PROVEN_TEST_ASSERT(all, "each one whole", "");
    cl_close(c);
    free(c);
}

static void case_pause(proven_u16 port) {
    cl_t *c = calloc(1, sizeof *c);
    PROVEN_TEST_ASSERT(cl_connect(c, port), "a connection", "");
    atomic_store(&g_bodies_while_paused, 0);
    const int q = cl_request(c, "POST", "/sink", 1000000, NULL, 0, true);
    PROVEN_TEST_ASSERT(cl_wait(c, q, q + 1, 10000) && c->r[q].status == 200 && body_is(&c->r[q], "got 1000000"), "1 MB through a window of 64 KiB, a stream's and the connection's", "");
    const int p = cl_request(c, "POST", "/pause", 1000000, NULL, 0, true);
    cl_run(c, 100);
    PROVEN_TEST_ASSERT(!c->r[p].up_done, "while the handler has paused, the client is held by the window: it cannot send 1 MB", "");
    PROVEN_TEST_ASSERT(cl_wait(c, p, p + 1, 10000), "after the resume the body arrives", "");
    PROVEN_TEST_ASSERT(c->r[p].status == 200 && c->r[p].got == 1000000 && !c->r[p].bad, "whole and in order", "");
    PROVEN_TEST_ASSERT(atomic_load(&g_bodies_while_paused) == 0, "and nothing was delivered while paused", "");
    cl_close(c);
    free(c);
}

int main(void) {
    PROVEN_TEST_SUITE("the event-driven HTTP server, speaking HTTP/3",
        "The handlers of an HTTP/1.1 server, unchanged, answering this library's HTTP/3 client over real UDP: requests and bodies both ways under flow control, compression, pause, resets, limits, time, the two goodbyes, Retry, stateless resets, Alt-Svc on the TCP side, and what a server must refuse to be made with.",
        "Inspect src/proven/http_event_h3.c: x_datagram for what arrives, d_request and d_body for requests, s_head and s_push for what leaves, d_service for a round, d_arm and d_on_timer for time.");

    g_heap = proven_heap_allocator();
    PROVEN_TEST_ASSERT(tp_build(), "the test's certificates and keys are issued", "");
    PROVEN_TEST_ASSERT(proven_job_system_init(g_heap, 1, 4, &g_threads) == PROVEN_OK, "a thread for the loop", "");
    proven_cert_store_t *anchors = NULL;
    PROVEN_TEST_ASSERT(proven_cert_store_create(g_heap, &anchors) == PROVEN_OK &&
                       proven_cert_store_add_pem(anchors, (proven_mem_view_t){ (const proven_byte_t *)TP_CA, strlen(TP_CA) }, NULL) == PROVEN_OK, "anchors", "");
    static const proven_u8str_view_t all[] = { { (const proven_byte_t *)"h2", 2 }, { (const proven_byte_t *)"http/1.1", 8 }, { (const proven_byte_t *)"h3", 2 } };
    static const proven_u8str_view_t h3[] = { { (const proven_byte_t *)"h3", 2 } }, h2[] = { { (const proven_byte_t *)"h2", 2 } };
    proven_tls_options_t so = { .alloc = g_heap, .certificate_pem = { (const proven_byte_t *)TP_SERVER_P256, strlen(TP_SERVER_P256) },
                                .private_key_pem = { (const proven_byte_t *)TP_SERVER_P256_KEY, strlen(TP_SERVER_P256_KEY) }, .alpn = all, .alpn_count = 3 };
    proven_tls_options_t so_two = so; so_two.alpn_count = 2;
    proven_tls_options_t co = { .alloc = g_heap, .anchors = anchors, .alpn = h3, .alpn_count = 1 };
    proven_tls_options_t co_h1 = { .alloc = g_heap, .anchors = anchors };
    proven_tls_options_t co_h2 = { .alloc = g_heap, .anchors = anchors, .alpn = h2, .alpn_count = 1 };
    static const proven_u8str_view_t h2h3[] = { { (const proven_byte_t *)"h2", 2 }, { (const proven_byte_t *)"h3", 2 } };
    proven_tls_options_t co_h2h3 = { .alloc = g_heap, .anchors = anchors, .alpn = h2h3, .alpn_count = 2 };
    PROVEN_TEST_ASSERT(proven_tls_config_create(&co_h2h3, &g_client_tls_h2h3) == PROVEN_OK, "a client TLS configuration that offers h2 and h3", "");
    proven_tls_config_t *server_two = NULL;
    PROVEN_TEST_ASSERT(proven_tls_config_create(&so, &g_server_tls) == PROVEN_OK && proven_tls_config_create(&so_two, &server_two) == PROVEN_OK &&
                       proven_tls_config_create(&co, &g_client_tls) == PROVEN_OK && proven_tls_config_create(&co_h1, &g_tcp_tls_h1) == PROVEN_OK &&
                       proven_tls_config_create(&co_h2, &g_tcp_tls_h2) == PROVEN_OK, "TLS configurations", "");

    PROVEN_TEST_SECTION("making a server", "HTTP/3 needs TLS, and what TLS may agree to and what the server speaks must be the same thing.", "Check the checks in proven_http_event_server_create.");
    {
        proven_loop_t *loop = NULL;
        proven_http_event_server_t *s = NULL;
        PROVEN_TEST_ASSERT(proven_loop_create(g_heap, &loop) == PROVEN_OK, "a loop", "");
        proven_http_event_server_config_t cfg = { .on = { .on_request = on_request }, .http3 = true };
        PROVEN_TEST_ASSERT(proven_http_event_server_create(loop, &cfg, &s) == PROVEN_ERR_INVALID_ARG && !s, "HTTP/3 without TLS is refused", "");
        cfg.tls = server_two; cfg.http2 = true;
        PROVEN_TEST_ASSERT(proven_http_event_server_create(loop, &cfg, &s) == PROVEN_ERR_INVALID_ARG && !s, "and over a TLS configuration that does not offer h3", "");
        cfg.tls = g_server_tls; cfg.http3 = false;
        PROVEN_TEST_ASSERT(proven_http_event_server_create(loop, &cfg, &s) == PROVEN_ERR_INVALID_ARG && !s, "and a configuration that offers h3 to a server that will not speak it", "");
        cfg.http3 = true; cfg.http3_window_bytes = (proven_size_t)1 << 31;
        PROVEN_TEST_ASSERT(proven_http_event_server_create(loop, &cfg, &s) == PROVEN_ERR_INVALID_ARG && !s, "and a window above 2^31-1", "");
        cfg.http3_window_bytes = 0;
        PROVEN_TEST_ASSERT(proven_http_event_server_create(loop, &cfg, &s) == PROVEN_OK && s, "together they are a server", "");
        /* A UDP port that is taken: the TCP listener is closed again, and the error said. */
        proven_net_udp_t taken;
        proven_net_addr_t at;
        PROVEN_TEST_ASSERT(udp_port_held(&taken, &at), "a UDP port held by someone else", "");
        PROVEN_TEST_ASSERT(proven_http_event_server_listen(s, at, NULL) != PROVEN_OK, "a listen whose UDP socket cannot be had fails", "");
        proven_net_listener_t l;
        PROVEN_TEST_ASSERT(proven_net_listen(at, 16, &l, NULL) == PROVEN_OK, "and leaves the TCP port free", "");
        (void)proven_net_listener_close(&l);
        (void)proven_net_udp_close(&taken);
        proven_net_addr_t bound;
        PROVEN_TEST_ASSERT(proven_http_event_server_listen(s, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &bound) == PROVEN_OK, "a listen", "");
        proven_net_udp_t again;
        PROVEN_TEST_ASSERT(proven_net_udp_open(bound, &again, NULL) != PROVEN_OK, "holds UDP on the same port as TCP", "");
        proven_http_event_server_destroy(s);
        PROVEN_TEST_ASSERT(proven_net_udp_open(bound, &again, NULL) == PROVEN_OK, "and destroy lets it go", "");
        (void)proven_net_udp_close(&again);
        proven_loop_destroy(loop);
    }

    PROVEN_TEST_SECTION("requests and responses", "Every route of the HTTP/2 tests that is not WebSocket, and three hundred requests on one connection.", "Check d_request, s_head, s_push and the round in d_service.");
    {
        const proven_u16 port = server_start((proven_http_event_server_config_t){ .max_body_bytes = 4 * 1024 * 1024, .compress_zstd = true });
        case_requests(port);
        server_stop();
    }

    PROVEN_TEST_SECTION("a paused request", "proven_http_stream_pause stops the stream's reading; QUIC's flow control holds the client to http3_window_bytes.", "Check proven_http_event_h3_pause_ and _resume_, and the window given to the QUIC configuration.");
    {
        const proven_u16 port = server_start((proven_http_event_server_config_t){ .max_body_bytes = 4 * 1024 * 1024, .http3_window_bytes = 65536 });
        case_pause(port);
        server_stop();
    }

    PROVEN_TEST_SECTION("Alt-Svc", "HTTP/1.1 and HTTP/2 responses name the UDP port; a handler's own Alt-Svc is kept; after stop_listening none is said.", "Check proven_http_event_alt_svc_ and its two callers.");
    {
        const proven_u16 port = server_start((proven_http_event_server_config_t){ 0 });
        char alt[64], want[32];
        snprintf(want, sizeof want, "h3=\":%u\"", (unsigned)port);
        PROVEN_TEST_ASSERT(tcp_alt_svc(port, false, "/hello", alt) && strcmp(alt, want) == 0, "an HTTP/1.1 response says h3 on the same port", alt);
        PROVEN_TEST_ASSERT(tcp_alt_svc(port, true, "/hello", alt) && strcmp(alt, want) == 0, "and so does an HTTP/2 one", alt);
        PROVEN_TEST_ASSERT(tcp_alt_svc(port, false, "/own-alt", alt) && strcmp(alt, "clear") == 0, "a handler's own Alt-Svc is the one sent", alt);
        PROVEN_TEST_ASSERT(tcp_alt_svc(port, true, "/own-alt", alt) && strcmp(alt, "clear") == 0, "over HTTP/2 too", alt);
        server_stop();
    }

    PROVEN_TEST_SECTION("limits", "A head with too many fields, a body larger than the server takes said and unsaid, and max_connections counting QUIC connections.", "Check the refusals in d_request and d_body, and the count in x_accept.");
    {
        const proven_u16 port = server_start((proven_http_event_server_config_t){ .max_headers = 8, .max_body_bytes = 100000, .max_connections = 1 });
        cl_t *c = calloc(1, sizeof *c), *c2 = calloc(1, sizeof *c2);
        PROVEN_TEST_ASSERT(cl_connect(c, port), "a connection", "");
        static const proven_http_header_t many[] = { H("a", "1"), H("b", "1"), H("c", "1"), H("d", "1"), H("e", "1"), H("f", "1"), H("g", "1"), H("h", "1") };
        const int fields = cl_request(c, "GET", "/hello", 0, many, 8, true);
        const int said = cl_request(c, "POST", "/sink", 200000, NULL, 0, true);
        const int unsaid = cl_request(c, "POST", "/sink", 200000, NULL, 0, false);
        const int fine = cl_request(c, "GET", "/hello", 0, NULL, 0, true);
        PROVEN_TEST_ASSERT(cl_wait(c, 0, c->n, 10000), "every request ends", "");
        PROVEN_TEST_ASSERT(c->r[fields].status == 431, "nine fields, with Host, where eight are taken: 431", "");
        PROVEN_TEST_ASSERT(c->r[said].status == 413, "a length above max_body_bytes: 413", "");
        PROVEN_TEST_ASSERT(c->r[unsaid].status == 413 || c->r[unsaid].reset, "a body of unsaid length that grows past it: 413, and the stream ended", "");
        PROVEN_TEST_ASSERT(c->r[fine].status == 200, "and the connection goes on", "");
        PROVEN_TEST_ASSERT(connections() == 1, "one QUIC connection is counted", "");
        PROVEN_TEST_ASSERT(cl_open(c2, port), "a second client", "");
        for (const proven_u64 end = ms_now() + 400; ms_now() < end && !c2->established;) { cl_pump(c2, 10); cl_pump(c, 0); }
        PROVEN_TEST_ASSERT(!c2->established && connections() == 1, "at max_connections a client's first packet is not answered", "");
        cl_close(c2);
        cl_close(c);
        free(c); free(c2);
        server_stop();
    }

    PROVEN_TEST_SECTION("max_concurrent_streams changed while the server runs", "proven_http_event_server_set_max_streams over QUIC: what a client was allowed stands, later credit follows the new number, a raise is given at once, and a connection made after opens with it.", "Check proven_quic_conn_set_max_streams_, the grant at a stream's end in quic_conn.c, and streams_bidi_now.");
    {
        const proven_u16 port = server_start((proven_http_event_server_config_t){ .max_concurrent_streams = 4 });
        cl_t *c = calloc(1, sizeof *c), *c2 = calloc(1, sizeof *c2);
        PROVEN_TEST_ASSERT(cl_connect(c, port), "a connection, allowed four requests at once", "");
        g_set_n = 2; on_loop(do_set_streams);
        PROVEN_TEST_ASSERT(atomic_load(&g_set_err) == PROVEN_OK, "the server lowers its streams to 2", "");
        for (int i = 0; i < 4; ++i) PROVEN_TEST_ASSERT(cl_request(c, "GET", "/hello", 0, NULL, 0, true) >= 0, "what the client was allowed stands: four requests begin", "");
        PROVEN_TEST_ASSERT(cl_wait(c, 0, 4, 10000) && c->r[3].status == 200, "and are answered", "");
        cl_run(c, 100);
        const int from = c->n;
        int begun = 0;
        for (int i = 0; i < 3; ++i) if (cl_request(c, "GET", "/never", 0, NULL, 0, true) >= 0) begun++;
        PROVEN_TEST_ASSERT(begun == 2, "once those ended, the client may have two open at once, not four", "");
        PROVEN_TEST_ASSERT(cl_connect(c2, port), "a connection made now", "");
        begun = 0;
        for (int i = 0; i < 3; ++i) if (cl_request(c2, "GET", "/never", 0, NULL, 0, true) >= 0) begun++;
        PROVEN_TEST_ASSERT(begun == 2, "opens allowed two", "");
        g_set_n = 4; on_loop(do_set_streams);
        PROVEN_TEST_ASSERT(atomic_load(&g_set_err) == PROVEN_OK, "raised back to 4", "");
        cl_run(c, 100); cl_run(c2, 100);
        PROVEN_TEST_ASSERT(cl_request(c, "GET", "/hello", 0, NULL, 0, true) >= 0 && cl_request(c2, "GET", "/hello", 0, NULL, 0, true) >= 0,
            "a raise is given at once: each may begin a third with two still open", "");
        PROVEN_TEST_ASSERT(cl_wait(c, c->n - 1, c->n, 10000) && cl_wait(c2, c2->n - 1, c2->n, 10000), "and it is answered", "");
        PROVEN_TEST_ASSERT(!c->closed && !c2->closed && from >= 4, "neither connection was ended by any of it", "");
        cl_close(c2);
        cl_close(c);
        free(c); free(c2);
        server_stop();
    }

    PROVEN_TEST_SECTION("the protocol agreed is h3", "A QUIC client that offers h2 before h3, to a server whose TLS lists h2 first: HTTP/3 agrees h3 alone (RFC 9114 section 3.1), and the request is answered.", "Check the .alpn of x->qc in src/proven/http_event_h3.c, and alpn_allowed in tls13.c.");
    {
        const proven_u16 port = server_start((proven_http_event_server_config_t){ 0 });
        proven_tls_config_t *only_h3 = g_client_tls;
        g_client_tls = g_client_tls_h2h3;
        cl_t *c = calloc(1, sizeof *c);
        PROVEN_TEST_ASSERT(cl_connect(c, port), "a connection", "");
        const proven_u8str_view_t agreed = proven_quic_conn_alpn_(c->q);
        const int h = cl_request(c, "GET", "/hello", 0, NULL, 0, true);
        PROVEN_TEST_ASSERT(agreed.size == 2 && memcmp(agreed.ptr, "h3", 2) == 0 && cl_wait(c, h, h + 1, 3000) && c->r[h].status == 200, "h3 agreed, and the request answered", "");
        cl_close(c);
        free(c);
        g_client_tls = only_h3;
        server_stop();
    }

    PROVEN_TEST_SECTION("time", "head_timeout_ms from the handshake to the first request, idle_timeout_ms with nothing open, body_timeout_ms for a body that stops, write_timeout_ms for a client that stops reading.", "Check d_arm and d_on_timer.");
    {
        const proven_u16 port = server_start((proven_http_event_server_config_t){ .head_timeout_ms = 300, .idle_timeout_ms = 300, .body_timeout_ms = 300, .write_timeout_ms = 300 });
        cl_t *c = calloc(1, sizeof *c);
        PROVEN_TEST_ASSERT(cl_connect(c, port), "a connection that asks nothing", "");
        PROVEN_TEST_ASSERT(cl_wait_closed(c, 3000) && c->remote_close && c->close_code == PROVEN_H3_NO_ERROR, "is closed after head_timeout_ms, with H3_NO_ERROR", "");
        cl_close(c);
        memset(c, 0, sizeof *c);
        PROVEN_TEST_ASSERT(cl_connect(c, port), "a connection", "");
        const int h = cl_request(c, "GET", "/hello", 0, NULL, 0, true);
        PROVEN_TEST_ASSERT(cl_wait(c, h, h + 1, 3000) && c->r[h].status == 200, "asks once", "");
        PROVEN_TEST_ASSERT(cl_wait_closed(c, 3000) && c->goaways >= 1 && c->close_code == PROVEN_H3_NO_ERROR, "and with nothing open is told goodbye and closed after idle_timeout_ms", "");
        cl_close(c);
        memset(c, 0, sizeof *c);
        reset_counts();
        PROVEN_TEST_ASSERT(cl_connect(c, port), "a connection", "");
        const char lb[] = "1000";
        const proven_http_header_t len[] = { { sv("content-length"), sv(lb) } };
        proven_u64 id = 0;
        const proven_http_header_t f[] = { { sv(":method"), sv("POST") }, { sv(":scheme"), sv("https") }, { sv(":authority"), sv("example.test") }, { sv(":path"), sv("/sink") }, len[0] };
        PROVEN_TEST_ASSERT(proven_h3_submit_request(c->h, f, 5, false, &id) == PROVEN_OK, "a body of 1,000 bytes promised", "");
        proven_size_t taken = 0;
        PROVEN_TEST_ASSERT(proven_h3_send_data(c->h, id, (proven_mem_view_t){ (const proven_byte_t *)"0123456789", 10 }, false, &taken) == PROVEN_OK, "ten of them sent", "");
        c->r[0] = (rq_t){ .id = id, .up_done = true };
        c->n = 1;
        PROVEN_TEST_ASSERT(cl_wait(c, 0, 1, 3000) && c->r[0].reset, "and no more: the stream is reset after body_timeout_ms", "");
        PROVEN_TEST_ASSERT(settled(1) && atomic_load(&g_done_timeout) == 1, "and the handler is told it timed out", "");
        /* The same beside a download that keeps the connection busy - packets far more often
         * than the loop's 16 ms tick, on a connection of its own: the stalled one is reset by its
         * own silence, in time. */
        {
            cl_t *b = calloc(1, sizeof *b);
            PROVEN_TEST_ASSERT(cl_connect(b, port), "a second connection", "");
            reset_counts();
            proven_u64 sid = 0;
            PROVEN_TEST_ASSERT(proven_h3_submit_request(b->h, f, 5, false, &sid) == PROVEN_OK && proven_h3_send_data(b->h, sid, (proven_mem_view_t){ (const proven_byte_t *)"0123456789", 10 }, false, &taken) == PROVEN_OK, "a body of 1,000 bytes begun", "");
            b->r[0] = (rq_t){ .id = sid, .up_done = true };
            b->n = 1;
            const int busy = cl_request(b, "GET", "/big/1000000000", 0, NULL, 0, true);
            const proven_u64 t0 = ms_now();
            PROVEN_TEST_ASSERT(cl_wait(b, 0, 1, 3000) && b->r[0].reset, "the stalled request is reset", "");
            const proven_u64 took = ms_now() - t0;
            PROVEN_TEST_ASSERT(took < 1000 && !b->r[busy].end && b->r[busy].got > 0, "within its body_timeout_ms and a tick or two, while the download goes on", "");
            cl_close(b);
            free(b);
            PROVEN_TEST_ASSERT(connections_fall_to(1), "the abandoned download's connection is let go", "");
        }
        /* A client that stops reading a large response - on a new connection: the first was idle
         * for longer than idle_timeout_ms meanwhile, and has been told goodbye. */
        cl_close(c);
        memset(c, 0, sizeof *c);
        PROVEN_TEST_ASSERT(cl_connect(c, port), "a connection", "");
        reset_counts();
        const int big = cl_request(c, "GET", "/big/5000000", 0, NULL, 0, true);
        PROVEN_TEST_ASSERT(big >= 0, "a request", "");
        proven_h3_stream_pause(c->h, c->r[big].id, true);        /* at once: QUIC's flow control stops the server at this side's window */
        PROVEN_TEST_ASSERT(cl_wait_closed(c, 5000) || c->r[big].reset, "a reader that stops is let go after write_timeout_ms", "");
        PROVEN_TEST_ASSERT(settled(1) && atomic_load(&g_done_timeout) == 1, "with on_done saying it timed out", "");
        cl_close(c);
        free(c);
        server_stop();
    }

    PROVEN_TEST_SECTION("goodbye", "stop_listening: two GOAWAYs a round trip apart, the request in flight answered, the connection closed with H3_NO_ERROR; a listen again; shutdown with a deadline; destroy with exchanges open.", "Check d_bye, d_bye_last, proven_http_event_h3_stop_ and _end_all_.");
    {
        proven_u16 port = server_start((proven_http_event_server_config_t){ 0 });
        cl_t *c = calloc(1, sizeof *c);
        PROVEN_TEST_ASSERT(cl_connect(c, port), "a connection", "");
        const int later = cl_request(c, "GET", "/later/100000", 0, NULL, 0, true);
        cl_run(c, 20);
        on_loop(do_stop_listening);
        PROVEN_TEST_ASSERT(cl_wait(c, later, later + 1, 5000) && c->r[later].status == 200 && c->r[later].got == 100000 && !c->r[later].bad, "the request on its way is answered whole", "");
        PROVEN_TEST_ASSERT(cl_wait_closed(c, 5000), "and the connection closes", "");
        PROVEN_TEST_ASSERT(c->goaways == 2 && c->goaway_id == 4 && c->close_code == PROVEN_H3_NO_ERROR, "after two GOAWAYs, the last naming the first request not acted on, with H3_NO_ERROR", "");
        PROVEN_TEST_ASSERT(connections_fall_to(0), "the server holds nothing once the closing period is over", "");
        cl_close(c);
        memset(c, 0, sizeof *c);
        g_relisten_port = port;
        on_loop(do_relisten);
        PROVEN_TEST_ASSERT(atomic_load(&g_relisten_err) == PROVEN_OK, "the same port is listened on again", "");
        /* And again while a connection still holds the UDP socket: the socket kept for it serves the
         * new listener, rather than a second one being refused the port. */
        PROVEN_TEST_ASSERT(cl_connect(c, port), "a connection", "");
        const int slow = cl_request(c, "GET", "/later/1000", 0, NULL, 0, true);
        cl_run(c, 20);
        on_loop(do_stop_listening);
        on_loop(do_relisten);
        PROVEN_TEST_ASSERT(atomic_load(&g_relisten_err) == PROVEN_OK, "a listen while the old socket is still in use succeeds", "");
        PROVEN_TEST_ASSERT(cl_wait(c, slow, slow + 1, 3000) && c->r[slow].status == 200 && c->r[slow].got == 1000, "and the old connection's request is still answered", "");
        cl_close(c);
        memset(c, 0, sizeof *c);
        PROVEN_TEST_ASSERT(cl_connect(c, port), "and HTTP/3 is served there", "");
        const int never = cl_request(c, "GET", "/never", 0, NULL, 0, true);
        reset_counts();
        cl_run(c, 50);
        atomic_store(&g_drain_grace, 300);
        atomic_store(&g_drained_calls, 0);
        on_loop(do_shutdown);
        for (const proven_u64 end = ms_now() + 2000; ms_now() < end && atomic_load(&g_drained_calls) == 0;) cl_pump(c, 5);
        PROVEN_TEST_ASSERT(atomic_load(&g_drained_calls) == 1 && atomic_load(&g_drained_why) == PROVEN_ERR_TIMEOUT, "a shutdown whose request does not finish ends at its deadline", "");
        PROVEN_TEST_ASSERT(atomic_load(&g_done_reset) == 1, "and the exchange is told it was cut", "");
        PROVEN_TEST_ASSERT(cl_wait(c, never, never + 1, 2000) || c->closed, "the client learns it is over", "");
        cl_close(c);
        server_stop();
        port = server_start((proven_http_event_server_config_t){ 0 });
        memset(c, 0, sizeof *c);
        PROVEN_TEST_ASSERT(cl_connect(c, port), "a connection", "");
        (void)cl_request(c, "GET", "/never", 0, NULL, 0, true);
        cl_run(c, 50);
        on_loop(do_destroy);
        PROVEN_TEST_ASSERT(atomic_load(&g_done_reset) == 1, "destroy with an exchange open: on_done with PROVEN_ERR_RESET", "");
        PROVEN_TEST_ASSERT(cl_wait_closed(c, 3000) && c->remote_close, "and the client is told the connection is closed", "");
        cl_close(c);
        server_stop();
        free(c);
    }

    PROVEN_TEST_SECTION("Retry and stateless resets", "With http3_retry the first packet is answered with a Retry and the connection is made after it; a short header to an unknown connection ID is answered with a stateless reset smaller than it.", "Check x_datagram.");
    {
        const proven_u16 port = server_start((proven_http_event_server_config_t){ .http3_retry = true });
        cl_t *c = calloc(1, sizeof *c);
        PROVEN_TEST_ASSERT(cl_connect(c, port), "a connection", "");
        PROVEN_TEST_ASSERT(c->first_type == 0xf, "made through a Retry: the server's first answer is one (long header, type 3)", "");
        const int h = cl_request(c, "GET", "/hello", 0, NULL, 0, true);
        PROVEN_TEST_ASSERT(cl_wait(c, h, h + 1, 3000) && c->r[h].status == 200 && body_is(&c->r[h], "hello"), "and a request on it", "");
        cl_close(c);
        free(c);
        proven_net_udp_t u;
        PROVEN_TEST_ASSERT(proven_net_udp_open(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &u, NULL) == PROVEN_OK, "a socket", "");
        proven_byte_t junk[60], back[200];
        for (proven_size_t i = 0; i < sizeof junk; ++i) junk[i] = (proven_byte_t)(i * 37 + 11);
        junk[0] = 0x40 | (junk[0] & 0x3f);          /* a short header: form bit clear, fixed bit set */
        PROVEN_TEST_ASSERT(proven_net_udp_send_to(&u, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, port), (proven_mem_view_t){ junk, sizeof junk }, proven_net_deadline_in(1000)) == PROVEN_OK, "a short header to an ID the server never issued", "");
        proven_result_size_t r = proven_net_udp_recv_from(&u, (proven_mem_mut_t){ back, sizeof back }, NULL, proven_net_deadline_in(2000));
        PROVEN_TEST_ASSERT(r.err == PROVEN_OK && r.value >= 21 && r.value < sizeof junk && (back[0] & 0xc0) == 0x40, "is answered with a stateless reset, smaller than what came", "");
        (void)proven_net_udp_close(&u);
        server_stop();
    }

    proven_tls_config_destroy(g_server_tls); proven_tls_config_destroy(server_two); proven_tls_config_destroy(g_client_tls); proven_tls_config_destroy(g_client_tls_h2h3);
    proven_tls_config_destroy(g_tcp_tls_h1); proven_tls_config_destroy(g_tcp_tls_h2);
    proven_cert_store_destroy(anchors);
    proven_job_system_close(g_threads); proven_job_system_destroy(g_threads);
    PROVEN_TEST_PASS("the handlers of an HTTP/1.1 server answer HTTP/3 clients unchanged, over UDP beside TCP on the same port.");
    return 0;
}

#else

int main(void) {
    PROVEN_TEST_SUITE("the event-driven HTTP server, speaking HTTP/3", "Built with PROVEN_NO_HTTP3: nothing to test.", "");
    PROVEN_TEST_PASS("skipped: PROVEN_NO_HTTP3");
    return 0;
}

#endif
