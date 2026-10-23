#include "example.h"
#include <stdio.h>
#include <string.h>

/*
 * HTTPS, both ends, in one program: the HTTP server of chapter 11 with a TLS config, and the
 * HTTP client of chapter 11 with the TLS wrap. Nothing else about either changes.
 *
 * The server is on another thread on the loopback interface, so the example needs no network.
 */

/* There is no certificate in this file. The server's identity is read from two PEM files named
 * on the command line: a certificate for 127.0.0.1 and its key. Run with none, the program says
 * how to make a self-signed pair with OpenSSL. The client is given that certificate as the one
 * thing it believes. */

/* What to run when no files are given. */
static const char *const HOW_TO_MAKE =
    "  openssl req -x509 -newkey ed25519 -nodes -days 30 -subj /CN=example.test \\\n"
    "      -addext subjectAltName=DNS:example.test,IP:127.0.0.1 -keyout key.pem -out cert.pem\n";

typedef struct {
    proven_http_server_t *server;
} ex_app_t;

static void handle(void *ctx, proven_http_exchange_t *x) {
    ex_app_t *app = ctx;
    const proven_http_request_t *req = proven_http_exchange_request(x);
    /* The handler is the same as without TLS: by the time it runs, the request is plain text. */
    if (proven_u8str_view_eq(req->target, PROVEN_LIT("/quit"))) {
        (void)proven_http_exchange_respond(x, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("bye\n")));
        proven_http_server_stop(app->server);
        return;
    }
    (void)proven_http_exchange_respond(x, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("hello over TLS\n")));
}

static void serve(void *arg) {
    (void)proven_http_server_run(((ex_app_t *)arg)->server);
}

int main(int argc, char **argv) {
    proven_allocator_t heap = proven_heap_allocator();

    // ---- an identity ---------------------------------------------------------------
    /* A certificate for the address this example serves on, and its key, from files. A real
     * server is given a certificate from an authority; a pair of machines you run yourself can
     * use a self-signed one, or a pin (chapter 14, section 7). */
    if (argc < 3) {
        printf("usage: %s CERTIFICATE.pem KEY.pem\nA self-signed pair for 127.0.0.1:\n%s", argv[0], HOW_TO_MAKE);
        return 0;
    }
    proven_result_mem_mut_t cert = proven_fs_read_all_bounded(heap, proven_u8str_view_from_cstr(argv[1]), 64 * 1024);
    proven_result_mem_mut_t key = proven_fs_read_all_bounded(heap, proven_u8str_view_from_cstr(argv[2]), 16 * 1024);
    EXAMPLE_REQUIRE(cert.err == PROVEN_OK && key.err == PROVEN_OK, "the certificate and the key are read");
    if (cert.err != PROVEN_OK || key.err != PROVEN_OK) return 1;
    const proven_byte_t *cert_pem = cert.value.ptr, *key_pem = key.value.ptr;
    const proven_size_t cert_len = cert.value.size, key_len = key.value.size;

    // ---- the server ----------------------------------------------------------------
    /* One field makes a server speak TLS: its config's `tls`. The TLS config holds the
     * certificate the server presents and the key that proves it is the server's. */
    proven_tls_options_t server_options = {
        .alloc = heap,
        .certificate_pem = { cert_pem, cert_len },
        .private_key_pem = { key_pem, key_len },
    };
    proven_tls_config_t *server_tls = NULL;
    EXAMPLE_REQUIRE(proven_tls_config_create(&server_options, &server_tls) == PROVEN_OK, "the server's TLS configuration");

    ex_app_t app = { 0 };
    proven_http_server_config_t server_config = { .alloc = heap, .handler = handle, .handler_ctx = &app, .tls = server_tls };
    proven_net_addr_t at;
    EXAMPLE_REQUIRE(proven_http_server_create(&server_config, &app.server) == PROVEN_OK &&
                    proven_http_server_listen(app.server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "an HTTPS server on a free port");
    proven_job_sys_t *threads = NULL;
    proven_job_group_t running;
    proven_job_group_init(&running);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &threads) == PROVEN_OK &&
                    proven_job_group_submit(threads, &running, serve, &app) == PROVEN_OK, "its loop runs on another thread");

    // ---- the client ----------------------------------------------------------------
    /* The client's TLS config says whom to believe: here, the certificate just made. For the
     * public internet it would be proven_cert_store_add_system instead. */
    proven_cert_store_t *anchors = NULL;
    EXAMPLE_REQUIRE(proven_cert_store_create(heap, &anchors) == PROVEN_OK &&
                    proven_cert_store_add_pem(anchors, (proven_mem_view_t){ cert_pem, cert_len }, NULL) == PROVEN_OK,
                    "the anchors");
    proven_tls_options_t client_options = { .alloc = heap, .anchors = anchors };
    proven_tls_config_t *client_tls = NULL;
    EXAMPLE_REQUIRE(proven_tls_config_create(&client_options, &client_tls) == PROVEN_OK, "the client's TLS configuration");

    /* Two fields make a client speak HTTPS: the wrap function this library provides, and the
     * TLS config as its context. */
    proven_http_client_config_t client_config = { .alloc = heap, .max_idle_connections = 1, .tls_wrap = proven_tls_http_wrap, .tls_ctx = client_tls };
    proven_http_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&client_config, &client) == PROVEN_OK, "an HTTP client");

    char url_text[64];
    int url_len = snprintf(url_text, sizeof url_text, "https://127.0.0.1:%u/", (unsigned)at.port);
    proven_u8str_view_t url = { (const proven_byte_t *)url_text, (proven_size_t)url_len };

    proven_http_client_response_t response;
    proven_u8str_t body = { 0 };
    EXAMPLE_REQUIRE(proven_http_client_get(client, url, &response) == PROVEN_OK && response.status == 200, "GET over https: 200");
    EXAMPLE_REQUIRE(proven_http_client_read_all(&response, heap, &body, 4096) == PROVEN_OK &&
                    proven_u8str_view_eq(proven_u8str_as_view(&body), PROVEN_LIT("hello over TLS\n")), "and the body, which crossed the wire encrypted");
    proven_u8str_destroy(heap, &body);
    proven_http_client_finish(&response);

    // ---- a client with nothing to believe ------------------------------------------
    /* There is no setting that verifies nothing. A client that would check a chain must be given
     * anchors, and one given an empty store believes no server at all. */
    proven_tls_options_t stranger_options = { .alloc = heap, .anchors = NULL };
    proven_tls_config_t *no_anchors = NULL;
    EXAMPLE_REQUIRE(proven_tls_config_create(&stranger_options, &no_anchors) == PROVEN_ERR_INVALID_ARG, "no anchors at all is refused when the config is made");

    proven_cert_store_t *empty = NULL;
    EXAMPLE_REQUIRE(proven_cert_store_create(heap, &empty) == PROVEN_OK, "an empty store");
    stranger_options.anchors = empty;
    EXAMPLE_REQUIRE(proven_tls_config_create(&stranger_options, &no_anchors) == PROVEN_OK, "a config that trusts nobody");
    proven_http_client_config_t wary_config = { .alloc = heap, .tls_wrap = proven_tls_http_wrap, .tls_ctx = no_anchors };
    proven_http_client_t *wary = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&wary_config, &wary) == PROVEN_OK, "an HTTP client");
    EXAMPLE_REQUIRE(proven_http_client_get(wary, url, &response) == PROVEN_ERR_UNTRUSTED, "the same server is PROVEN_ERR_UNTRUSTED, and no request was sent to it");
    proven_http_client_finish(&response);
    proven_http_client_destroy(wary);

    // ---- done ----------------------------------------------------------------------
    url_len = snprintf(url_text, sizeof url_text, "https://127.0.0.1:%u/quit", (unsigned)at.port);
    EXAMPLE_REQUIRE(proven_http_client_get(client, (proven_u8str_view_t){ (const proven_byte_t *)url_text, (proven_size_t)url_len }, &response) == PROVEN_OK, "the server is asked to stop");
    proven_http_client_finish(&response);
    proven_job_group_wait(threads, &running);

    proven_http_client_destroy(client);
    proven_http_server_destroy(app.server);
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    proven_tls_config_destroy(client_tls);
    proven_tls_config_destroy(no_anchors);
    proven_tls_config_destroy(server_tls);
    proven_cert_store_destroy(anchors);
    proven_cert_store_destroy(empty);
    proven_mem_wipe(key.value);
    heap.free_fn(heap.ctx, key.value.ptr);
    heap.free_fn(heap.ctx, cert.value.ptr);
    return EXAMPLE_OK();
}
