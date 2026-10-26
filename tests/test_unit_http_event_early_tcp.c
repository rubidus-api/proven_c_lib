#include "proven.h"
#include "proven_test.h"
#include "test_unit_tls_pki.h"
#include "../src/proven/proven_internal_http_event_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 0-RTT over TCP (RFC-0027), the event-driven client against the event-driven server on one loop,
 * over loopback, in HTTP/1.1 and in HTTP/2. With `early_data` on both sides a GET on a second
 * connection to an origin goes with the ClientHello and is answered before the client's Finished;
 * the handler sees proven_http_stream_early. A POST never goes early. A handler's 425 makes the
 * client send the request again after the handshake; a request that another client sends early
 * and is not safe to repeat is answered 425 by the server itself. A refused attempt is sent again
 * and answered. Without `early_data` on either side, nothing goes early.
 */

static proven_mem_view_t mv(const char *s) { return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }
static proven_u8str_view_t sv(const char *s) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }

static proven_allocator_t g_heap;
static proven_loop_t *g_loop;

static void spin_until(const bool *flag, proven_u32 ms) {
    proven_net_deadline_t until = proven_net_deadline_in(ms);
    while (!*flag && proven_time_monotonic_now() < until) PROVEN_TEST_ASSERT(proven_loop_poll(g_loop, proven_net_deadline_in(10)) == PROVEN_OK, "a round", "");
}
static void spin(proven_u32 ms) { bool never = false; spin_until(&never, ms); }

/* What the server's handler saw. */
static int g_calls, g_early_calls;
static proven_http_method_t g_last_method;

static void srv_request(void *ctx, proven_http_stream_t *s, const proven_http_request_t *req) {
    (void)ctx;
    ++g_calls;
    g_last_method = req->method;
    const bool early = proven_http_stream_early(s);
    if (early) ++g_early_calls;
    /* /too-early: this handler will not act on a copy that may have been replayed. */
    if (early && req->target.size == 10 && memcmp(req->target.ptr, "/too-early", 10) == 0) { (void)proven_http_stream_respond(s, 425, NULL, 0, mv("")); return; }
    if (req->method == PROVEN_HTTP_POST) return;                 /* answered when its body has come */
    (void)proven_http_stream_respond(s, 200, NULL, 0, mv(early ? "early" : "late"));
}
static void srv_body(void *ctx, proven_http_stream_t *s, proven_mem_view_t piece, bool last) {
    (void)ctx; (void)piece;
    if (last) (void)proven_http_stream_respond(s, 200, NULL, 0, mv("posted"));
}
static void srv_done(void *ctx, proven_http_stream_t *s, proven_err_t why) { (void)ctx; (void)s; (void)why; }

static proven_http_event_server_t *server_on(proven_tls_config_t *tls, proven_u16 *port) {
    proven_http_event_server_config_t cfg = { .on = { srv_request, srv_body, NULL, srv_done }, .tls = tls, .http2 = true };
    proven_http_event_server_t *server = NULL;
    proven_net_addr_t at;
    PROVEN_TEST_ASSERT(proven_http_event_server_create(g_loop, &cfg, &server) == PROVEN_OK &&
                       proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "a server", "");
    *port = at.port;
    return server;
}

typedef struct { proven_http_event_request_t *request; int status, early, protocol; bool done; proven_err_t why; char body[16]; proven_size_t body_len; } call_t;

static void on_response(void *ctx, proven_http_event_request_t *r, const proven_http_response_t *head) {
    call_t *c = ctx;
    c->status = head->status;
    c->early = proven_http_event_client_early_(r);
    c->protocol = proven_http_event_request_protocol(r);
}
static void on_body(void *ctx, proven_http_event_request_t *r, proven_mem_view_t piece, bool last) {
    (void)r; (void)last;
    call_t *c = ctx;
    for (proven_size_t i = 0; i < piece.size && c->body_len < sizeof c->body - 1; ++i) c->body[c->body_len++] = (char)piece.ptr[i];
}
static void on_done(void *ctx, proven_http_event_request_t *r, proven_err_t why) { (void)r; call_t *c = ctx; c->why = why; c->done = true; }

/* One request over TCP and TLS, run to its end. */
static call_t ask(proven_http_event_client_t *client, proven_u16 port, const char *method, const char *path, const char *body) {
    static char url[96];
    int n = snprintf(url, sizeof url, "https://127.0.0.1:%u%s", (unsigned)port, path);
    call_t c = { 0 };
    proven_http_event_request_options_t o = { .method = sv(method), .url = { (const proven_byte_t *)url, (proven_size_t)n },
        .body = body ? mv(body) : (proven_mem_view_t){ 0 }, .on = { .on_response = on_response, .on_body = on_body, .on_done = on_done }, .ctx = &c };
    PROVEN_TEST_ASSERT(proven_http_event_client_start(client, &o, &c.request) == PROVEN_OK, "a request starts", "");
    spin_until(&c.done, 5000);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK, "and ends", "");
    return c;
}

static proven_http_event_client_t *client_of(proven_tls_config_t *tls, bool h2) {
    proven_http_event_client_config_t cfg = { .alloc = g_heap, .tls = tls, .http2 = h2, .idle_timeout_ms = 100 };
    proven_http_event_client_t *client = NULL;
    PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &cfg, &client) == PROVEN_OK, "a client", "");
    return client;
}

/* A client that has made one connection to the origin and let it close: it holds a ticket. */
static proven_http_event_client_t *warmed(proven_tls_config_t *tls, bool h2, proven_u16 port) {
    proven_http_event_client_t *client = client_of(tls, h2);
    call_t a = ask(client, port, "GET", "/", NULL);
    PROVEN_TEST_ASSERT(a.status == 200 && a.early == 0 && a.protocol == (h2 ? 2 : 1), "a first connection: a full handshake, nothing early", "");
    spin(400);                                     /* idle past 100 ms: closed, its ticket kept */
    return client;
}

static void cases(bool h2, proven_tls_config_t *c_early, proven_tls_config_t *c_plain, proven_tls_config_t *s_early, proven_tls_config_t *s_plain) {
    const char *how = h2 ? "HTTP/2" : "HTTP/1.1";
    const int proto = h2 ? 2 : 1;
    proven_u16 port = 0;
    proven_http_event_server_t *server = server_on(s_early, &port);

    proven_http_event_client_t *client = warmed(c_early, h2, port);
    g_calls = g_early_calls = 0;
    call_t b = ask(client, port, "GET", "/", NULL);
    PROVEN_TEST_ASSERT(b.status == 200 && b.early == 1 && b.protocol == proto && g_early_calls == 1 && b.body_len == 5 && memcmp(b.body, "early", 5) == 0,
        "the second connection's GET went as early data, was taken, and its handler saw proven_http_stream_early", how);
    call_t d = ask(client, port, "GET", "/", NULL);
    PROVEN_TEST_ASSERT(d.status == 200 && d.early == 0 && memcmp(d.body, "late", 4) == 0, "a GET on that connection once it is up: not early", how);
    proven_http_event_client_destroy(client);
    spin(50);

    client = warmed(c_early, h2, port);
    g_early_calls = 0;
    b = ask(client, port, "POST", "/", "data");
    PROVEN_TEST_ASSERT(b.status == 200 && b.early == 0 && g_early_calls == 0 && memcmp(b.body, "posted", 6) == 0, "a POST waits for the handshake and is answered", how);
    proven_http_event_client_destroy(client);
    spin(50);

    /* A POST and a GET started together: only the GET may go early, whichever is older. */
    client = warmed(c_early, h2, port);
    {
        static char url[96];
        int len = snprintf(url, sizeof url, "https://127.0.0.1:%u/", (unsigned)port);
        static call_t p, g;
        memset(&p, 0, sizeof p); memset(&g, 0, sizeof g);
        proven_http_event_request_options_t po = { .method = sv("POST"), .url = { (const proven_byte_t *)url, (proven_size_t)len }, .body = mv("data"),
                                                   .on = { .on_response = on_response, .on_body = on_body, .on_done = on_done }, .ctx = &p };
        proven_http_event_request_options_t go = po;
        go.method = sv("GET"); go.body = (proven_mem_view_t){ 0 }; go.ctx = &g;
        PROVEN_TEST_ASSERT(proven_http_event_client_start(client, &po, &p.request) == PROVEN_OK && proven_http_event_client_start(client, &go, &g.request) == PROVEN_OK, "two requests start", how);
        spin_until(&p.done, 5000); spin_until(&g.done, 5000);
        PROVEN_TEST_ASSERT(p.done && g.done && p.why == PROVEN_OK && g.why == PROVEN_OK && p.early == 0 && g.early == 1 && memcmp(p.body, "posted", 6) == 0,
            "the POST waited for the handshake; the GET went early", how);
    }
    proven_http_event_client_destroy(client);
    spin(50);

    client = warmed(c_early, h2, port);
    g_calls = g_early_calls = 0;
    b = ask(client, port, "GET", "/too-early", NULL);
    PROVEN_TEST_ASSERT(b.status == 200 && b.early == 3 && g_calls == 2 && g_early_calls == 1 && memcmp(b.body, "late", 4) == 0,
        "a handler's 425 is not delivered: the request goes again after the handshake and is answered (two calls, the first early)", how);
    proven_http_event_client_destroy(client);
    spin(50);

    proven_http_event_client_test_early_any_(true);
    client = warmed(c_early, h2, port);
    g_calls = g_early_calls = 0;
    b = ask(client, port, "POST", "/", "data");
    PROVEN_TEST_ASSERT(b.status == 200 && g_calls == 1 && g_early_calls == 0 && g_last_method == PROVEN_HTTP_POST && memcmp(b.body, "posted", 6) == 0,
        "a POST another client sends early is answered 425 by the server, never reaching the handler; sent again, it is answered", how);
    proven_http_event_client_destroy(client);
    proven_http_event_client_test_early_any_(false);
    spin(50);

    /* A register of one: the first early attempt fills it, the next is refused and sent again. */
    proven_tls_test_early_cap_(s_early, 1);
    client = warmed(c_early, h2, port);
    b = ask(client, port, "GET", "/", NULL);
    proven_http_event_client_destroy(client);
    spin(50);
    client = warmed(c_early, h2, port);
    g_calls = g_early_calls = 0;
    b = ask(client, port, "GET", "/", NULL);
    PROVEN_TEST_ASSERT(b.status == 200 && b.early == 2 && g_calls == 1 && g_early_calls == 0 && memcmp(b.body, "late", 4) == 0,
        "refused (a full register): the request goes again under the handshake's keys and is answered once", how);
    proven_http_event_client_destroy(client);
    proven_tls_test_early_cap_(s_early, 0);
    spin(50);

    client = warmed(c_plain, h2, port);
    g_early_calls = 0;
    b = ask(client, port, "GET", "/", NULL);
    PROVEN_TEST_ASSERT(b.status == 200 && b.early == 0 && g_early_calls == 0, "a client without early_data sends nothing early", how);
    proven_http_event_client_destroy(client);
    proven_http_event_server_destroy(server);
    spin(50);

    server = server_on(s_plain, &port);
    client = warmed(c_early, h2, port);
    g_early_calls = 0;
    b = ask(client, port, "GET", "/", NULL);
    PROVEN_TEST_ASSERT(b.status == 200 && b.early == 0 && g_early_calls == 0, "a server without early_data issues tickets that allow none", how);
    proven_http_event_client_destroy(client);
    proven_http_event_server_destroy(server);
    spin(50);
}

int main(void) {
    PROVEN_TEST_SUITE("0-RTT over TCP, client and server",
        "In HTTP/1.1 and HTTP/2: a GET on a second connection goes as early data and is answered, its handler told; a POST never goes early; a 425 is sent again after the handshake; the server answers 425 itself to what is not safe to repeat; a refused attempt is sent again; without early_data on either side, nothing goes early.",
        "Inspect cl_flush and cl_settle_early in src/proven/http_event_client.c, c_settle_early and c_submit_waiting in http_event_client_h2.c; the early branches in http_event.c and d_request in http_event_h2.c; the engine is test_unit_tls's \"early data over TCP, taken\".");

    g_heap = proven_heap_allocator();
    PROVEN_TEST_ASSERT(proven_loop_create(g_heap, &g_loop) == PROVEN_OK, "a loop", "");
    PROVEN_TEST_ASSERT(tp_build(), "the test's certificates and keys are issued", "");
    proven_cert_store_t *anchors = NULL;
    PROVEN_TEST_ASSERT(proven_cert_store_create(g_heap, &anchors) == PROVEN_OK &&
                       proven_cert_store_add_pem(anchors, (proven_mem_view_t){ (const proven_byte_t *)TP_CA, strlen(TP_CA) }, NULL) == PROVEN_OK, "anchors", "");
    static const proven_u8str_view_t both[] = { { (const proven_byte_t *)"h2", 2 }, { (const proven_byte_t *)"http/1.1", 8 } };
    static const proven_u8str_view_t h1[] = { { (const proven_byte_t *)"http/1.1", 8 } };
    proven_tls_options_t so = { .alloc = g_heap, .certificate_pem = { (const proven_byte_t *)TP_SERVER_P256, strlen(TP_SERVER_P256) },
                                .private_key_pem = { (const proven_byte_t *)TP_SERVER_P256_KEY, strlen(TP_SERVER_P256_KEY) }, .alpn = both, .alpn_count = 2, .early_data = true };
    proven_tls_options_t so_plain = so; so_plain.early_data = false;
    proven_tls_config_t *s_early = NULL, *s_plain = NULL;
    PROVEN_TEST_ASSERT(proven_tls_config_create(&so, &s_early) == PROVEN_OK && proven_tls_config_create(&so_plain, &s_plain) == PROVEN_OK, "server TLS configurations", "");
    for (int h2 = 0; h2 < 2; ++h2) {
        proven_tls_options_t co = { .alloc = g_heap, .anchors = anchors, .alpn = h2 ? both : h1, .alpn_count = h2 ? 2 : 1, .early_data = true };
        proven_tls_options_t co_plain = co; co_plain.early_data = false;
        proven_tls_config_t *c_early = NULL, *c_plain = NULL;
        PROVEN_TEST_ASSERT(proven_tls_config_create(&co, &c_early) == PROVEN_OK && proven_tls_config_create(&co_plain, &c_plain) == PROVEN_OK, "client TLS configurations", "");
        if (h2) PROVEN_TEST_SECTION("HTTP/2", "The client agrees h2; the preface, the settings and the request go early.", "Check c_settle_early and the early branch of c_submit_waiting.");
        else PROVEN_TEST_SECTION("HTTP/1.1", "The client offers http/1.1 only; the request head goes early.", "Check cl_flush and cl_settle_early.");
        cases(h2 != 0, c_early, c_plain, s_early, s_plain);
        proven_tls_config_destroy(c_early); proven_tls_config_destroy(c_plain);
    }
    proven_tls_config_destroy(s_early); proven_tls_config_destroy(s_plain);
    proven_cert_store_destroy(anchors);
    proven_loop_destroy(g_loop);
    PROVEN_TEST_PASS("0-RTT over TCP: early data taken, refused and sent again, 425 from a handler and from the server, in HTTP/1.1 and HTTP/2.");
    return 0;
}
