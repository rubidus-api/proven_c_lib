#include "proven.h"
#include "proven_test.h"
#include "test_unit_tls_pki.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/*
 * TLS over real sockets: the transport wrapper by itself, then HTTPS and WebSocket-over-TLS
 * through the library's own HTTP client and server on the loopback interface.
 *
 * The server's loop runs on another thread; its handlers run there too, or on workers when a
 * job system is given - the cases run both ways. The certificates are made at start by
 * test_unit_tls_pki.h and name 127.0.0.1. The wall clock is used here (they are valid 2026 to
 * 2036).
 */

static proven_mem_view_t pem(const char *s) { return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }
static proven_u8str_view_t sv(const char *s) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }
static proven_mem_view_t mv(const char *s) { return pem(s); }

static proven_allocator_t g_heap;
static proven_cert_store_t *g_anchors, *g_other;
static proven_tls_config_t *g_client_tls, *g_server_tls, *g_stranger_tls;
static proven_job_sys_t *g_loop, *g_workers;
static proven_job_group_t g_group;

// -----------------------------------------------------------------------------
// The transport wrapper against itself
// -----------------------------------------------------------------------------

typedef struct {
    proven_net_listener_t listener;
    proven_u16 port;
    int mode;                       /* 0: echo until the peer closes; 1: accept and never speak */
    proven_err_t handshake, last_read;
    proven_size_t echoed;
    bool had_conn;
} peer_t;

static void peer_job(void *arg) {
    peer_t *p = arg;
    proven_net_conn_t sock;
    if (proven_net_accept(&p->listener, proven_net_deadline_in(10000), &sock, NULL) != PROVEN_OK) return;
    if (p->mode == 1) { proven_time_sleep(700); (void)proven_net_close(&sock); return; }
    proven_transport_t tls;
    p->handshake = proven_tls_transport_server(proven_net_conn_transport(&sock), g_server_tls, proven_net_deadline_in(10000), &tls);
    if (p->handshake != PROVEN_OK) { (void)proven_net_close(&sock); return; }
    p->had_conn = proven_tls_transport_conn(tls) != NULL && proven_tls_is_established(proven_tls_transport_conn(tls));
    static proven_byte_t buf[20000];
    for (;;) {
        proven_result_size_t r = proven_transport_read(tls, (proven_mem_mut_t){ .ptr = buf, .size = sizeof buf }, proven_net_deadline_in(10000));
        if (r.err != PROVEN_OK) { p->last_read = r.err; break; }
        if (proven_transport_write_all(tls, (proven_mem_view_t){ .ptr = buf, .size = r.value }, proven_net_deadline_in(10000)).err != PROVEN_OK) break;
        p->echoed += r.value;
    }
    (void)proven_transport_close(tls);
}

static void peer_start(peer_t *p, int mode) {
    memset(p, 0, sizeof *p);
    p->mode = mode;
    proven_net_addr_t at;
    PROVEN_TEST_ASSERT(proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 4, &p->listener, &at) == PROVEN_OK, "a peer listens", "");
    p->port = at.port;
    proven_job_group_init(&g_group);
    PROVEN_TEST_ASSERT(proven_job_group_submit(g_workers, &g_group, peer_job, p) == PROVEN_OK, "its thread starts", "");
}

static void peer_finish(peer_t *p) {
    proven_job_group_wait(g_workers, &g_group);
    (void)proven_net_listener_close(&p->listener);
}

static void transport_cases(void) {
    peer_t peer;
    static proven_byte_t data[150000], back[150000];
    for (proven_size_t i = 0; i < sizeof data; ++i) data[i] = (proven_byte_t)(i * 7 + 1);

    PROVEN_TEST_SECTION("the transport wrapper",
        "A handshake over a socket, data in both directions, a proper close seen as an end of stream and a cut connection seen as a reset.",
        "Check src/proven/tls_transport.c.");
    peer_start(&peer, 0);
    proven_net_conn_t sock;
    proven_transport_t tls = { 0 };
    PROVEN_TEST_ASSERT(proven_net_connect(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, peer.port), proven_net_deadline_in(5000), &sock) == PROVEN_OK, "connect", "");
    PROVEN_TEST_ASSERT(proven_tls_transport_conn(proven_net_conn_transport(&sock)) == NULL, "a plain transport has no TLS connection inside", "");
    PROVEN_TEST_ASSERT(proven_tls_transport_client(proven_net_conn_transport(&sock), g_client_tls, sv("127.0.0.1"), NULL, proven_net_deadline_in(10000), &tls) == PROVEN_OK,
        "the client handshake completes, the server verified by its IP address", "");
    proven_tls_conn_t *conn = proven_tls_transport_conn(tls);
    PROVEN_TEST_ASSERT(conn != NULL && proven_tls_is_established(conn) && proven_tls_cipher_suite(conn) != 0, "and the transport gives its connection for questions", "");
    static const proven_size_t sizes[] = { 1, 100, 16384, 16385, 150000 };
    bool all = true;
    for (proven_size_t k = 0; k < sizeof sizes / sizeof sizes[0]; ++k) {
        proven_result_size_t w = proven_transport_write_all(tls, (proven_mem_view_t){ .ptr = data, .size = sizes[k] }, proven_net_deadline_in(10000));
        proven_size_t got = 0;
        while (w.err == PROVEN_OK && got < sizes[k]) {
            proven_result_size_t r = proven_transport_read(tls, (proven_mem_mut_t){ .ptr = back + got, .size = sizeof back - got }, proven_net_deadline_in(10000));
            if (r.err != PROVEN_OK) break;
            got += r.value;
        }
        all = all && w.err == PROVEN_OK && got == sizes[k] && memcmp(data, back, got) == 0;
    }
    PROVEN_TEST_ASSERT(all, "1 byte to 150,000 bytes written and echoed back intact", "");
    PROVEN_TEST_ASSERT(proven_transport_read(tls, (proven_mem_mut_t){ .ptr = back, .size = 10 }, proven_net_deadline_in(150)).err == PROVEN_ERR_TIMEOUT,
        "a read with nothing to read ends at its deadline", "");
    PROVEN_TEST_ASSERT(proven_transport_close(tls) == PROVEN_OK, "close", "");
    peer_finish(&peer);
    PROVEN_TEST_ASSERT(peer.handshake == PROVEN_OK && peer.had_conn && peer.echoed == 1 + 100 + 16384 + 16385 + 150000 && peer.last_read == PROVEN_ERR_EOF,
        "the server side saw every byte and then a proper end of stream", "");

    peer_start(&peer, 0);
    PROVEN_TEST_ASSERT(proven_net_connect(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, peer.port), proven_net_deadline_in(5000), &sock) == PROVEN_OK &&
                       proven_tls_transport_client(proven_net_conn_transport(&sock), g_client_tls, sv("127.0.0.1"), NULL, proven_net_deadline_in(10000), &tls) == PROVEN_OK &&
                       proven_transport_write_all(tls, mv("half a sent"), proven_net_deadline_in(5000)).err == PROVEN_OK, "another connection, with something said", "");
    proven_time_sleep(100);
    (void)proven_net_close(&sock);                      /* the TCP connection goes away with no TLS goodbye */
    peer_finish(&peer);
    PROVEN_TEST_ASSERT(peer.echoed == 11 && peer.last_read == PROVEN_ERR_RESET, "a connection cut without close_notify is PROVEN_ERR_RESET, not an end of stream", "");
    (void)proven_transport_close(tls);                  /* frees the client's side; its socket is already closed */

    peer_start(&peer, 0);
    PROVEN_TEST_ASSERT(proven_net_connect(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, peer.port), proven_net_deadline_in(5000), &sock) == PROVEN_OK, "connect", "");
    PROVEN_TEST_ASSERT(proven_tls_transport_client(proven_net_conn_transport(&sock), g_client_tls, sv("localhost.invalid"), NULL, proven_net_deadline_in(10000), &tls) == PROVEN_ERR_NAME_MISMATCH,
        "the same server reached under another name is PROVEN_ERR_NAME_MISMATCH", "");
    (void)proven_net_close(&sock);                      /* a failed wrap leaves the socket to the caller */
    peer_finish(&peer);
    PROVEN_TEST_ASSERT(peer.handshake == PROVEN_ERR_PROTOCOL, "and the server's handshake ends with the client's alert", "");

    peer_start(&peer, 1);
    PROVEN_TEST_ASSERT(proven_net_connect(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, peer.port), proven_net_deadline_in(5000), &sock) == PROVEN_OK, "connect to a peer that will say nothing", "");
    proven_time_t t0 = proven_time_monotonic_now();
    proven_err_t e = proven_tls_transport_client(proven_net_conn_transport(&sock), g_client_tls, sv("127.0.0.1"), NULL, proven_net_deadline_in(250), &tls);
    proven_time_t took = proven_time_monotonic_now() - t0;
    PROVEN_TEST_ASSERT(e == PROVEN_ERR_TIMEOUT && took < 600000000, "a handshake nobody answers ends at its deadline with PROVEN_ERR_TIMEOUT", "");
    (void)proven_net_close(&sock);
    peer_finish(&peer);
}

// -----------------------------------------------------------------------------
// HTTPS and WebSocket over TLS
// -----------------------------------------------------------------------------

static proven_http_server_t *g_server;
static atomic_int g_requests;

static void serve(void *arg) { (void)arg; (void)proven_http_server_run(g_server); }

static void handler(void *ctx, proven_http_exchange_t *x) {
    (void)ctx;
    const proven_http_request_t *r = proven_http_exchange_request(x);
    atomic_fetch_add(&g_requests, 1);
    if (proven_u8str_view_eq(r->target, sv("/echo-ws"))) {
        proven_ws_conn_config_t cfg = { .alloc = g_heap };
        proven_ws_conn_t *ws = NULL;
        if (proven_ws_conn_accept(x, sv(""), &cfg, &ws) != PROVEN_OK) return;
        proven_ws_message_t m;
        while (proven_ws_conn_receive(ws, proven_net_deadline_in(10000), &m) == PROVEN_OK) {
            if (m.text) (void)proven_ws_conn_send_text(ws, (proven_u8str_view_t){ .ptr = m.data.ptr, .size = m.data.size });
            else (void)proven_ws_conn_send_binary(ws, m.data);
        }
        proven_ws_conn_destroy(ws);
        return;
    }
    if (proven_u8str_view_eq(r->target, sv("/upload"))) {
        /* Answer with how many bytes arrived and a sum of them. */
        static _Thread_local proven_byte_t buf[8192];
        proven_size_t total = 0;
        unsigned sum = 0;
        for (;;) {
            proven_result_size_t got = proven_http_exchange_read(x, (proven_mem_mut_t){ .ptr = buf, .size = sizeof buf });
            if (got.err != PROVEN_OK || got.value == 0) break;
            for (proven_size_t i = 0; i < got.value; ++i) sum += buf[i];
            total += got.value;
        }
        char text[64];
        int n = snprintf(text, sizeof text, "%u %u", (unsigned)total, sum);
        (void)proven_http_exchange_respond(x, 200, NULL, 0, (proven_mem_view_t){ .ptr = (const proven_byte_t *)text, .size = (proven_size_t)n });
        return;
    }
    if (proven_u8str_view_eq(r->target, sv("/big"))) {
        static proven_byte_t big[300000];
        for (proven_size_t i = 0; i < sizeof big; ++i) big[i] = (proven_byte_t)('a' + i % 23);
        (void)proven_http_exchange_respond(x, 200, NULL, 0, (proven_mem_view_t){ .ptr = big, .size = sizeof big });
        return;
    }
    (void)proven_http_exchange_respond(x, 200, NULL, 0, mv("hello over tls"));
}

static char g_url[128];
static proven_u8str_view_t url(const char *scheme, proven_u16 port, const char *path) {
    int n = snprintf(g_url, sizeof g_url, "%s://127.0.0.1:%u%s", scheme, (unsigned)port, path);
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)g_url, .size = (proven_size_t)n };
}

static bool body_is(proven_http_client_response_t *resp, const char *want) {
    proven_u8str_t body = { 0 };
    bool ok = proven_http_client_read_all(resp, g_heap, &body, 1024 * 1024) == PROVEN_OK;
    proven_u8str_view_t v = proven_u8str_as_view(&body);
    ok = ok && v.size == strlen(want) && memcmp(v.ptr, want, v.size) == 0;
    proven_u8str_destroy(g_heap, &body);
    return ok;
}

static void https_cases(proven_job_sys_t *handlers, const char *how) {
    proven_http_server_config_t scfg = { .alloc = g_heap, .handler = handler, .jobs = handlers, .tls = g_server_tls, .head_timeout_ms = 400, .max_body_bytes = 4 * 1024 * 1024 };
    proven_net_addr_t at;
    PROVEN_TEST_ASSERT(proven_http_server_create(&scfg, &g_server) == PROVEN_OK &&
                       proven_http_server_listen(g_server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "an HTTPS server", how);
    proven_job_group_t running;
    proven_job_group_init(&running);
    PROVEN_TEST_ASSERT(proven_job_group_submit(g_loop, &running, serve, NULL) == PROVEN_OK, "its loop runs on another thread", how);

    proven_http_client_config_t ccfg = { .alloc = g_heap, .max_idle_connections = 2, .tls_wrap = proven_tls_http_wrap, .tls_ctx = g_client_tls };
    proven_http_client_t *client = NULL;
    PROVEN_TEST_ASSERT(proven_http_client_create(&ccfg, &client) == PROVEN_OK, "an HTTP client with the TLS wrap", how);
    proven_http_client_response_t resp;

    for (int i = 0; i < 3; ++i) {
        proven_err_t e = proven_http_client_get(client, url("https", at.port, "/"), &resp);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && resp.status == 200 && body_is(&resp, "hello over tls"), "GET over https, and again on the kept connection", how);
        proven_http_client_finish(&resp);
    }

    /* A body sent up, larger than a few records. */
    static proven_byte_t upload[200000];
    unsigned sum = 0;
    for (proven_size_t i = 0; i < sizeof upload; ++i) { upload[i] = (proven_byte_t)(i * 3 + 1); sum += upload[i]; }
    proven_http_client_request_t req = { .method = sv("POST"), .url = url("https", at.port, "/upload"), .body = { .ptr = upload, .size = sizeof upload } };
    char want[64];
    snprintf(want, sizeof want, "%u %u", (unsigned)sizeof upload, sum);
    proven_err_t e = proven_http_client_send(client, &req, &resp);
    PROVEN_TEST_ASSERT(e == PROVEN_OK && resp.status == 200 && body_is(&resp, want), "a 200,000-byte request body arrives whole", how);
    proven_http_client_finish(&resp);

    e = proven_http_client_get(client, url("https", at.port, "/big"), &resp);
    proven_u8str_t body = { 0 };
    bool big_ok = e == PROVEN_OK && proven_http_client_read_all(&resp, g_heap, &body, 1024 * 1024) == PROVEN_OK && proven_u8str_as_view(&body).size == 300000;
    for (proven_size_t i = 0; big_ok && i < 300000; i += 997) big_ok = proven_u8str_as_view(&body).ptr[i] == (proven_byte_t)('a' + i % 23);
    PROVEN_TEST_ASSERT(big_ok, "a 300,000-byte response body arrives whole", how);
    proven_u8str_destroy(g_heap, &body);
    proven_http_client_finish(&resp);

    /* WebSocket over TLS: the same upgrade, on the encrypted connection. */
    proven_ws_conn_config_t wcfg = { .alloc = g_heap };
    proven_ws_conn_t *ws = NULL;
    proven_ws_message_t m;
    e = proven_ws_conn_connect(client, url("wss", at.port, "/echo-ws"), NULL, 0, sv(""), &wcfg, &ws, NULL);
    PROVEN_TEST_ASSERT(e == PROVEN_OK && ws != NULL, "a wss:// URL connects", how);
    bool ws_ok = proven_ws_conn_send_text(ws, sv("over tls")) == PROVEN_OK && proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &m) == PROVEN_OK &&
                 m.text && m.data.size == 8 && memcmp(m.data.ptr, "over tls", 8) == 0;
    ws_ok = ws_ok && proven_ws_conn_send_binary(ws, (proven_mem_view_t){ .ptr = upload, .size = 70000 }) == PROVEN_OK &&
            proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &m) == PROVEN_OK && !m.text && m.data.size == 70000 && memcmp(m.data.ptr, upload, 70000) == 0;
    PROVEN_TEST_ASSERT(ws_ok, "a text message and a 70,000-byte binary message are echoed back", how);
    PROVEN_TEST_ASSERT(proven_ws_conn_close(ws, 1000, sv("done")) == PROVEN_OK, "and the WebSocket closes cleanly", how);
    proven_ws_conn_destroy(ws);

    /* Two requests in one write: the second is inside the TLS layer when the first is answered,
     * and no new readiness will ever announce it. */
    proven_net_conn_t sock;
    proven_transport_t tls;
    PROVEN_TEST_ASSERT(proven_net_connect(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, at.port), proven_net_deadline_in(5000), &sock) == PROVEN_OK &&
                       proven_tls_transport_client(proven_net_conn_transport(&sock), g_client_tls, sv("127.0.0.1"), NULL, proven_net_deadline_in(5000), &tls) == PROVEN_OK, "a raw TLS client", how);
    static const char two[] = "GET /a HTTP/1.1\r\nHost: x\r\n\r\nGET /b HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    PROVEN_TEST_ASSERT(proven_transport_write_all(tls, mv(two), proven_net_deadline_in(5000)).err == PROVEN_OK, "two pipelined requests in one record", how);
    static char got[4096];
    proven_size_t n = 0;
    for (;;) {
        proven_result_size_t r = proven_transport_read(tls, (proven_mem_mut_t){ .ptr = (proven_byte_t *)got + n, .size = sizeof got - 1 - n }, proven_net_deadline_in(5000));
        if (r.err != PROVEN_OK) break;
        n += r.value;
    }
    got[n] = 0;
    int answers = 0;
    for (const char *p = got; (p = strstr(p, "HTTP/1.1 200")) != NULL; ++p) answers++;
    PROVEN_TEST_ASSERT(answers == 2, "both are answered", how);
    (void)proven_transport_close(tls);

    /* A client that connects and says nothing must not hold up anybody else, and is dropped. */
    proven_net_conn_t silent;
    PROVEN_TEST_ASSERT(proven_net_connect(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, at.port), proven_net_deadline_in(5000), &silent) == PROVEN_OK, "a client that says nothing", how);
    proven_time_t t0 = proven_time_monotonic_now();
    e = proven_http_client_get(client, url("https", at.port, "/"), &resp);
    proven_time_t took = proven_time_monotonic_now() - t0;
    PROVEN_TEST_ASSERT(e == PROVEN_OK && body_is(&resp, "hello over tls") && took < 300000000, "others are served at once while its handshake is pending", how);
    proven_http_client_finish(&resp);
    /* The server may say a TLS goodbye first; what matters is that the connection then ends. */
    proven_byte_t drop[64];
    proven_result_size_t gone;
    proven_net_deadline_t give_up = proven_net_deadline_in(3000);
    do { gone = proven_net_read(&silent, (proven_mem_mut_t){ .ptr = drop, .size = sizeof drop }, give_up); } while (gone.err == PROVEN_OK);
    PROVEN_TEST_ASSERT(gone.err == PROVEN_ERR_EOF || gone.err == PROVEN_ERR_RESET, "and after the head timeout the server closes it", how);
    (void)proven_net_close(&silent);

    /* Plain HTTP sent to the TLS port is not answered. */
    proven_http_client_config_t plain_cfg = { .alloc = g_heap, .io_timeout_ms = 2000 };
    proven_http_client_t *plain = NULL;
    PROVEN_TEST_ASSERT(proven_http_client_create(&plain_cfg, &plain) == PROVEN_OK, "a client without TLS", how);
    int before = atomic_load(&g_requests);
    e = proven_http_client_get(plain, url("http", at.port, "/"), &resp);
    PROVEN_TEST_ASSERT(e != PROVEN_OK && atomic_load(&g_requests) == before, "a plain http:// request to the TLS port fails and reaches no handler", how);
    proven_http_client_finish(&resp);
    PROVEN_TEST_ASSERT(proven_http_client_get(plain, url("https", at.port, "/"), &resp) == PROVEN_ERR_UNSUPPORTED, "an https:// URL on a client with no TLS wrap is PROVEN_ERR_UNSUPPORTED, as before", how);
    proven_http_client_finish(&resp);
    proven_http_client_destroy(plain);

    /* A client that does not trust this server's CA. */
    proven_http_client_config_t other_cfg = { .alloc = g_heap, .tls_wrap = proven_tls_http_wrap, .tls_ctx = g_stranger_tls };
    proven_http_client_t *other = NULL;
    PROVEN_TEST_ASSERT(proven_http_client_create(&other_cfg, &other) == PROVEN_OK, "a client with other anchors", how);
    before = atomic_load(&g_requests);
    e = proven_http_client_get(other, url("https", at.port, "/"), &resp);
    PROVEN_TEST_ASSERT(e == PROVEN_ERR_UNTRUSTED && atomic_load(&g_requests) == before, "a server whose CA the client does not hold: PROVEN_ERR_UNTRUSTED, and nothing was sent to it", how);
    proven_http_client_finish(&resp);
    proven_http_client_destroy(other);

    proven_http_client_destroy(client);
    proven_http_server_stop(g_server);
    proven_job_group_wait(g_loop, &running);
    proven_http_server_destroy(g_server);
    g_server = NULL;
}

int main(void) {
    PROVEN_TEST_SUITE("TLS over sockets: the transport, HTTPS and WebSocket",
        "The engine carried by real connections: the wrapper by itself, then through the HTTP client and server with handlers on the loop thread and on workers.",
        "Inspect src/proven/tls_transport.c and the TLS branches of src/proven/http_server.c. The engine itself is test_unit_tls's business.");

    g_heap = proven_heap_allocator();
    PROVEN_TEST_ASSERT(tp_build(), "the test's certificates and keys are issued", "Check src/proven/tls_issue.c.");
    proven_job_sys_t *handlers = NULL;
    PROVEN_TEST_ASSERT(proven_job_system_init(g_heap, 1, 4, &g_loop) == PROVEN_OK && proven_job_system_init(g_heap, 2, 8, &g_workers) == PROVEN_OK &&
                       proven_job_system_init(g_heap, 3, 16, &handlers) == PROVEN_OK, "threads", "");
    PROVEN_TEST_ASSERT(proven_cert_store_create(g_heap, &g_anchors) == PROVEN_OK && proven_cert_store_add_pem(g_anchors, pem(TP_CA), NULL) == PROVEN_OK &&
                       proven_cert_store_create(g_heap, &g_other) == PROVEN_OK && proven_cert_store_add_pem(g_other, pem(TP_OTHER_CA), NULL) == PROVEN_OK, "anchors", "");
    proven_u8str_view_t protos[1] = { PROVEN_LIT("http/1.1") };
    proven_tls_options_t co = { .alloc = g_heap, .anchors = g_anchors, .alpn = protos, .alpn_count = 1 };
    proven_tls_options_t so = { .alloc = g_heap, .certificate_pem = pem(TP_SERVER_P256), .private_key_pem = pem(TP_SERVER_P256_KEY), .alpn = protos, .alpn_count = 1 };
    proven_tls_options_t xo = { .alloc = g_heap, .anchors = g_other };
    PROVEN_TEST_ASSERT(proven_tls_config_create(&co, &g_client_tls) == PROVEN_OK && proven_tls_config_create(&so, &g_server_tls) == PROVEN_OK &&
                       proven_tls_config_create(&xo, &g_stranger_tls) == PROVEN_OK, "configs with the operating system's clock and random source", "");

    transport_cases();

    PROVEN_TEST_SECTION("HTTPS and wss, handlers on the loop's thread", "The server's own loop drives each handshake as bytes arrive.", "Check the `tls` branches of sv_accept, sv_readable and sv_service.");
    https_cases(NULL, "handlers on the loop thread");
    PROVEN_TEST_SECTION("HTTPS and wss, handlers on workers", "The same, with connections changing hands between the loop and worker threads.", "Check that the TLS transport travels with the connection.");
    https_cases(handlers, "handlers on workers");

    proven_tls_config_destroy(g_client_tls); proven_tls_config_destroy(g_server_tls); proven_tls_config_destroy(g_stranger_tls);
    proven_cert_store_destroy(g_anchors); proven_cert_store_destroy(g_other);
    proven_job_system_close(handlers); proven_job_system_destroy(handlers);
    proven_job_system_close(g_workers); proven_job_system_destroy(g_workers);
    proven_job_system_close(g_loop); proven_job_system_destroy(g_loop);
    PROVEN_TEST_PASS("TLS carries the transport, HTTP and WebSocket over real connections.");
    return 0;
}
