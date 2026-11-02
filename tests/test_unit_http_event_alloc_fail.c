#include "proven.h"
#include "proven_test.h"
#include "test_unit_tls_pki.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Every allocation of an exchange fails in turn, on each side and over each protocol, and the
 * calls that hand out a body keep their word: once `on_done` has run - inside the call, when
 * the failure ends the stream there - the call says PROVEN_ERR_RESET, never PROVEN_OK. A
 * write that said OK about a request it had just freed sent its caller into freed memory (the
 * allocation-failure sweep of 2026-11-02 found it in five drivers). The client (and its TLS
 * configuration) is given an allocator that fails its N-th allocation, N = 1, 2, ... until an
 * exchange runs without reaching it; then the server is. The calls are made from outside the loop's
 * callbacks - from the program, between rounds - as a program answering later makes them: inside
 * a callback the driver sends when the callback returns, and outside it sends in the call. Then a
 * peer that goes before the server answers (the HTTP/2 server learns it in the call's send), and
 * the loop's own allocations failing.
 */

#ifndef PROVEN_NO_HTTP3

#define BODY 40000u
#define PIECE 1000u                    /* under the output limit: what a write takes goes out in its last pump */

static proven_u8str_view_t sv(const char *s) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }

static proven_allocator_t g_heap;
static proven_loop_t *g_loop;
static proven_cert_store_t *g_anchors;
static proven_byte_t g_body[BODY];

/* An allocator that fails its N-th allocation (or growing reallocation), and only that one. */
typedef struct {
    proven_allocator_t inner;
    unsigned long count, fail_at;
    bool from, failed;                 /* from: every allocation from the N-th on fails */
} fail_t;

static bool f_fails(fail_t *f) {
    ++f->count;
    if (f->count == f->fail_at || (f->from && f->count > f->fail_at)) { f->failed = true; return true; }
    return false;
}
static proven_result_mem_mut_t f_alloc(void *ctx, proven_size_t size, proven_size_t align) {
    fail_t *f = ctx;
    if (f_fails(f)) return (proven_result_mem_mut_t){ .err = PROVEN_ERR_NOMEM };
    return f->inner.alloc_fn(f->inner.ctx, size, align);
}
static proven_result_mem_mut_t f_realloc(void *ctx, void *p, proven_size_t old, proven_size_t n, proven_size_t align) {
    fail_t *f = ctx;
    if (n > old && f_fails(f)) return (proven_result_mem_mut_t){ .err = PROVEN_ERR_NOMEM };
    return f->inner.realloc_fn(f->inner.ctx, p, old, n, align);
}
static void f_free(void *ctx, void *p) { fail_t *f = ctx; f->inner.free_fn(f->inner.ctx, p); }
static proven_allocator_t failing(fail_t *f) { return (proven_allocator_t){ .ctx = f, .alloc_fn = f_alloc, .realloc_fn = f_realloc, .free_fn = f_free }; }

static void up_drive(void *u);
static void down_drive(void);
/* Rounds of the loop until `flag`, and between them what the program has been told to do. */
static void spin_until(const bool *flag, proven_u32 ms, void *up) {
    proven_net_deadline_t until = proven_net_deadline_in(ms);
    while (!*flag && proven_time_monotonic_now() < until) {
        (void)proven_loop_poll(g_loop, proven_net_deadline_in(10));
        if (up) up_drive(up);
        down_drive();
    }
}
static void settle(void) { for (int i = 0; i < 6; ++i) (void)proven_loop_poll(g_loop, proven_net_deadline_in(2)); }

/* What a protocol needs of each side's TLS configuration. */
static const proven_u8str_view_t k_h2[] = { { (const proven_byte_t *)"h2", 2 }, { (const proven_byte_t *)"http/1.1", 8 } };
static const proven_u8str_view_t k_h3[] = { { (const proven_byte_t *)"h3", 2 } };
static const proven_u8str_view_t k_all[] = { { (const proven_byte_t *)"h2", 2 }, { (const proven_byte_t *)"http/1.1", 8 }, { (const proven_byte_t *)"h3", 2 } };

static proven_err_t client_tls(proven_allocator_t a, int proto, proven_tls_config_t **out) {
    proven_tls_options_t o = { .alloc = a, .anchors = g_anchors };
    if (proto == 2) { o.alpn = k_h2; o.alpn_count = 2; }
    if (proto == 3) { o.alpn = k_h3; o.alpn_count = 1; }
    return proven_tls_config_create(&o, out);
}
static proven_err_t server_tls(proven_allocator_t a, proven_tls_config_t **out) {
    proven_tls_options_t o = { .alloc = a, .certificate_pem = { (const proven_byte_t *)TP_SERVER_P256, strlen(TP_SERVER_P256) },
                               .private_key_pem = { (const proven_byte_t *)TP_SERVER_P256_KEY, strlen(TP_SERVER_P256_KEY) }, .alpn = k_all, .alpn_count = 3 };
    return proven_tls_config_create(&o, out);
}
static proven_err_t client_new(proven_allocator_t a, proven_tls_config_t *tls, int proto, proven_http_event_client_t **out) {
    proven_http_event_client_config_t cfg = { .alloc = a, .tls = tls, .http2 = proto == 2, .http3 = proto == 3, .decompress = true,
                                              .connect_timeout_ms = 1500, .response_timeout_ms = 1500, .body_timeout_ms = 1500,
                                              .write_timeout_ms = 1500, .max_buffered_output = 8192 };
    return proven_http_event_client_create(g_loop, &cfg, out);
}

// -----------------------------------------------------------------------------
// The server: a request body is read and thrown away; /down answers BODY bytes in pieces,
// compressed, flushed once - every call that can end the stream on the way.
// -----------------------------------------------------------------------------

typedef struct {
    proven_http_stream_t *s;
    proven_size_t sent;
    bool done, ended, flushed, begun, due;   /* due: the program is to call begin, or write again */
    int bad;                           /* a call that said OK after on_done, or RESET before it */
} down_t;
static down_t g_down;
static proven_net_conn_t *g_close_before_flush;   /* a peer to close between the first write and the flush */

static void down_pump(void) {
    down_t *d = &g_down;
    while (!d->ended) {
        if (d->sent == BODY) { d->ended = true; (void)proven_http_stream_end(d->s); return; }
        proven_result_size_t w = proven_http_stream_write(d->s, (proven_mem_view_t){ g_body + d->sent, BODY - d->sent < PIECE ? BODY - d->sent : PIECE });
        if (w.err == PROVEN_ERR_RESET) { if (!d->done) d->bad++; return; }
        if (w.err != PROVEN_OK) return;
        if (d->done) { d->bad++; return; }
        d->sent += w.value;
        if (!d->flushed) {
            d->flushed = true;
            if (g_close_before_flush) { (void)proven_net_close(g_close_before_flush); g_close_before_flush = NULL; }
            const proven_err_t e = proven_http_stream_flush(d->s);
            if (e == PROVEN_ERR_RESET) { if (!d->done) d->bad++; return; }
            if (d->done) { d->bad++; return; }
        }
        if (w.value == 0) return;                  /* on_writable says when */
    }
}

static void srv_request(void *ctx, proven_http_stream_t *s, const proven_http_request_t *req) {
    (void)ctx;
    if (req->target.size != 5 || memcmp(req->target.ptr, "/down", 5) != 0) {
        (void)proven_http_stream_respond(s, 200, NULL, 0, (proven_mem_view_t){ (const proven_byte_t *)"ok", 2 });
        return;
    }
    g_down = (down_t){ .s = s, .due = true };      /* answered later, by the program */
    proven_http_stream_set_user(s, &g_down);
    proven_http_stream_compress(s);
}
static void srv_writable(void *ctx, proven_http_stream_t *s) {
    (void)ctx;
    if (proven_http_stream_user(s) == &g_down) g_down.due = true;
}
/* What the program does between the loop's rounds. */
static void down_drive(void) {
    down_t *d = &g_down;
    if (!d->due || d->done || !d->s) return;
    d->due = false;
    if (!d->begun) {
        d->begun = true;
        const proven_err_t e = proven_http_stream_begin(d->s, 200, NULL, 0, PROVEN_HTTP_EVENT_LENGTH_UNKNOWN);
        if (e == PROVEN_ERR_RESET) { if (!d->done) d->bad++; return; }
        if (e != PROVEN_OK) return;
        if (d->done) { d->bad++; return; }
    }
    down_pump();
}
static void srv_done(void *ctx, proven_http_stream_t *s, proven_err_t why) {
    (void)ctx; (void)why;
    if (proven_http_stream_user(s) == &g_down) g_down.done = true;
}

static proven_err_t server_new(proven_allocator_t a, proven_tls_config_t *tls, proven_http_event_server_t **out, proven_u16 *port) {
    proven_http_event_server_config_t cfg = { .alloc = a, .on = { srv_request, NULL, srv_writable, srv_done }, .tls = tls, .http2 = true, .http3 = true,
                                              .head_timeout_ms = 1500, .body_timeout_ms = 1500, .write_timeout_ms = 1500, .idle_timeout_ms = 1500,
                                              .max_buffered_output = 8192, .compress_level = 1 };
    proven_err_t e = proven_http_event_server_create(g_loop, &cfg, out);
    proven_net_addr_t at;
    if (e == PROVEN_OK) e = proven_http_event_server_listen(*out, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at);
    if (e == PROVEN_OK) *port = at.port;
    return e;
}

/* The same server in the clear, over TCP only: HTTP/2 when the client begins with its preface. */
static proven_err_t server_new_clear(proven_http_event_server_t **out, proven_u16 *port) {
    proven_http_event_server_config_t cfg = { .alloc = g_heap, .on = { srv_request, NULL, srv_writable, srv_done }, .http2 = true,
                                              .head_timeout_ms = 1500, .body_timeout_ms = 1500, .write_timeout_ms = 1500, .idle_timeout_ms = 1500,
                                              .max_buffered_output = 8192, .compress_level = 1 };
    proven_err_t e = proven_http_event_server_create(g_loop, &cfg, out);
    proven_net_addr_t at;
    if (e == PROVEN_OK) e = proven_http_event_server_listen(*out, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at);
    if (e == PROVEN_OK) *port = at.port;
    return e;
}

// -----------------------------------------------------------------------------
// The client: a PUT of BODY bytes with no length, written as the client takes it.
// -----------------------------------------------------------------------------

typedef struct up {
    proven_http_event_request_t *r;
    proven_size_t sent;
    bool done, ended, due;
    int bad;
} up_t;

static void up_pump(up_t *u) {
    while (!u->ended) {
        if (u->sent == BODY) { u->ended = true; (void)proven_http_event_request_end(u->r); return; }
        proven_result_size_t w = proven_http_event_request_write(u->r, (proven_mem_view_t){ g_body + u->sent, BODY - u->sent < PIECE ? BODY - u->sent : PIECE });
        if (w.err == PROVEN_ERR_RESET) { if (!u->done) u->bad++; return; }
        if (w.err != PROVEN_OK) return;
        if (u->done) { u->bad++; return; }
        u->sent += w.value;
        if (w.value == 0) return;
    }
}
static void up_writable(void *ctx, proven_http_event_request_t *r) { (void)r; ((up_t *)ctx)->due = true; }
static void up_drive(void *p) { up_t *u = p; if (u->due && !u->done) { u->due = false; up_pump(u); } }
static void up_done(void *ctx, proven_http_event_request_t *r, proven_err_t why) { (void)r; (void)why; ((up_t *)ctx)->done = true; }
static void dn_done(void *ctx, proven_http_event_request_t *r, proven_err_t why) { (void)r; (void)why; *(bool *)ctx = true; }

static char g_url[96];
static proven_u8str_view_t url_to(proven_u16 port, const char *path) {
    const int n = snprintf(g_url, sizeof g_url, "https://127.0.0.1:%u%s", (unsigned)port, path);
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)g_url, .size = (proven_size_t)n };
}

/* The client's allocations fail in turn. Returns how many there were in a whole exchange. */
static unsigned long sweep_client(proven_u16 port, int proto, const char *how) {
    unsigned long n = 1;
    for (;; ++n) {
        fail_t f = { .inner = g_heap, .fail_at = n };
        const proven_allocator_t a = failing(&f);
        proven_tls_config_t *tls = NULL;
        proven_http_event_client_t *cl = NULL;
        up_t u = { 0 };
        if (client_tls(a, proto, &tls) == PROVEN_OK && client_new(a, tls, proto, &cl) == PROVEN_OK) {
            proven_http_event_request_options_t o = { .method = sv("PUT"), .url = url_to(port, "/up"), .body_length = PROVEN_HTTP_EVENT_LENGTH_UNKNOWN,
                                                      .http3 = proto == 3, .on = { NULL, NULL, up_writable, up_done }, .ctx = &u };
            if (proven_http_event_client_start(cl, &o, &u.r) == PROVEN_OK) {
                up_pump(&u);
                spin_until(&u.done, 10000, &u);
                PROVEN_TEST_ASSERT(u.done, "the request ends, whatever failed", how);
            }
        }
        proven_http_event_client_destroy(cl);
        settle();
        proven_tls_config_destroy(tls);
        if (u.bad) PROVEN_TEST_INFO("{}: the client's allocation {} failing", PROVEN_ARG(how), PROVEN_ARG(n));
        PROVEN_TEST_ASSERT(u.bad == 0, "no client call says PROVEN_OK about a request on_done has ended, nor PROVEN_ERR_RESET before it", how);
        if (!f.failed) break;                      /* the whole exchange ran without reaching n */
        PROVEN_TEST_ASSERT(n < 100000, "the sweep ends", how);
    }
    return n - 1;
}

/* The server's allocations fail in turn, against a client that has the heap. */
static unsigned long sweep_server(proven_tls_config_t *client_tls_cfg, int proto, const char *how) {
    unsigned long n = 1;
    for (;; ++n) {
        fail_t f = { .inner = g_heap, .fail_at = n };
        const proven_allocator_t a = failing(&f);
        proven_tls_config_t *tls = NULL;
        proven_http_event_server_t *server = NULL;
        proven_http_event_client_t *cl = NULL;
        proven_u16 port = 0;
        g_down = (down_t){ 0 };
        if (server_tls(a, &tls) == PROVEN_OK && server_new(a, tls, &server, &port) == PROVEN_OK) {
            PROVEN_TEST_ASSERT(client_new(g_heap, client_tls_cfg, proto, &cl) == PROVEN_OK, "a client", how);
            bool done = false;
            proven_http_event_request_t *r = NULL;
            proven_http_event_request_options_t o = { .method = sv("GET"), .url = url_to(port, "/down"), .http3 = proto == 3,
                                                      .on = { NULL, NULL, NULL, dn_done }, .ctx = &done };
            PROVEN_TEST_ASSERT(proven_http_event_client_start(cl, &o, &r) == PROVEN_OK, "a GET", how);
            spin_until(&done, 10000, NULL);
            PROVEN_TEST_ASSERT(done, "the request ends, whatever failed", how);
        }
        proven_http_event_client_destroy(cl);
        proven_http_event_server_destroy(server);
        settle();
        proven_tls_config_destroy(tls);
        if (g_down.bad) PROVEN_TEST_INFO("{}: the server's allocation {} failing", PROVEN_ARG(how), PROVEN_ARG(n));
        PROVEN_TEST_ASSERT(g_down.bad == 0, "no server call says PROVEN_OK about a stream on_done has ended, nor PROVEN_ERR_RESET before it", how);
        if (!f.failed) break;
        PROVEN_TEST_ASSERT(n < 100000, "the sweep ends", how);
    }
    return n - 1;
}

/* The loop's allocations fail - its N-th only, or every one from the N-th on - while both sides
 * have the heap: the PUT and then the GET on one loop. The loop allocates the bytes waiting to be
 * sent, so a write or flush whose send it refuses ends the stream in the call. */
static unsigned long sweep_loop(proven_tls_config_t *stls, proven_tls_config_t *ctls, int proto, bool from, const char *how) {
    proven_loop_t *const main_loop = g_loop;
    unsigned long n = 1;
    for (;; ++n) {
        fail_t f = { .inner = g_heap, .fail_at = n, .from = from };
        proven_http_event_server_t *server = NULL;
        proven_http_event_client_t *cl = NULL;
        proven_u16 port = 0;
        up_t u = { 0 };
        g_down = (down_t){ 0 };
        g_loop = NULL;
        if (proven_loop_create(failing(&f), &g_loop) == PROVEN_OK) {
            if (server_new(g_heap, stls, &server, &port) == PROVEN_OK && client_new(g_heap, ctls, proto, &cl) == PROVEN_OK) {
                proven_http_event_request_options_t o = { .method = sv("PUT"), .url = url_to(port, "/up"), .body_length = PROVEN_HTTP_EVENT_LENGTH_UNKNOWN,
                                                          .http3 = proto == 3, .on = { NULL, NULL, up_writable, up_done }, .ctx = &u };
                if (proven_http_event_client_start(cl, &o, &u.r) == PROVEN_OK) {
                    up_pump(&u);
                    spin_until(&u.done, 10000, &u);
                    PROVEN_TEST_ASSERT(u.done, "the PUT ends, whatever failed", how);
                }
                bool done = false;
                proven_http_event_request_t *r = NULL;
                proven_http_event_request_options_t g = { .method = sv("GET"), .url = url_to(port, "/down"), .http3 = proto == 3,
                                                          .on = { NULL, NULL, NULL, dn_done }, .ctx = &done };
                if (proven_http_event_client_start(cl, &g, &r) == PROVEN_OK) {
                    spin_until(&done, 10000, NULL);
                    PROVEN_TEST_ASSERT(done, "the GET ends, whatever failed", how);
                }
            }
            proven_http_event_client_destroy(cl);
            proven_http_event_server_destroy(server);
            settle();
            proven_loop_destroy(g_loop);
        }
        g_loop = main_loop;
        if (u.bad || g_down.bad) PROVEN_TEST_INFO("{}: the loop's allocation {} failing{}", PROVEN_ARG(how), PROVEN_ARG(n), PROVEN_ARG(from ? ", and every one after it" : ""));
        PROVEN_TEST_ASSERT(u.bad == 0 && g_down.bad == 0, "no call says PROVEN_OK about a stream on_done has ended, nor PROVEN_ERR_RESET before it", how);
        if (!f.failed) break;
        PROVEN_TEST_ASSERT(n < 100000, "the sweep ends", how);
    }
    return n - 1;
}

/* A peer that goes: the request comes in the clear from a socket of the test's, which closes
 * before the program answers - with the server's bytes unread (a reset at once: the first send
 * is refused), or after reading them (the first send goes, and the reset it meets refuses the
 * next) - or, the response compressed, between its first write and its flush, unread. No round
 * of the loop's comes between the close and the program's calls, so the server learns of it
 * inside the call that sends: for HTTP/2, in the pump after which begin, write and flush once
 * took their answer. */
enum { GOES_UNREAD, GOES_READ, GOES_BEFORE_FLUSH };
static void peer_goes(proven_u16 port, int proto, int when, bool gzip, const char *how) {
    const bool drain = when == GOES_READ;
    proven_byte_t req[160];
    proven_size_t n = 0;
    if (proto == 1) {
        n = (proven_size_t)snprintf((char *)req, sizeof req, "GET /down HTTP/1.1\r\nHost: t\r\n%s\r\n", gzip ? "Accept-Encoding: gzip\r\n" : "");
    } else {
        static const proven_byte_t settings[9] = { 0, 0, 0, 4, 0, 0, 0, 0, 0 };
        /* :method GET, :scheme http, :authority t, :path /down, and accept-encoding: gzip, deflate */
        static const proven_byte_t block[] = { 0x82, 0x86, 0x41, 1, 't', 0x44, 5, '/', 'd', 'o', 'w', 'n', 0x90 };
        const proven_size_t blen = sizeof block - (gzip ? 0 : 1);
        memcpy(req, PROVEN_H2_PREFACE, PROVEN_H2_PREFACE_SIZE); n = PROVEN_H2_PREFACE_SIZE;
        memcpy(req + n, settings, sizeof settings); n += sizeof settings;
        const proven_byte_t head[9] = { 0, 0, (proven_byte_t)blen, 1, 0x05, 0, 0, 0, 1 };     /* HEADERS, END_STREAM | END_HEADERS, stream 1 */
        memcpy(req + n, head, sizeof head); n += sizeof head;
        memcpy(req + n, block, blen); n += blen;
    }
    g_down = (down_t){ 0 };
    proven_net_conn_t peer;
    PROVEN_TEST_ASSERT(proven_net_connect(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, port), proven_net_deadline_in(2000), &peer) == PROVEN_OK, "the peer connects", how);
    PROVEN_TEST_ASSERT(proven_net_write_all(&peer, (proven_mem_view_t){ req, n }, proven_net_deadline_in(2000)).err == PROVEN_OK, "the peer asks", how);
    const proven_net_deadline_t until = proven_net_deadline_in(5000);
    while (!g_down.s && proven_time_monotonic_now() < until) (void)proven_loop_poll(g_loop, proven_net_deadline_in(10));
    PROVEN_TEST_ASSERT(g_down.s != NULL, "the request comes in", how);
    if (drain) {
        settle();
        proven_byte_t sink[512];
        for (;;) { const proven_result_size_t r = proven_net_read(&peer, (proven_mem_mut_t){ sink, sizeof sink }, PROVEN_NET_DONT_WAIT); if (r.err != PROVEN_OK || r.value == 0) break; }
    }
    if (when == GOES_BEFORE_FLUSH) g_close_before_flush = &peer;
    else (void)proven_net_close(&peer);
    down_drive();                                  /* before the loop's next round */
    PROVEN_TEST_ASSERT(g_close_before_flush == NULL, "the peer was closed", how);
    spin_until(&g_down.done, 5000, NULL);
    PROVEN_TEST_ASSERT(g_down.done, "the stream ends", how);
    if (g_down.bad) PROVEN_TEST_INFO("{}: the peer gone, {}, {}", PROVEN_ARG(how), PROVEN_ARG(when == GOES_READ ? "its bytes read" : when == GOES_UNREAD ? "its bytes unread" : "before the flush"), PROVEN_ARG(gzip ? "gzip" : "identity"));
    PROVEN_TEST_ASSERT(g_down.bad == 0, "no server call says PROVEN_OK about a stream on_done has ended, nor PROVEN_ERR_RESET before it", how);
    settle();
}

int main(void) {
    PROVEN_TEST_SUITE("the event-driven drivers when an allocation fails",
        "Each allocation of a whole exchange fails in turn - the client's, then the server's - over HTTP/1.1, HTTP/2 and HTTP/3; the calls that hand out a body never say PROVEN_OK once on_done has run.",
        "Inspect where proven_http_stream_write, _flush, _begin and proven_http_event_request_write take their answer (http_event.c, http_event_h2.c, http_event_h3.c and the client's three files): after the last pump or watch, which can end the stream.");
    g_heap = proven_heap_allocator();
    for (proven_size_t i = 0; i < BODY; ++i) g_body[i] = (proven_byte_t)('a' + (i * 7 + i / 89) % 26);
    PROVEN_TEST_ASSERT(proven_loop_create(g_heap, &g_loop) == PROVEN_OK, "a loop", "");
    PROVEN_TEST_ASSERT(tp_build(), "the test's certificates and keys are issued", "");
    PROVEN_TEST_ASSERT(proven_cert_store_create(g_heap, &g_anchors) == PROVEN_OK &&
                       proven_cert_store_add_pem(g_anchors, (proven_mem_view_t){ (const proven_byte_t *)TP_CA, strlen(TP_CA) }, NULL) == PROVEN_OK, "anchors", "");

    /* A server on the heap for the client's sweeps. */
    proven_tls_config_t *stls = NULL;
    proven_http_event_server_t *server = NULL;
    proven_u16 port = 0;
    PROVEN_TEST_ASSERT(server_tls(g_heap, &stls) == PROVEN_OK, "the server's TLS", "");
    const proven_err_t le = server_new(g_heap, stls, &server, &port);
    if (le == PROVEN_ERR_PERMISSION || le == PROVEN_ERR_UNSUPPORTED) {
        proven_http_event_server_destroy(server);
        PROVEN_TEST_PASS("skipped: no sockets here");
        return 0;
    }
    PROVEN_TEST_ASSERT(le == PROVEN_OK, "a server on TCP and UDP", "");

    static const char *const names[] = { "", "HTTP/1.1", "HTTP/2", "HTTP/3" };
    PROVEN_TEST_SECTION("the client's allocations", "A PUT written in pieces as the client takes it, its client's N-th allocation failing, for every N.", "Check proven_http_event_request_write in http_event_client.c, _h2.c and _h3.c.");
    for (int proto = 1; proto <= 3; ++proto) {
        const unsigned long k = sweep_client(port, proto, names[proto]);
        PROVEN_TEST_INFO("{}: {} client allocations, each failed once", PROVEN_ARG(names[proto]), PROVEN_ARG(k));
        PROVEN_TEST_ASSERT(k > 10, "an exchange allocates, so the sweep failed something", names[proto]);
    }
    proven_http_event_server_destroy(server);
    settle();
    proven_tls_config_destroy(stls);

    PROVEN_TEST_SECTION("the server's allocations", "A compressed response written in pieces and flushed once, its server's N-th allocation failing, for every N.", "Check proven_http_stream_begin, _write and _flush in http_event.c, http_event_h2.c and http_event_h3.c.");
    for (int proto = 1; proto <= 3; ++proto) {
        proven_tls_config_t *ctls = NULL;
        PROVEN_TEST_ASSERT(client_tls(g_heap, proto, &ctls) == PROVEN_OK, "the client's TLS", names[proto]);
        const unsigned long k = sweep_server(ctls, proto, names[proto]);
        PROVEN_TEST_INFO("{}: {} server allocations, each failed once", PROVEN_ARG(names[proto]), PROVEN_ARG(k));
        PROVEN_TEST_ASSERT(k > 10, "an exchange allocates, so the sweep failed something", names[proto]);
        proven_tls_config_destroy(ctls);
    }

    PROVEN_TEST_SECTION("a peer that goes", "A request in the clear whose peer closes before the program answers - its bytes unread or read, the response compressed or not.", "Check where proven_http_stream_begin, _write and _flush take their answer in http_event_h2.c: after d_pump, whose send the reset refuses.");
    {
        proven_http_event_server_t *clear = NULL;
        proven_u16 cport = 0;
        PROVEN_TEST_ASSERT(server_new_clear(&clear, &cport) == PROVEN_OK, "a server in the clear", "");
        for (int proto = 1; proto <= 2; ++proto)
            for (int when = GOES_UNREAD; when <= GOES_BEFORE_FLUSH; ++when)
                for (int gz = when == GOES_BEFORE_FLUSH; gz <= 1; ++gz) peer_goes(cport, proto, when, gz, names[proto]);
        proven_http_event_server_destroy(clear);
        settle();
    }

    PROVEN_TEST_SECTION("the loop's allocations", "The PUT and the GET on a loop whose N-th allocation fails, or every one from the N-th on, for every N; both sides have the heap.", "Check where the drivers' write, flush and begin take their answer against ev_watch, cl_watch and the sends: a send the loop refuses ends the stream.");
    PROVEN_TEST_ASSERT(server_tls(g_heap, &stls) == PROVEN_OK, "the server's TLS", "");
    for (int proto = 1; proto <= 3; ++proto) {
        proven_tls_config_t *ctls = NULL;
        PROVEN_TEST_ASSERT(client_tls(g_heap, proto, &ctls) == PROVEN_OK, "the client's TLS", names[proto]);
        for (int from = 0; from <= 1; ++from) {
            const unsigned long k = sweep_loop(stls, ctls, proto, from, names[proto]);
            PROVEN_TEST_INFO("{}: {} loop allocations{}", PROVEN_ARG(names[proto]), PROVEN_ARG(k), PROVEN_ARG(from ? ", failing from each on" : ", each failed once"));
            PROVEN_TEST_ASSERT(k >= 2, "the loop allocates, so the sweep failed something", names[proto]);
        }
        proven_tls_config_destroy(ctls);
    }
    proven_tls_config_destroy(stls);

    settle();
    proven_loop_destroy(g_loop);
    proven_cert_store_destroy(g_anchors);
    PROVEN_TEST_PASS("the event-driven drivers keep their word when an allocation fails");
    return 0;
}

#else

int main(void) {
    PROVEN_TEST_SUITE("the event-driven drivers when an allocation fails", "Built with PROVEN_NO_HTTP3: nothing to test.", "");
    PROVEN_TEST_PASS("skipped: PROVEN_NO_HTTP3");
    return 0;
}

#endif
