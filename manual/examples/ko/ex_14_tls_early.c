#include "example.h"
#include <stdio.h>
#include <string.h>

/*
 * HTTPS 위의 early data(0-RTT), 양쪽을 한 프로그램에: 11장의 서버와 11장의 클라이언트가 각자 TLS 설정에
 * `early_data`를 둔다. 첫 요청은 전체 핸드셰이크를 하고 티켓을 남기고, 두 번째 요청은 다음 연결의
 * ClientHello와 함께 가며, 핸들러는 그것을 알 수 있다.
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
    /* early data로 온 요청은 누군가 다시 보낸 사본일 수 있다. 본문 없는 GET, HEAD, OPTIONS만 early로
     * 여기까지 온다 - 그 밖의 것은 서버가 스스로 425로 답한다 - 그리고 무언가를 바꾸는 GET도 425로
     * 답해야 한다: 클라이언트는 핸드셰이크 뒤에 그것을 다시 보낸다. */
    const bool early = proven_http_exchange_early(x);
    (void)proven_http_exchange_respond(x, 200, NULL, 0, proven_mem_view_from_u8(early ? PROVEN_LIT("early\n") : PROVEN_LIT("after the handshake\n")));
}

static void serve(void *arg) {
    (void)proven_http_server_run(((ex_app_t *)arg)->server);
}

/* `url`을 GET하고 본문이 `want`인지 돌려준다. */
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

    /* 서버: `early_data`가 있으면 티켓이 early data를 허락하고 서버가 그것을 받는다 - 티켓마다 한 번,
     * 티켓 나이의 10초 안에서. */
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

    /* 클라이언트: `early_data`가 있으면 설정이 서버마다 가장 새 세션을 간직하고, 반복해도 안전한
     * 요청을 다음 연결의 ClientHello와 함께 보낸다. */
    proven_cert_store_t *anchors = NULL;
    EXAMPLE_REQUIRE(proven_cert_store_create(heap, &anchors) == PROVEN_OK &&
                    proven_cert_store_add_pem(anchors, (proven_mem_view_t){ cert.value.ptr, cert.value.size }, NULL) == PROVEN_OK, "the anchors");
    proven_tls_options_t client_options = { .alloc = heap, .anchors = anchors, .early_data = true };
    proven_tls_config_t *client_tls = NULL;
    EXAMPLE_REQUIRE(proven_tls_config_create(&client_options, &client_tls) == PROVEN_OK, "the client's TLS configuration");
    /* 연결을 간직하지 않아, 요청마다 새 연결을 만든다. */
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
