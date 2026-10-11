#include "proven.h"
#include "proven_test.h"
#include "test_unit_tls_pki.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * HTTP/2 in the event-driven client, over loopback.
 *
 * One thread: the client and the event-driven server it talks to sit on the same loop, and the
 * test drives that loop by hand. The handlers on both sides are the ones an HTTP/1.1 program
 * would write. What is shown is that requests share one connection, that each still ends
 * exactly once, and that an origin which turns out to speak only HTTP/1.1 is served as that
 * without the caller's objects changing under it. That a server which is not this library's
 * agrees is a private program's evidence (Node's http2 server).
 */

static proven_mem_view_t mv(const char *s) { return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }
static proven_u8str_view_t sv(const char *s) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }

static proven_allocator_t g_heap;
static proven_loop_t *g_loop;
static proven_tls_config_t *g_server_tls_h2, *g_server_tls_h1, *g_client_tls;

static proven_byte_t pattern(proven_size_t i) { return (proven_byte_t)('a' + (i * 7 + i / 251) % 26); }

static void spin_until(const bool *flag, proven_u32 ms) {
    proven_net_deadline_t until = proven_net_deadline_in(ms);
    while (!*flag && proven_time_monotonic_now() < until) PROVEN_TEST_ASSERT(proven_loop_poll(g_loop, proven_net_deadline_in(10)) == PROVEN_OK, "a round", "");
}
static void spin(proven_u32 ms) { bool never = false; spin_until(&never, ms); }

// -----------------------------------------------------------------------------
// The server: an ordinary handler
// -----------------------------------------------------------------------------

typedef struct { proven_http_stream_t *stream; proven_loop_timer_t timer; proven_size_t sent, total, got; proven_byte_t *echo; proven_size_t cap; bool echoing; } served_t;
static int g_served, g_served_done, g_served_h2;

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
    if (proven_http_stream_protocol(s) == 2) g_served_h2++;
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

// -----------------------------------------------------------------------------
// The client's side
// -----------------------------------------------------------------------------

typedef struct {
    proven_http_event_request_t *request;
    proven_loop_timer_t timer;
    proven_size_t got, to_send, sent;
    int status, protocol, responses, dones, writables;
    proven_err_t why;
    bool done, bad, plain, pause_once, paused_now, body_while_paused;
    int tag;
    proven_byte_t first[8];
} call_t;

static bool g_in_start, g_done_in_start;
static int g_in_write, g_reentered;

static void call_resume(void *ctx) { call_t *c = ctx; c->paused_now = false; proven_http_event_request_resume(c->request); }

static void on_response(void *ctx, proven_http_event_request_t *r, const proven_http_response_t *head) {
    call_t *c = ctx;
    c->responses++;
    c->status = head->status;
    c->protocol = proven_http_event_request_protocol(r);
    if (proven_http_event_request_user(r) != c || head->version_minor != 1) c->bad = true;
    if (c->pause_once) {
        c->pause_once = false;
        c->paused_now = true;
        proven_http_event_request_pause(r);
        proven_loop_timer_set(g_loop, &c->timer, 100, call_resume, c);
    }
}

static void on_body(void *ctx, proven_http_event_request_t *r, proven_mem_view_t piece, bool last) {
    (void)r; (void)last;
    call_t *c = ctx;
    if (c->paused_now) c->body_while_paused = true;
    for (proven_size_t i = 0; i < piece.size; i++) {
        if (c->got + i < sizeof c->first) c->first[c->got + i] = piece.ptr[i];
        if (!c->plain && piece.ptr[i] != pattern(c->got + i)) c->bad = true;
    }
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
    c->why = why;
    proven_loop_timer_cancel(g_loop, &c->timer);
}

static char g_url[200];
static proven_u16 g_port;
static bool g_https;
static proven_u8str_view_t url(const char *path) {
    int n = snprintf(g_url, sizeof g_url, "%s://127.0.0.1:%u%s", g_https ? "https" : "http", (unsigned)g_port, path);
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)g_url, .size = (proven_size_t)n };
}

static proven_err_t start(proven_http_event_client_t *client, call_t *c, const char *method, const char *path, proven_mem_view_t body, proven_u64 body_length) {
    memset(c, 0, sizeof *c);
    proven_http_event_request_options_t o = { .method = sv(method), .url = url(path), .body = body, .body_length = body_length,
        .on = { .on_response = on_response, .on_body = on_body, .on_writable = on_writable, .on_done = on_done }, .ctx = c };
    g_in_start = true;
    proven_err_t e = proven_http_event_client_start(client, &o, &c->request);
    g_in_start = false;
    if (e == PROVEN_OK) proven_http_event_request_set_user(c->request, c);
    return e;
}

static proven_http_event_server_t *server_on(const proven_tls_config_t *tls, bool http2, proven_size_t max_streams) {
    proven_http_event_server_config_t cfg = { .on = { srv_request, srv_body, srv_writable, srv_done }, .tls = tls, .http2 = http2, .max_concurrent_streams = max_streams };
    proven_http_event_server_t *server = NULL;
    proven_net_addr_t at;
    PROVEN_TEST_ASSERT(proven_http_event_server_create(g_loop, &cfg, &server) == PROVEN_OK &&
                       proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "a server", "");
    g_port = at.port;
    g_https = tls != NULL;
    g_served = g_served_done = g_served_h2 = 0;
    return server;
}

static void cases(bool tls) {
    const char *how = tls ? "over TLS, by ALPN" : "in the clear, by prior knowledge";
    proven_http_event_server_t *server = server_on(tls ? g_server_tls_h2 : NULL, true, 0);
    proven_http_event_client_config_t ccfg = { .alloc = g_heap, .tls = tls ? g_client_tls : NULL, .http2 = tls, .http2_cleartext = !tls, .decompress = true, .idle_timeout_ms = 400 };
    proven_http_event_client_t *client = NULL;
    PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &ccfg, &client) == PROVEN_OK, "a client that may speak HTTP/2", how);
    static call_t c, many[40];

    c.plain = true;
    PROVEN_TEST_ASSERT(start(client, &c, "GET", "/hello", mv(""), 0) == PROVEN_OK, "a request is started", how);
    c.plain = true;
    PROVEN_TEST_ASSERT(proven_http_event_request_protocol(c.request) == 1, "while its connection is being made, a request does not yet claim HTTP/2", how);
    spin_until(&c.done, 5000);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.status == 200 && c.got == 5 && memcmp(c.first, "hello", 5) == 0 && c.protocol == 2 && c.responses == 1 && !c.bad,
        "and is answered over HTTP/2, with the user pointer it was given", how);
    PROVEN_TEST_ASSERT(g_served_h2 == 1 && proven_http_event_server_connections(server) == 1 && proven_http_event_client_requests(client) == 0, "the server agrees: one connection, HTTP/2", how);

    /* Forty at once, on the connection that is already there. */
    for (int i = 0; i < 40; i++) { PROVEN_TEST_ASSERT(start(client, &many[i], "GET", i % 2 ? "/big/20000" : "/later", mv(""), 0) == PROVEN_OK, "a request is started", how); many[i].plain = i % 2 == 0; }
    PROVEN_TEST_ASSERT(proven_http_event_client_requests(client) == 40, "forty requests are under way", how);
    for (int i = 0; i < 40; i++) spin_until(&many[i].done, 8000);
    bool all = true;
    for (int i = 0; i < 40; i++) all = all && many[i].done && many[i].why == PROVEN_OK && many[i].status == 200 && many[i].dones == 1 && !many[i].bad && many[i].got == (proven_size_t)(i % 2 ? 20000 : 5) && many[i].protocol == 2;
    PROVEN_TEST_ASSERT(all && proven_http_event_server_connections(server) == 1, "all forty are answered in full, each ended once - on the one connection", how);

    /* Bodies: down, compressed, up from memory, up in pieces of known and of unknown length. */
    PROVEN_TEST_ASSERT(start(client, &c, "GET", "/big/400000", mv(""), 0) == PROVEN_OK, "a large download", how);
    spin_until(&c.done, 8000);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.got == 400000 && !c.bad, "arrives whole", how);
    PROVEN_TEST_ASSERT(start(client, &c, "GET", "/zip/300000", mv(""), 0) == PROVEN_OK, "a compressed download", how);
    spin_until(&c.done, 8000);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.got == 300000 && !c.bad, "is delivered decoded, every byte", how);
    PROVEN_TEST_ASSERT(start(client, &c, "HEAD", "/big/5000", mv(""), 0) == PROVEN_OK, "HEAD", how);
    spin_until(&c.done, 5000);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.status == 200 && c.got == 0, "has a head and no body", how);
    static proven_byte_t up[200000];
    for (proven_size_t i = 0; i < sizeof up; i++) up[i] = pattern(i);
    PROVEN_TEST_ASSERT(start(client, &c, "POST", "/echo", (proven_mem_view_t){ up, sizeof up }, 0) == PROVEN_OK, "a body held in memory, larger than a window", how);
    spin_until(&c.done, 8000);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.got == sizeof up && !c.bad, "is sent whole and echoed whole", how);
    for (int unknown = 0; unknown < 2; unknown++) {
        PROVEN_TEST_ASSERT(start(client, &c, "POST", "/echo", mv(""), unknown ? PROVEN_HTTP_EVENT_LENGTH_UNKNOWN : 150000) == PROVEN_OK, "a body to be written in pieces", how);
        c.to_send = 150000;
        call_pump(&c);
        spin_until(&c.done, 8000);
        PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.got == 150000 && !c.bad && c.writables > 0, unknown ? "of unknown length: written as the windows allow, echoed whole" : "of known length: written as the windows allow, echoed whole", how);
    }
    PROVEN_TEST_ASSERT(g_reentered == 0, "on_writable is never called from inside the caller's own write", how);

    /* Pause: nothing of the body until resume, and then all of it - decoded, if it was compressed. */
    for (int zip = 0; zip < 2; zip++) {
        PROVEN_TEST_ASSERT(start(client, &c, "GET", zip ? "/zip/200000" : "/big/200000", mv(""), 0) == PROVEN_OK, "a download that will be paused at its head", how);
        c.pause_once = true;
        spin_until(&c.done, 8000);
        PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.got == 200000 && !c.bad && !c.body_while_paused, zip ? "a paused compressed download is held back and then delivered decoded" : "a paused download is held back and then delivered whole", how);
    }

    /* Giving up, and a server that does not answer: one request ends; the connection does not. */
    PROVEN_TEST_ASSERT(start(client, &c, "GET", "/never", mv(""), 0) == PROVEN_OK, "a request that will be given up", how);
    spin(50);
    proven_http_event_request_abort(c.request);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_ERR_RESET && c.dones == 1, "an aborted request ends at once, with PROVEN_ERR_RESET", how);
    c.plain = true;
    PROVEN_TEST_ASSERT(start(client, &c, "GET", "/hello", mv(""), 0) == PROVEN_OK, "the next request", how);
    c.plain = true;
    spin_until(&c.done, 5000);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.got == 5 && proven_http_event_server_connections(server) == 1, "goes over the same connection", how);
    PROVEN_TEST_ASSERT(!g_done_in_start, "on_done was never called from inside proven_http_event_client_start", how);

    /* An idle connection is let go, and the next request makes another. */
    spin(700);
    PROVEN_TEST_ASSERT(proven_http_event_server_connections(server) == 0, "left idle past the idle timeout, the connection is closed", how);
    PROVEN_TEST_ASSERT(start(client, &c, "GET", "/hello", mv(""), 0) == PROVEN_OK, "a request after that", how);
    c.plain = true;
    spin_until(&c.done, 5000);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.protocol == 2 && proven_http_event_server_connections(server) == 1, "makes a new connection and is answered", how);

    /* Destroying the client ends what is open, each once. */
    for (int i = 0; i < 5; i++) PROVEN_TEST_ASSERT(start(client, &many[i], "GET", "/never", mv(""), 0) == PROVEN_OK, "a request that will not be answered", how);
    spin(100);
    proven_http_event_client_destroy(client);
    bool cut = true;
    for (int i = 0; i < 5; i++) cut = cut && many[i].done && many[i].dones == 1 && many[i].why == PROVEN_ERR_RESET;
    PROVEN_TEST_ASSERT(cut, "a client destroyed with requests open tells each, once, that it was cut off", how);
    spin(100);
    proven_http_event_server_destroy(server);
    PROVEN_TEST_ASSERT(g_served == g_served_done, "and the server's handlers each saw their end", how);
}

int main(void) {
    PROVEN_TEST_SUITE("the event-driven HTTP client, speaking HTTP/2",
        "Requests that share one connection kept by the client, in the clear and over TLS: bodies both ways, compression, pause, abort, the server's limit on streams, time, and an origin that turns out to speak only HTTP/1.1.",
        "Inspect src/proven/http_event_client_h2.c: proven_http_event_client_h2_start_ for the connection a request joins, r_submit for what is sent, c_response and c_body for what arrives, c_fall_back for an HTTP/1.1 origin, c_pump for the round.");

    g_heap = proven_heap_allocator();
    PROVEN_TEST_ASSERT(tp_build(), "the test's certificates and keys are issued", "");
    PROVEN_TEST_ASSERT(proven_loop_create(g_heap, &g_loop) == PROVEN_OK, "a loop", "");
    proven_cert_store_t *anchors = NULL;
    PROVEN_TEST_ASSERT(proven_cert_store_create(g_heap, &anchors) == PROVEN_OK &&
                       proven_cert_store_add_pem(anchors, (proven_mem_view_t){ (const proven_byte_t *)TP_CA, strlen(TP_CA) }, NULL) == PROVEN_OK, "anchors", "");
    static const proven_u8str_view_t both[] = { { (const proven_byte_t *)"h2", 2 }, { (const proven_byte_t *)"http/1.1", 8 } };
    proven_tls_options_t co = { .alloc = g_heap, .anchors = anchors, .alpn = both, .alpn_count = 2 };
    proven_tls_options_t co_plain = { .alloc = g_heap, .anchors = anchors };
    proven_tls_options_t so = { .alloc = g_heap, .certificate_pem = { (const proven_byte_t *)TP_SERVER_ED, strlen(TP_SERVER_ED) }, .private_key_pem = { (const proven_byte_t *)TP_SERVER_ED_KEY, strlen(TP_SERVER_ED_KEY) },
                                .alpn = both, .alpn_count = 2 };
    proven_tls_options_t so_h1 = so; so_h1.alpn = NULL; so_h1.alpn_count = 0;
    proven_tls_config_t *client_no_alpn = NULL;
    PROVEN_TEST_ASSERT(proven_tls_config_create(&co, &g_client_tls) == PROVEN_OK && proven_tls_config_create(&co_plain, &client_no_alpn) == PROVEN_OK &&
                       proven_tls_config_create(&so, &g_server_tls_h2) == PROVEN_OK && proven_tls_config_create(&so_h1, &g_server_tls_h1) == PROVEN_OK, "TLS configurations", "");

    PROVEN_TEST_SECTION("making a client", "What TLS may agree to and what the client will speak must be the same thing.", "Check the ALPN comparison in proven_http_event_client_create.");
    {
        proven_http_event_client_t *client = NULL;
        proven_http_event_client_config_t cfg = { .alloc = g_heap, .tls = client_no_alpn, .http2 = true };
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &cfg, &client) == PROVEN_ERR_INVALID_ARG && !client, "HTTP/2 with a TLS configuration that does not offer h2 is refused", "");
        cfg.tls = g_client_tls; cfg.http2 = false;
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &cfg, &client) == PROVEN_ERR_INVALID_ARG && !client, "and so is a TLS configuration that offers h2 to a client that will not speak it", "");
        cfg.tls = NULL; cfg.http2 = true;
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &cfg, &client) == PROVEN_ERR_INVALID_ARG && !client, "and HTTP/2 by ALPN with no TLS at all", "");
    }

    PROVEN_TEST_SECTION("in the clear", "Every case, by prior knowledge.", "Check c_on_io: a connection in the clear is HTTP/2 as soon as it is connected.");
    cases(false);
    PROVEN_TEST_SECTION("over TLS", "The same cases, the protocol chosen by ALPN.", "Check c_readable: the choice is read when the handshake has finished.");
    cases(true);

    PROVEN_TEST_SECTION("the server's limit on streams", "More requests than the server allows at once on a connection.", "Check c_submit_waiting: a request the engine cannot take yet goes on waiting, and is sent when a stream ends.");
    {
        proven_http_event_server_t *server = server_on(NULL, true, 3);
        proven_http_event_client_config_t ccfg = { .alloc = g_heap, .http2_cleartext = true, .max_waiting_requests = 20 };
        proven_http_event_client_t *client = NULL;
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &ccfg, &client) == PROVEN_OK, "a client", "");
        static call_t c, many[21], extra;
        PROVEN_TEST_ASSERT(start(client, &c, "GET", "/hello", mv(""), 0) == PROVEN_OK, "a first request, to make the connection and learn the limit", "");
        c.plain = true;
        spin_until(&c.done, 5000);
        for (int i = 0; i < 21; i++) { PROVEN_TEST_ASSERT(start(client, &many[i], "GET", "/later", mv(""), 0) == PROVEN_OK, "a request is started", ""); many[i].plain = true; }
        /* Three are on streams; eighteen wait; the limit on waiting is twenty. */
        PROVEN_TEST_ASSERT(start(client, &extra, "GET", "/later", mv(""), 0) == PROVEN_OK && start(client, &c, "GET", "/later", mv(""), 0) == PROVEN_OK, "the nineteenth and twentieth to wait are taken", "");
        c.plain = extra.plain = true;
        static call_t over;
        PROVEN_TEST_ASSERT(start(client, &over, "GET", "/later", mv(""), 0) == PROVEN_ERR_BUSY, "and the next is PROVEN_ERR_BUSY: nothing waits without bound", "");
        for (int i = 0; i < 21; i++) spin_until(&many[i].done, 10000);
        spin_until(&c.done, 5000); spin_until(&extra.done, 5000);
        bool all = c.done && extra.done && c.why == PROVEN_OK && extra.why == PROVEN_OK;
        for (int i = 0; i < 21; i++) all = all && many[i].done && many[i].why == PROVEN_OK && many[i].got == 5 && many[i].dones == 1;
        PROVEN_TEST_ASSERT(all && proven_http_event_server_connections(server) == 1, "twenty-three requests at a limit of three streams are all answered, three at a time, on one connection", "");
        proven_http_event_client_destroy(client);
        /* And with no first request to learn the limit by: a burst on a connection that does
         * not exist yet waits for the server's settings, so none is sent ahead of them to be
         * refused. */
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &ccfg, &client) == PROVEN_OK, "a new client", "");
        for (int i = 0; i < 12; i++) { PROVEN_TEST_ASSERT(start(client, &many[i], "GET", "/later", mv(""), 0) == PROVEN_OK, "a request is started", ""); many[i].plain = true; }
        for (int i = 0; i < 12; i++) spin_until(&many[i].done, 10000);
        all = true;
        for (int i = 0; i < 12; i++) all = all && many[i].done && many[i].why == PROVEN_OK && many[i].got == 5;
        PROVEN_TEST_ASSERT(all, "twelve requests started at once on a new connection, at a limit of three, are all answered: none was sent before the limit was known", "");
        proven_http_event_client_destroy(client);
        spin(50);
        proven_http_event_server_destroy(server);
    }

    PROVEN_TEST_SECTION("a server that says goodbye", "GOAWAY with requests in flight and requests still waiting.", "Check the GOAWAY case of c_event: what was on a stream finishes, what was waiting is told it may be started again, and the next request makes a new connection.");
    {
        proven_http_event_server_t *server = server_on(NULL, true, 2);
        proven_http_event_client_config_t ccfg = { .alloc = g_heap, .http2_cleartext = true };
        proven_http_event_client_t *client = NULL;
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &ccfg, &client) == PROVEN_OK, "a client", "");
        static call_t many[6], after;
        for (int i = 0; i < 6; i++) { PROVEN_TEST_ASSERT(start(client, &many[i], "GET", "/later", mv(""), 0) == PROVEN_OK, "a request is started", ""); many[i].plain = true; }
        spin(30);                                  /* two are on streams, at a limit of two; four wait */
        proven_http_event_server_stop_listening(server);
        for (int i = 0; i < 6; i++) spin_until(&many[i].done, 5000);
        int ok = 0, again = 0; bool once = true;
        for (int i = 0; i < 6; i++) { if (many[i].why == PROVEN_OK && many[i].got == 5) ok++; if (many[i].why == PROVEN_ERR_AGAIN && many[i].responses == 0) again++; once = once && many[i].dones == 1; }
        PROVEN_TEST_ASSERT(ok == 2 && again == 4 && once, "the two the server had taken are answered; the four that were waiting end with PROVEN_ERR_AGAIN - they may be started again - each once", "");
        spin(100);
        PROVEN_TEST_ASSERT(proven_http_event_server_connections(server) == 0, "and the connection is gone", "");
        proven_net_addr_t at;
        PROVEN_TEST_ASSERT(proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "the server listens again", "");
        g_port = at.port;
        PROVEN_TEST_ASSERT(start(client, &after, "GET", "/hello", mv(""), 0) == PROVEN_OK, "a request started again", "");
        after.plain = true;
        spin_until(&after.done, 5000);
        PROVEN_TEST_ASSERT(after.done && after.why == PROVEN_OK && after.protocol == 2 && proven_http_event_server_connections(server) == 1, "makes a new connection and is answered", "");
        proven_http_event_client_destroy(client);
        spin(50);
        proven_http_event_server_destroy(server);
    }

    PROVEN_TEST_SECTION("two origins, one client", "Requests to two servers, interleaved.", "Check the comparison of scheme, host, port and address in proven_http_event_client_h2_start_: a connection is shared by the requests of its own origin and no other's.");
    {
        proven_http_event_server_t *first = server_on(NULL, true, 0);
        const proven_u16 first_port = g_port;
        proven_http_event_server_t *second = server_on(NULL, true, 0);
        const proven_u16 second_port = g_port;
        proven_http_event_client_config_t ccfg = { .alloc = g_heap, .http2_cleartext = true };
        proven_http_event_client_t *client = NULL;
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &ccfg, &client) == PROVEN_OK, "a client", "");
        static call_t many[12];
        for (int i = 0; i < 12; i++) { g_port = i % 2 ? second_port : first_port; PROVEN_TEST_ASSERT(start(client, &many[i], "GET", "/hello", mv(""), 0) == PROVEN_OK, "a request is started", ""); many[i].plain = true; }
        for (int i = 0; i < 12; i++) spin_until(&many[i].done, 5000);
        bool all = true;
        for (int i = 0; i < 12; i++) all = all && many[i].done && many[i].why == PROVEN_OK && many[i].got == 5 && many[i].protocol == 2;
        PROVEN_TEST_ASSERT(all && proven_http_event_server_connections(first) == 1 && proven_http_event_server_connections(second) == 1 && g_served_h2 == 12,
            "twelve requests, six to each of two servers, are answered on two connections: one for each origin", "");
        proven_http_event_client_destroy(client);
        spin(50);
        proven_http_event_server_destroy(first);
        proven_http_event_server_destroy(second);
    }

    PROVEN_TEST_SECTION("time", "A request that is not answered.", "Check r_expect and c_on_timer: each request has its own deadline.");
    {
        proven_http_event_server_t *server = server_on(NULL, true, 0);
        proven_http_event_client_config_t ccfg = { .alloc = g_heap, .http2_cleartext = true, .response_timeout_ms = 150 };
        proven_http_event_client_t *client = NULL;
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &ccfg, &client) == PROVEN_OK, "a client", "");
        static call_t slow, quick;
        PROVEN_TEST_ASSERT(start(client, &slow, "GET", "/never", mv(""), 0) == PROVEN_OK, "a request the server will not answer", "");
        spin_until(&slow.done, 3000);
        PROVEN_TEST_ASSERT(slow.done && slow.why == PROVEN_ERR_TIMEOUT && slow.dones == 1, "ends with PROVEN_ERR_TIMEOUT after the response timeout", "");
        PROVEN_TEST_ASSERT(start(client, &quick, "GET", "/hello", mv(""), 0) == PROVEN_OK, "and another, after it", "");
        quick.plain = true;
        spin_until(&quick.done, 3000);
        PROVEN_TEST_ASSERT(quick.done && quick.why == PROVEN_OK && proven_http_event_server_connections(server) == 1, "is answered on the same connection: one request's silence is its own", "");
        proven_http_event_client_destroy(client);
        spin(50);
        proven_http_event_server_destroy(server);
    }

    PROVEN_TEST_SECTION("an origin that speaks only HTTP/1.1", "A client that asks for HTTP/2, and a server whose TLS does not offer it.", "Check c_fall_back: the requests that were waiting start again as HTTP/1.1 requests, known to their caller by the objects it already had.");
    {
        proven_http_event_server_t *server = server_on(g_server_tls_h1, false, 0);
        proven_http_event_client_config_t ccfg = { .alloc = g_heap, .tls = g_client_tls, .http2 = true };
        proven_http_event_client_t *client = NULL;
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &ccfg, &client) == PROVEN_OK, "a client that would speak HTTP/2", "");
        static call_t many[6], up, later;
        for (int i = 0; i < 6; i++) { PROVEN_TEST_ASSERT(start(client, &many[i], "GET", i % 2 ? "/big/50000" : "/hello", mv(""), 0) == PROVEN_OK, "a request, before the origin is known", ""); many[i].plain = i % 2 == 0; }
        /* One whose body is written in pieces, begun while it is still waiting. */
        PROVEN_TEST_ASSERT(start(client, &up, "POST", "/echo", mv(""), 60000) == PROVEN_OK, "and one with a body to be written", "");
        up.to_send = 60000;
        call_pump(&up);
        PROVEN_TEST_ASSERT(up.sent == 0 && proven_http_event_client_requests(client) == 7, "which is not taken yet: there is no connection to write to", "");
        proven_http_event_request_t *held = many[0].request;
        for (int i = 0; i < 6; i++) spin_until(&many[i].done, 8000);
        spin_until(&up.done, 8000);
        bool all = true;
        for (int i = 0; i < 6; i++) all = all && many[i].done && many[i].why == PROVEN_OK && many[i].status == 200 && many[i].dones == 1 && !many[i].bad && many[i].protocol == 1 && many[i].got == (proven_size_t)(i % 2 ? 50000 : 5);
        PROVEN_TEST_ASSERT(all, "all six are answered - over HTTP/1.1, each with the user pointer it was given", "");
        PROVEN_TEST_ASSERT(held == many[0].request, "by the objects the caller already held", "");
        PROVEN_TEST_ASSERT(up.done && up.why == PROVEN_OK && up.got == 60000 && !up.bad && up.writables > 0 && up.protocol == 1, "and the body that was waiting to be written is asked for, written and echoed", "");
        PROVEN_TEST_ASSERT(g_served_h2 == 0 && g_served == 7, "the server saw seven HTTP/1.1 requests and no HTTP/2", "");
        /* The origin is remembered: the next request does not try again. */
        PROVEN_TEST_ASSERT(start(client, &later, "GET", "/hello", mv(""), 0) == PROVEN_OK, "a later request", "");
        later.plain = true;
        spin_until(&later.done, 5000);
        PROVEN_TEST_ASSERT(later.done && later.why == PROVEN_OK && later.protocol == 1 && g_served == 8 && proven_http_event_client_requests(client) == 0, "goes straight to HTTP/1.1", "");
        proven_http_event_client_destroy(client);
        spin(50);
        proven_http_event_server_destroy(server);
    }

    proven_tls_config_destroy(g_client_tls); proven_tls_config_destroy(client_no_alpn); proven_tls_config_destroy(g_server_tls_h2); proven_tls_config_destroy(g_server_tls_h1);
    proven_cert_store_destroy(anchors);
    proven_loop_destroy(g_loop);
    PROVEN_TEST_PASS("requests of the event-driven client share an HTTP/2 connection, each ends once, and an HTTP/1.1 origin is served as one.");
    return 0;
}
