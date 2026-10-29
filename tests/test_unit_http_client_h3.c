#include "proven.h"
#include "proven_test.h"
#include "test_unit_tls_pki.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The blocking client speaking HTTP/3 (RFC-0034), against the event-driven server over real UDP
 * on loopback. The servers run on a loop of their own, on a thread; the client blocks in the
 * test's thread, running the loop of its own while it waits. A second server speaks only TCP, for
 * a redirect that leaves the origin.
 */

#ifndef PROVEN_NO_HTTP3

static proven_u8str_view_t sv(const char *s) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }
static proven_mem_view_t mv(const char *s) { return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }
static proven_byte_t pattern(proven_size_t i) { return (proven_byte_t)('a' + (i * 7 + i / 251) % 26); }

static proven_allocator_t g_heap;
static proven_loop_t *g_loop;
static proven_http_event_server_t *g_server, *g_tcp, *g_short;
static proven_job_sys_t *g_threads;
static proven_job_group_t g_running;
static proven_u16 g_port, g_tcp_port, g_short_port;
static atomic_int g_ran;
static atomic_ulong g_conn_count;
static proven_byte_t g_big[300000];
static char g_text[20001];

typedef struct { proven_http_stream_t *stream; proven_byte_t *echo; proven_size_t got, cap; bool echoing; } app_t;

static void on_request(void *ctx, proven_http_stream_t *s, const proven_http_request_t *req) {
    (void)ctx;
    app_t *a = calloc(1, sizeof *a);
    a->stream = s;
    proven_http_stream_set_user(s, a);
    const proven_u8str_view_t t = req->target;
    char path[64] = { 0 };
    memcpy(path, t.ptr, t.size < 63 ? t.size : 63);
    proven_u8str_view_t v = { 0 };
    if (strcmp(path, "/hello") == 0) (void)proven_http_stream_respond(s, 200, NULL, 0, mv("hello"));
    else if (strcmp(path, "/proto") == 0) (void)proven_http_stream_respond(s, 200, NULL, 0, proven_http_stream_protocol(s) == 3 ? mv("3") : mv("tcp"));
    else if (strcmp(path, "/echo") == 0) a->echoing = true;
    else if (strcmp(path, "/big") == 0) (void)proven_http_stream_respond(s, 200, NULL, 0, (proven_mem_view_t){ g_big, sizeof g_big });
    else if (strcmp(path, "/zip") == 0) { proven_http_stream_compress(s); (void)proven_http_stream_respond(s, 200, NULL, 0, mv(g_text)); }
    else if (strcmp(path, "/redir") == 0) { const proven_http_header_t h = { sv("Location"), sv("/hello") }; (void)proven_http_stream_respond(s, 302, &h, 1, mv("")); }
    else if (strcmp(path, "/redir-other") == 0) {
        static char loc[80];
        snprintf(loc, sizeof loc, "https://127.0.0.1:%u/proto", (unsigned)g_tcp_port);
        const proven_http_header_t h = { sv("Location"), sv(loc) };
        (void)proven_http_stream_respond(s, 302, &h, 1, mv(""));
    } else if (strcmp(path, "/auth") == 0) {
        if (proven_http_header_find(req->headers, req->header_count, sv("Authorization"), &v) && v.size == 10 && memcmp(v.ptr, "Basic dTpw", 10) == 0)
            (void)proven_http_stream_respond(s, 200, NULL, 0, mv("in"));
        else { const proven_http_header_t h = { sv("WWW-Authenticate"), sv("Basic realm=\"t\"") }; (void)proven_http_stream_respond(s, 401, &h, 1, mv("")); }
    } else if (strcmp(path, "/set") == 0) { const proven_http_header_t h = { sv("Set-Cookie"), sv("k=v; Path=/") }; (void)proven_http_stream_respond(s, 200, &h, 1, mv("")); }
    else if (strcmp(path, "/cookie") == 0) {
        if (proven_http_header_find(req->headers, req->header_count, sv("Cookie"), &v)) (void)proven_http_stream_respond(s, 200, NULL, 0, (proven_mem_view_t){ v.ptr, v.size });
        else (void)proven_http_stream_respond(s, 200, NULL, 0, mv("none"));
    } else if (strcmp(path, "/never") == 0) { /* no answer */ }
    else (void)proven_http_stream_respond(s, 404, NULL, 0, mv(""));
}
static void on_body(void *ctx, proven_http_stream_t *s, proven_mem_view_t piece, bool last) {
    (void)ctx;
    app_t *a = proven_http_stream_user(s);
    if (!a || !a->echoing) return;
    if (a->got + piece.size > a->cap) { a->cap = (a->got + piece.size) * 2 + 1024; a->echo = realloc(a->echo, a->cap); }
    if (piece.size) memcpy(a->echo + a->got, piece.ptr, piece.size);
    a->got += piece.size;
    if (last) (void)proven_http_stream_respond(s, 200, NULL, 0, (proven_mem_view_t){ a->echo, a->got });
}
static void on_done(void *ctx, proven_http_stream_t *s, proven_err_t why) {
    (void)ctx; (void)why;
    app_t *a = proven_http_stream_user(s);
    if (a) { free(a->echo); free(a); }
}

static void run_loop(void *arg) { (void)arg; (void)proven_loop_run(g_loop); }
static void do_count(void *ctx) { (void)ctx; atomic_store(&g_conn_count, (unsigned long)proven_http_event_server_connections(g_server)); atomic_fetch_add(&g_ran, 1); }
static void do_destroy(void *ctx) { (void)ctx; proven_http_event_server_destroy(g_server); proven_http_event_server_destroy(g_tcp); proven_http_event_server_destroy(g_short); g_server = g_tcp = g_short = NULL; atomic_fetch_add(&g_ran, 1); }
static void on_loop(proven_loop_fn fn) {
    int before = atomic_load(&g_ran);
    PROVEN_TEST_ASSERT(proven_loop_post(g_loop, fn, NULL) == PROVEN_OK, "posted to the loop", "");
    for (int i = 0; i < 4000 && atomic_load(&g_ran) == before; ++i) proven_time_sleep(1);
}
static unsigned long connections(void) { on_loop(do_count); return atomic_load(&g_conn_count); }

static bool body_is(proven_http_client_response_t *r, const proven_byte_t *want, proven_size_t n) {
    proven_u8str_t got = { 0 };
    const proven_err_t e = proven_http_client_read_all(r, g_heap, &got, 1 << 22);
    const proven_u8str_view_t v = proven_u8str_as_view(&got);
    const bool ok = e == PROVEN_OK && v.size == n && (n == 0 || memcmp(v.ptr, want, n) == 0);
    proven_u8str_destroy(g_heap, &got);
    return ok;
}

int main(void) {
    PROVEN_TEST_SUITE("the blocking HTTP client, speaking HTTP/3",
        "The blocking client asked for HTTP/3 (RFC-0034) against the event-driven server over real UDP: what it is refused for, requests and bodies both ways, redirects within the origin and out of it, a challenge, cookies, decoding, a kept connection and one remade after the server let it go, io_timeout_ms.",
        "Inspect the HTTP/3 part of src/proven/http_client.c: cl_exchange_h3 for the request and head, cl_h3_next for the body, the use_h3 choice in proven_http_client_send.");

    g_heap = proven_heap_allocator();
    PROVEN_TEST_ASSERT(tp_build(), "the test's certificates and keys are issued", "");
    for (proven_size_t i = 0; i < sizeof g_big; ++i) g_big[i] = pattern(i);
    for (int i = 0; i < 20000; ++i) g_text[i] = "the blocking client, over HTTP/3. "[i % 34];
    PROVEN_TEST_ASSERT(proven_job_system_init(g_heap, 1, 4, &g_threads) == PROVEN_OK, "a thread for the servers' loop", "");
    proven_cert_store_t *anchors = NULL;
    PROVEN_TEST_ASSERT(proven_cert_store_create(g_heap, &anchors) == PROVEN_OK &&
                       proven_cert_store_add_pem(anchors, (proven_mem_view_t){ (const proven_byte_t *)TP_CA, strlen(TP_CA) }, NULL) == PROVEN_OK, "anchors", "");
    static const proven_u8str_view_t all[] = { { (const proven_byte_t *)"h2", 2 }, { (const proven_byte_t *)"http/1.1", 8 }, { (const proven_byte_t *)"h3", 2 } };
    static const proven_u8str_view_t h3[] = { { (const proven_byte_t *)"h3", 2 } };
    proven_tls_options_t so = { .alloc = g_heap, .certificate_pem = { (const proven_byte_t *)TP_SERVER_P256, strlen(TP_SERVER_P256) },
                                .private_key_pem = { (const proven_byte_t *)TP_SERVER_P256_KEY, strlen(TP_SERVER_P256_KEY) }, .alpn = all, .alpn_count = 3 };
    proven_tls_options_t so_tcp = so; so_tcp.alpn_count = 2;
    proven_tls_options_t co = { .alloc = g_heap, .anchors = anchors, .alpn = h3, .alpn_count = 1 };
    proven_tls_options_t co_tcp = { .alloc = g_heap, .anchors = anchors };
    proven_tls_config_t *server_tls = NULL, *tcp_tls = NULL, *client_tls = NULL, *client_tcp = NULL;
    PROVEN_TEST_ASSERT(proven_tls_config_create(&so, &server_tls) == PROVEN_OK && proven_tls_config_create(&so_tcp, &tcp_tls) == PROVEN_OK &&
                       proven_tls_config_create(&co, &client_tls) == PROVEN_OK && proven_tls_config_create(&co_tcp, &client_tcp) == PROVEN_OK, "TLS configurations", "");

    PROVEN_TEST_ASSERT(proven_loop_create(g_heap, &g_loop) == PROVEN_OK, "the servers' loop", "");
    proven_http_event_server_config_t scfg = { .on = { on_request, on_body, NULL, on_done }, .tls = server_tls, .http2 = true, .http3 = true, .idle_timeout_ms = 60000 };
    proven_http_event_server_config_t tcfg = { .on = { on_request, on_body, NULL, on_done }, .tls = tcp_tls, .http2 = true };
    proven_net_addr_t at;
    PROVEN_TEST_ASSERT(proven_http_event_server_create(g_loop, &scfg, &g_server) == PROVEN_OK &&
                       proven_http_event_server_listen(g_server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "a server that speaks HTTP/3", "");
    g_port = at.port;
    PROVEN_TEST_ASSERT(proven_http_event_server_create(g_loop, &tcfg, &g_tcp) == PROVEN_OK &&
                       proven_http_event_server_listen(g_tcp, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "and one over TCP only", "");
    g_tcp_port = at.port;
    proven_http_event_server_config_t shcfg = scfg;
    shcfg.idle_timeout_ms = 300;
    PROVEN_TEST_ASSERT(proven_http_event_server_create(g_loop, &shcfg, &g_short) == PROVEN_OK &&
                       proven_http_event_server_listen(g_short, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "and one that lets an idle connection go after 300 ms", "");
    g_short_port = at.port;
    proven_job_group_init(&g_running);
    PROVEN_TEST_ASSERT(proven_job_group_submit(g_threads, &g_running, run_loop, NULL) == PROVEN_OK, "their loop's thread", "");
    char url[128];
#define URL(path) (snprintf(url, sizeof url, "https://127.0.0.1:%u%s", (unsigned)g_port, (path)), sv(url))

    PROVEN_TEST_SECTION("what is refused", "http3 without a TLS configuration that offers h3, http3_fallback without http3; a request asked over HTTP/3 on a client without it, to an http URL, with upgrade, through a proxy.", "Check proven_http_client_create and the first checks of proven_http_client_send.");
    proven_http_client_t *cl = NULL;
    {
        proven_http_client_config_t c = { .alloc = g_heap, .http3 = true };
        PROVEN_TEST_ASSERT(proven_http_client_create(&c, &cl) == PROVEN_ERR_INVALID_ARG && !cl, "http3 without tls", "");
        c.tls = client_tcp;
        PROVEN_TEST_ASSERT(proven_http_client_create(&c, &cl) == PROVEN_ERR_INVALID_ARG && !cl, "over a TLS configuration that does not offer h3", "");
        proven_http_client_config_t f = { .alloc = g_heap, .http3_fallback = true };
        PROVEN_TEST_ASSERT(proven_http_client_create(&f, &cl) == PROVEN_ERR_INVALID_ARG && !cl, "http3_fallback without http3", "");
        proven_http_client_config_t plain = { .alloc = g_heap, .tls_wrap = proven_tls_http_wrap, .tls_ctx = client_tcp };
        PROVEN_TEST_ASSERT(proven_http_client_create(&plain, &cl) == PROVEN_OK, "a client without http3", "");
        proven_http_client_response_t r;
        proven_http_client_request_t q = { .url = URL("/hello"), .http3 = true };
        PROVEN_TEST_ASSERT(proven_http_client_send(cl, &q, &r) == PROVEN_ERR_INVALID_ARG, "is refused a request that asks for HTTP/3", "");
        proven_http_client_finish(&r);
        proven_http_client_destroy(cl);
        proven_http_client_config_t h = { .alloc = g_heap, .http3 = true, .tls = client_tls, .proxy = sv("http://127.0.0.1:9") };
        PROVEN_TEST_ASSERT(proven_http_client_create(&h, &cl) == PROVEN_OK, "a client with http3 and a proxy", "");
        PROVEN_TEST_ASSERT(proven_http_client_send(cl, &q, &r) == PROVEN_ERR_UNSUPPORTED, "an HTTP/3 request through a proxy: PROVEN_ERR_UNSUPPORTED (no CONNECT-UDP)", "");
        proven_http_client_finish(&r);
        snprintf(url, sizeof url, "http://127.0.0.1:%u/hello", (unsigned)g_port);
        q.url = sv(url);
        PROVEN_TEST_ASSERT(proven_http_client_send(cl, &q, &r) == PROVEN_ERR_INVALID_ARG, "an http URL asked over HTTP/3: PROVEN_ERR_INVALID_ARG", "");
        proven_http_client_finish(&r);
        q.url = URL("/hello"); q.upgrade = sv("websocket");
        PROVEN_TEST_ASSERT(proven_http_client_send(cl, &q, &r) == PROVEN_ERR_UNSUPPORTED, "upgrade over HTTP/3: PROVEN_ERR_UNSUPPORTED", "");
        proven_http_client_finish(&r);
        proven_http_client_destroy(cl);
    }

    static proven_http_cookie_jar_t jar_store;
    proven_http_cookie_jar_t *jar = &jar_store;
    PROVEN_TEST_ASSERT(proven_http_cookie_jar_init(jar, g_heap, 16) == PROVEN_OK, "a cookie jar", "");
    proven_http_client_config_t cc = { .alloc = g_heap, .http3 = true, .tls = client_tls, .tls_wrap = proven_tls_http_wrap, .tls_ctx = client_tcp,
                                       .max_redirects = 3, .username = sv("u"), .password = sv("p"), .cookies = jar, .decompress = true, .io_timeout_ms = 5000 };
    PROVEN_TEST_ASSERT(proven_http_client_create(&cc, &cl) == PROVEN_OK, "a client that speaks HTTP/3 when asked", "");

    PROVEN_TEST_SECTION("requests and bodies", "A GET over HTTP/3; a body from memory and one from a stream, echoed; 300,000 bytes read a kilobyte at a time.", "Check cl_exchange_h3, cl_h3_pump and cl_h3_next.");
    {
        proven_http_client_response_t r;
        proven_http_client_request_t q = { .url = URL("/proto"), .http3 = true };
        PROVEN_TEST_ASSERT(proven_http_client_send(cl, &q, &r) == PROVEN_OK && r.status == 200 && body_is(&r, (const proven_byte_t *)"3", 1), "a GET, answered over HTTP/3", "");
        proven_http_client_finish(&r);
        static proven_byte_t up[100000];
        for (proven_size_t i = 0; i < sizeof up; ++i) up[i] = pattern(i * 3);
        proven_http_client_request_t post = { .method = sv("POST"), .url = URL("/echo"), .body = { up, sizeof up }, .http3 = true };
        PROVEN_TEST_ASSERT(proven_http_client_send(cl, &post, &r) == PROVEN_OK && r.status == 200 && body_is(&r, up, sizeof up), "100,000 bytes from memory, echoed whole", "");
        proven_http_client_finish(&r);
        proven_reader_view_t rv;
        proven_http_client_request_t sp = { .method = sv("POST"), .url = URL("/echo"), .body_stream = proven_reader_from_view(&rv, (proven_u8str_view_t){ up, 50000 }), .http3 = true };
        PROVEN_TEST_ASSERT(proven_http_client_send(cl, &sp, &r) == PROVEN_OK && r.status == 200 && body_is(&r, up, 50000), "50,000 bytes from a stream, echoed whole", "");
        proven_http_client_finish(&r);
        q.url = URL("/big");
        PROVEN_TEST_ASSERT(proven_http_client_send(cl, &q, &r) == PROVEN_OK && r.status == 200, "a download of 300,000 bytes", "");
        static proven_byte_t piece[1024];
        proven_size_t got = 0;
        bool intact = true;
        for (;;) {
            proven_result_size_t rd = proven_http_client_read(&r, (proven_mem_mut_t){ piece, sizeof piece });
            if (rd.err != PROVEN_OK) { intact = intact && rd.err == PROVEN_ERR_EOF; break; }
            for (proven_size_t i = 0; i < rd.value; ++i) if (piece[i] != g_big[got + i]) intact = false;
            got += rd.value;
        }
        PROVEN_TEST_ASSERT(intact && got == sizeof g_big, "read a kilobyte at a time, whole and in order, then PROVEN_ERR_EOF", "");
        proven_http_client_finish(&r);
    }

    PROVEN_TEST_SECTION("what is decided above the exchange", "A redirect within the origin stays on HTTP/3; one to another origin goes there over TCP; a Basic challenge answered; a cookie set and sent back; a gzip body decoded.", "Check the use_h3 choice and the redirect branch of proven_http_client_send.");
    {
        proven_http_client_response_t r;
        proven_http_client_request_t q = { .url = URL("/redir"), .http3 = true };
        PROVEN_TEST_ASSERT(proven_http_client_send(cl, &q, &r) == PROVEN_OK && r.status == 200 && r.redirects == 1 && body_is(&r, (const proven_byte_t *)"hello", 5),
            "a redirect within the origin, followed over HTTP/3", "");
        proven_http_client_finish(&r);
        q.url = URL("/redir-other");
        PROVEN_TEST_ASSERT(proven_http_client_send(cl, &q, &r) == PROVEN_OK && r.status == 200 && r.redirects == 1 && body_is(&r, (const proven_byte_t *)"tcp", 3),
            "a redirect to another origin, followed there over TCP (HTTP/1.1, through tls_wrap)", "");
        proven_http_client_finish(&r);
        q.url = URL("/auth");
        PROVEN_TEST_ASSERT(proven_http_client_send(cl, &q, &r) == PROVEN_OK && r.status == 200 && body_is(&r, (const proven_byte_t *)"in", 2), "a Basic challenge, answered", "");
        proven_http_client_finish(&r);
        q.url = URL("/set");
        PROVEN_TEST_ASSERT(proven_http_client_send(cl, &q, &r) == PROVEN_OK && r.status == 200 && body_is(&r, NULL, 0), "a cookie set", "");
        proven_http_client_finish(&r);
        q.url = URL("/cookie");
        PROVEN_TEST_ASSERT(proven_http_client_send(cl, &q, &r) == PROVEN_OK && r.status == 200 && body_is(&r, (const proven_byte_t *)"k=v", 3), "and sent back", "");
        proven_http_client_finish(&r);
        q.url = URL("/zip");
        proven_u8str_view_t ce = { 0 };
        PROVEN_TEST_ASSERT(proven_http_client_send(cl, &q, &r) == PROVEN_OK && r.status == 200 && proven_http_header_find(r.headers, r.header_count, sv("Content-Encoding"), &ce) &&
                           body_is(&r, (const proven_byte_t *)g_text, 20000), "a compressed body, decoded", "");
        proven_http_client_finish(&r);
    }

    PROVEN_TEST_SECTION("the connection, and time", "Every request above on one QUIC connection; a connection the server let go between calls made again; io_timeout_ms for an answer that does not come.", "Check cl_h3_ready, cl_h3_wait and the event client's retry_unsent underneath.");
    {
        PROVEN_TEST_ASSERT(connections() == 1, "one QUIC connection carried all of them", "");
        /* Kept between calls, with nobody to answer its packets: the server lets it go. */
        proven_http_client_response_t r0;
        snprintf(url, sizeof url, "https://127.0.0.1:%u/hello", (unsigned)g_short_port);
        proven_http_client_request_t q0 = { .url = sv(url), .http3 = true };
        PROVEN_TEST_ASSERT(proven_http_client_send(cl, &q0, &r0) == PROVEN_OK && r0.status == 200 && body_is(&r0, (const proven_byte_t *)"hello", 5), "a request to a server with a short idle time", "");
        proven_http_client_finish(&r0);
        proven_time_sleep(1500);
        PROVEN_TEST_ASSERT(proven_http_client_send(cl, &q0, &r0) == PROVEN_OK && r0.status == 200 && body_is(&r0, (const proven_byte_t *)"hello", 5),
            "a second one after the server let the connection go: answered, on a new connection", "");
        proven_http_client_finish(&r0);
        proven_http_client_destroy(cl);
        cc.io_timeout_ms = 300;
        cc.connect_timeout_ms = 1000;
        PROVEN_TEST_ASSERT(proven_http_client_create(&cc, &cl) == PROVEN_OK, "a client whose io_timeout_ms is 300 and connect_timeout_ms 1,000", "");
        proven_http_client_response_t r;
        proven_http_client_request_t q = { .url = URL("/never"), .http3 = true };
        const proven_time_t t0 = proven_time_monotonic_now();
        PROVEN_TEST_ASSERT(proven_http_client_send(cl, &q, &r) == PROVEN_ERR_TIMEOUT, "an answer that does not come: PROVEN_ERR_TIMEOUT", "");
        const proven_time_t took = (proven_time_monotonic_now() - t0) / 1000000;
        PROVEN_TEST_ASSERT(took >= 300 && took < 1000 + 300 + 2000, "after io_timeout_ms, within connect_timeout_ms and io_timeout_ms (the head's wait covers a connection to be made)", "");
        proven_http_client_finish(&r);
        proven_http_client_destroy(cl);
    }

    proven_http_cookie_jar_destroy(jar);
    on_loop(do_destroy);
    proven_loop_stop(g_loop);
    proven_job_group_wait(g_threads, &g_running);
    proven_loop_destroy(g_loop);
    proven_job_system_close(g_threads);
    proven_job_system_destroy(g_threads);
    proven_tls_config_destroy(server_tls); proven_tls_config_destroy(tcp_tls); proven_tls_config_destroy(client_tls); proven_tls_config_destroy(client_tcp);
    proven_cert_store_destroy(anchors);
    PROVEN_TEST_PASS("the blocking client speaks HTTP/3 when asked, with everything above the exchange as for HTTP/1.1.");
    return 0;
}

#else

int main(void) {
    PROVEN_TEST_SUITE("the blocking HTTP client, speaking HTTP/3", "Built with PROVEN_NO_HTTP3: nothing to test.", "");
    PROVEN_TEST_PASS("skipped: PROVEN_NO_HTTP3");
    return 0;
}

#endif
