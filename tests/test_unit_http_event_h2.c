#include "proven.h"
#include "proven_test.h"
#include "test_unit_tls_pki.h"
#include "../src/proven/proven_internal_tls.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * HTTP/2 in the event-driven server, over loopback.
 *
 * The server runs on a loop thread with the handlers below - the same four callbacks and the
 * same stream calls an HTTP/1.1 handler uses. The test's main thread is the client: an
 * HTTP/2 connection engine of this library's (h2.h) with a socket moved through it by a few
 * lines, in the clear by prior knowledge and over TLS with ALPN. That another implementation
 * agrees with both ends is a private program's evidence (curl, Node).
 *
 * What the handlers record is atomic: it is read from the main thread.
 */

static proven_u8str_view_t sv(const char *s) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }
#define H(n, v) { .name = { (const proven_byte_t *)(n), sizeof(n) - 1 }, .value = { (const proven_byte_t *)(v), strlen(v) } }

static proven_allocator_t g_heap;
static proven_loop_t *g_loop;
static proven_http_event_server_t *g_server;
static proven_job_sys_t *g_threads;
static proven_job_group_t g_running;
static proven_tls_config_t *g_server_tls, *g_client_tls, *g_client_tls_h1, *g_client_tls_12;

static atomic_ulong g_max_buffered;
static atomic_int g_reentered;
static int g_in_write;                 /* the loop's thread only: a handler's write is on the stack */
static atomic_int g_requests, g_done_ok, g_done_reset, g_done_timeout, g_done_other, g_writable, g_bodies_while_paused, g_protocol_bad, g_dispatch_bad;

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
    char path[40];
} app_t;

static void app_pump(app_t *a) {
    static proven_byte_t piece[8192];
    while (a->sent < a->total) {
        proven_size_t n = a->total - a->sent < sizeof piece ? a->total - a->sent : sizeof piece;
        for (proven_size_t i = 0; i < n; i++) piece[i] = pat(a->sent + i);
        g_in_write++;
        proven_result_size_t w = proven_http_stream_write(a->stream, (proven_mem_view_t){ piece, n });
        g_in_write--;
        if (w.err != PROVEN_OK) return;
        const unsigned long held = (unsigned long)proven_http_stream_buffered(a->stream);
        if (held > atomic_load(&g_max_buffered)) atomic_store(&g_max_buffered, held);
        a->sent += w.value;
        if (w.value < n) return;                   /* refused: on_writable says when */
    }
    (void)proven_http_stream_end(a->stream);
}

static void resume_fire(void *ctx) { proven_http_stream_resume(((app_t *)ctx)->stream); }

/* An answer that is not ready when the request arrives. */
static void later_fire(void *ctx) {
    app_t *a = ctx;
    static const proven_http_header_t plain[] = { H("Content-Type", "text/plain") };
    if (proven_http_stream_begin(a->stream, 200, plain, 1, a->total) == PROVEN_OK) app_pump(a);
}
static void resume_fire_checked(void *ctx);

static bool path_is(const proven_http_request_t *req, const char *p) { return req->target.size == strlen(p) && memcmp(req->target.ptr, p, req->target.size) == 0; }
static bool path_starts(const proven_http_request_t *req, const char *p) { return req->target.size >= strlen(p) && memcmp(req->target.ptr, p, strlen(p)) == 0; }

static void ws_never(void *ctx, proven_ws_stream_t *ws, bool text, proven_mem_view_t piece, bool first, bool last) {
    (void)ctx; (void)ws; (void)text; (void)piece; (void)first; (void)last;
}

static void on_request(void *ctx, proven_http_stream_t *s, const proven_http_request_t *req) {
    (void)ctx;
    atomic_fetch_add(&g_requests, 1);
    app_t *a = calloc(1, sizeof *a);
    a->stream = s;
    snprintf(a->path, sizeof a->path, "%.*s", (int)(req->target.size < 39 ? req->target.size : 39), (const char *)req->target.ptr);
    proven_http_stream_set_user(s, a);
    /* Every call a handler has, made on whatever kind of stream this is: none may mistake it. */
    if (proven_http_stream_user(s) != a || proven_http_stream_buffered(s) > (1u << 20) || req->version_minor != 1) atomic_fetch_add(&g_dispatch_bad, 1);
    proven_net_addr_t peer = proven_http_stream_peer(s);
    if (peer.family != PROVEN_NET_FAMILY_IPV4) atomic_fetch_add(&g_dispatch_bad, 1);
    static const proven_http_header_t plain[] = { H("Content-Type", "text/plain"), H("Keep-Alive", "timeout=5") };

    if (path_is(req, "/hello")) {
        (void)proven_http_stream_respond(s, 200, plain, 2, (proven_mem_view_t){ (const proven_byte_t *)"hello", 5 });
    } else if (path_is(req, "/proto")) {
        const int p = proven_http_stream_protocol(s);
        /* The request a handler sees has a Host field whichever protocol carried it. */
        if (proven_http_header_count(req->headers, req->header_count, sv("Host")) != 1) atomic_fetch_add(&g_protocol_bad, 1);
        (void)proven_http_stream_respond(s, 200, plain, 1, (proven_mem_view_t){ (const proven_byte_t *)(p == 2 ? "2" : "1"), 1 });
    } else if (path_starts(req, "/big/")) {
        a->total = (proven_size_t)strtoul(a->path + 5, NULL, 10);
        if (proven_http_stream_begin(s, 200, plain, 1, a->total) == PROVEN_OK) app_pump(a);
    } else if (path_starts(req, "/whole/")) {       /* a body of N bytes, given whole */
        static proven_byte_t body[400000];
        if (body[1] == 0) for (proven_size_t i = 0; i < sizeof body; i++) body[i] = pat(i);
        const proven_size_t n = (proven_size_t)strtoul(a->path + 7, NULL, 10);
        (void)proven_http_stream_respond(s, 200, plain, 1, (proven_mem_view_t){ body, n < sizeof body ? n : sizeof body });
    } else if (path_starts(req, "/later/")) {
        a->total = (proven_size_t)strtoul(a->path + 7, NULL, 10);
        proven_loop_timer_set(g_loop, &a->timer, 200, later_fire, a);
    } else if (path_starts(req, "/stream/")) {      /* a body of unknown length */
        a->total = (proven_size_t)strtoul(a->path + 8, NULL, 10);
        if (proven_http_stream_begin(s, 200, plain, 1, PROVEN_HTTP_EVENT_LENGTH_UNKNOWN) == PROVEN_OK) app_pump(a);
    } else if (path_starts(req, "/gzip/")) {
        a->total = (proven_size_t)strtoul(a->path + 6, NULL, 10);
        proven_http_stream_compress(s);
        if (proven_http_stream_begin(s, 200, plain, 1, a->total) == PROVEN_OK) app_pump(a);
    } else if (path_is(req, "/gzip-whole")) {
        static proven_byte_t body[20000];
        for (proven_size_t i = 0; i < sizeof body; i++) body[i] = pat(i);
        proven_http_stream_compress(s);
        (void)proven_http_stream_respond(s, 200, plain, 1, (proven_mem_view_t){ body, sizeof body });
    } else if (path_is(req, "/body-sign")) {
        /* What an HTTP/1.1 handler looks at to learn whether a body follows. */
        const bool chunked = proven_http_header_has_token(req->headers, req->header_count, sv("Transfer-Encoding"), sv("chunked"));
        const bool length = proven_http_header_count(req->headers, req->header_count, sv("Content-Length")) > 0;
        const char *sign = chunked ? "chunked" : length ? "length" : "none";
        if (chunked || length) { a->sink = true; snprintf(a->path, sizeof a->path, "%s", sign); }
        else (void)proven_http_stream_respond(s, 200, NULL, 0, (proven_mem_view_t){ (const proven_byte_t *)sign, strlen(sign) });
    } else if (path_is(req, "/echo") || path_is(req, "/pause") || path_is(req, "/pause-long")) {
        if (!path_is(req, "/echo")) {
            /* Not now: nothing of the body is to be delivered until the timer resumes it. */
            a->paused = true;
            proven_http_stream_pause(s);
            proven_loop_timer_set(g_loop, &a->timer, path_is(req, "/pause") ? 120 : 20000, resume_fire_checked, a);
        }
        if (req->method != PROVEN_HTTP_POST) (void)proven_http_stream_respond(s, 405, NULL, 0, (proven_mem_view_t){ NULL, 0 });
    } else if (path_is(req, "/sink")) {
        a->sink = true;
    } else if (path_is(req, "/ws")) {
        /* An upgrade to WebSocket, asked of a stream that may be HTTP/2: it must be refused
         * without mistaking the stream for a connection. */
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
        /* Promise ten bytes, write three, and say it is the end. */
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
    if (a->paused) {
        /* Called while paused: only the resume itself may do that, and it clears the flag first. */
        atomic_fetch_add(&g_bodies_while_paused, 1);
    }
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

static void resume_now(app_t *a) { a->paused = false; }

static void on_writable(void *ctx, proven_http_stream_t *s) {
    (void)ctx;
    atomic_fetch_add(&g_writable, 1);
    /* Called from inside this handler's own write, it would write again from a place the
     * outer call has not yet moved on from: the same bytes twice, and out of order. */
    if (g_in_write > 0) atomic_fetch_add(&g_reentered, 1);
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

/* The resume timer clears the flag just before it resumes, so that on_body can tell a
 * delivery made by the resume from one made behind the handler's back. */
static void resume_fire_checked(void *ctx) { resume_now(ctx); resume_fire(ctx); }

static void run_loop(void *arg) { (void)arg; (void)proven_loop_run(g_loop); }

static atomic_int g_ran;
static atomic_ulong g_conn_count;
static void do_count(void *ctx) { (void)ctx; atomic_store(&g_conn_count, (unsigned long)proven_http_event_server_connections(g_server)); atomic_fetch_add(&g_ran, 1); }
static void do_destroy(void *ctx) { (void)ctx; proven_http_event_server_destroy(g_server); g_server = NULL; atomic_fetch_add(&g_ran, 1); }
static void do_stop_listening(void *ctx) { (void)ctx; proven_http_event_server_stop_listening(g_server); atomic_fetch_add(&g_ran, 1); }
static void on_loop(proven_loop_fn fn) {
    int before = atomic_load(&g_ran);
    PROVEN_TEST_ASSERT(proven_loop_post(g_loop, fn, NULL) == PROVEN_OK, "posted to the loop", "");
    for (int i = 0; i < 4000 && atomic_load(&g_ran) == before; ++i) proven_time_sleep(1);
}
static unsigned long connections(void) { on_loop(do_count); return atomic_load(&g_conn_count); }
static int done_total(void) { return atomic_load(&g_done_ok) + atomic_load(&g_done_reset) + atomic_load(&g_done_timeout) + atomic_load(&g_done_other); }
static bool settled(int requests) {
    for (int i = 0; i < 6000; ++i) { if (done_total() == requests) return true; proven_time_sleep(1); }
    return false;
}
static void reset_counts(void) {
    atomic_store(&g_requests, 0); atomic_store(&g_done_ok, 0); atomic_store(&g_done_reset, 0); atomic_store(&g_done_timeout, 0); atomic_store(&g_done_other, 0);
    atomic_store(&g_writable, 0);
}

static proven_u16 server_start(proven_http_event_server_config_t cfg) {
    PROVEN_TEST_ASSERT(proven_loop_create(g_heap, &g_loop) == PROVEN_OK, "a loop", "");
    cfg.on = (proven_http_event_callbacks_t){ .on_request = on_request, .on_body = on_body, .on_writable = on_writable, .on_done = on_done };
    cfg.http2 = true;
    PROVEN_TEST_ASSERT(proven_http_event_server_create(g_loop, &cfg, &g_server) == PROVEN_OK, "a server that may speak HTTP/2", "");
    proven_net_addr_t bound;
    PROVEN_TEST_ASSERT(proven_http_event_server_listen(g_server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &bound) == PROVEN_OK, "listening", "");
    proven_job_group_init(&g_running);
    PROVEN_TEST_ASSERT(proven_job_group_submit(g_threads, &g_running, run_loop, NULL) == PROVEN_OK, "the loop's thread", "");
    reset_counts();
    return bound.port;
}

static void server_stop(void) {
    on_loop(do_destroy);
    proven_loop_stop(g_loop);
    proven_job_group_wait(g_threads, &g_running);
    proven_loop_destroy(g_loop);
    g_loop = NULL;
}

// -----------------------------------------------------------------------------
// The client: the engine of h2.h, and a socket
// -----------------------------------------------------------------------------

#define CL_STREAMS 128
typedef struct {
    int status; proven_size_t got; bool end, reset, bad, gzip, has_length, has_date, has_keep_alive;
    proven_u32 code; proven_size_t length;
    proven_byte_t first[8];
} res_t;

typedef struct {
    proven_net_conn_t sock;
    proven_transport_t t;
    proven_h2_conn_t *e;
    bool closed, goaway, gone, hold, plain_body;
    bool mute;                         /* its sending side is closed: what the engine queues is dropped */
    proven_u32 close_code;
    res_t r[CL_STREAMS];
} cl_t;

static bool cl_open_with(cl_t *c, proven_tls_config_t *tls, proven_u16 port, proven_u32 window) {
    memset(c, 0, sizeof *c);
    if (proven_net_connect(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, port), proven_net_deadline_in(5000), &c->sock) != PROVEN_OK) return false;
    c->t = proven_net_conn_transport(&c->sock);
    if (tls && proven_tls_transport_client(c->t, tls, sv("127.0.0.1"), NULL, proven_net_deadline_in(5000), &c->t) != PROVEN_OK) return false;
    proven_h2_config_t cfg = { .alloc = g_heap, .initial_window = window, .max_header_list_bytes = 65536 };
    return proven_h2_conn_create(&cfg, &c->e) == PROVEN_OK;
}

static void cl_close(cl_t *c) {
    proven_h2_conn_destroy(c->e);
    (void)proven_transport_close(c->t);
}

/* One round: write what is queued, read for at most `ms`, take every event. False when
 * nothing arrived. */
static bool cl_pump(cl_t *c, proven_u32 ms) {
    static proven_byte_t in[65536];
    proven_mem_view_t out = proven_h2_conn_pending_output(c->e);
    if (out.size > 0 && !c->gone) {
        if (!c->mute && proven_transport_write_all(c->t, out, proven_net_deadline_in(5000)).err != PROVEN_OK) c->gone = true;
        proven_h2_conn_output_sent(c->e, out.size);
    }
    proven_size_t n = 0;
    if (!c->gone) {
        proven_result_size_t got = proven_transport_read(c->t, (proven_mem_mut_t){ in, sizeof in }, proven_net_deadline_in(ms));
        if (got.err == PROVEN_OK) n = got.value;
        else if (got.err != PROVEN_ERR_TIMEOUT) c->gone = true;
    }
    proven_size_t pos = 0;
    while (!c->closed) {
        proven_size_t used = 0;
        proven_h2_event_t ev;
        if (proven_h2_conn_feed(c->e, (proven_mem_view_t){ n - pos ? in + pos : NULL, n - pos }, &used, &ev) != PROVEN_OK) { c->closed = true; break; }
        pos += used;
        res_t *r = ev.stream_id && (ev.stream_id - 1) / 2 < CL_STREAMS ? &c->r[(ev.stream_id - 1) / 2] : NULL;
        switch (ev.kind) {
            case PROVEN_H2_EVENT_HEADERS:
                if (r && ev.status >= 200) {
                    r->status = ev.status;
                    for (proven_size_t i = 0; i < ev.field_count; i++) {
                        proven_u8str_view_t nm = ev.fields[i].name, v = ev.fields[i].value;
                        if (nm.size == 16 && memcmp(nm.ptr, "content-encoding", 16) == 0 && v.size == 4 && memcmp(v.ptr, "gzip", 4) == 0) r->gzip = true;
                        if (nm.size == 14 && memcmp(nm.ptr, "content-length", 14) == 0) { r->has_length = true; r->length = 0; for (proven_size_t k = 0; k < v.size; k++) r->length = r->length * 10 + (proven_size_t)(v.ptr[k] - '0'); }
                        if (nm.size == 4 && memcmp(nm.ptr, "date", 4) == 0) r->has_date = true;
                        if (nm.size == 10 && memcmp(nm.ptr, "keep-alive", 10) == 0) r->has_keep_alive = true;
                    }
                    if (ev.end_stream) r->end = true;
                }
                break;
            case PROVEN_H2_EVENT_DATA:
                if (r) {
                    for (proven_size_t i = 0; i < ev.data.size; i++) {
                        if (r->got + i < sizeof r->first) r->first[r->got + i] = ev.data.ptr[i];
                        if (!r->gzip && !c->plain_body && ev.data.ptr[i] != pat(r->got + i)) r->bad = true;
                    }
                    r->got += ev.data.size;
                    if (ev.end_stream) r->end = true;
                }
                if (!c->hold) proven_h2_conn_consumed(c->e, ev.stream_id, ev.data.size);
                break;
            case PROVEN_H2_EVENT_TRAILERS: if (r) r->end = true; break;
            case PROVEN_H2_EVENT_STREAM_RESET: if (r) { r->reset = true; r->code = ev.code; } break;
            case PROVEN_H2_EVENT_GOAWAY: c->goaway = true; break;
            case PROVEN_H2_EVENT_CLOSED: c->closed = true; c->close_code = ev.code; break;
            default: break;
        }
        if (ev.kind == PROVEN_H2_EVENT_NONE && pos == n) break;
    }
    /* What the events queued (window updates) goes out with the next round, or now. */
    out = proven_h2_conn_pending_output(c->e);
    if (out.size > 0 && !c->gone) {
        if (!c->mute && proven_transport_write_all(c->t, out, proven_net_deadline_in(5000)).err != PROVEN_OK) c->gone = true;
        proven_h2_conn_output_sent(c->e, out.size);
    }
    return n > 0;
}

static proven_u32 cl_ask(cl_t *c, const char *method, const char *path, bool end) {
    proven_http_header_t h[] = { H(":method", method), H(":scheme", "http"), H(":authority", "127.0.0.1"), H(":path", path), H("accept-encoding", "gzip") };
    proven_u32 id = 0;
    PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(c->e, h, 5, end, &id) == PROVEN_OK, "a request is begun", path);
    return id;
}

/* Send a body of the pattern, pumping while the windows are shut. */
static void cl_body(cl_t *c, proven_u32 id, proven_size_t len, bool end) {
    static proven_byte_t buf[1 << 16];
    proven_size_t sent = 0;
    for (int round = 0; round < 20000 && (sent < len || (len == 0 && end && round == 0)); round++) {
        proven_size_t n = len - sent < sizeof buf ? len - sent : sizeof buf;
        for (proven_size_t i = 0; i < n; i++) buf[i] = pat(sent + i);
        proven_result_size_t w = proven_h2_conn_send_data(c->e, id, (proven_mem_view_t){ buf, n }, end && sent + n == len);
        if (w.err != PROVEN_OK) return;
        sent += w.value;
        if (w.value < n || sent < len) (void)cl_pump(c, 5);
    }
}

static res_t *cl_res(cl_t *c, proven_u32 id) { return &c->r[(id - 1) / 2]; }

/* Pump until the stream has ended one way or the other. */
static bool cl_wait(cl_t *c, proven_u32 id, proven_u32 ms) {
    proven_time_t until = proven_time_monotonic_now() + (proven_time_t)ms * 1000000;
    while (!cl_res(c, id)->end && !cl_res(c, id)->reset && !c->closed && !c->gone && proven_time_monotonic_now() < until) (void)cl_pump(c, 20);
    return cl_res(c, id)->end || cl_res(c, id)->reset;
}

static bool cl_get(cl_t *c, const char *path, int status, proven_size_t size) {
    proven_u32 id = cl_ask(c, "GET", path, true);
    if (!cl_wait(c, id, 8000)) return false;
    res_t *r = cl_res(c, id);
    return r->end && !r->reset && r->status == status && r->got == size && !r->bad;
}

// -----------------------------------------------------------------------------
// The cases, for one transport
// -----------------------------------------------------------------------------

static void cases(proven_tls_config_t *client_tls, const char *how) {
    proven_http_event_server_config_t cfg = { .tls = client_tls ? g_server_tls : NULL };
    proven_u16 port = server_start(cfg);
    cl_t c;
    PROVEN_TEST_ASSERT(cl_open_with(&c, client_tls, port, 0), "a client connects", how);

    c.plain_body = true;
    PROVEN_TEST_ASSERT(cl_get(&c, "/hello", 200, 5) && memcmp(c.r[0].first, "hello", 5) == 0, "a request is answered", how);
    PROVEN_TEST_ASSERT(c.r[0].has_length && c.r[0].length == 5 && c.r[0].has_date && !c.r[0].has_keep_alive,
        "the response has its length and a date, and the field that describes an HTTP/1.1 connection is not sent", how);
    proven_u32 id = cl_ask(&c, "GET", "/proto", true);
    PROVEN_TEST_ASSERT(cl_wait(&c, id, 5000) && cl_res(&c, id)->first[0] == '2', "the handler is told the protocol is 2, and sees a Host field", how);
    PROVEN_TEST_ASSERT(cl_get(&c, "/nowhere", 404, 2), "a path nobody serves is 404", how);
    PROVEN_TEST_ASSERT(cl_get(&c, "/ws", 400, 2), "an upgrade to WebSocket asked of an HTTP/2 stream is refused, and the stream answered as any other", how);
    id = cl_ask(&c, "HEAD", "/hello", true);
    PROVEN_TEST_ASSERT(cl_wait(&c, id, 5000) && cl_res(&c, id)->status == 200 && cl_res(&c, id)->got == 0 && cl_res(&c, id)->length == 5, "HEAD has the length and no body", how);
    PROVEN_TEST_ASSERT(cl_get(&c, "/bad-header", 200, 2), "a response head the server would not send is refused to the handler, which may send another", how);
    c.plain_body = false;

    /* Bodies down: of a known length, of an unknown one, and compressed - with a window
     * small enough that the server's writes are refused and resumed many times. */
    PROVEN_TEST_ASSERT(cl_get(&c, "/big/0", 200, 0) && cl_get(&c, "/big/1", 200, 1) && cl_get(&c, "/big/300000", 200, 300000), "bodies of a known length arrive whole", how);
    PROVEN_TEST_ASSERT(cl_get(&c, "/stream/200000", 200, 200000), "a body of unknown length arrives whole", how);
    PROVEN_TEST_ASSERT(atomic_load(&g_writable) > 0, "a write that was refused is told when it may go on", how);
    id = cl_ask(&c, "GET", "/gzip/100000", true);
    PROVEN_TEST_ASSERT(cl_wait(&c, id, 8000) && cl_res(&c, id)->gzip && !cl_res(&c, id)->has_length && cl_res(&c, id)->got > 100 && cl_res(&c, id)->got < 30000 && cl_res(&c, id)->first[0] == 0x1f,
        "a streamed response that asked for it is compressed: gzip, shorter, and no length announced", how);
    id = cl_ask(&c, "GET", "/gzip-whole", true);
    PROVEN_TEST_ASSERT(cl_wait(&c, id, 8000) && cl_res(&c, id)->gzip && cl_res(&c, id)->has_length && cl_res(&c, id)->length == cl_res(&c, id)->got && cl_res(&c, id)->got < 20000,
        "a whole response that asked for it is compressed, with the compressed length", how);

    /* A handler learns that a body is coming the way HTTP/1.1 taught it: from the fields. */
    c.plain_body = true;
    id = cl_ask(&c, "GET", "/body-sign", true);
    PROVEN_TEST_ASSERT(cl_wait(&c, id, 5000) && memcmp(cl_res(&c, id)->first, "none", 4) == 0, "a request with no body has no sign of one", how);
    id = cl_ask(&c, "POST", "/body-sign", false);
    cl_body(&c, id, 10, true);
    PROVEN_TEST_ASSERT(cl_wait(&c, id, 5000) && memcmp(cl_res(&c, id)->first, "chunked", 7) == 0,
        "a body that HTTP/2 sends with no length announced is given the field that means that: Transfer-Encoding: chunked", how);
    {
        proven_http_header_t h[] = { H(":method", "POST"), H(":scheme", "http"), H(":authority", "127.0.0.1"), H(":path", "/body-sign"), H("content-length", "10") };
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(c.e, h, 5, false, &id) == PROVEN_OK, "a request that announces its length", how);
        cl_body(&c, id, 10, true);
        PROVEN_TEST_ASSERT(cl_wait(&c, id, 5000) && memcmp(cl_res(&c, id)->first, "length", 6) == 0, "and one that announces its length is left as it is", how);
    }
    c.plain_body = false;

    /* Bodies up, echoed. */
    static const proven_size_t sizes[] = { 0, 1, 16384, 65535, 65536, 200000 };
    for (proven_size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        id = cl_ask(&c, "POST", "/echo", false);
        cl_body(&c, id, sizes[k], true);
        PROVEN_TEST_ASSERT(cl_wait(&c, id, 10000) && cl_res(&c, id)->status == 200 && cl_res(&c, id)->got == sizes[k] && !cl_res(&c, id)->bad, "a request body is echoed whole", how);
    }

    /* Many at once on the one connection. */
    proven_u32 ids[60];
    for (int i = 0; i < 60; i++) ids[i] = cl_ask(&c, "GET", i % 3 == 0 ? "/big/20000" : i % 3 == 1 ? "/stream/5000" : "/big/0", true);
    bool all = true;
    for (int i = 0; i < 60; i++) {
        all = all && cl_wait(&c, ids[i], 10000);
        res_t *r = cl_res(&c, ids[i]);
        all = all && r->status == 200 && !r->bad && r->got == (proven_size_t)(i % 3 == 0 ? 20000 : i % 3 == 1 ? 5000 : 0);
    }
    PROVEN_TEST_ASSERT(all, "sixty requests in flight together are each answered in full", how);

    /* Pause: nothing of the body reaches the handler until it resumes, and then all of it. */
    id = cl_ask(&c, "POST", "/pause", false);
    cl_body(&c, id, 50000, true);
    PROVEN_TEST_ASSERT(cl_wait(&c, id, 10000) && cl_res(&c, id)->got == 50000 && !cl_res(&c, id)->bad && atomic_load(&g_bodies_while_paused) == 0,
        "a paused request's body is held back, then delivered whole on resume", how);

    /* The handler gives up; the handler ends short of what it promised. */
    id = cl_ask(&c, "GET", "/abort", true);
    PROVEN_TEST_ASSERT(cl_wait(&c, id, 5000) && cl_res(&c, id)->reset, "an aborted exchange is a reset stream, and the connection lives", how);
    id = cl_ask(&c, "GET", "/short", true);
    PROVEN_TEST_ASSERT(cl_wait(&c, id, 5000) && cl_res(&c, id)->reset && !cl_res(&c, id)->end, "a response that ends short of its promised length is reset, not passed off as whole", how);
    c.plain_body = true;
    PROVEN_TEST_ASSERT(cl_get(&c, "/hello", 200, 5), "and the next request on the same connection is served", how);

    /* The client gives up in the middle of a download. */
    c.hold = true;
    id = cl_ask(&c, "GET", "/big/2000000", true);
    for (int i = 0; i < 25; i++) (void)cl_pump(&c, 20);
    const proven_size_t stopped_at = cl_res(&c, id)->got;
    const int told = atomic_load(&g_writable);
    for (int i = 0; i < 10; i++) (void)cl_pump(&c, 20);
    /* (At most a window; at least half of one, since that is how far behind the client's
     * returns for the connection as a whole may be.) */
    PROVEN_TEST_ASSERT(stopped_at >= 32768 && stopped_at <= 65535 && cl_res(&c, id)->got == stopped_at && !cl_res(&c, id)->end,
        "a client that consumes nothing is sent what its windows allow - at most 65,535 bytes - and then nothing", how);
    PROVEN_TEST_ASSERT(atomic_load(&g_writable) - told <= 1, "and while its window is shut the handler is not told, over and over, that it may write", how);
    const int before_reset = atomic_load(&g_done_reset);
    PROVEN_TEST_ASSERT(proven_h2_conn_reset(c.e, id, PROVEN_H2_CANCEL) == PROVEN_OK, "the client resets the stream", how);
    c.hold = false;
    for (int i = 0; i < 200 && atomic_load(&g_done_reset) == before_reset; i++) (void)cl_pump(&c, 10);
    PROVEN_TEST_ASSERT(atomic_load(&g_done_reset) == before_reset + 1, "the handler is told the exchange ended in a reset", how);
    PROVEN_TEST_ASSERT(cl_get(&c, "/hello", 200, 5), "and the connection goes on", how);

    PROVEN_TEST_ASSERT(settled(atomic_load(&g_requests)) && atomic_load(&g_done_other) == 1 && atomic_load(&g_dispatch_bad) == 0 && atomic_load(&g_protocol_bad) == 0,
        "every request the handler was given was finished exactly once, and no call mistook the kind of stream", how);
    PROVEN_TEST_ASSERT(connections() == 1, "all of it on one connection", how);

    /* A client that closes its sending side after asking still gets its answer. */
    cl_t half;
    PROVEN_TEST_ASSERT(cl_open_with(&half, client_tls, port, 0), "a second client", how);
    half.plain_body = false;
    /* The answer is not ready until after the client has closed its side: a server that took
     * end of input for the end of the connection would never send it. (Sixty thousand bytes:
     * within the window the client opened with, since it can no longer say that it wants more.) */
    id = cl_ask(&half, "GET", "/later/60000", true);
    (void)cl_pump(&half, 1);
    PROVEN_TEST_ASSERT(proven_transport_shutdown(half.t) == PROVEN_OK, "it closes its sending side", how);
    half.mute = true;
    PROVEN_TEST_ASSERT(cl_wait(&half, id, 8000) && cl_res(&half, id)->end && cl_res(&half, id)->got == 60000 && !cl_res(&half, id)->bad, "and is still sent the whole response", how);
    cl_close(&half);

    /* Goodbye: the server stops listening; what is open finishes; the connection ends. */
    c.plain_body = false;
    id = cl_ask(&c, "GET", "/big/500000", true);
    (void)cl_pump(&c, 20);
    on_loop(do_stop_listening);
    PROVEN_TEST_ASSERT(cl_wait(&c, id, 10000) && cl_res(&c, id)->end && cl_res(&c, id)->got == 500000 && !cl_res(&c, id)->bad, "a server that stops listening finishes the response it had begun", how);
    for (int i = 0; i < 300 && !c.closed && !c.gone; i++) (void)cl_pump(&c, 10);
    PROVEN_TEST_ASSERT(c.goaway && c.closed && c.close_code == PROVEN_H2_NO_ERROR, "says goodbye, and the connection ends without an error", how);
    for (int i = 0; i < 2000 && connections() != 0; i++) proven_time_sleep(1);
    PROVEN_TEST_ASSERT(connections() == 0, "and the server has let go of it", how);
    cl_close(&c);
    server_stop();
}

/* An allocator that counts what is live. */
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
    if (old) { memcpy(m.value.ptr, old, old_size < new_size ? old_size : new_size); count_free(ctx, old); }
    return m;
}
static void quiet_request(void *ctx, proven_http_stream_t *s, const proven_http_request_t *req) {
    (void)ctx; (void)req;
    (void)proven_http_stream_respond(s, 200, NULL, 0, (proven_mem_view_t){ (const proven_byte_t *)"ok", 2 });
}

/* What an HTTP/2 connection that has made a request and now says nothing costs the server. */
static void memory_case(bool no_tables) {
    counting_t count = { .heap = g_heap, .live = 0 };
    proven_allocator_t alloc = { .ctx = &count, .alloc_fn = count_alloc, .realloc_fn = count_realloc, .free_fn = count_free };
    proven_loop_t *loop = NULL;
    proven_http_event_server_t *server = NULL;
    proven_http_event_server_config_t cfg = { .alloc = alloc, .on = { .on_request = quiet_request }, .http2 = true, .http2_no_header_table = no_tables };
    proven_net_addr_t at;
    PROVEN_TEST_ASSERT(proven_loop_create(g_heap, &loop) == PROVEN_OK && proven_http_event_server_create(loop, &cfg, &server) == PROVEN_OK &&
                       proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "a server with a counting allocator", "");
    enum { N = 200 };
    static proven_net_conn_t socks[N];
    const proven_size_t base = count.live;
    for (int i = 0; i < N; ++i) {
        /* What a client sends to open a connection and ask for one page, made by an engine of its own. */
        proven_h2_conn_t *e = NULL;
        proven_h2_config_t ecfg = { .alloc = g_heap };
        proven_http_header_t h[] = { H(":method", "GET"), H(":scheme", "http"), H(":authority", "x"), H(":path", "/") };
        proven_u32 id;
        PROVEN_TEST_ASSERT(proven_h2_conn_create(&ecfg, &e) == PROVEN_OK && proven_h2_conn_submit_request(e, h, 4, true, &id) == PROVEN_OK, "a client's opening and one request", "");
        PROVEN_TEST_ASSERT(proven_net_connect(at, proven_net_deadline_in(5000), &socks[i]) == PROVEN_OK &&
                           proven_net_write_all(&socks[i], proven_h2_conn_pending_output(e), proven_net_deadline_in(5000)).err == PROVEN_OK, "sent on a connection of its own", "");
        proven_h2_conn_destroy(e);
        if (i % 16 == 15) PROVEN_TEST_ASSERT(proven_loop_poll(loop, proven_net_deadline_in(20)) == PROVEN_OK, "a round", "");
    }
    proven_net_deadline_t until = proven_net_deadline_in(400);
    while (proven_time_monotonic_now() < until) PROVEN_TEST_ASSERT(proven_loop_poll(loop, until) == PROVEN_OK, "a round", "");
    PROVEN_TEST_ASSERT(proven_http_event_server_connections(server) == N, "two hundred HTTP/2 connections, each answered once, all idle", "");
    const proven_size_t shared = proven_h2_header_buffer_size(0);
    const proven_size_t each = (count.live - base - shared) / N;
    fprintf(stderr, "[PROVEN][TEST][INFO] an idle HTTP/2 connection %s holds %u bytes; one buffer of %u bytes for reading header lists is shared by all\n",
            no_tables ? "with no header tables" : "with the default header tables", (unsigned)each, (unsigned)shared);
    if (no_tables) PROVEN_TEST_ASSERT(each < 2048, "with http2_no_header_table an idle HTTP/2 connection holds under 2 KiB", "");
    PROVEN_TEST_ASSERT(count.live - base > shared && each < 3072,
        "an idle HTTP/2 connection that has carried one small request holds under 3 KiB: the record, the driver, the engine, and header tables that have grown only as far as they were used - and no buffer of its own for reading header lists", "");
    for (int i = 0; i < N; ++i) (void)proven_net_close(&socks[i]);
    until = proven_net_deadline_in(300);
    while (proven_time_monotonic_now() < until) PROVEN_TEST_ASSERT(proven_loop_poll(loop, until) == PROVEN_OK, "a round", "");
    PROVEN_TEST_ASSERT(proven_http_event_server_connections(server) == 0 && count.live == base + shared, "closed by their clients, they have given everything back", "");
    proven_http_event_server_destroy(server);
    PROVEN_TEST_ASSERT(count.live == 0, "and so has the server, destroyed", "");
    proven_loop_destroy(loop);
}

int main(void) {
    PROVEN_TEST_SUITE("the event-driven HTTP server, speaking HTTP/2",
        "The handlers of an HTTP/1.1 server, unchanged, answering an HTTP/2 client: requests, bodies both ways under flow control, compression, pause, resets from either side, limits, time, goodbye - in the clear by prior knowledge and over TLS with ALPN - and both protocols on one listener.",
        "Inspect src/proven/http_event_h2.c: d_request and d_body for what arrives, s_head and s_push for what leaves, d_pump for the round, d_arm and h2_on_timer for time. The choice of protocol is in ev_process (http_event.c).");

    g_heap = proven_heap_allocator();
    PROVEN_TEST_ASSERT(tp_build(), "the test's certificates and keys are issued", "");
    PROVEN_TEST_ASSERT(proven_job_system_init(g_heap, 1, 4, &g_threads) == PROVEN_OK, "a thread for the loop", "");
    proven_cert_store_t *anchors = NULL;
    PROVEN_TEST_ASSERT(proven_cert_store_create(g_heap, &anchors) == PROVEN_OK &&
                       proven_cert_store_add_pem(anchors, (proven_mem_view_t){ (const proven_byte_t *)TP_CA, strlen(TP_CA) }, NULL) == PROVEN_OK, "anchors", "");
    static const proven_u8str_view_t both[] = { { (const proven_byte_t *)"h2", 2 }, { (const proven_byte_t *)"http/1.1", 8 } };
    proven_tls_options_t co = { .alloc = g_heap, .anchors = anchors, .alpn = both, .alpn_count = 2 };
    proven_tls_options_t co_h1 = { .alloc = g_heap, .anchors = anchors };
    proven_tls_options_t co_12 = co; co_12.max_version = PROVEN_TLS_VERSION_1_2;
    proven_tls_options_t so = { .alloc = g_heap, .certificate_pem = { (const proven_byte_t *)TP_SERVER_ED, strlen(TP_SERVER_ED) }, .private_key_pem = { (const proven_byte_t *)TP_SERVER_ED_KEY, strlen(TP_SERVER_ED_KEY) },
                                .alpn = both, .alpn_count = 2 };
    proven_tls_options_t so_plain = so; so_plain.alpn = NULL; so_plain.alpn_count = 0;
    proven_tls_config_t *server_no_alpn = NULL;
    PROVEN_TEST_ASSERT(proven_tls_config_create(&co, &g_client_tls) == PROVEN_OK && proven_tls_config_create(&co_h1, &g_client_tls_h1) == PROVEN_OK &&
                       proven_tls_config_create(&co_12, &g_client_tls_12) == PROVEN_OK && proven_tls_config_create(&so, &g_server_tls) == PROVEN_OK &&
                       proven_tls_config_create(&so_plain, &server_no_alpn) == PROVEN_OK, "TLS configurations", "");

    PROVEN_TEST_SECTION("making a server", "What TLS may agree to and what the server will speak must be the same thing.", "Check the ALPN comparison in proven_http_event_server_create.");
    {
        proven_loop_t *loop = NULL;
        proven_http_event_server_t *s = NULL;
        PROVEN_TEST_ASSERT(proven_loop_create(g_heap, &loop) == PROVEN_OK, "a loop", "");
        proven_http_event_server_config_t cfg = { .on = { .on_request = on_request }, .http2 = true, .tls = server_no_alpn };
        PROVEN_TEST_ASSERT(proven_http_event_server_create(loop, &cfg, &s) == PROVEN_ERR_INVALID_ARG && s == NULL, "HTTP/2 over a TLS configuration that does not offer h2 is refused", "");
        cfg.http2 = false; cfg.tls = g_server_tls;
        PROVEN_TEST_ASSERT(proven_http_event_server_create(loop, &cfg, &s) == PROVEN_ERR_INVALID_ARG && s == NULL, "and so is a TLS configuration that offers h2 to a server that will not speak it", "");
        cfg.http2 = true;
        PROVEN_TEST_ASSERT(proven_http_event_server_create(loop, &cfg, &s) == PROVEN_OK && s, "the two together are a server", "");
        proven_http_event_server_destroy(s);
        proven_loop_destroy(loop);
    }

    PROVEN_TEST_SECTION("in the clear", "A client that begins with the HTTP/2 preface, on a server that also speaks HTTP/1.1.", "Check the preface comparison in ev_process and proven_http_event_h2_take_.");
    cases(NULL, "in the clear");
    PROVEN_TEST_SECTION("over TLS 1.3", "ALPN chooses h2.", "Check the ALPN branch in ev_process and proven_http_event_raw_offer_ under TLS.");
    cases(g_client_tls, "over TLS 1.3");
    PROVEN_TEST_SECTION("over TLS 1.2", "The same, for a client that speaks nothing newer.", "Check h2_tls_adequate: the 1.2 suites this library offers by default are all allowed.");
    cases(g_client_tls_12, "over TLS 1.2");

    PROVEN_TEST_SECTION("both protocols on one listener", "HTTP/1.1 clients of every kind on a server that may speak HTTP/2.", "Check that ev_process falls through to the HTTP/1.1 parser when the preface, or ALPN, says so.");
    for (int tls = 0; tls < 2; tls++) {
        proven_http_event_server_config_t cfg = { .tls = tls ? g_server_tls : NULL };
        proven_u16 port = server_start(cfg);
        char url[96];
        static const char *const paths[] = { "/proto", "/hello" };
        for (int k = 0; k < 2; k++) {
            snprintf(url, sizeof url, "%s://127.0.0.1:%u%s", tls ? "https" : "http", (unsigned)port, paths[k]);
            proven_http_client_t *client = NULL;
            proven_http_client_config_t cc = { .alloc = g_heap, .max_idle_connections = 1, .tls_wrap = tls ? proven_tls_http_wrap : NULL, .tls_ctx = g_client_tls_h1 };
            PROVEN_TEST_ASSERT(proven_http_client_create(&cc, &client) == PROVEN_OK, "the blocking HTTP/1.1 client", "");
            proven_http_client_request_t rq = { .method = sv(k == 1 ? "POST" : "GET"), .url = sv(url) };
            proven_http_client_response_t resp;
            proven_u8str_t body = { 0 };
            PROVEN_TEST_ASSERT(proven_http_client_send(client, &rq, &resp) == PROVEN_OK && resp.status == 200 &&
                               proven_http_client_read_all(&resp, g_heap, &body, 4096) == PROVEN_OK, "an HTTP/1.1 request to the same port is answered", tls ? "over TLS, no ALPN from the client" : "in the clear");
            proven_u8str_view_t v = proven_u8str_as_view(&body);
            PROVEN_TEST_ASSERT(k == 1 ? (v.size == 5 && memcmp(v.ptr, "hello", 5) == 0) : (v.size == 1 && v.ptr[0] == '1'),
                k == 1 ? "a POST - which begins with the preface's first letter - is HTTP/1.1" : "and the handler is told the protocol is 1", "");
            proven_u8str_destroy(g_heap, &body);
            proven_http_client_finish(&resp);
            proven_http_client_destroy(client);
        }
        /* And an HTTP/2 client on the same listener, at the same time. */
        cl_t c;
        PROVEN_TEST_ASSERT(cl_open_with(&c, tls ? g_client_tls : NULL, port, 0), "an HTTP/2 client on the same port", "");
        c.plain_body = true;
        proven_u32 id = cl_ask(&c, "GET", "/proto", true);
        PROVEN_TEST_ASSERT(cl_wait(&c, id, 5000) && cl_res(&c, id)->first[0] == '2', "is HTTP/2", "");
        cl_close(&c);
        PROVEN_TEST_ASSERT(settled(atomic_load(&g_requests)) && atomic_load(&g_protocol_bad) == 0, "every exchange finished, each with one Host field", "");
        server_stop();
    }

    PROVEN_TEST_SECTION("limits", "Streams at once, the size of a body and of a head, and a request that names two targets.", "Check d_request and d_body, and the engine's configuration in proven_http_event_h2_take_.");
    {
        proven_http_event_server_config_t cfg = { .max_concurrent_streams = 3, .max_body_bytes = 10000, .max_headers = 8 };
        proven_u16 port = server_start(cfg);
        cl_t c;
        PROVEN_TEST_ASSERT(cl_open_with(&c, NULL, port, 0), "a client", "");
        c.plain_body = true;
        (void)cl_pump(&c, 50);                      /* the server's settings: it allows three */
        proven_u32 ids[3], extra = 0;
        for (int i = 0; i < 3; i++) ids[i] = cl_ask(&c, "GET", "/never", true);
        proven_http_header_t h[] = { H(":method", "GET"), H(":scheme", "http"), H(":authority", "127.0.0.1"), H(":path", "/hello") };
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(c.e, h, 4, true, &extra) == PROVEN_ERR_BUSY, "the client's own engine holds it to the three streams the server allows", "");
        for (int i = 0; i < 20; i++) (void)cl_pump(&c, 10);
        PROVEN_TEST_ASSERT(atomic_load(&g_requests) == 3, "three requests are with the handler", "");
        for (int i = 0; i < 3; i++) PROVEN_TEST_ASSERT(proven_h2_conn_reset(c.e, ids[i], PROVEN_H2_CANCEL) == PROVEN_OK, "the client gives them up", "");
        for (int i = 0; i < 300 && atomic_load(&g_done_reset) < 3; i++) (void)cl_pump(&c, 10);
        PROVEN_TEST_ASSERT(atomic_load(&g_done_reset) == 3, "and the handler is told of each", "");

        proven_u32 id = cl_ask(&c, "POST", "/sink", false);
        cl_body(&c, id, 10000, true);
        PROVEN_TEST_ASSERT(cl_wait(&c, id, 5000) && cl_res(&c, id)->status == 200 && cl_res(&c, id)->got == 9 && memcmp(cl_res(&c, id)->first, "got 1000", 8) == 0, "a body of exactly the limit is taken", "");
        id = cl_ask(&c, "POST", "/sink", false);
        cl_body(&c, id, 10001, true);
        PROVEN_TEST_ASSERT(cl_wait(&c, id, 5000) && cl_res(&c, id)->status == 413, "one byte more is 413, sent before the body had finished arriving", "");
        proven_http_header_t announced[] = { H(":method", "POST"), H(":scheme", "http"), H(":authority", "127.0.0.1"), H(":path", "/sink"), H("content-length", "10001") };
        const int before = atomic_load(&g_requests);
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(c.e, announced, 5, false, &id) == PROVEN_OK && cl_wait(&c, id, 5000) && cl_res(&c, id)->status == 413 && atomic_load(&g_requests) == before,
            "a body announced as too large is 413 before the handler is troubled", "");
        proven_http_header_t many[] = { H(":method", "GET"), H(":scheme", "http"), H(":authority", "127.0.0.1"), H(":path", "/hello"),
                                        H("a", "1"), H("b", "2"), H("c", "3"), H("d", "4"), H("e", "5"), H("f", "6"), H("g", "7"), H("i", "8") };
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(c.e, many, 12, true, &id) == PROVEN_OK && cl_wait(&c, id, 5000) && cl_res(&c, id)->status == 431 && atomic_load(&g_requests) == before,
            "more fields than the server's array holds is 431, never copied past it", "");
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(c.e, many, 11, true, &id) == PROVEN_OK && cl_wait(&c, id, 5000) && cl_res(&c, id)->status == 200,
            "one fewer - the array exactly full, with the Host field the server adds - is served", "");
        proven_http_header_t two[] = { H(":method", "GET"), H(":scheme", "http"), H(":authority", "127.0.0.1"), H(":path", "/hello"), H("host", "elsewhere.example") };
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(c.e, two, 5, true, &id) == PROVEN_OK && cl_wait(&c, id, 5000) && cl_res(&c, id)->reset && cl_res(&c, id)->code == PROVEN_H2_PROTOCOL_ERROR,
            "a request whose Host and :authority disagree is reset: it is never passed on", "");
        proven_http_header_t same[] = { H(":method", "GET"), H(":scheme", "http"), H(":authority", "127.0.0.1"), H(":path", "/proto"), H("host", "127.0.0.1") };
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(c.e, same, 5, true, &id) == PROVEN_OK && cl_wait(&c, id, 5000) && cl_res(&c, id)->status == 200,
            "one whose Host and :authority agree is served, with the one Host field", "");
        PROVEN_TEST_ASSERT(!c.closed && !c.gone && atomic_load(&g_protocol_bad) == 0, "and none of this ended the connection", "");
        cl_close(&c);
        server_stop();
    }

    PROVEN_TEST_SECTION("a client that does not read", "Output for a client whose socket is full, with windows that would allow everything.", "Check d_room: body bytes are refused when the engine holds max_buffered_output, whatever the peer's windows say.");
    for (int tls = 0; tls < 2; tls++) {
        proven_http_event_server_config_t cfg = { .tls = tls ? g_server_tls : NULL };
        proven_u16 port = server_start(cfg);
        cl_t c;
        PROVEN_TEST_ASSERT(cl_open_with(&c, tls ? g_client_tls : NULL, port, 1u << 30), "a client whose windows are a gigabyte", "");
        atomic_store(&g_max_buffered, 0);
        proven_u32 id = cl_ask(&c, "GET", "/big/20000000", true);
        (void)cl_pump(&c, 20);
        /* It reads nothing: its socket fills, and then the server's. However slow the machine,
         * that happens; the wait is for it to have happened, and then a little longer to see
         * that the amount held has stopped growing. */
        for (int i = 0; i < 500 && atomic_load(&g_max_buffered) <= 30000; i++) proven_time_sleep(10);
        proven_time_sleep(300);
        const unsigned long held = atomic_load(&g_max_buffered);
        PROVEN_TEST_ASSERT(held > 30000 && held <= 100000, "the server holds about max_buffered_output for it - 64 KiB, a piece, and with TLS a record - and no more", tls ? "over TLS" : "in the clear");
        PROVEN_TEST_ASSERT(cl_wait(&c, id, 20000) && cl_res(&c, id)->end && cl_res(&c, id)->got == 20000000 && !cl_res(&c, id)->bad, "and when it reads, all twenty megabytes arrive", "");
        PROVEN_TEST_ASSERT(atomic_load(&g_max_buffered) <= 100000, "with the bound held all the way", "");
        /* Two more at once, to a client that takes them as fast as they come: every write is
         * refused and resumed, and a second stream is ready whenever the first is. */
        proven_u32 a2 = cl_ask(&c, "GET", "/big/3000000", true), b2 = cl_ask(&c, "GET", "/big/6000000", true);
        PROVEN_TEST_ASSERT(cl_wait(&c, a2, 20000) && cl_wait(&c, b2, 20000) && cl_res(&c, a2)->got == 3000000 && cl_res(&c, b2)->got == 6000000 && !cl_res(&c, a2)->bad && !cl_res(&c, b2)->bad,
            "two large responses side by side arrive whole and in order", tls ? "over TLS" : "in the clear");
        PROVEN_TEST_ASSERT(atomic_load(&g_reentered) == 0, "and on_writable is never called from inside the handler's own write", "");
        cl_close(&c);
        server_stop();
    }

    PROVEN_TEST_SECTION("a hundred requests in one read, answered whole", "Whole-body responses that together are many times what the server may hold as output.", "Check d_pump and proven_http_event_h2_respond_: output held back while a read is worked through leaves before the engine is full, and a body never takes the room a later response's head needs - no stream is reset for want of room.");
    for (int tls = 0; tls < 2; tls++) {
        /* A bound so small that the heads of a hundred responses are more than the engine may
         * hold, never mind their bodies. */
        proven_http_event_server_config_t cfg = { .tls = tls ? g_server_tls : NULL, .max_buffered_output = 600 };
        proven_u16 port = server_start(cfg);
        cl_t c;
        PROVEN_TEST_ASSERT(cl_open_with(&c, tls ? g_client_tls : NULL, port, 1u << 20), "a client", "");
        PROVEN_TEST_ASSERT(cl_get(&c, "/whole/10", 200, 10), "the connection is up", "");
        /* All of them queued before a byte is written: they leave in one write and, as far as
         * the system allows, arrive in one read. */
        proven_u32 ids[100];
        for (int i = 0; i < 100; i++) ids[i] = cl_ask(&c, "GET", i % 2 ? "/whole/3000" : "/whole/700", true);
        int good = 0, resets = 0;
        for (int i = 0; i < 100; i++) {
            (void)cl_wait(&c, ids[i], 10000);
            const res_t *r = cl_res(&c, ids[i]);
            good += r->end && !r->reset && r->status == 200 && r->got == (proven_size_t)(i % 2 ? 3000 : 700) && !r->bad;
            resets += r->reset;
        }
        PROVEN_TEST_ASSERT(good == 100 && resets == 0 && !c.closed, "all hundred are answered whole and none is reset", tls ? "over TLS" : "in the clear");
        cl_close(&c);
        server_stop();
    }
    {
        /* And with the default bound, bodies that together are more than the socket will take
         * from a server whose client is not reading yet: the engine's output stays full, and
         * the responses behind still have to begin. */
        proven_http_event_server_config_t cfg = { 0 };
        proven_u16 port = server_start(cfg);
        cl_t c;
        PROVEN_TEST_ASSERT(cl_open_with(&c, NULL, port, 1u << 30), "a client whose windows hold everything asked for", "");
        PROVEN_TEST_ASSERT(cl_get(&c, "/whole/10", 200, 10), "the connection is up", "");
        proven_u32 ids[100];
        for (int i = 0; i < 100; i++) ids[i] = cl_ask(&c, "GET", "/whole/300000", true);
        int good = 0, resets = 0;
        for (int i = 0; i < 100; i++) {
            (void)cl_wait(&c, ids[i], 30000);
            const res_t *r = cl_res(&c, ids[i]);
            good += r->end && !r->reset && r->status == 200 && r->got == 300000 && !r->bad;
            resets += r->reset;
        }
        PROVEN_TEST_ASSERT(good == 100 && resets == 0 && !c.closed, "thirty megabytes in a hundred whole responses arrive, and none is reset", "");
        cl_close(&c);
        server_stop();
    }

    PROVEN_TEST_SECTION("time", "A request whose body stops arriving, a connection with nothing on it, and one that never asks.", "Check d_arm and h2_on_timer.");
    {
        proven_http_event_server_config_t cfg = { .body_timeout_ms = 150, .idle_timeout_ms = 400, .head_timeout_ms = 300 };
        proven_u16 port = server_start(cfg);
        cl_t c;
        PROVEN_TEST_ASSERT(cl_open_with(&c, NULL, port, 0), "a client", "");
        c.plain_body = true;
        proven_u32 stalled = cl_ask(&c, "POST", "/sink", false);
        cl_body(&c, stalled, 100, false);
        PROVEN_TEST_ASSERT(cl_wait(&c, stalled, 3000) && cl_res(&c, stalled)->reset && atomic_load(&g_done_timeout) == 1,
            "a request whose body stops arriving is reset after the body timeout, and its handler told", "");
        PROVEN_TEST_ASSERT(!c.closed && cl_get(&c, "/hello", 200, 5), "the connection itself goes on", "");
        /* A stalled request beside a busy one: its own silence is what is measured, so the
         * neighbour's traffic does not keep it alive. */
        const int timeouts = atomic_load(&g_done_timeout);
        proven_u32 quiet = cl_ask(&c, "POST", "/sink", false), busy = cl_ask(&c, "POST", "/sink", false);
        cl_body(&c, quiet, 10, false);
        for (int i = 0; i < 40 && !cl_res(&c, quiet)->reset; i++) { cl_body(&c, busy, 10, false); (void)cl_pump(&c, 20); }
        PROVEN_TEST_ASSERT(cl_res(&c, quiet)->reset && atomic_load(&g_done_timeout) == timeouts + 1 && !cl_res(&c, busy)->reset && !cl_res(&c, busy)->end,
            "a request that has stalled is reset though another on the same connection keeps sending - and that other is left alone", "");
        cl_body(&c, busy, 0, true);
        PROVEN_TEST_ASSERT(cl_wait(&c, busy, 3000) && cl_res(&c, busy)->status == 200, "which then finishes", "");
        for (int i = 0; i < 300 && !c.closed && !c.gone; i++) (void)cl_pump(&c, 10);
        PROVEN_TEST_ASSERT(c.goaway && c.closed && c.close_code == PROVEN_H2_NO_ERROR, "left with nothing to do, the server says goodbye after the idle timeout", "");
        cl_close(&c);
        /* A connection that opens and never asks for anything. */
        PROVEN_TEST_ASSERT(cl_open_with(&c, NULL, port, 0), "another client", "");
        for (int i = 0; i < 300 && !c.gone && !c.closed; i++) (void)cl_pump(&c, 10);
        PROVEN_TEST_ASSERT(c.gone || c.closed, "a client that connects and asks for nothing is dropped after the head timeout", "");
        cl_close(&c);
        for (int i = 0; i < 2000 && connections() != 0; i++) proven_time_sleep(1);
        PROVEN_TEST_ASSERT(connections() == 0, "and the server holds neither connection", "");
        server_stop();
    }

    PROVEN_TEST_SECTION("what an idle connection holds", "Two hundred HTTP/2 connections that have each made a request and now say nothing.", "Check the engine's release of its buffers when no stream is open, and that the buffer for header lists is the server's, not each connection's.");
    memory_case(false);
    memory_case(true);

    PROVEN_TEST_SECTION("TLS that HTTP/2 may not use", "TLS 1.2 with a CBC suite, and \"h2\" agreed over it.", "Check h2_tls_adequate and its use in proven_http_event_h2_take_: RFC 9113, section 9.2.");
    {
        /* The suite is one the library keeps out unless both configurations name it; the
         * client is made to offer no other. */
        proven_tls_options_t so_old = so, co_old = co_12;
        so_old.legacy = PROVEN_TLS_LEGACY_CBC; co_old.legacy = PROVEN_TLS_LEGACY_CBC;
        proven_tls_config_t *server_old = NULL, *client_old = NULL;
        PROVEN_TEST_ASSERT(proven_tls_config_create(&so_old, &server_old) == PROVEN_OK && proven_tls_config_create(&co_old, &client_old) == PROVEN_OK, "configurations that allow CBC suites", "");
        proven_http_event_server_config_t cfg = { .tls = server_old };
        proven_u16 port = server_start(cfg);
        cl_t c;
        proven_tls_test_only_suite12(0xc023);
        bool up = cl_open_with(&c, client_old, port, 0);
        proven_tls_test_only_suite12(0);
        PROVEN_TEST_ASSERT(up, "a client that offers TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256 alone, and \"h2\"", "");
        int before = atomic_load(&g_requests);
        proven_u32 id = cl_ask(&c, "GET", "/hello", true);
        for (int i = 0; i < 300 && !c.closed && !c.gone; i++) (void)cl_pump(&c, 10);
        PROVEN_TEST_ASSERT(c.goaway && c.closed && c.close_code == PROVEN_H2_INADEQUATE_SECURITY, "the server ends the connection with INADEQUATE_SECURITY", "");
        PROVEN_TEST_ASSERT(id != 0 && !cl_res(&c, id)->end && atomic_load(&g_requests) == before, "and the request sent on it reached no handler", "");
        cl_close(&c);
        /* The same client with a suite HTTP/2 may use is served. */
        PROVEN_TEST_ASSERT(cl_open_with(&c, client_old, port, 0), "the same configurations, the client offering all it has", "");
        id = cl_ask(&c, "GET", "/hello", true);
        PROVEN_TEST_ASSERT(cl_wait(&c, id, 5000) && cl_res(&c, id)->end && cl_res(&c, id)->status == 200, "TLS 1.2 with an AEAD suite and forward secrecy: served", "");
        cl_close(&c);
        server_stop();
        proven_tls_config_destroy(server_old); proven_tls_config_destroy(client_old);
    }

    PROVEN_TEST_SECTION("turns", "Two responses streamed at once to a client whose windows hold them both: what bounds them is this server's own output bound.", "Check `turn` and `sharing` in d_pump and d_room in src/proven/http_event_h2.c: the round goes on behind the stream that had the last turn, and a turn moves H2_TURN_BYTES.");
    {
        proven_http_event_server_config_t cfg = { 0 };
        proven_u16 port = server_start(cfg);
        cl_t c;
        PROVEN_TEST_ASSERT(cl_open_with(&c, NULL, port, 1u << 30), "a client whose windows are a gigabyte", "");
        c.plain_body = true;
        /* The client asks for both and reads nothing for a while: what the system will hold
         * fills, and from then on the server can send only as the client reads. */
        const proven_u32 small = cl_ask(&c, "GET", "/big/16000000", true);
        const proven_u32 large = cl_ask(&c, "GET", "/big/48000000", true);
        proven_mem_view_t out = proven_h2_conn_pending_output(c.e);
        PROVEN_TEST_ASSERT(proven_transport_write_all(c.t, out, proven_net_deadline_in(5000)).err == PROVEN_OK, "two requests are sent", "");
        proven_h2_conn_output_sent(c.e, out.size);
        proven_time_sleep(300);
        for (int i = 0; i < 40000 && !cl_res(&c, small)->end && !c.closed && !c.gone; i++) (void)cl_pump(&c, 5);
        const proven_size_t large_then = cl_res(&c, large)->got;
        PROVEN_TEST_ASSERT(cl_res(&c, small)->end && cl_res(&c, small)->got == 16000000, "the response of sixteen megabytes, asked for first, arrives whole", "");
        PROVEN_TEST_ASSERT(large_then < 40000000, "while the one of forty-eight, asked for with it, is under way: the newer stream did not take every turn", "");
        PROVEN_TEST_ASSERT(cl_wait(&c, large, 30000) && cl_res(&c, large)->end && cl_res(&c, large)->got == 48000000, "which then arrives whole too", "");
        cl_close(&c);
        server_stop();
    }

    PROVEN_TEST_SECTION("http2_window_bytes", "How much of a request body a client may send before the handler has taken any.", "Check initial_window in proven_http_event_h2_take_ and the limit in proven_http_event_server_create.");
    {
        proven_http_event_server_config_t huge = { .on = { .on_request = on_request }, .http2 = true, .http2_window_bytes = (proven_size_t)0x80000000u };
        proven_http_event_server_t *none = NULL;
        proven_loop_t *spare = NULL;
        PROVEN_TEST_ASSERT(proven_loop_create(g_heap, &spare) == PROVEN_OK, "a loop to make servers on", "");
        PROVEN_TEST_ASSERT(proven_http_event_server_create(spare, &huge, &none) == PROVEN_ERR_INVALID_ARG && none == NULL, "a window above 2^31-1 is refused", "");
        proven_loop_destroy(spare);
        /* The handler pauses and takes nothing; the client sends until its engine takes no
         * more. What it could send is the window. */
        static const proven_size_t windows[2] = { 0, 300000 };
        for (int w = 0; w < 2; ++w) {
            proven_http_event_server_config_t cfg = { .http2_window_bytes = windows[w] };
            proven_u16 port = server_start(cfg);
            cl_t c;
            PROVEN_TEST_ASSERT(cl_open_with(&c, NULL, port, 0), "a client connects", "");
            proven_u32 id = cl_ask(&c, "POST", "/pause-long", false);
            static proven_byte_t zeros[1 << 16];
            proven_size_t sent = 0;
            for (int round = 0, still = 0; round < 2000 && still < 20; ++round) {
                proven_result_size_t r = proven_h2_conn_send_data(c.e, id, (proven_mem_view_t){ zeros, sizeof zeros }, false);
                if (r.err != PROVEN_OK) break;
                sent += r.value;
                still = r.value == 0 ? still + 1 : 0;
                (void)cl_pump(&c, 5);
            }
            PROVEN_TEST_ASSERT(sent == (windows[w] ? windows[w] : 65535), windows[w] ? "with http2_window_bytes 300,000 a paused request is sent 300,000 bytes and then no more" : "by default a paused request is sent 65,535 bytes and then no more", "");
            (void)proven_h2_conn_reset(c.e, id, PROVEN_H2_CANCEL);
            (void)cl_pump(&c, 20);
            cl_close(&c);
            server_stop();
        }
    }

    PROVEN_TEST_SECTION("the end of the server", "Destroying a server with exchanges open.", "Check h2_on_closed: every open stream's handler is told once.");
    {
        proven_http_event_server_config_t cfg = { 0 };
        proven_u16 port = server_start(cfg);
        cl_t c;
        PROVEN_TEST_ASSERT(cl_open_with(&c, NULL, port, 0), "a client", "");
        for (int i = 0; i < 5; i++) (void)cl_ask(&c, "GET", "/never", true);
        for (int i = 0; i < 300 && atomic_load(&g_requests) < 5; i++) (void)cl_pump(&c, 10);
        PROVEN_TEST_ASSERT(atomic_load(&g_requests) == 5, "five requests are with their handlers", "");
        server_stop();
        PROVEN_TEST_ASSERT(atomic_load(&g_done_reset) == 5 && done_total() == 5, "destroying the server tells each of the five, once, that its exchange was cut off", "");
        cl_close(&c);
    }

    proven_tls_config_destroy(g_client_tls); proven_tls_config_destroy(g_client_tls_h1); proven_tls_config_destroy(g_client_tls_12);
    proven_tls_config_destroy(g_server_tls); proven_tls_config_destroy(server_no_alpn); proven_cert_store_destroy(anchors);
    proven_job_system_close(g_threads); proven_job_system_destroy(g_threads);
    PROVEN_TEST_PASS("the handlers of an HTTP/1.1 server answer HTTP/2 clients unchanged, in the clear and over TLS, beside HTTP/1.1 clients on the same port.");
    return 0;
}
