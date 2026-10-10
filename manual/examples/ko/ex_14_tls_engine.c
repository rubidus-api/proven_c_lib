#include "example.h"
#include <string.h>

/*
 * 네트워크가 보이지 않는 TLS 엔진: 한 프로그램 안의 클라이언트 연결과 서버 연결, 그리고 한쪽에서
 * 다른 쪽으로 바이트를 나르는 이 파일. 네트워크가 둘에게 해 주는 일은 그것이 전부다.
 *
 * 전송 래퍼 아래의 층이다. 전송이 여러분의 것일 때 직접 쓴다: 이벤트 루프, 직렬 선, 테스트.
 */

/* 이 파일에는 인증서가 없다. 서버의 신원은 프로그램이 돌 때 만들어진다: example.test를 위한
 * 자체 서명 인증서와 그 키. 아무도 보증하지 않는 인증서는 이미 그것을 가진 상대만 믿으므로,
 * 클라이언트에는 그것을 앵커로 준다. */

/* 엔진은 시계를 읽지 않는다. 프로그램이 건네준다. 이 시계는 언제나 2027-06-01이라고 말하므로
 * 예제는 어느 날에 돌려도 똑같이 동작한다. */
static proven_i64 fixed_now(void *ctx) { (void)ctx; return 1811808000; }

/* 한쪽이 보내려는 것을 모두 다른 쪽에 건넨다. 받는 쪽은 읽히기를 기다리는 애플리케이션
 * 데이터가 있으면 받은 것보다 적게 가져갈 수 있다. 가져가지 않은 것은 보류 상태로 남는다. */
static proven_err_t carry(proven_tls_conn_t *from, proven_tls_conn_t *to) {
    proven_mem_view_t out = proven_tls_pending_output(from);
    proven_size_t at = 0;
    while (at < out.size) {
        proven_size_t used = 0;
        proven_err_t e = proven_tls_feed(to, (proven_mem_view_t){ out.ptr + at, out.size - at }, &used);
        if (e != PROVEN_OK) return e;
        if (used == 0) break;                 /* 애플리케이션 데이터를 먼저 읽어 주기를 원한다 */
        at += used;
    }
    proven_tls_output_sent(from, at);
    return PROVEN_OK;
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    // ---- 신원 ---------------------------------------------------------------------
    /* proven_tls_self_signed는 Ed25519 키와, 여러분이 요청한 이름을 담고 두 시각 사이에 유효한
     * 인증서를 만든다. 여기서는 이 예제의 고정된 시계 하루 전부터 일 년 뒤까지. 실제 서버라면
     * 대신 기관이 낸 인증서를 받는다. */
    proven_byte_t cert_pem[1024], key_pem[256];
    proven_size_t cert_len = 0, key_len = 0;
    proven_u8str_view_t names[1] = { PROVEN_LIT("example.test") };
    EXAMPLE_REQUIRE(proven_tls_self_signed(names, 1, 1811808000 - 86400, 1811808000 + 365 * 86400, NULL, NULL,
                                           (proven_mem_mut_t){ cert_pem, sizeof cert_pem }, &cert_len,
                                           (proven_mem_mut_t){ key_pem, sizeof key_pem }, &key_len) == PROVEN_OK, "자체 서명 인증서와 그 키");

    // ---- 설정 둘 ------------------------------------------------------------------
    /* 설정은 누구를 믿을지, 여러분이 누구인지 말한다. 한 번 만들어 모든 연결이 함께 쓴다.
     * 클라이언트는 방금 만든 인증서를 믿고, 서버는 그 인증서와 키다. */
    proven_cert_store_t *anchors = NULL;
    EXAMPLE_REQUIRE(proven_cert_store_create(heap, &anchors) == PROVEN_OK &&
                    proven_cert_store_add_pem(anchors, (proven_mem_view_t){ cert_pem, cert_len }, NULL) == PROVEN_OK,
                    "앵커");

    proven_u8str_view_t protocols[1] = { PROVEN_LIT("example/1") };
    proven_tls_options_t client_options = { .alloc = heap, .anchors = anchors, .now = fixed_now, .alpn = protocols, .alpn_count = 1 };
    proven_tls_options_t server_options = {
        .alloc = heap, .now = fixed_now, .alpn = protocols, .alpn_count = 1,
        .certificate_pem = { cert_pem, cert_len },
        .private_key_pem = { key_pem, key_len },
    };
    proven_tls_config_t *client_config = NULL, *server_config = NULL;
    EXAMPLE_REQUIRE(proven_tls_config_create(&client_options, &client_config) == PROVEN_OK, "클라이언트 설정");
    EXAMPLE_REQUIRE(proven_tls_config_create(&server_options, &server_config) == PROVEN_OK, "서버 설정: 키가 인증서와 맞는다");

    // ---- 핸드셰이크 ----------------------------------------------------------------
    /* 클라이언트 연결은 생기는 순간 첫 메시지를 준비해 둔다. 서버 연결은 기다린다. 세션은
     * 다음번을 위한 티켓이 놓일 자리다. */
    proven_tls_session_t session = { 0 };
    proven_tls_conn_t *client = NULL, *server = NULL;
    EXAMPLE_REQUIRE(proven_tls_client_create(client_config, PROVEN_LIT("example.test"), &session, &client) == PROVEN_OK, "클라이언트");
    EXAMPLE_REQUIRE(proven_tls_server_create(server_config, &server) == PROVEN_OK, "서버");
    EXAMPLE_REQUIRE(proven_tls_pending_output(client).size > 0 && proven_tls_pending_output(server).size == 0, "클라이언트가 먼저 말한다");

    /* 둘 다 끝날 때까지 바이트를 오가게 한다. 왕복 두 번이면 된다. */
    for (int round = 0; round < 10 && !(proven_tls_is_established(client) && proven_tls_is_established(server)); ++round) {
        EXAMPLE_REQUIRE(carry(client, server) == PROVEN_OK, "클라이언트에서 서버로");
        EXAMPLE_REQUIRE(carry(server, client) == PROVEN_OK, "서버에서 클라이언트로");
    }
    EXAMPLE_REQUIRE(proven_tls_is_established(client) && proven_tls_is_established(server), "양쪽이 수립되었다");
    EXAMPLE_REQUIRE(proven_tls_cipher_suite(client) == proven_tls_cipher_suite(server) && proven_tls_cipher_suite(client) != 0, "암호 스위트에 합의했다");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_tls_alpn(client), PROVEN_LIT("example/1")), "애플리케이션 프로토콜에도");
    EXAMPLE_REQUIRE(!proven_tls_resumed(client), "이번은 전체 핸드셰이크였다");

    // ---- 애플리케이션 데이터 --------------------------------------------------------
    /* 쓰기는 보류 출력 안으로 암호화하고, 읽기는 넣어 준 것을 복호화한 결과를 준다. */
    proven_result_size_t wrote = proven_tls_write(client, proven_mem_view_from_u8(PROVEN_LIT("hello, server")));
    EXAMPLE_REQUIRE(wrote.err == PROVEN_OK && wrote.value == 13, "열세 바이트를 받았다");
    proven_mem_view_t wire = proven_tls_pending_output(client);
    EXAMPLE_REQUIRE(wire.size > 13 && memchr(wire.ptr, 'h', wire.size) == NULL, "선 위로 가는 것은 레코드이고, 그 텍스트가 아니다");
    EXAMPLE_REQUIRE(carry(client, server) == PROVEN_OK, "클라이언트에서 서버로");
    proven_byte_t buf[64];
    proven_result_size_t got = proven_tls_read(server, (proven_mem_mut_t){ buf, sizeof buf });
    EXAMPLE_REQUIRE(got.err == PROVEN_OK && got.value == 13 && memcmp(buf, "hello, server", 13) == 0, "서버가 클라이언트가 쓴 것을 읽는다");
    EXAMPLE_REQUIRE(proven_tls_read(server, (proven_mem_mut_t){ buf, sizeof buf }).err == PROVEN_ERR_NEED_MORE, "그 뒤로는 아직 아무것도 없다");

    // ---- 닫기 ----------------------------------------------------------------------
    /* 닫기도 메시지다: 끝이 끊어진 선이 아니라 정말 끝이라는 것을 상대에게 알린다. */
    EXAMPLE_REQUIRE(proven_tls_close(client) == PROVEN_OK && carry(client, server) == PROVEN_OK, "닫기가 전달된다");
    EXAMPLE_REQUIRE(proven_tls_read(server, (proven_mem_mut_t){ buf, sizeof buf }).err == PROVEN_ERR_EOF, "서버의 읽기가 제대로 된 끝을 보고한다");
    proven_tls_conn_destroy(client);
    proven_tls_conn_destroy(server);

    // ---- 재개 ----------------------------------------------------------------------
    /* 첫 연결이 세션에 티켓을 남겼다. 같은 세션을 받은 두 번째 연결은 그것을 내밀고,
     * 핸드셰이크는 인증서와 서명 하나를 건너뛴다. */
    EXAMPLE_REQUIRE(proven_tls_client_create(client_config, PROVEN_LIT("example.test"), &session, &client) == PROVEN_OK &&
                    proven_tls_server_create(server_config, &server) == PROVEN_OK, "두 번째 쌍");
    for (int round = 0; round < 10 && !(proven_tls_is_established(client) && proven_tls_is_established(server)); ++round) {
        EXAMPLE_REQUIRE(carry(client, server) == PROVEN_OK && carry(server, client) == PROVEN_OK, "클라이언트에서 서버로");
    }
    EXAMPLE_REQUIRE(proven_tls_resumed(client) && proven_tls_resumed(server), "양쪽이 재개했다");
    proven_tls_conn_destroy(client);
    proven_tls_conn_destroy(server);

    // ---- 요청한 서버가 아닌 서버 ---------------------------------------------------
    /* 같은 서버에, 인증서에 없는 이름으로 닿는다. 클라이언트는 거부하고 이유를 정확히 말한다. */
    proven_tls_conn_t *wrong = NULL;
    EXAMPLE_REQUIRE(proven_tls_client_create(client_config, PROVEN_LIT("elsewhere.test"), NULL, &wrong) == PROVEN_OK &&
                    proven_tls_server_create(server_config, &server) == PROVEN_OK, "두 번째 쌍");
    EXAMPLE_REQUIRE(carry(wrong, server) == PROVEN_OK, "클라이언트에서 서버로");
    EXAMPLE_REQUIRE(carry(server, wrong) == PROVEN_ERR_NAME_MISMATCH, "클라이언트의 feed가 PROVEN_ERR_NAME_MISMATCH를 돌려준다");
    EXAMPLE_REQUIRE(proven_tls_peer_fault(wrong) == PROVEN_CERT_FAULT_NAME_MISMATCH && !proven_tls_is_established(wrong), "정확한 fault와 함께, 연결은 없다");
    /* 실패는 보류 출력에 경고를 남긴다. 보내라: 상대가 알게 되는 길이다. */
    EXAMPLE_REQUIRE(proven_tls_pending_output(wrong).size > 0 && carry(wrong, server) == PROVEN_ERR_PROTOCOL, "서버가 통보받는다");
    EXAMPLE_REQUIRE(proven_tls_alert_received(server) == 42, "bad_certificate, 그 번호로");
    proven_tls_conn_destroy(wrong);
    proven_tls_conn_destroy(server);

    proven_mem_wipe((proven_mem_mut_t){ session.opaque, sizeof session.opaque });
    proven_mem_wipe((proven_mem_mut_t){ key_pem, sizeof key_pem });
    proven_tls_config_destroy(client_config);
    proven_tls_config_destroy(server_config);
    proven_cert_store_destroy(anchors);
    return EXAMPLE_OK();
}
