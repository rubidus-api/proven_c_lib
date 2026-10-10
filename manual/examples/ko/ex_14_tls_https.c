#include "example.h"
#include <stdio.h>
#include <string.h>

/*
 * HTTPS, 양쪽 끝을 한 프로그램에: 11장의 HTTP 서버에 TLS 설정을, 11장의 HTTP 클라이언트에 TLS
 * 랩을 붙인 것. 그 밖에는 어느 쪽도 달라지지 않는다.
 *
 * 서버는 루프백 인터페이스에서 다른 스레드로 돌아가므로 예제에 네트워크가 필요 없다.
 */

/* 이 파일에는 인증서가 없다. 서버의 신원은 프로그램이 돌 때 만들어진다: 127.0.0.1을 위한 자체
 * 서명 인증서와 그 키, 지금을 전후한 하루 동안 유효. 클라이언트에는 그 인증서를 믿는 단 하나의
 * 것으로 준다. */

typedef struct {
    proven_http_server_t *server;
} app_t;

static void handle(void *ctx, proven_http_exchange_t *x) {
    app_t *app = ctx;
    const proven_http_request_t *req = proven_http_exchange_request(x);
    /* 핸들러는 TLS가 없을 때와 같다: 핸들러가 돌 때쯤 요청은 평문이다. */
    if (proven_u8str_view_eq(req->target, PROVEN_LIT("/quit"))) {
        (void)proven_http_exchange_respond(x, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("bye\n")));
        proven_http_server_stop(app->server);
        return;
    }
    (void)proven_http_exchange_respond(x, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("hello over TLS\n")));
}

static void serve(void *arg) {
    (void)proven_http_server_run(((app_t *)arg)->server);
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    // ---- 신원 ---------------------------------------------------------------------
    /* 이 예제가 서비스하는 주소를 위한 자체 서명 인증서. 실제 서버라면 기관이 낸 인증서를 받는다.
     * 직접 운영하는 기계 한 쌍이라면 이것과 핀을 쓸 수 있다(14장 7절). */
    proven_byte_t cert_pem[1024], key_pem[256];
    proven_size_t cert_len = 0, key_len = 0;
    proven_u8str_view_t names[1] = { PROVEN_LIT("127.0.0.1") };
    proven_i64 now = proven_time_now() / 1000000000;
    EXAMPLE_REQUIRE(proven_tls_self_signed(names, 1, now - 3600, now + 86400, NULL, NULL,
                                           (proven_mem_mut_t){ cert_pem, sizeof cert_pem }, &cert_len,
                                           (proven_mem_mut_t){ key_pem, sizeof key_pem }, &key_len) == PROVEN_OK, "자체 서명 인증서와 그 키");

    // ---- 서버 ----------------------------------------------------------------------
    /* 필드 하나가 서버를 TLS로 말하게 한다: 설정의 `tls`. TLS 설정은 서버가 내미는 인증서와,
     * 그것이 서버의 것임을 증명하는 키를 담는다. */
    proven_tls_options_t server_options = {
        .alloc = heap,
        .certificate_pem = { cert_pem, cert_len },
        .private_key_pem = { key_pem, key_len },
    };
    proven_tls_config_t *server_tls = NULL;
    EXAMPLE_REQUIRE(proven_tls_config_create(&server_options, &server_tls) == PROVEN_OK, "서버의 TLS 설정");

    app_t app = { 0 };
    proven_http_server_config_t server_config = { .alloc = heap, .handler = handle, .handler_ctx = &app, .tls = server_tls };
    proven_net_addr_t at;
    EXAMPLE_REQUIRE(proven_http_server_create(&server_config, &app.server) == PROVEN_OK &&
                    proven_http_server_listen(app.server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "빈 포트의 HTTPS 서버");
    proven_job_sys_t *threads = NULL;
    proven_job_group_t running;
    proven_job_group_init(&running);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &threads) == PROVEN_OK &&
                    proven_job_group_submit(threads, &running, serve, &app) == PROVEN_OK, "루프가 다른 스레드에서 돈다");

    // ---- 클라이언트 ----------------------------------------------------------------
    /* 클라이언트의 TLS 설정은 누구를 믿을지 말한다: 여기서는 방금 만든 인증서. 공개 인터넷이라면
     * 대신 proven_cert_store_add_system이다. */
    proven_cert_store_t *anchors = NULL;
    EXAMPLE_REQUIRE(proven_cert_store_create(heap, &anchors) == PROVEN_OK &&
                    proven_cert_store_add_pem(anchors, (proven_mem_view_t){ cert_pem, cert_len }, NULL) == PROVEN_OK,
                    "앵커");
    proven_tls_options_t client_options = { .alloc = heap, .anchors = anchors };
    proven_tls_config_t *client_tls = NULL;
    EXAMPLE_REQUIRE(proven_tls_config_create(&client_options, &client_tls) == PROVEN_OK, "클라이언트의 TLS 설정");

    /* 필드 둘이 클라이언트를 HTTPS로 말하게 한다: 이 라이브러리가 주는 랩 함수와, 그 컨텍스트로
     * 쓰이는 TLS 설정. */
    proven_http_client_config_t client_config = { .alloc = heap, .max_idle_connections = 1, .tls_wrap = proven_tls_http_wrap, .tls_ctx = client_tls };
    proven_http_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&client_config, &client) == PROVEN_OK, "HTTP 클라이언트");

    char url_text[64];
    int url_len = snprintf(url_text, sizeof url_text, "https://127.0.0.1:%u/", (unsigned)at.port);
    proven_u8str_view_t url = { (const proven_byte_t *)url_text, (proven_size_t)url_len };

    proven_http_client_response_t response;
    proven_u8str_t body = { 0 };
    EXAMPLE_REQUIRE(proven_http_client_get(client, url, &response) == PROVEN_OK && response.status == 200, "https로 GET: 200");
    EXAMPLE_REQUIRE(proven_http_client_read_all(&response, heap, &body, 4096) == PROVEN_OK &&
                    proven_u8str_view_eq(proven_u8str_as_view(&body), PROVEN_LIT("hello over TLS\n")), "그리고 본문, 선 위를 암호화된 채 건너왔다");
    proven_u8str_destroy(heap, &body);
    proven_http_client_finish(&response);

    // ---- 믿을 것이 없는 클라이언트 --------------------------------------------------
    /* 아무것도 검증하지 않는 설정은 없다. 체인을 검사할 클라이언트에는 앵커를 줘야 하고, 빈
     * 저장소를 받은 클라이언트는 어떤 서버도 믿지 않는다. */
    proven_tls_options_t stranger_options = { .alloc = heap, .anchors = NULL };
    proven_tls_config_t *no_anchors = NULL;
    EXAMPLE_REQUIRE(proven_tls_config_create(&stranger_options, &no_anchors) == PROVEN_ERR_INVALID_ARG, "앵커가 아예 없으면 설정을 만들 때 거부된다");

    proven_cert_store_t *empty = NULL;
    EXAMPLE_REQUIRE(proven_cert_store_create(heap, &empty) == PROVEN_OK, "빈 저장소");
    stranger_options.anchors = empty;
    EXAMPLE_REQUIRE(proven_tls_config_create(&stranger_options, &no_anchors) == PROVEN_OK, "아무도 믿지 않는 설정");
    proven_http_client_config_t wary_config = { .alloc = heap, .tls_wrap = proven_tls_http_wrap, .tls_ctx = no_anchors };
    proven_http_client_t *wary = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&wary_config, &wary) == PROVEN_OK, "HTTP 클라이언트");
    EXAMPLE_REQUIRE(proven_http_client_get(wary, url, &response) == PROVEN_ERR_UNTRUSTED, "같은 서버가 PROVEN_ERR_UNTRUSTED이고, 요청은 보내지지 않았다");
    proven_http_client_finish(&response);
    proven_http_client_destroy(wary);

    // ---- 끝 ------------------------------------------------------------------------
    url_len = snprintf(url_text, sizeof url_text, "https://127.0.0.1:%u/quit", (unsigned)at.port);
    EXAMPLE_REQUIRE(proven_http_client_get(client, (proven_u8str_view_t){ (const proven_byte_t *)url_text, (proven_size_t)url_len }, &response) == PROVEN_OK, "서버에 멈추라고 요청한다");
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
    proven_mem_wipe((proven_mem_mut_t){ key_pem, sizeof key_pem });
    return EXAMPLE_OK();
}
