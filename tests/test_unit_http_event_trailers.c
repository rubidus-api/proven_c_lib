#include "proven.h"
#include "proven_test.h"
#include "test_unit_tls_pki.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Trailers (RFC 9110 section 6.5) through the event-driven server and client, this library at
 * both ends on one loop, over HTTP/1.1, HTTP/2 and HTTP/3: a request's trailers read by the
 * server after its body, a response's read by the client after its own; what may not be sent
 * refused; trailers where there is nowhere to put them refused.
 */

#ifndef PROVEN_NO_HTTP3

static proven_u8str_view_t sv(const char *s) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }
static proven_mem_view_t mv(const char *s) { return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }

static proven_allocator_t g_heap;
static proven_loop_t *g_loop;
static proven_tls_config_t *g_server_tls, *g_client_tls, *g_client_tls_h2, *g_client_tls_h1;

static void spin_until(const bool *flag, proven_u32 ms) {
    proven_net_deadline_t until = proven_net_deadline_in(ms);
    while (!*flag && proven_time_monotonic_now() < until)
        PROVEN_TEST_ASSERT(proven_loop_poll(g_loop, proven_net_deadline_in(10)) == PROVEN_OK, "a round", "");
}

static unsigned sum_of(const proven_byte_t *p, proven_size_t n, unsigned s) { for (proven_size_t i = 0; i < n; ++i) s = s * 31 + p[i]; return s; }

static bool field_is(const proven_http_header_t *f, proven_size_t n, const char *name, const char *value) {
    proven_u8str_view_t v = { 0 };
    return proven_http_header_find(f, n, sv(name), &v) && proven_u8str_view_eq(v, sv(value));
}

// -----------------------------------------------------------------------------
// The server
// -----------------------------------------------------------------------------

typedef struct {
    char path[32];
    unsigned sum;
    proven_size_t got;
} served_t;

static int g_srv_refused, g_srv_misplaced, g_srv_trailers, g_srv_matched, g_srv_none, g_srv_early;
static int g_srv_done_saw;             /* the request's trailers could still be read in on_done */

static void srv_request(void *ctx, proven_http_stream_t *s, const proven_http_request_t *req) {
    (void)ctx;
    served_t *a = calloc(1, sizeof *a);
    if (!a) { proven_http_stream_abort(s); return; }
    memcpy(a->path, req->target.ptr, req->target.size < sizeof a->path - 1 ? req->target.size : sizeof a->path - 1);
    proven_http_stream_set_user(s, a);
    proven_http_header_t f[4];
    proven_size_t n = 0;
    if (proven_http_stream_trailers(s, f, 4, &n) == PROVEN_ERR_INVALID_STATE) g_srv_early++;     /* not before the body's end */
}

static void srv_body(void *ctx, proven_http_stream_t *s, proven_mem_view_t piece, bool last) {
    (void)ctx;
    served_t *a = proven_http_stream_user(s);
    a->sum = sum_of(piece.ptr, piece.size, a->sum);
    a->got += piece.size;
    if (!last) return;
    if (strcmp(a->path, "/len") == 0) {
        /* A response with a length has nowhere to put trailers. */
        const proven_http_header_t t = { sv("X-Status"), sv("x") };
        if (proven_http_stream_begin(s, 200, NULL, 0, 2) == PROVEN_OK) {
            (void)proven_http_stream_write(s, mv("ok"));
            if (proven_http_stream_end_trailers(s, &t, 1) == PROVEN_ERR_INVALID_STATE) g_srv_misplaced++;
            (void)proven_http_stream_end(s);
        }
        return;
    }
    proven_http_header_t f[4];
    proven_size_t n = 9;
    char mine[16];
    snprintf(mine, sizeof mine, "%u", a->sum);
    const proven_err_t e = proven_http_stream_trailers(s, f, 4, &n);
    if (e == PROVEN_OK && n > 0) g_srv_trailers++;
    if (e == PROVEN_OK && n == 0) g_srv_none++;
    const bool matched = e == PROVEN_OK && field_is(f, n, "x-sum", mine);
    if (matched) g_srv_matched++;
    /* /zip: compressed, so that the body is still queued when the trailers are asked for. */
    const bool zip = strcmp(a->path, "/zip") == 0;
    if (zip) proven_http_stream_compress(s);
    if (proven_http_stream_begin(s, 200, NULL, 0, PROVEN_HTTP_EVENT_LENGTH_UNKNOWN) != PROVEN_OK) return;
    if (zip) {
        static proven_byte_t text[20000];
        for (proven_size_t i = 0; i < sizeof text; ++i) text[i] = (proven_byte_t)('a' + (i * 7 + i / 97) % 26);
        proven_result_size_t w = proven_http_stream_write(s, (proven_mem_view_t){ text, sizeof text });
        if (w.err != PROVEN_OK || w.value != sizeof text) g_srv_refused -= 100;     /* the test's assumption broken: seen as a failure */
    } else {
        (void)proven_http_stream_write(s, mv("stored"));
    }
    /* A field that may not be a trailer is refused, and leaves the response open. */
    const proven_http_header_t bad = { sv("Content-Type"), sv("text/plain") };
    if (proven_http_stream_end_trailers(s, &bad, 1) == PROVEN_ERR_INVALID_ARG) g_srv_refused++;
    const proven_http_header_t t[2] = { { sv("X-Status"), matched ? sv("verified") : sv("corrupt") }, { sv("Server-Timing"), sv("db;dur=7") } };
    (void)proven_http_stream_end_trailers(s, t, 2);
}

static void srv_done(void *ctx, proven_http_stream_t *s, proven_err_t why) {
    (void)ctx; (void)why;
    served_t *a = proven_http_stream_user(s);
    proven_http_header_t f[4];
    proven_size_t n = 0;
    if (a && strcmp(a->path, "/len") != 0 && proven_http_stream_trailers(s, f, 4, &n) == PROVEN_OK && n > 0) g_srv_done_saw++;
    free(a);
}

// -----------------------------------------------------------------------------
// The client
// -----------------------------------------------------------------------------

typedef struct {
    proven_http_event_request_t *request;
    const char *data;                  /* the body, written in two pieces as the client takes it */
    proven_size_t sent;
    bool trailers_only;                /* ended at once, before it can be on a stream */
    bool ended, done, coded;
    int status, protocol;
    proven_err_t why, before, bad_end, last_read;
    proven_http_header_t got[4];
    proven_size_t got_count, done_count, body_bytes;
    char status_text[16], timing[16];
} call_t;

static void call_end(call_t *c) {
    char sum[16];
    snprintf(sum, sizeof sum, "%u", sum_of((const proven_byte_t *)c->data, strlen(c->data), 0));
    const proven_http_header_t bad = { sv("Content-Length"), sv("3") };
    c->bad_end = proven_http_event_request_end_trailers(c->request, &bad, 1);
    const proven_http_header_t t = { sv("X-Sum"), sv(sum) };
    c->ended = proven_http_event_request_end_trailers(c->request, &t, 1) == PROVEN_OK;
}

static void call_pump(call_t *c) {
    if (c->ended || c->trailers_only || !c->data) return;       /* (no data: the test writes it itself) */
    const proven_size_t n = strlen(c->data);
    while (c->sent < n) {
        const proven_size_t piece = c->sent == 0 ? n / 2 : n - c->sent;
        proven_result_size_t w = proven_http_event_request_write(c->request, (proven_mem_view_t){ .ptr = (const proven_byte_t *)c->data + c->sent, .size = piece });
        if (w.err != PROVEN_OK || w.value == 0) return;          /* on_writable says when */
        c->sent += w.value;
    }
    call_end(c);
}

static void on_response(void *ctx, proven_http_event_request_t *r, const proven_http_response_t *head) {
    call_t *c = ctx;
    c->status = head->status;
    c->protocol = proven_http_event_request_protocol(r);
    proven_u8str_view_t coding = { 0 };
    c->coded = proven_http_header_find(head->headers, head->header_count, sv("Content-Encoding"), &coding);
    proven_http_header_t f[4];
    proven_size_t n = 0;
    c->before = proven_http_event_request_trailers(r, f, 4, &n);
}
static void on_body(void *ctx, proven_http_event_request_t *r, proven_mem_view_t piece, bool last) {
    call_t *c = ctx;
    c->body_bytes += piece.size;
    if (!last) return;
    c->last_read = proven_http_event_request_trailers(r, c->got, 4, &c->got_count);
    proven_u8str_view_t v = { 0 };
    if (proven_http_header_find(c->got, c->got_count, sv("x-status"), &v)) memcpy(c->status_text, v.ptr, v.size < 15 ? v.size : 15);
    if (proven_http_header_find(c->got, c->got_count, sv("server-timing"), &v)) memcpy(c->timing, v.ptr, v.size < 15 ? v.size : 15);
}
static void on_writable(void *ctx, proven_http_event_request_t *r) { (void)r; call_pump(ctx); }
static void on_done(void *ctx, proven_http_event_request_t *r, proven_err_t why) {
    call_t *c = ctx;
    proven_http_header_t f[4];
    if (proven_http_event_request_trailers(r, f, 4, &c->done_count) != PROVEN_OK) c->done_count = 99;
    c->why = why;
    c->done = true;
}

static char g_url[128];
static proven_u8str_view_t url_to(proven_u16 port, const char *path) {
    int n = snprintf(g_url, sizeof g_url, "https://127.0.0.1:%u%s", (unsigned)port, path);
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)g_url, .size = (proven_size_t)n };
}

static void one(proven_http_event_client_t *cl, proven_u16 port, bool h3, call_t *c, const char *path, const char *data, bool trailers_only, const char *how) {
    *c = (call_t){ .data = data, .trailers_only = trailers_only, .why = PROVEN_ERR_INVALID_STATE };
    proven_http_event_request_options_t o = { .method = sv("PUT"), .url = url_to(port, path), .body_length = PROVEN_HTTP_EVENT_LENGTH_UNKNOWN, .http3 = h3,
                                              .on = { on_response, on_body, on_writable, on_done }, .ctx = c };
    PROVEN_TEST_ASSERT(proven_http_event_client_start(cl, &o, &c->request) == PROVEN_OK, "a PUT with a body of no length", how);
    if (trailers_only) call_end(c);
    else call_pump(c);
    spin_until(&c->done, 10000);
}

static void over(proven_http_event_client_t *cl, proven_u16 port, bool h3, int protocol, const char *how) {
    g_srv_refused = g_srv_misplaced = g_srv_trailers = g_srv_matched = g_srv_none = g_srv_done_saw = g_srv_early = 0;
    static call_t c;
    /* First, on a client with no connection yet: ended at once, with nothing written - over HTTP/2
     * and HTTP/3 before the request is on a stream. */
    one(cl, port, h3, &c, "/up", "", true, how);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.ended && c.protocol == protocol && g_srv_trailers == 1 && g_srv_matched == 1 &&
                       strcmp(c.status_text, "verified") == 0, "trailers with no body before them, ended before the request had a connection", how);
    one(cl, port, h3, &c, "/up", "the bytes of a file, sent as they are read", false, how);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.status == 200 && c.protocol == protocol, "answered, over the protocol meant", how);
    PROVEN_TEST_ASSERT(c.bad_end == PROVEN_ERR_INVALID_ARG && c.ended, "the client refuses Content-Length as a trailer, then ends with X-Sum", how);
    PROVEN_TEST_ASSERT(g_srv_trailers == 2 && g_srv_matched == 2 && g_srv_done_saw == 2,
                       "the server reads the request's X-Sum after its body, and it matches the body; it can still in on_done", how);
    PROVEN_TEST_ASSERT(g_srv_refused == 2, "the server refuses Content-Type as a trailer without ending its response", how);
    PROVEN_TEST_ASSERT(c.before == PROVEN_ERR_INVALID_STATE && c.last_read == PROVEN_OK && c.got_count == 2 && strcmp(c.status_text, "verified") == 0 &&
                       strcmp(c.timing, "db;dur=7") == 0 && c.done_count == 2,
                       "the client reads the response's two trailers after its body, not before it, and still in on_done", how);
    /* A compressed response: its trailers after the compressor's last bytes. */
    one(cl, port, h3, &c, "/zip", "zz", false, how);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.coded && g_srv_matched == 3 && c.body_bytes == 20000 && c.got_count == 2 && strcmp(c.status_text, "verified") == 0 && g_srv_refused == 3,
                       "a compressed response decodes whole, its trailers after it", how);
    /* Nowhere to put them: a response with a length, a request with a length. */
    c = (call_t){ .why = PROVEN_ERR_INVALID_STATE };
    proven_http_event_request_options_t o = { .method = sv("POST"), .url = url_to(port, "/len"), .body_length = 3, .http3 = h3,
                                              .on = { on_response, on_body, on_writable, on_done }, .ctx = &c };
    PROVEN_TEST_ASSERT(proven_http_event_client_start(cl, &o, &c.request) == PROVEN_OK, "a POST of three bytes", how);
    const proven_http_header_t t = { sv("X-Sum"), sv("1") };
    PROVEN_TEST_ASSERT(proven_http_event_request_end_trailers(c.request, &t, 1) == PROVEN_ERR_INVALID_STATE &&
                       proven_http_event_request_end_trailers(c.request, &t, 0) == PROVEN_ERR_INVALID_ARG, "trailers after a body with a length: PROVEN_ERR_INVALID_STATE; none: PROVEN_ERR_INVALID_ARG", how);
    bool wrote = false;
    for (int i = 0; i < 500 && !wrote && !c.done; ++i) {
        proven_result_size_t w = proven_http_event_request_write(c.request, mv("abc"));
        wrote = w.err == PROVEN_OK && w.value == 3;
        if (!wrote) spin_until(&c.done, 10);
    }
    PROVEN_TEST_ASSERT(wrote && proven_http_event_request_end(c.request) == PROVEN_OK, "its body written and ended", how);
    spin_until(&c.done, 10000);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.status == 200 && g_srv_misplaced == 1 && c.last_read == PROVEN_OK && c.got_count == 0,
                       "a response with a length refuses trailers (PROVEN_ERR_INVALID_STATE) and has none", how);
    PROVEN_TEST_ASSERT(g_srv_early == 4 && g_srv_none == 0, "in on_request, before a body's end, the server's trailers are PROVEN_ERR_INVALID_STATE", how);
}

int main(void) {
    PROVEN_TEST_SUITE("trailers through the event-driven server and client",
        "The event-driven server and client on one loop, over HTTP/1.1, HTTP/2 and HTTP/3: a request's trailers read after its body, a response's after its own; fields that may not be trailers refused; trailers where there is nowhere to put them refused.",
        "Inspect proven_http_stream_trailers and _end_trailers (http_event.c, http_event_h2.c, http_event_h3.c), proven_http_event_request_trailers and _end_trailers (http_event_client.c, _h2.c, _h3.c), and the trailers_ helpers in http_coding.c.");
    g_heap = proven_heap_allocator();
    PROVEN_TEST_ASSERT(proven_loop_create(g_heap, &g_loop) == PROVEN_OK, "a loop", "");
    PROVEN_TEST_ASSERT(tp_build(), "the test's certificates and keys are issued", "");
    proven_cert_store_t *anchors = NULL;
    PROVEN_TEST_ASSERT(proven_cert_store_create(g_heap, &anchors) == PROVEN_OK &&
                       proven_cert_store_add_pem(anchors, (proven_mem_view_t){ (const proven_byte_t *)TP_CA, strlen(TP_CA) }, NULL) == PROVEN_OK, "anchors", "");
    static const proven_u8str_view_t all[] = { { (const proven_byte_t *)"h2", 2 }, { (const proven_byte_t *)"http/1.1", 8 }, { (const proven_byte_t *)"h3", 2 } };
    static const proven_u8str_view_t h2h3[] = { { (const proven_byte_t *)"h2", 2 }, { (const proven_byte_t *)"h3", 2 } };
    static const proven_u8str_view_t h2[] = { { (const proven_byte_t *)"h2", 2 }, { (const proven_byte_t *)"http/1.1", 8 } };
    proven_tls_options_t so = { .alloc = g_heap, .certificate_pem = { (const proven_byte_t *)TP_SERVER_P256, strlen(TP_SERVER_P256) },
                                .private_key_pem = { (const proven_byte_t *)TP_SERVER_P256_KEY, strlen(TP_SERVER_P256_KEY) }, .alpn = all, .alpn_count = 3 };
    proven_tls_options_t co = { .alloc = g_heap, .anchors = anchors, .alpn = h2h3, .alpn_count = 2 };
    proven_tls_options_t co_h2 = { .alloc = g_heap, .anchors = anchors, .alpn = h2, .alpn_count = 2 };
    proven_tls_options_t co_h1 = { .alloc = g_heap, .anchors = anchors };
    PROVEN_TEST_ASSERT(proven_tls_config_create(&so, &g_server_tls) == PROVEN_OK && proven_tls_config_create(&co, &g_client_tls) == PROVEN_OK &&
                       proven_tls_config_create(&co_h1, &g_client_tls_h1) == PROVEN_OK && proven_tls_config_create(&co_h2, &g_client_tls_h2) == PROVEN_OK, "TLS configurations", "");

    proven_http_event_server_t *server = NULL;
    proven_net_addr_t at;
    proven_http_event_server_config_t scfg = { .on = { srv_request, srv_body, NULL, srv_done }, .tls = g_server_tls, .http2 = true, .http3 = true };
    const proven_err_t le = proven_http_event_server_create(g_loop, &scfg, &server) == PROVEN_OK
                            ? proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) : PROVEN_ERR_NOMEM;
    if (le == PROVEN_ERR_PERMISSION || le == PROVEN_ERR_UNSUPPORTED) {
        proven_http_event_server_destroy(server);
        PROVEN_TEST_PASS("skipped: no sockets here");
        return 0;
    }
    PROVEN_TEST_ASSERT(le == PROVEN_OK, "a server on TCP and UDP", "");

    PROVEN_TEST_SECTION("over HTTP/1.1", "After the last chunk, both ways; a chunked body keeps its trailer section in a buffer while it arrives.", "Check ev_end and the trailer buffer in http_event.c; cl_end and cl_drop_trailers in http_event_client.c.");
    {
        proven_http_event_client_t *cl = NULL;
        proven_http_event_client_config_t cfg = { .alloc = g_heap, .tls = g_client_tls_h1, .decompress = true };
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &cfg, &cl) == PROVEN_OK, "a client", "HTTP/1.1");
        over(cl, at.port, false, 1, "HTTP/1.1");
        proven_http_event_client_destroy(cl);
    }
    PROVEN_TEST_SECTION("over HTTP/2", "A HEADERS frame that ends the stream, both ways, once the body before it has gone.", "Check s_push and proven_http_event_h2_end_ in http_event_h2.c; r_push and proven_http_event_client_h2_end_ in http_event_client_h2.c.");
    {
        proven_http_event_client_t *cl = NULL;
        proven_http_event_client_config_t cfg = { .alloc = g_heap, .tls = g_client_tls_h2, .http2 = true, .decompress = true };
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &cfg, &cl) == PROVEN_OK, "a client", "HTTP/2");
        over(cl, at.port, false, 2, "HTTP/2");
        proven_http_event_client_destroy(cl);
    }
    PROVEN_TEST_SECTION("over HTTP/3", "A HEADERS frame that ends the stream, both ways.", "Check s_push and proven_http_event_h3_end_ in http_event_h3.c; r_push and proven_http_event_client_h3_end_ in http_event_client_h3.c.");
    {
        proven_http_event_client_t *cl = NULL;
        proven_http_event_client_config_t cfg = { .alloc = g_heap, .tls = g_client_tls, .http2 = true, .http3 = true, .decompress = true };
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &cfg, &cl) == PROVEN_OK, "a client", "HTTP/3");
        over(cl, at.port, true, 3, "HTTP/3");
        proven_http_event_client_destroy(cl);
    }

    proven_http_event_server_destroy(server);
    for (int i = 0; i < 20; ++i) (void)proven_loop_poll(g_loop, proven_net_deadline_in(5));
    proven_loop_destroy(g_loop);
    proven_tls_config_destroy(g_server_tls);
    proven_tls_config_destroy(g_client_tls);
    proven_tls_config_destroy(g_client_tls_h1);
    proven_tls_config_destroy(g_client_tls_h2);
    proven_cert_store_destroy(anchors);
    PROVEN_TEST_PASS("trailers through the event-driven server and client");
    return 0;
}

#else

int main(void) {
    PROVEN_TEST_SUITE("trailers through the event-driven server and client", "Built with PROVEN_NO_HTTP3: nothing to test.", "");
    PROVEN_TEST_PASS("skipped: PROVEN_NO_HTTP3");
    return 0;
}

#endif
