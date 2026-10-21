#include "proven.h"
#include "proven_test.h"
#include "test_unit_tls_pki.h"
#include "../src/proven/proven_internal_http_event_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 0-RTT over HTTP/3 (RFC-0021), the event-driven client against the event-driven server on one
 * loop, over loopback. With `early_data` on both sides, a GET on a second connection to an origin
 * goes as early data and is answered; the handler sees proven_http_stream_early. A POST never goes
 * early. A handler's 425 to an early request makes the client send it once more after the
 * handshake; a request that another client sends early and is not safe to repeat is answered 425
 * by the server itself, without its handler. A server without `early_data` issues tickets that
 * allow none, and a client without it sends none.
 */

#ifndef PROVEN_NO_HTTP3

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
    proven_http_event_server_config_t cfg = { .on = { srv_request, srv_body, NULL, srv_done }, .tls = tls, .http3 = true };
    proven_http_event_server_t *server = NULL;
    proven_net_addr_t at;
    PROVEN_TEST_ASSERT(proven_http_event_server_create(g_loop, &cfg, &server) == PROVEN_OK &&
                       proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "a server", "");
    *port = at.port;
    return server;
}

typedef struct { proven_http_event_request_t *request; int status, early; bool done; proven_err_t why; char body[16]; proven_size_t body_len; } call_t;

static void on_response(void *ctx, proven_http_event_request_t *r, const proven_http_response_t *head) {
    call_t *c = ctx;
    c->status = head->status;
    c->early = proven_http_event_client_h3_early_(r);
}
static void on_body(void *ctx, proven_http_event_request_t *r, proven_mem_view_t piece, bool last) {
    (void)r; (void)last;
    call_t *c = ctx;
    for (proven_size_t i = 0; i < piece.size && c->body_len < sizeof c->body - 1; ++i) c->body[c->body_len++] = (char)piece.ptr[i];
}
static void on_done(void *ctx, proven_http_event_request_t *r, proven_err_t why) { (void)r; call_t *c = ctx; c->why = why; c->done = true; }

/* One request over HTTP/3, run to its end. */
static call_t ask(proven_http_event_client_t *client, proven_u16 port, const char *method, const char *path, const char *body) {
    static char url[96];
    int n = snprintf(url, sizeof url, "https://127.0.0.1:%u%s", (unsigned)port, path);
    call_t c = { 0 };
    proven_http_event_request_options_t o = { .method = sv(method), .url = { (const proven_byte_t *)url, (proven_size_t)n }, .http3 = true,
        .body = body ? mv(body) : (proven_mem_view_t){ 0 }, .on = { .on_response = on_response, .on_body = on_body, .on_done = on_done }, .ctx = &c };
    PROVEN_TEST_ASSERT(proven_http_event_client_start(client, &o, &c.request) == PROVEN_OK, "a request starts", "");
    spin_until(&c.done, 5000);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK, "and ends", "");
    return c;
}

static proven_http_event_client_t *client_of(proven_tls_config_t *tls) {
    proven_http_event_client_config_t cfg = { .alloc = g_heap, .tls = tls, .http3 = true, .idle_timeout_ms = 100 };
    proven_http_event_client_t *client = NULL;
    PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &cfg, &client) == PROVEN_OK, "a client", "");
    return client;
}

int main(void) {
    PROVEN_TEST_SUITE("0-RTT over HTTP/3, client and server",
        "A GET on a second connection goes as early data and is answered, its handler told; a POST never goes early; a 425 is sent again once after the handshake; the server answers 425 itself to what is not safe to repeat; without early_data on either side, nothing goes early.",
        "Inspect r_early_ok, c_submit_waiting, c_response's 425 and r_again in src/proven/http_event_client_h3.c; d_request's early branch in src/proven/http_event_h3.c; proven_http_stream_early in http_event.c.");

    g_heap = proven_heap_allocator();
    PROVEN_TEST_ASSERT(proven_loop_create(g_heap, &g_loop) == PROVEN_OK, "a loop", "");
    PROVEN_TEST_ASSERT(tp_build(), "the test's certificates and keys are issued", "");
    proven_cert_store_t *anchors = NULL;
    PROVEN_TEST_ASSERT(proven_cert_store_create(g_heap, &anchors) == PROVEN_OK &&
                       proven_cert_store_add_pem(anchors, (proven_mem_view_t){ (const proven_byte_t *)TP_CA, strlen(TP_CA) }, NULL) == PROVEN_OK, "anchors", "");
    static const proven_u8str_view_t h3[] = { { (const proven_byte_t *)"h3", 2 } };
    proven_tls_options_t so = { .alloc = g_heap, .certificate_pem = { (const proven_byte_t *)TP_SERVER_P256, strlen(TP_SERVER_P256) },
                                .private_key_pem = { (const proven_byte_t *)TP_SERVER_P256_KEY, strlen(TP_SERVER_P256_KEY) }, .alpn = h3, .alpn_count = 1, .early_data = true };
    proven_tls_options_t so_plain = so; so_plain.early_data = false;
    proven_tls_options_t co = { .alloc = g_heap, .anchors = anchors, .alpn = h3, .alpn_count = 1, .early_data = true };
    proven_tls_options_t co_plain = co; co_plain.early_data = false;
    proven_tls_config_t *s_early = NULL, *s_plain = NULL, *c_early = NULL, *c_plain = NULL;
    PROVEN_TEST_ASSERT(proven_tls_config_create(&so, &s_early) == PROVEN_OK && proven_tls_config_create(&so_plain, &s_plain) == PROVEN_OK &&
                       proven_tls_config_create(&co, &c_early) == PROVEN_OK && proven_tls_config_create(&co_plain, &c_plain) == PROVEN_OK, "TLS configurations", "");
    proven_u16 port = 0;
    proven_http_event_server_t *server = server_on(s_early, &port);

    PROVEN_TEST_SECTION("a GET as early data", "The first connection learns a ticket that allows early data; the second sends its GET with its first packet, and the handler is told.",
        "Check c_submit_waiting before the handshake and proven_http_stream_early.");
    {
        proven_http_event_client_t *client = client_of(c_early);
        g_calls = g_early_calls = 0;
        call_t a = ask(client, port, "GET", "/", NULL);
        PROVEN_TEST_ASSERT(a.status == 200 && a.early == 0 && g_early_calls == 0, "the first connection: a full handshake, nothing early", "");
        spin(400);                                         /* idle past 100 ms: closed, its ticket kept */
        call_t b = ask(client, port, "GET", "/", NULL);
        PROVEN_TEST_ASSERT(b.status == 200 && b.early == 1 && g_early_calls == 1 && b.body_len == 5 && memcmp(b.body, "early", 5) == 0,
            "the second: the GET went as early data, was accepted, and its handler saw proven_http_stream_early", "");
        call_t d = ask(client, port, "GET", "/", NULL);
        PROVEN_TEST_ASSERT(d.status == 200 && d.early == 0 && memcmp(d.body, "late", 4) == 0, "a GET on the connection once it is up: not early", "");
        /* That connection had a new ticket after its first request ended; it is still open (no
         * new request may use it) when the next connection is made: the next one gets the new
         * ticket, not the one already spent on early data, and its GET is taken early again. */
        spin(30);
        proven_http_event_client_h3_test_retire_(client);
        call_t f = ask(client, port, "GET", "/", NULL);
        PROVEN_TEST_ASSERT(f.status == 200 && f.early == 1, "a connection made while the last is still open: early again, on the ticket that one learned", "");
        proven_http_event_client_destroy(client);
        spin(50);
    }

    PROVEN_TEST_SECTION("a POST never early", "A POST as the first request of a resuming connection waits for the handshake.", "Check r_early_ok.");
    {
        proven_http_event_client_t *client = client_of(c_early);
        (void)ask(client, port, "GET", "/", NULL);
        spin(400);
        g_early_calls = 0;
        call_t b = ask(client, port, "POST", "/", "data");
        PROVEN_TEST_ASSERT(b.status == 200 && b.early == 0 && g_early_calls == 0 && memcmp(b.body, "posted", 6) == 0, "the POST waited and was answered", "");
        proven_http_event_client_destroy(client);
        spin(50);
    }

    PROVEN_TEST_SECTION("425 from a handler", "A handler that will not act on an early copy answers 425; the client sends the request again once the handshake is over.", "Check c_response's 425 and r_again.");
    {
        proven_http_event_client_t *client = client_of(c_early);
        (void)ask(client, port, "GET", "/", NULL);
        spin(400);
        g_calls = g_early_calls = 0;
        call_t b = ask(client, port, "GET", "/too-early", NULL);
        PROVEN_TEST_ASSERT(b.status == 200 && g_calls == 2 && g_early_calls == 1 && memcmp(b.body, "late", 4) == 0,
            "the 425 was not delivered: the request went again after the handshake and was answered (two calls, the first early)", "");
        proven_http_event_client_destroy(client);
        spin(50);
    }

    PROVEN_TEST_SECTION("425 from the server", "Another client sends a POST early: the server answers 425 without calling its handler, and the request, sent again, is answered.",
        "Check d_request's early branch.");
    {
        proven_http_event_client_h3_test_early_any_(true);
        proven_http_event_client_t *client = client_of(c_early);
        (void)ask(client, port, "GET", "/", NULL);
        spin(400);
        g_calls = g_early_calls = 0;
        call_t b = ask(client, port, "POST", "/", "data");
        PROVEN_TEST_ASSERT(b.status == 200 && g_calls == 1 && g_early_calls == 0 && g_last_method == PROVEN_HTTP_POST && memcmp(b.body, "posted", 6) == 0,
            "the early POST never reached the handler; sent again after the handshake, it did", "");
        proven_http_event_client_destroy(client);
        proven_http_event_client_h3_test_early_any_(false);
        spin(50);
    }

    PROVEN_TEST_SECTION("without early_data", "A server without it issues tickets that allow none; a client without it sends none.", "Check the ticket's allowance and the option on each side.");
    {
        proven_http_event_client_t *client = client_of(c_plain);
        (void)ask(client, port, "GET", "/", NULL);
        spin(400);
        g_early_calls = 0;
        call_t b = ask(client, port, "GET", "/", NULL);
        PROVEN_TEST_ASSERT(b.status == 200 && b.early == 0 && g_early_calls == 0, "a client without early_data: nothing early", "");
        proven_http_event_client_destroy(client);
        proven_http_event_server_destroy(server);
        spin(50);
        server = server_on(s_plain, &port);
        client = client_of(c_early);
        (void)ask(client, port, "GET", "/", NULL);
        spin(400);
        b = ask(client, port, "GET", "/", NULL);
        PROVEN_TEST_ASSERT(b.status == 200 && b.early == 0 && g_early_calls == 0, "a server without early_data: its ticket allows none, nothing early", "");
        proven_http_event_client_destroy(client);
    }

    proven_http_event_server_destroy(server);
    spin(50);
    proven_tls_config_destroy(s_early); proven_tls_config_destroy(s_plain); proven_tls_config_destroy(c_early); proven_tls_config_destroy(c_plain);
    proven_cert_store_destroy(anchors);
    proven_loop_destroy(g_loop);
    PROVEN_TEST_PASS("0-RTT over HTTP/3: early where it is safe, 425 where it is not");
    return 0;
}

#else

int main(void) {
    PROVEN_TEST_SUITE("0-RTT over HTTP/3, client and server", "Built with PROVEN_NO_HTTP3: nothing to test.", "");
    PROVEN_TEST_PASS("skipped: PROVEN_NO_HTTP3");
    return 0;
}

#endif
