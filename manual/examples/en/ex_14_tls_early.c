#include "example.h"
#include <stdio.h>
#include <string.h>

/*
 * Early data (0-RTT) over HTTPS, both ends in one program: the server of chapter 11 and the
 * client of chapter 11, each with `early_data` in its TLS configuration. The first request makes
 * a full handshake and leaves a ticket; the second goes with the next connection's ClientHello,
 * and the handler can tell.
 */

static const char *const HOW_TO_MAKE =
    "  openssl req -x509 -newkey ed25519 -nodes -days 30 -subj /CN=example.test \\\n"
    "      -addext subjectAltName=DNS:example.test,IP:127.0.0.1 -keyout key.pem -out cert.pem\n";

typedef struct {
    proven_http_server_t *server;
} ex_app_t;

static void handle(void *ctx, proven_http_exchange_t *x) {
    ex_app_t *app = ctx;
    const proven_http_request_t *req = proven_http_exchange_request(x);
    if (proven_u8str_view_eq(req->target, PROVEN_LIT("/quit"))) {
        (void)proven_http_exchange_respond(x, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("bye\n")));
        proven_http_server_stop(app->server);
        return;
    }
    /* A request that came as early data may be a copy someone replayed. Only GET, HEAD and
     * OPTIONS without a body get this far early - the server answers anything else 425 itself -
     * and a GET that changes something should answer 425 too: the client sends it again after
     * the handshake. */
    const bool early = proven_http_exchange_early(x);
    (void)proven_http_exchange_respond(x, 200, NULL, 0, proven_mem_view_from_u8(early ? PROVEN_LIT("early\n") : PROVEN_LIT("after the handshake\n")));
}

static void serve(void *arg) {
    (void)proven_http_server_run(((ex_app_t *)arg)->server);
}

/* GET `url` and return whether the body is `want`. */
static bool get_is(proven_http_client_t *client, proven_allocator_t heap, proven_u8str_view_t url, proven_u8str_view_t want) {
    proven_http_client_response_t response;
    proven_u8str_t body = { 0 };
    bool ok = proven_http_client_get(client, url, &response) == PROVEN_OK && response.status == 200 &&
              proven_http_client_read_all(&response, heap, &body, 4096) == PROVEN_OK && proven_u8str_view_eq(proven_u8str_as_view(&body), want);
    proven_u8str_destroy(heap, &body);
    proven_http_client_finish(&response);
    return ok;
}

int main(int argc, char **argv) {
    proven_allocator_t heap = proven_heap_allocator();
    if (argc < 3) {
        printf("usage: %s CERTIFICATE.pem KEY.pem\nA self-signed pair for 127.0.0.1:\n%s", argv[0], HOW_TO_MAKE);
        return 0;
    }
    proven_result_mem_mut_t cert = proven_fs_read_all_bounded(heap, proven_u8str_view_from_cstr(argv[1]), 64 * 1024);
    proven_result_mem_mut_t key = proven_fs_read_all_bounded(heap, proven_u8str_view_from_cstr(argv[2]), 16 * 1024);
    EXAMPLE_REQUIRE(cert.err == PROVEN_OK && key.err == PROVEN_OK, "the certificate and the key are read");
    if (cert.err != PROVEN_OK || key.err != PROVEN_OK) return 1;

    /* The server: `early_data` makes its tickets allow early data, and it takes it - once per
     * ticket, within 10 seconds of the ticket's age. */
    proven_tls_options_t server_options = {
        .alloc = heap, .early_data = true,
        .certificate_pem = { cert.value.ptr, cert.value.size }, .private_key_pem = { key.value.ptr, key.value.size },
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

    /* The client: `early_data` makes its configuration keep the newest session of each server,
     * and send a request that is safe to repeat with the next connection's ClientHello. */
    proven_cert_store_t *anchors = NULL;
    EXAMPLE_REQUIRE(proven_cert_store_create(heap, &anchors) == PROVEN_OK &&
                    proven_cert_store_add_pem(anchors, (proven_mem_view_t){ cert.value.ptr, cert.value.size }, NULL) == PROVEN_OK, "the anchors");
    proven_tls_options_t client_options = { .alloc = heap, .anchors = anchors, .early_data = true };
    proven_tls_config_t *client_tls = NULL;
    EXAMPLE_REQUIRE(proven_tls_config_create(&client_options, &client_tls) == PROVEN_OK, "the client's TLS configuration");
    /* No connection kept, so that each request makes a new one. */
    proven_http_client_config_t client_config = { .alloc = heap, .tls_wrap = proven_tls_http_wrap, .tls_ctx = client_tls };
    proven_http_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&client_config, &client) == PROVEN_OK, "an HTTP client");

    char url_text[64];
    int url_len = snprintf(url_text, sizeof url_text, "https://127.0.0.1:%u/", (unsigned)at.port);
    proven_u8str_view_t url = { (const proven_byte_t *)url_text, (proven_size_t)url_len };
    EXAMPLE_REQUIRE(get_is(client, heap, url, PROVEN_LIT("after the handshake\n")), "the first connection: a full handshake, and a ticket");
    EXAMPLE_REQUIRE(get_is(client, heap, url, PROVEN_LIT("early\n")), "the second: the GET went with its ClientHello");

    url_len = snprintf(url_text, sizeof url_text, "https://127.0.0.1:%u/quit", (unsigned)at.port);
    url.size = (proven_size_t)url_len;
    (void)get_is(client, heap, url, PROVEN_LIT("bye\n"));
    proven_job_group_wait(threads, &running);
    proven_http_client_destroy(client);
    proven_http_server_destroy(app.server);
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    proven_tls_config_destroy(client_tls);
    proven_tls_config_destroy(server_tls);
    proven_cert_store_destroy(anchors);
    proven_mem_wipe(key.value);
    heap.free_fn(heap.ctx, key.value.ptr);
    heap.free_fn(heap.ctx, cert.value.ptr);
    return EXAMPLE_OK();
}
