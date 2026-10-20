#include "proven.h"
#include "proven_test.h"
#include "test_unit_tls_pki.h"
#include "../src/proven/proven_internal_http_event_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * TLS session reuse in the event-driven client (RFC-0020): a second connection to an origin
 * resumes the session the first one learned - over HTTP/1.1, HTTP/2 and HTTP/3 - unless the TLS
 * configuration says no_resumption; TCP's sessions are not offered over QUIC; a server whose
 * ticket keys changed is still answered, by a full handshake; the cache keeps 64 origins per
 * transport, the least recently used given up. Against the event-driven server on the same
 * loop, over loopback.
 */

static proven_mem_view_t mv(const char *s) { return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }
static proven_u8str_view_t sv(const char *s) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }

static proven_allocator_t g_heap;
static proven_loop_t *g_loop;
static proven_tls_config_t *g_server_tls, *g_server_tls_new, *g_server_tls_none;
static proven_tls_config_t *g_client_h1, *g_client_h1_none, *g_client_h2, *g_client_h3;

static void spin_until(const bool *flag, proven_u32 ms) {
    proven_net_deadline_t until = proven_net_deadline_in(ms);
    while (!*flag && proven_time_monotonic_now() < until) PROVEN_TEST_ASSERT(proven_loop_poll(g_loop, proven_net_deadline_in(10)) == PROVEN_OK, "a round", "");
}
static void spin(proven_u32 ms) { bool never = false; spin_until(&never, ms); }

static void srv_request(void *ctx, proven_http_stream_t *s, const proven_http_request_t *req) {
    (void)ctx; (void)req;
    (void)proven_http_stream_respond(s, 200, NULL, 0, mv("hello"));
}
static void srv_body(void *ctx, proven_http_stream_t *s, proven_mem_view_t piece, bool last) { (void)ctx; (void)s; (void)piece; (void)last; }
static void srv_done(void *ctx, proven_http_stream_t *s, proven_err_t why) { (void)ctx; (void)s; (void)why; }

static proven_http_event_server_t *server_on(proven_tls_config_t *tls, proven_u16 port, proven_u16 *got) {
    proven_http_event_server_config_t cfg = { .on = { srv_request, srv_body, NULL, srv_done }, .tls = tls, .http2 = true,
#ifndef PROVEN_NO_HTTP3
                                              .http3 = true,
#endif
    };
    proven_http_event_server_t *server = NULL;
    proven_net_addr_t at;
    PROVEN_TEST_ASSERT(proven_http_event_server_create(g_loop, &cfg, &server) == PROVEN_OK &&
                       proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, port), &at) == PROVEN_OK, "a server", "");
    *got = at.port;
    return server;
}

typedef struct { proven_http_event_request_t *request; int status, protocol; bool resumed, done; proven_err_t why; } call_t;

static void on_response(void *ctx, proven_http_event_request_t *r, const proven_http_response_t *head) {
    call_t *c = ctx;
    c->status = head->status;
    c->protocol = proven_http_event_request_protocol(r);
    c->resumed = proven_http_event_request_resumed_(r);
}
static void on_body(void *ctx, proven_http_event_request_t *r, proven_mem_view_t piece, bool last) { (void)ctx; (void)r; (void)piece; (void)last; }
static void on_done(void *ctx, proven_http_event_request_t *r, proven_err_t why) { (void)r; call_t *c = ctx; c->why = why; c->done = true; }

/* One request to https://127.0.0.1:port/, run to its end. */
static call_t ask(proven_http_event_client_t *client, proven_u16 port, bool h3) {
    static char url[64];
    int n = snprintf(url, sizeof url, "https://127.0.0.1:%u/", (unsigned)port);
    call_t c = { 0 };
    proven_http_event_request_options_t o = { .method = sv("GET"), .url = { (const proven_byte_t *)url, (proven_size_t)n }, .http3 = h3,
        .on = { .on_response = on_response, .on_body = on_body, .on_done = on_done }, .ctx = &c };
    PROVEN_TEST_ASSERT(proven_http_event_client_start(client, &o, &c.request) == PROVEN_OK, "a request starts", "");
    spin_until(&c.done, 5000);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_OK && c.status == 200, "and is answered", "");
    return c;
}

static proven_http_event_client_t *client_of(proven_tls_config_t *tls, bool h2, bool h3, bool no_reuse) {
    proven_http_event_client_config_t cfg = { .alloc = g_heap, .tls = tls, .http2 = h2, .no_connection_reuse = no_reuse, .idle_timeout_ms = 100,
#ifndef PROVEN_NO_HTTP3
                                              .http3 = h3,
#endif
    };
    (void)h3;
    proven_http_event_client_t *client = NULL;
    PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &cfg, &client) == PROVEN_OK, "a client", "");
    return client;
}

int main(void) {
    PROVEN_TEST_SUITE("the event-driven HTTP client, resuming TLS sessions",
        "A second connection to an origin resumes the session the first learned, over HTTP/1.1, HTTP/2 and HTTP/3, unless no_resumption; TCP's sessions stay off QUIC; a changed server is answered by a full handshake; 64 origins per transport are kept.",
        "Inspect proven_http_event_client_session_start_ and _keep_ in src/proven/http_event_client.c, and where each path calls them: cl_conn_open, cl_conn_keep and cl_conn_close; c_open and c_close_socket in http_event_client_h2.c; c_open, r_finish and c_kill in http_event_client_h3.c.");

    g_heap = proven_heap_allocator();
    PROVEN_TEST_ASSERT(proven_loop_create(g_heap, &g_loop) == PROVEN_OK, "a loop", "");
    PROVEN_TEST_ASSERT(tp_build(), "the test's certificates and keys are issued", "");
    proven_cert_store_t *anchors = NULL;
    PROVEN_TEST_ASSERT(proven_cert_store_create(g_heap, &anchors) == PROVEN_OK &&
                       proven_cert_store_add_pem(anchors, (proven_mem_view_t){ (const proven_byte_t *)TP_CA, strlen(TP_CA) }, NULL) == PROVEN_OK, "anchors", "");
    static const proven_u8str_view_t all[] = { { (const proven_byte_t *)"h2", 2 }, { (const proven_byte_t *)"http/1.1", 8 }, { (const proven_byte_t *)"h3", 2 } };
    static const proven_u8str_view_t h2[] = { { (const proven_byte_t *)"h2", 2 } };
    static const proven_u8str_view_t h2h3[] = { { (const proven_byte_t *)"h2", 2 }, { (const proven_byte_t *)"h3", 2 } };
    proven_tls_options_t so = { .alloc = g_heap, .certificate_pem = { (const proven_byte_t *)TP_SERVER_P256, strlen(TP_SERVER_P256) },
                                .private_key_pem = { (const proven_byte_t *)TP_SERVER_P256_KEY, strlen(TP_SERVER_P256_KEY) }, .alpn = all, .alpn_count = 3 };
    proven_tls_options_t so_none = so; so_none.no_resumption = true;
    proven_tls_options_t c1 = { .alloc = g_heap, .anchors = anchors };
    proven_tls_options_t c1_none = c1; c1_none.no_resumption = true;
    proven_tls_options_t c2 = { .alloc = g_heap, .anchors = anchors, .alpn = h2, .alpn_count = 1 };
    proven_tls_options_t c3 = { .alloc = g_heap, .anchors = anchors, .alpn = h2h3, .alpn_count = 2 };
    PROVEN_TEST_ASSERT(proven_tls_config_create(&so_none, &g_server_tls_none) == PROVEN_OK, "a server configuration that resumes nothing", "");
    PROVEN_TEST_ASSERT(proven_tls_config_create(&so, &g_server_tls) == PROVEN_OK && proven_tls_config_create(&so, &g_server_tls_new) == PROVEN_OK &&
                       proven_tls_config_create(&c1, &g_client_h1) == PROVEN_OK && proven_tls_config_create(&c1_none, &g_client_h1_none) == PROVEN_OK &&
                       proven_tls_config_create(&c2, &g_client_h2) == PROVEN_OK && proven_tls_config_create(&c3, &g_client_h3) == PROVEN_OK, "TLS configurations", "");

    proven_u16 port = 0;
    proven_http_event_server_t *server = server_on(g_server_tls, 0, &port);

    PROVEN_TEST_SECTION("HTTP/1.1", "A connection a request: the second resumes; with no_resumption none does.", "Check cl_conn_open, cl_conn_keep and cl_conn_close.");
    {
        proven_http_event_client_t *client = client_of(g_client_h1, false, false, true);
        call_t a = ask(client, port, false), b = ask(client, port, false), d = ask(client, port, false);
        PROVEN_TEST_ASSERT(a.protocol == 1 && !a.resumed, "the first connection makes a full handshake", "");
        PROVEN_TEST_ASSERT(b.resumed && d.resumed, "the next ones, each a connection of its own, resume", "");
        proven_http_event_client_destroy(client);
        client = client_of(g_client_h1_none, false, false, true);
        a = ask(client, port, false); b = ask(client, port, false);
        PROVEN_TEST_ASSERT(!a.resumed && !b.resumed, "with no_resumption no connection resumes", "");
        proven_http_event_client_destroy(client);
    }

    PROVEN_TEST_SECTION("HTTP/2", "A connection an origin: when it has gone idle and closed, the next resumes.", "Check c_open and c_close_socket in http_event_client_h2.c.");
    {
        proven_http_event_client_t *client = client_of(g_client_h2, true, false, false);
        call_t a = ask(client, port, false);
        spin(400);                                         /* idle past 100 ms: closed */
        call_t b = ask(client, port, false);
        PROVEN_TEST_ASSERT(a.protocol == 2 && b.protocol == 2 && !a.resumed && b.resumed, "the second HTTP/2 connection resumes", "");
        proven_http_event_client_destroy(client);
    }

#ifndef PROVEN_NO_HTTP3
    PROVEN_TEST_SECTION("HTTP/3", "A QUIC connection resumes what the last QUIC connection learned - never a session learned over TCP.", "Check c_open, r_finish and c_kill in http_event_client_h3.c.");
    {
        proven_http_event_client_t *client = client_of(g_client_h3, true, true, false);
        call_t a = ask(client, port, false);              /* over TCP: a TCP session is learned */
        spin(400);
        call_t b = ask(client, port, true);               /* the first QUIC connection */
        spin(400);
        call_t d = ask(client, port, true);
        PROVEN_TEST_ASSERT(a.protocol == 2 && b.protocol == 3 && d.protocol == 3, "HTTP/2, then HTTP/3 twice", "");
        PROVEN_TEST_ASSERT(!b.resumed, "the first QUIC connection does not offer TCP's session", "");
        PROVEN_TEST_ASSERT(d.resumed, "the second QUIC connection resumes", "");
        proven_http_event_client_destroy(client);
    }
#endif

    PROVEN_TEST_SECTION("a server that changed", "The origin's server is replaced by one with other ticket keys: the client offers its ticket, which is refused, and is answered by a full handshake.", "Check that a refused ticket falls back inside the engine.");
    {
        proven_http_event_client_t *client = client_of(g_client_h1, false, false, true);
        call_t a = ask(client, port, false), b = ask(client, port, false);
        PROVEN_TEST_ASSERT(!a.resumed && b.resumed, "resuming against the first server", "");
        proven_http_event_server_destroy(server);
        spin(100);
        proven_u16 again = 0;
        server = server_on(g_server_tls_new, port, &again);
        PROVEN_TEST_ASSERT(again == port, "a new server on the same port", "");
        call_t d = ask(client, port, false), e = ask(client, port, false);
        PROVEN_TEST_ASSERT(!d.resumed && e.resumed, "the old ticket is refused and answered all the same; the next connection resumes the new server's", "");
        /* Replaced again, by a server that resumes nothing and issues no ticket: the ticket the
         * client holds is refused and nothing takes its place - it is dropped, not kept and
         * offered for ever. */
        proven_http_event_server_destroy(server);
        spin(100);
        server = server_on(g_server_tls_none, port, &again);
        PROVEN_TEST_ASSERT(again == port, "a server that resumes nothing, on the same port", "");
        call_t f = ask(client, port, false);
        PROVEN_TEST_ASSERT(!f.resumed, "the ticket is refused", "");
        proven_tls_session_t held;
        (void)proven_http_event_client_session_start_(client, sv("127.0.0.1"), port, false, &held);
        bool none = true;
        for (proven_size_t i = 0; i < sizeof held.opaque; ++i) if (held.opaque[i]) none = false;
        PROVEN_TEST_ASSERT(none, "and the dead ticket is dropped from the cache", "");
        proven_http_event_client_destroy(client);
        proven_http_event_server_destroy(server);
        spin(100);
        server = server_on(g_server_tls, port, &again);
    }

    PROVEN_TEST_SECTION("connections at once", "Three connections to one origin made together each start from the same ticket and each store what they learn; the next connection still resumes.", "Check the mark in proven_http_event_client_session_keep_: a connection stores what it learned, never what it merely carried.");
    {
        proven_http_event_client_t *client = client_of(g_client_h1, false, false, true);
        call_t a = ask(client, port, false);
        PROVEN_TEST_ASSERT(!a.resumed, "a ticket is learned", "");
        static char url[64];
        int n = snprintf(url, sizeof url, "https://127.0.0.1:%u/", (unsigned)port);
        call_t at_once[3] = { 0 };
        for (int i = 0; i < 3; ++i) {
            proven_http_event_request_options_t o = { .method = sv("GET"), .url = { (const proven_byte_t *)url, (proven_size_t)n },
                .on = { .on_response = on_response, .on_body = on_body, .on_done = on_done }, .ctx = &at_once[i] };
            PROVEN_TEST_ASSERT(proven_http_event_client_start(client, &o, &at_once[i].request) == PROVEN_OK, "three requests started together", "");
        }
        for (int i = 0; i < 3; ++i) spin_until(&at_once[i].done, 5000);
        PROVEN_TEST_ASSERT(at_once[0].resumed && at_once[1].resumed && at_once[2].resumed, "each, a connection of its own, resumes the one ticket", "");
        call_t b = ask(client, port, false);
        PROVEN_TEST_ASSERT(b.resumed, "and the next connection still resumes", "");
        proven_http_event_client_destroy(client);
    }

    PROVEN_TEST_SECTION("the cache's bound", "64 origins per transport; the 65th gives up the least recently used, and the other transport's are untouched.", "Check the slot choice in proven_http_event_client_session_keep_.");
    {
        proven_http_event_client_t *client = client_of(g_client_h1, false, false, false);
        proven_tls_session_t s, out;
        char host[32];
        for (int i = 0; i <= 64; ++i) {
            memset(&s, i + 1, sizeof s);
            int n = snprintf(host, sizeof host, "host%d.test", i);
            /* host0, the oldest, is used again before the 65th comes: host1 is then the least recently used. */
            if (i == 64) (void)proven_http_event_client_session_start_(client, sv("host0.test"), 443, false, &out);
            proven_http_event_client_session_keep_(client, (proven_u8str_view_t){ (const proven_byte_t *)host, (proven_size_t)n }, 443, false, &s, 0, true, false);
            if (i == 0) proven_http_event_client_session_keep_(client, (proven_u8str_view_t){ (const proven_byte_t *)host, (proven_size_t)n }, 443, true, &s, 0, true, false);
        }
        proven_http_event_client_session_start_(client, sv("host1.test"), 443, false, &out);
        bool zero = true;
        for (proven_size_t i = 0; i < sizeof out.opaque; ++i) if (out.opaque[i]) zero = false;
        PROVEN_TEST_ASSERT(zero, "the least recently used of TCP's 64 (host1, host0 having been used again) was given up for the 65th", "");
        proven_http_event_client_session_start_(client, sv("host0.test"), 443, false, &out);
        PROVEN_TEST_ASSERT(out.opaque[0] == 1, "the one used again is kept", "");
        proven_http_event_client_session_start_(client, sv("host64.test"), 443, false, &out);
        PROVEN_TEST_ASSERT(out.opaque[0] == 65 && out.opaque[sizeof out.opaque - 1] == 65, "the newest is there", "");
        proven_http_event_client_session_start_(client, sv("HOST2.test"), 443, false, &out);
        PROVEN_TEST_ASSERT(out.opaque[0] == 3, "a host name is matched without regard to case", "");
        proven_http_event_client_session_start_(client, sv("host0.test"), 443, true, &out);
        PROVEN_TEST_ASSERT(out.opaque[0] == 1, "QUIC's session for the same origin is kept apart, untouched", "");
        proven_http_event_client_session_start_(client, sv("host5.test"), 443, true, &out);
        PROVEN_TEST_ASSERT(out.opaque[0] == 0, "and an origin known only over TCP gives QUIC nothing", "");
        proven_http_event_client_session_start_(client, sv("host2.test"), 8443, false, &out);
        PROVEN_TEST_ASSERT(out.opaque[0] == 0, "another port is another origin", "");
        proven_http_event_client_destroy(client);
    }

    proven_http_event_server_destroy(server);
    spin(50);
    proven_tls_config_destroy(g_server_tls); proven_tls_config_destroy(g_server_tls_new); proven_tls_config_destroy(g_server_tls_none);
    proven_tls_config_destroy(g_client_h1); proven_tls_config_destroy(g_client_h1_none);
    proven_tls_config_destroy(g_client_h2); proven_tls_config_destroy(g_client_h3);
    proven_cert_store_destroy(anchors);
    proven_loop_destroy(g_loop);
    PROVEN_TEST_PASS("a second connection resumes, where it may");
    return 0;
}
