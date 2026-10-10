# 14장: TLS

**5부 — 운영체제와 대화하기. 선행 조건: 인증서와 신뢰 앵커를 다루는 [13장](manual-13-certificates-ko.md).
HTTPS는 [11장](manual-11-http-client-server-ko.md), `wss`는 [12장](manual-12-websocket-ko.md).**
**이 장을 마치면** HTTPS로 서비스하고 가져오고, `wss` 연결을 열고, 여러분 자신의 어떤 연결이든 TLS로
감싸고, 소켓이 없는 곳에서는 프로토콜을 직접 몰 수 있다 - 그리고 프로그램이 상대에 대해 무엇을
믿는지, 왜 믿는지 말할 수 있다.

이 장은 `tls.h`를 다룬다. 그 안의 엔진은 운영체제를 부르지 않으며
[프리스탠딩(freestanding)](manual-freestanding-ko.md) 빌드에서 쓸 수 있다. 전송 래퍼와 소켓에 닿는
모든 것은 호스티드(hosted) 전용이고 `PROVEN_NO_NET`으로 빠진다.

**먼저 읽을 것.** 이것은 외부 감사를 받지 않은 새 TLS 구현이다. 프로토콜의 공표된 예시
핸드셰이크와, 다른 구현 둘과 양쪽 역할로, 그리고 적대적 입력 모음과 대조해 시험했다(정확히 무엇을
했고 무엇이 불가능했는지는 9절). 그것은 증거이지 증명이 아니다. 능력 있는 공격자를 상대로 사용자가
TLS에 의존하는 프로그램이라면 감사받은 것에서 TLS를 끝내고, 이 라이브러리는 그 뒤에서 써야 한다.

## 목차

1. [TLS가 하는 일, 한 쪽으로](#1-tls가-하는-일-한-쪽으로)
2. [설정](#2-설정)
3. [HTTPS와 wss: 두 줄](#3-https와-wss-두-줄)
4. [어떤 연결이든: 전송 래퍼](#4-어떤-연결이든-전송-래퍼)
5. [엔진](#5-엔진)
6. [재개](#6-재개)
7. [클라이언트 인증서와 핀](#7-클라이언트-인증서와-핀)
8. [실패할 때](#8-실패할-때)
9. [무엇을 협상하고, 무엇을 붙들고, 무엇이 없는가](#9-무엇을-협상하고-무엇을-붙들고-무엇이-없는가)
10. [레거시 묶음: 설정 하나하나가 무엇에 노출시키는가](#10-레거시-묶음-설정-하나하나가-무엇에-노출시키는가)

## 1. TLS가 하는 일, 한 쪽으로

TCP 연결은 파이프 하나를 준다. 두 끝 사이의 누구나 - 카페의 무선 공유기, 인터넷 사업자, 라우터를
장악한 누군가 - 그 안을 지나는 것을 읽고 바꿀 수 있으며, 어느 끝도 그것을 알아챌 수 없다.
TLS(Transport Layer Security. 이 라이브러리는 버전 1.3(RFC 8446)과, 줄여 낸 버전 1.2(RFC 5246)를
말한다 - 어느 부분인지는 9절에 있다. 더 오래된 부분은 설정이 이름을 들어 요구할 때에만 있다, 10절)는 그
파이프 위에 세 가지 성질을 얹는다.

| 성질 | 뜻 | 무엇으로 만드는가 |
|---|---|---|
| **인증** | 반대쪽 끝이 닿으려던 그 상대다 | 13장대로 검사한 상대의 인증서와, 인증서의 키를 갖고 있음을 증명하는 서명 |
| **기밀성** | 중간의 누구도 데이터를 읽을 수 없다 | 중간에서 지켜보고도 계산할 수 없는 키 교환으로 합의한 키 |
| **무결성** | 중간의 누구도 데이터를 들키지 않고 바꾸거나 버리거나 순서를 바꿀 수 없다 | 모든 레코드에는 키를 가진 쪽만 만들 수 있는 태그가 붙는다. 깨끗한 끝도 그 자체가 메시지다 |

두 단계로 일어난다. **핸드셰이크**는 처음의 메시지 몇 개다. 양쪽이 알고리즘에 합의하고, 키 몫을
교환하고, 서버가(선택적으로 클라이언트도) 인증서를 내밀어 그 키의 주인임을 증명하고, 둘이 같은
비밀 키를 유도한다. 그 뒤로는 **레코드**가 암호화되고 태그가 붙은 채 여러분의 데이터를 나른다.

한 줄을 쓰기 전에 알아 둘 귀결 셋:

- **이름이 절반이다.** 검증하지 않은 상대에게 하는 암호화는 대답한 아무에게나 하는 암호화다.
  클라이언트는 어느 이름에 닿으려 했는지 알아야 하고, 인증서는 그 이름을 위한 것이어야 한다.
- **깨끗한 닫기가 중요하다.** 연결이 그냥 멈추면 TLS는 "상대가 끝냈다"와 "누가 나머지를 숨기려고
  선을 끊었다"를 구별할 수 없다. 그래서 닫기는 메시지이고, 그것 없이 끝난 연결은 데이터의 끝이
  아니라 에러로 보고된다.
- **비용이 든다.** 핸드셰이크는 공개 키 연산이다. 이 구현의 숫자는 9절에 있고, 작지 않다.

## 2. 설정

```c
proven_err_t proven_tls_config_create(const proven_tls_options_t *options, proven_tls_config_t **out);
void proven_tls_config_destroy(proven_tls_config_t *config);
```

`proven_tls_config_t`는 모든 연결에 똑같은 것을 담는다. 누구를 믿을지, 여러분이 누구인지, 어떤
애플리케이션 프로토콜을 말하는지. **한 번 만들면 바뀌지 않으므로** 설정 하나를 모든 연결과 모든
스레드가 함께 쓴다. `proven_tls_config_create`는 받은 것을 복사하고 - 앵커 저장소만 예외로, 설정보다
오래 살아야 한다 - `proven_tls_config_destroy`는 해제하면서 개인 키를 지운다.

`proven_tls_options_t`를 0으로 초기화하고 `alloc`을 채운 뒤 해당하는 것을 채운다.

| 필드 | 누구에게 | 뜻 |
|---|---|---|
| `alloc` | 양쪽 | 필수. 이 설정으로 만든 연결도 이 할당자(allocator)로 할당한다(예제에서는 힙(heap) 할당자) |
| `anchors` | 검증하는 쪽 | 신뢰 앵커(13장). 클라이언트에는 필요하다. 서버는 클라이언트 인증서를 검사할 때만 필요하다 |
| `verify` | 검증하는 쪽 | `PROVEN_TLS_VERIFY_CHAIN`(기본): 앵커까지의 체인, 지금 유효, 기대한 이름. `PROVEN_TLS_VERIFY_PIN_ONLY`: 7절 |
| `pins`, `pin_count` | 검증하는 쪽 | 공개 키 핀(7절) |
| `certificate_pem`, `private_key_pem` | 서버. 인증서를 가진 클라이언트 | 이쪽의 인증서들(자기 것이 먼저)과 키. 키는 P-256, Ed25519, 또는 2048~4096비트의 RSA이고, PKCS #8 블록(라벨 `PRIVATE KEY`)이나 `EC PRIVATE KEY` 블록이나 `RSA PRIVATE KEY` 블록, 암호화되지 않은 것 |
| `client_auth` | 서버 | 클라이언트에게 인증서를 요구할지(7절) |
| `alpn`, `alpn_count` | 양쪽 | 애플리케이션 프로토콜 이름, 선호하는 순서(`http/1.1`). 서버가 클라이언트가 내민 것 가운데 자기 첫 선택을 고른다. 양쪽에 목록이 있고 겹치는 것이 없으면 핸드셰이크는 실패한다 |
| `random`, `now` | 양쪽 | 예측할 수 없는 바이트와 시각의 출처. 호스티드 시스템에서는 널로 둔다: 운영체제의 원천과 벽시계를 쓴다. 프리스탠딩 빌드는 둘 다 공급해야 한다 |
| `no_resumption`, `ticket_lifetime_s` | 양쪽, 서버 | 6절 |
| `keep_peer_certificate` | 검증하는 쪽 | 핸드셰이크 뒤에도 상대의 인증서를 보관한다(`proven_tls_peer_certificate`용) |
| `max_handshake_bytes` | 양쪽 | 받아들이는 가장 큰 핸드셰이크 메시지. 기본 65,536 |
| `min_version`, `max_version` | 양쪽 | 이쪽이 동의하는 가장 오래된, 가장 새로운 프로토콜 버전: `PROVEN_TLS_VERSION_1_2` 또는 `PROVEN_TLS_VERSION_1_3`. 0이 기본값이다 - 최소는 1.2, 최대는 1.3. **TLS 1.2를 거부하려면 `min_version = PROVEN_TLS_VERSION_1_3`으로 둔다**(9절). `PROVEN_TLS_VERSION_1_1`과 `PROVEN_TLS_VERSION_1_0`은 레거시다(10절) |
| `legacy` | 양쪽 | `PROVEN_TLS_LEGACY_*` 비트: 여기서 이름을 들지 않으면 빠져 있는 TLS의 부분들. 기본값 0은 그 어느 것도 아니다. **하나라도 켜기 전에 10절을 읽을 것** |

**`anchors`, `verify`, `pins`는 어느 역할에서든 상대를 어떻게 검증하는지를 말한다.** 아무것도
검증하지 않는 설정은 없다. 체인을 검사할 클라이언트에 앵커가 없으면 설정을 만들 때 거부된다. 공인
기관을 건너뛰고 싶은 테스트는 자기 루트를 만들거나 키를 핀으로 삼는다.

**옵션에서 틀릴 수 있는 것은 모두 여기서 에러가 된다** - 남의 기계에서 새벽 세 시에 일어나는 첫
핸드셰이크가 아니라.

| 반환 | 언제 |
|---|---|
| `PROVEN_ERR_INVALID_ARG` | 할당자가 없다. 이쪽이 체인을 검증하는데 앵커가 없다. 핀 없는 `PIN_ONLY`. 키 없는 인증서 또는 인증서 없는 키. 인증서가 8개를 넘는다. 빈 ALPN 이름. 존재하지 않는 버전, 또는 최대보다 큰 최소. `PROVEN_TLS_LEGACY_CBC` 없이 TLS 1.2 미만인 최소. 존재하지 않는 `legacy` 비트 |
| `PROVEN_ERR_INVALID_FORMAT` | 파싱되지 않는 PEM, 또는 인증서나 키가 들어 있지 않은 PEM. 2048~4096비트 밖이거나, 부분들이 서로 맞지 않거나, 서명하지 못하는 RSA 키 |
| `PROVEN_ERR_UNSUPPORTED` | 다른 종류의 키 - 예컨대 P-384 - 또는 암호화된 키 파일 |
| `PROVEN_ERR_INVALID_STATE` | 키가 인증서의 키가 아니다 |
| `PROVEN_ERR_NOMEM` | 할당자가 거부했다 |

**어느 키를 고를까.** 세 종류 모두 연결의 어느 쪽에서든 쓸 수 있다. 비용은 같지 않다: RSA 키로는
이쪽이 서명하는 전체 핸드셰이크마다 P-256이나 Ed25519보다 몇 배 오래 걸리고(수치는 9절에 있다), 서버에서는
그것이 핸드셰이크의 가장 큰 부분이다. 이미 가진 인증서가 요구하는 곳에서는 RSA를, 새로 만드는 것이라면
P-256이나 Ed25519를 쓴다. RSA 키는 설정을 만들 때 시험된다 - 서명 하나를 만들고 검사한다 - 그래서 손상된
키 파일은 첫 연결이 아니라 거기서 거절된다.

**기관 없는 신원.**

```c
proven_err_t proven_tls_self_signed(const proven_u8str_view_t *names, proven_size_t name_count,
                                    proven_i64 not_before, proven_i64 not_after,
                                    proven_tls_random_fn random, void *random_ctx,
                                    proven_mem_mut_t certificate_pem, proven_size_t *certificate_len,
                                    proven_mem_mut_t private_key_pem, proven_size_t *private_key_len);
```

`proven_tls_self_signed`는 Ed25519 키와, 바로 그 키로 서명한 인증서를 둘 다 PEM으로 만든다.
인증서는 `names`(DNS 이름 또는 IP 리터럴)를 담고, 두 시각 사이에 유효하며, 서버로도 클라이언트로도
쓸 수 있다. 이름 몇 개라면 1,024바이트와 256바이트에 들어간다. 버퍼가 작으면
`PROVEN_ERR_OUT_OF_BOUNDS`다.

**자체 서명 인증서는 아무도 보증하지 않는다.** 상대는 그것을 이미 갖고 있어야만 믿는다. 인증서를
상대의 앵커 저장소에 넣거나, 상대에게 키의 핀과 `PROVEN_TLS_VERIFY_PIN_ONLY`를 준다(7절). 직접
운영하는 기계 사이에서, 그리고 개발과 테스트에서 옳은 방식이다. 기관만 믿는 브라우저를 쓰는 일반
사용자가 닿게 하는 방법은 아니다. 키는 여러분이 안전하게 보관하고 지워야 한다.

## 3. HTTPS와 wss: 두 줄

HTTP에서는 설정 말고 아무것도 바뀌지 않는다.

**클라이언트**는 이 라이브러리가 주는 랩 함수와, 그 컨텍스트로 쓸 설정을 받으면 `https` URL을
쓸 수 있다.

```c
proven_err_t proven_tls_http_wrap(void *config, proven_transport_t plain, proven_u8str_view_t host,
                                  proven_net_deadline_t until, proven_transport_t *out);
```

11장의 `proven_http_client_config_t`에서 `tls_wrap = proven_tls_http_wrap`으로, `tls_ctx`를 여러분의
`proven_tls_config_t *`로 둔다. 그때부터 `https` URL 요청은 연결하고 - 프록시가 있으면 프록시
터널을 지나 - URL의 호스트를 상대로 핸드셰이크를 하고 진행한다. `proven_ws_conn_connect`에 준 `wss`
URL도 같다. 공개 인터넷이라면 설정의 앵커는 시스템의 것이다(`proven_cert_store_add_system`). 검증할
수 없는 서버는 요청에서 `PROVEN_ERR_UNTRUSTED`, `PROVEN_ERR_EXPIRED`, `PROVEN_ERR_NOT_YET_VALID`
또는 `PROVEN_ERR_NAME_MISMATCH`가 되고, **요청의 어떤 부분도 보내지지 않았다**.

**서버**는 `proven_http_server_config_t`의 `tls`에 인증서를 가진 설정을 두면 TLS로 말한다. 그러면
받아들이는 모든 연결이 TLS다. 그 포트에 평문 HTTP를 보낸 클라이언트는 끊긴다. 핸들러는 바뀌지
않는다. 핸들러가 돌 때쯤 요청은 평문이고, 핸들러가 쓰는 것은 나가는 길에 암호화된다. 그런
핸들러에서 수락한 WebSocket은 `wss`다.

핸드셰이크는 클라이언트의 바이트가 도착하는 대로 서버 자신의 루프가 몬다. 연결 하나를 기다리는
일이 없으므로, 연결만 하고 아무 말도 하지 않는 클라이언트는 `head_timeout_ms`까지 자리 하나를
차지할 뿐 - 새 연결의 핸드셰이크는 이 시간을 첫 요청의 헤드와 함께 쓴다 - 누구도 붙잡지 않는다.

`http`와 `https`를 함께 서비스하려면 서버를 둘 돌린다. 서버 하나는 둘 중 하나다.

테스트 스위트가 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_14_tls_https.c -->
```c
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
```

## 4. 어떤 연결이든: 전송 래퍼

```c
proven_err_t proven_tls_transport_client(proven_transport_t plain, const proven_tls_config_t *config,
                                         proven_u8str_view_t server_name, proven_tls_session_t *session,
                                         proven_net_deadline_t until, proven_transport_t *out);
proven_err_t proven_tls_transport_server(proven_transport_t plain, const proven_tls_config_t *config,
                                         proven_net_deadline_t until, proven_transport_t *out);
proven_tls_conn_t *proven_tls_transport_conn(proven_transport_t tls);
```

`proven_transport_t`(9장)는 인터페이스로서의 연결이다: 읽기, 쓰기, 종료, 닫기.
`proven_tls_transport_client`와 `proven_tls_transport_server`는 하나를 받아 `until` 안에 그 위에서
핸드셰이크를 하고 다른 하나를 돌려준다 - 3절의 HTTP 랩이 이것으로 만들어졌고, 여러분 자신의
프로토콜에 쓰는 것도 이것이다.

- **성공하면 새 전송이 옛 전송을 소유(owned)한다.** 새 전송을 닫으면 TLS 작별을 고하고 밑의 연결을
  닫는다. `plain`을 다시 쓰지 마라.
- **실패하면 `plain`은 손대지 않은 채 여전히 여러분의 것**이고, 닫는 것도 여러분이다.
- `server_name`은 클라이언트가 닿으려던 이름이다. 서버에 보내지고, 서버의 인증서와 대조된다.
- 읽기가 `PROVEN_ERR_EOF`를 돌려주면 상대가 제대로 닫은 것이다. 다른 식으로 끝난 연결은
  `PROVEN_ERR_RESET`이다. 받은 것을 완전하다고 여기지 마라.
- `proven_tls_transport_conn`은 안의 엔진을 준다 - 5절의 질문들(어느 스위트, 어느 프로토콜, 누구의
  키)을 위해서. 이 둘이 만든 것이 아닌 전송에는 널을 돌려준다.

읽기와 쓰기는 모든 전송이 그렇듯 기한을 받는다. 읽기는 써야 할 때가 있고(프로토콜이 가끔 상대에게
메시지를 빚진다) 쓰기는 읽어야 할 일이 없다.

## 5. 엔진

래퍼 아래에는 I/O가 없는 상태 기계가 있다. 도착한 바이트를 주면, 보낼 바이트를 준다. 전송이
여러분의 것일 때 - 이벤트 루프, 직렬 선, 테스트 안의 메모리 버퍼 - 또는 운영체제가 아예 없을 때
직접 쓴다.

```c
proven_err_t proven_tls_client_create(const proven_tls_config_t *config, proven_u8str_view_t server_name,
                                      proven_tls_session_t *session, proven_tls_conn_t **out);
proven_err_t proven_tls_server_create(const proven_tls_config_t *config, proven_tls_conn_t **out);
void proven_tls_conn_destroy(proven_tls_conn_t *conn);

proven_err_t proven_tls_feed(proven_tls_conn_t *conn, proven_mem_view_t in, proven_size_t *consumed);
proven_mem_view_t proven_tls_pending_output(proven_tls_conn_t *conn);
void proven_tls_output_sent(proven_tls_conn_t *conn, proven_size_t n);

bool proven_tls_is_established(const proven_tls_conn_t *conn);
proven_result_size_t proven_tls_read(proven_tls_conn_t *conn, proven_mem_mut_t dest);
proven_result_size_t proven_tls_write(proven_tls_conn_t *conn, proven_mem_view_t src);
proven_err_t proven_tls_close(proven_tls_conn_t *conn);
```

쓰는 법 전부가 루프 하나다.

1. **`proven_tls_pending_output`이 보여 주는 것은 무엇이든 보내고**, 얼마가 나갔는지
   `proven_tls_output_sent`로 알린다. 클라이언트는 만들어지는 순간 출력이 있다.
2. **도착한 것은 무엇이든 `proven_tls_feed`에 준다.** 레코드 하나 전체일 필요도, 하나뿐일 필요도
   없다. 미완성 레코드는 안에 보관된다.
3. **feed를 할 때마다 `proven_tls_read`를 `PROVEN_ERR_NEED_MORE`라고 할 때까지 부른다.**
4. `proven_tls_is_established`가 되면 `proven_tls_write`가 여러분의 데이터를 보류 출력 안으로
   암호화한다. 1로 간다.

이 루프에서 틀리기 쉬운 규칙 둘:

- **`proven_tls_feed`는 준 것보다 적게 소비할 수 있다.** 여러분이 아직 읽지 않은 복호화된
  애플리케이션 데이터가 있으면 멈추고, 가져간 양을 `*consumed`에 적는다. 읽은 다음 나머지를 준다.
  소비되지 않은 부분을 버리는 호출자는 데이터를 잃는다.
- **`proven_tls_pending_output`의 뷰(view)는 그 연결에 대한 다음 호출까지만 유효하다.** 거기서
  바로 보내거나 복사하라. feed를 넘겨 들고 있지 마라.

`proven_tls_read`는 상대가 제대로 닫았고 그 전의 것을 모두 읽었으면 `PROVEN_ERR_EOF`를 돌려준다.
`proven_tls_close`는 이쪽의 작별을 보류 출력에 넣는다. 그 뒤로는 더 쓸 수 없고, 읽기는 상대도 닫을
때까지 계속된다. `proven_tls_write`는 보내지 않은 출력이 64 KiB를 넘게 쌓여 있지 않은 한 전부
받는다. 넘으면 들어가는 만큼만 - 어쩌면 0 - 받고, 여러분은 먼저 얼마간 보낸다.

연결은 한 번에 한 스레드가 쓴다. 설정과 그 밑의 앵커 저장소는 몇이든 함께 쓸 수 있다.

수립된 뒤, 무엇에 합의했는가:

```c
proven_u16 proven_tls_version(const proven_tls_conn_t *conn);
proven_u16 proven_tls_cipher_suite(const proven_tls_conn_t *conn);
proven_u8str_view_t proven_tls_alpn(const proven_tls_conn_t *conn);
bool proven_tls_resumed(const proven_tls_conn_t *conn);
bool proven_tls_peer_key_sha256(const proven_tls_conn_t *conn, proven_byte_t out[32]);
proven_mem_view_t proven_tls_peer_certificate(const proven_tls_conn_t *conn);
proven_u8str_view_t proven_tls_server_name(const proven_tls_conn_t *conn);
proven_err_t proven_tls_key_update(proven_tls_conn_t *conn);
```

- `proven_tls_version`은 `PROVEN_TLS_VERSION_1_3` 또는 `PROVEN_TLS_VERSION_1_2`(또는, 허락한 설정에서는
  두 레거시 버전 중 하나)이고, 상대의 hello가
  그것을 정하기 전에는 0이다. 무언가를 TLS 1.2 위로 실어서는 안 되는 프로그램은 여기서 확인하거나 -
  더 낫게는 - `min_version`을 두어 거기까지 가지 않는다.
- `proven_tls_cipher_suite`는 스위트의 번호(9절), `proven_tls_alpn`은 합의한 애플리케이션
  프로토콜이며 없으면 비어 있다.
- `proven_tls_peer_key_sha256`은 상대 공개 키의 해시 - 핀이 담는 값 - 를 주고, 상대가 인증서를
  내밀지 않았으면 `false`를 돌려준다. 인증서 자체는 설정이 `keep_peer_certificate`를 켜지 않은 한
  핸드셰이크 뒤에 보관하지 않는다. 켰다면 `proven_tls_peer_certificate`가 `proven_cert_parse`에 넣을
  수 있게 돌려준다.
- `proven_tls_server_name`은 서버 역할에서, 핸드셰이크 동안에만, 클라이언트가 요청한 이름이다.
- `proven_tls_key_update`는 이쪽의 송신 키를 다음 키로 바꾼다. 엔진은 키가 너무 많은 것을
  보호하기 한참 전에 스스로 이렇게 한다. 정책이 더 일찍 원할 때 부른다. TLS 1.2에는 그런 메시지가
  없다: 거기서는 `PROVEN_ERR_UNSUPPORTED`를 돌려주고, 레코드를 2^31개 보낸 연결은 더 쓰기를
  거부한다(`PROVEN_ERR_OVERFLOW`) - 새 연결을 연다.

테스트 스위트가 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_14_tls_engine.c -->
```c
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

/* `text`의 바이트가 그 순서대로 `data` 어딘가에 나타나면 참. */
static bool appears(proven_mem_view_t data, const char *text) {
    proven_size_t n = strlen(text);
    for (proven_size_t i = 0; i + n <= data.size; ++i) {
        if (memcmp(data.ptr + i, text, n) == 0) return true;
    }
    return false;
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
    EXAMPLE_REQUIRE(proven_tls_version(client) == PROVEN_TLS_VERSION_1_3 && proven_tls_version(server) == PROVEN_TLS_VERSION_1_3, "양쪽이 가진 가장 새 버전인 TLS 1.3을 말했다");
    EXAMPLE_REQUIRE(proven_tls_cipher_suite(client) == proven_tls_cipher_suite(server) && proven_tls_cipher_suite(client) != 0, "암호 스위트에 합의했다");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_tls_alpn(client), PROVEN_LIT("example/1")), "애플리케이션 프로토콜에도");
    EXAMPLE_REQUIRE(!proven_tls_resumed(client), "이번은 전체 핸드셰이크였다");

    // ---- 애플리케이션 데이터 --------------------------------------------------------
    /* 쓰기는 보류 출력 안으로 암호화하고, 읽기는 넣어 준 것을 복호화한 결과를 준다. */
    proven_result_size_t wrote = proven_tls_write(client, proven_mem_view_from_u8(PROVEN_LIT("hello, server")));
    EXAMPLE_REQUIRE(wrote.err == PROVEN_OK && wrote.value == 13, "열세 바이트를 받았다");
    proven_mem_view_t wire = proven_tls_pending_output(client);
    EXAMPLE_REQUIRE(wire.size > 13 && !appears(wire, "hello, server"), "선 위로 가는 것은 레코드이고, 그 텍스트가 아니다");
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
```

## 6. 재개

전체 핸드셰이크는 서명 둘과 인증서 체인 하나가 든다. 전에 서버와 대화한 적이 있는 클라이언트는
그것을 건너뛸 수 있다. 서버가 첫 핸드셰이크 끝에 **티켓**을 줬고, 그 티켓을 내미는 두 번째 연결은
양쪽이 이미 비밀을 공유한다는 것을 그것으로 증명한다. 새 키 교환은 여전히 일어나므로 새 연결의
키는 그 연결만의 것이다.

클라이언트에서는 구조체 하나다.

```c
typedef struct { proven_byte_t opaque[PROVEN_TLS_SESSION_SIZE]; } proven_tls_session_t;
```

`proven_tls_session_t`를 0으로 초기화하고, 같은 서버 이름으로 가는 연결마다
`proven_tls_client_create`(또는 `proven_tls_transport_client`)에 넘긴다. 서버가 티켓을 보내면 연결이
거기에 적어 넣고, 다음 연결이 그것을 내민다. 서버가 받아들였는지는 `proven_tls_resumed`가 말한다.
포인터가 없는 평범한 바이트 블록이므로 어디에든, 얼마 동안이든 두어도 되고, **비밀을 담고 있으므로**
다 쓰면 `proven_mem_wipe`로 지운다. 만들어진 그 이름에만 내밀고, 서버가 준 수명을 넘겨서는 절대
내밀지 않는다. 세션 하나는 한 번에 연결 하나를 섬긴다.

서버에서는 할 일이 없다. `no_resumption`이 켜져 있지 않으면 티켓을 발급하고, `ticket_lifetime_s`
(기본 두 시간, 최대 일주일) 동안 인정한다. **서버는 클라이언트별로 아무것도 저장하지 않는다.**
티켓은 이 프로세스만 가진 키로 봉인한 세션 상태다. 그 키는 설정을 만들 때 생기고, 티켓 수명마다
교체되며, 나가 있는 티켓이 계속 열리도록 수명 하나만큼 더 보관되고, 어디에도 기록되지 않는다 -
그래서 티켓은 재시작을 넘기지 못하고, 두 프로세스는 서로의 티켓을 받지 않는다.

여기서 재개가 하지 않는 것:

- **조기 데이터(0-RTT) 없음.** 핸드셰이크가 끝나기 전에 보낸 데이터는 공격자가 재전송할 수 있다.
  안전하게 쓰려면 그것을 위해 쓰인 애플리케이션이 필요하고, 이 라이브러리는 제공하지 않는다.
- **`proven_tls_http_wrap`을 통해서는 안 된다.** HTTP 클라이언트의 연결은 재개하지 않는다. 대신
  열린 연결을 다시 쓰며, 그쪽이 더 싸다.
- 재개된 연결의 상대는 세션이 만들어질 때 검증된 그 상대다. 인증서는 다시 보내지지 않으므로 다시
  검사되지 않고, 그 사이에 지난 만료는 알아채지 못한다. 티켓 수명이 그것이 문제될 수 있는 기간을
  제한한다.

**TLS 1.2에서 재개된 연결은 새 연결보다 약하며, 어떻게 약한지 알아야 한다.** 새 키 교환이 없다: 새
연결의 키는 첫 연결의 마스터 시크릿과 새 난수 둘에서 나온다. 그래서 나중에 서버의 티켓 키를 얻는
자는 그 키가 봉인한 티켓으로 재개된 모든 연결을, 그리고 첫 연결의 시크릿까지 읽을 수 있다 - 전체
핸드셰이크와 TLS 1.3의 재개는 이것을 허락하지 않는다. 여기서 티켓 키는 프로세스의 메모리에만 있고 티켓
수명마다 교체되므로 노출은 그만큼으로 제한된다. `no_resumption`은 그것을 없앤다. 한 버전에서 만든
세션은 다른 버전에 내놓지도, 다른 버전에서 받아들이지도 않는다: `proven_tls_session_t` 하나는 마지막
연결이 남긴 종류를 담고, 다른 버전으로 끝나는 핸드셰이크는 그저 전체 핸드셰이크가 된다.

## 7. 클라이언트 인증서와 핀

**클라이언트 인증서.** 서버는 클라이언트도 자신이 누구인지 증명하기를 요구할 수 있다 - 상호 TLS,
서비스 사이에서 흔한 선택. 서버에서 `client_auth`를 정하고 `anchors`를 준다.

| `client_auth` | 인증서 없는 클라이언트 | 검증되지 않는 인증서를 가진 클라이언트 |
|---|---|---|
| `PROVEN_TLS_CLIENT_AUTH_NONE` | (요구하지 않음) | (요구하지 않음) |
| `PROVEN_TLS_CLIENT_AUTH_REQUEST` | 들여보낸다. `proven_tls_peer_key_sha256`이 `false`를 돌려준다 | 거부 |
| `PROVEN_TLS_CLIENT_AUTH_REQUIRE` | 거부 | 거부 |

클라이언트에서는 `certificate_pem`과 `private_key_pem`을 정한다. 요구받으면 내밀고 아니면 내밀지
않는다. 서버가 알게 되는 것은 클라이언트가 어떤 기관이 보증한 키를 갖고 있다는 *사실*이다. *어느*
클라이언트인지는 여러분이 정한다 - `proven_tls_peer_key_sha256`으로, 또는
인증서(`keep_peer_certificate`)와 그 이름으로 - 그리고 `PROVEN_TLS_CLIENT_AUTH_REQUEST`는 "증명했는지"
조차 여러분에게 남긴다. 요청에 따라 행동하기 전에 확인하라.

HTTP 서버에서는 엔진이 서버 안에 있고, 이 버전에서 핸들러는 거기에 닿을 호출이 없다. 거기서는
`REQUIRE`를 써서 핸들러가 보는 모든 요청이 검증된 클라이언트에게서 온 것이 되게 하라.

**핀.** 핀은 공개 키의 SHA-256이다(13장 7절). `pins`를 주면 상대의 키가 그 가운데 하나여야 한다.

- `PROVEN_TLS_VERIFY_CHAIN`에서는 체인에 **더해서**. 그러면 세상 어느 기관이 낸 유효한 인증서도
  충분하지 않다. 기대한 키를 위한 것이어야 한다.
- `PROVEN_TLS_VERIFY_PIN_ONLY`에서는 체인 **대신**. 체인도, 날짜도, 이름도 없다 - 키가 신원이고
  `server_name`은 비어 있어도 된다. 자체 서명 인증서로 직접 운영하는 상대를 위한 모드이며, 핀이
  보안의 전부라는 뜻이다.

핀은 둘 - 쓰고 있는 키와 예비 - 실어 보내라. 그렇지 않으면 잃어버린 키 하나가 연결하지 못하는
기기 무리가 된다.

## 8. 실패할 때

연결은 한 번, 에러 하나로 실패하고, 실패한 채로 남는다.

| 에러 | 무슨 일이 있었나 | 흔한 원인 |
|---|---|---|
| `PROVEN_ERR_UNTRUSTED` | 상대의 인증서가 앵커로 이어지지 않거나, 핀에 없거나, 필수 클라이언트 인증서를 내밀지 않았다(상대에게는 TLS 1.3에서 `certificate_required`, 더 가까운 말이 없는 1.2에서는 `handshake_failure`로 알린다) | 사설 또는 자체 서명 기관. 빠진 중간 인증서. Windows에서는 기계가 아직 받아 오지 않은 루트(13장) |
| `PROVEN_ERR_EXPIRED`, `PROVEN_ERR_NOT_YET_VALID` | 경로 위의 인증서가 유효 기간 밖이다 | 갱신하지 않은 인증서 - 또는 이 기계의 시계 |
| `PROVEN_ERR_NAME_MISMATCH` | 인증서는 유효하고, 다른 이름을 위한 것이다 | 틀린 URL, 잘못 설정된 서버, 또는 가로채기 |
| `PROVEN_ERR_PROTOCOL` | 상대가 한 그 밖의 모든 것: 이쪽이 말하지 않는 버전이나 알고리즘, 형식이 틀렸거나 예상하지 못한 메시지, 인증되지 않는 레코드 - 또는 상대가 자기 경고로 핸드셰이크를 끝냈다 | TLS 1.1 이하만 말하거나, TLS 1.2 가운데 이 라이브러리가 뺀 부분만 말하는 상대(9절). `min_version`이 1.3일 때의 TLS 1.2 상대. 공통된 암호 스위트나 그룹이 없다. *이쪽의* 인증서를 거부한 상대. 손상 |
| `PROVEN_ERR_RESET` (래퍼) | TLS 닫기 없이 연결이 끝났다 | 상대가 죽었거나 끊겼다. 받은 것을 완전하다고 믿지 마라 |
| `PROVEN_ERR_TIMEOUT` (래퍼) | 기한이 지났다 | 응답을 멈춘 상대 |

```c
proven_cert_fault_t proven_tls_peer_fault(const proven_tls_conn_t *conn);
int proven_tls_alert_received(const proven_tls_conn_t *conn);
int proven_tls_alert_sent(const proven_tls_conn_t *conn);
```

로그 한 줄을 위해: `proven_tls_peer_fault`는 인증서가 거부된 정확한 이유다(13장의 목록에
`PROVEN_CERT_FAULT_PIN_MISMATCH`를 더한 것). `proven_tls_alert_received`는 상대가 보낸 경고를
RFC 8446 6절의 번호로 준다 - 답이 `PROVEN_ERR_PROTOCOL`이고 *상대쪽*이 무엇에 반대했는지 알고
싶을 때 여기를 본다(48 `unknown_ca`와 42 `bad_certificate`는 여러분의 인증서를 받아들이지 않았다는
뜻이고, 40 `handshake_failure`는 공통된 것이 없다는 뜻, 70 `protocol_version`은 다른 버전을
원한다는 뜻이다). `proven_tls_alert_sent`는 이쪽이 상대에게 말한 것이다. 둘 다 없었으면 -1이다.

엔진에서는 에러가 `proven_tls_feed`에서 한 번 돌아온다. 그때 상대에게 알리는 경고가 보류 출력에
있고 - 보내라 - 그 뒤로는 출력 쌍과 질문들을 뺀 모든 호출이 `PROVEN_ERR_INVALID_STATE`를 돌려준다.

**에러는 일부러 적다.** 인증에 실패한 상대는 실패했다는 것만 알게 된다. `proven_tls_peer_fault`나
`PROVEN_ERR_PROTOCOL`의 세부를 연결 반대편에 전달하지 마라. 그것은 여러분의 로그를 위한 것이다.

## 9. 무엇을 협상하고, 무엇을 붙들고, 무엇이 없는가

**협상하는 것.** TLS 1.3, 그리고 아직 방어할 수 있는 부분만 남기고 줄인 TLS 1.2. 양쪽이 모두 1.3을
말할 수 있으면 1.3을 말한다.

TLS 1.3에서:

| 무엇 | 내용 |
|---|---|
| 암호 스위트 | `TLS_AES_128_GCM_SHA256` (0x1301), `TLS_AES_256_GCM_SHA384` (0x1302), `TLS_CHACHA20_POLY1305_SHA256` (0x1303) |
| 그 순서 | AES 명령이 있는 프로세서(AES-NI가 있는 x86-64, 암호 확장이 있는 AArch64)에서는 AES-128, AES-256, ChaCha20. **그 밖의 모든 곳에서는 ChaCha20 먼저**: 그 명령이 없으면 이 라이브러리의 AES는 상수 시간이되 ChaCha20보다 서른 배쯤 느리다 |
| 키 교환 | X25519. 상대가 고집하면 P-256(왕복 한 번 추가) |
| 이쪽의 키 | P-256의 ECDSA, Ed25519, 또는 2048~4096비트의 RSA(RSA-PSS와 SHA-256으로 서명) |
| 상대의 키 | 그 둘, P-384의 ECDSA, RSA(2048~8192비트, RSA-PSS) |
| 그 밖에 | ALPN, 서버 이름 표시, 세션 티켓, 키 갱신, `record_size_limit`, 미들박스 호환 메시지 |

TLS 1.2에서:

| 무엇 | 내용 |
|---|---|
| 암호 스위트 | `ECDHE-ECDSA`와 `ECDHE-RSA`, 각각 `AES128-GCM-SHA256`, `AES256-GCM-SHA384`, `CHACHA20-POLY1305`와 함께: 0xC02B, 0xC02C, 0xCCA9, 0xC02F, 0xC030, 0xCCA8. 암호의 순서는 위와 같다 |
| 키 교환 | X25519 또는 P-256, 연결마다 새 키 쌍 - 그래서 언제나 순방향 비밀성이 있다 |
| 이쪽의 키 | `ECDHE-ECDSA` 스위트에는 P-256의 ECDSA 또는 Ed25519. `ECDHE-RSA`에는 RSA이며, 상대가 내놓으면 RSA-PSS로, 아니면 SHA-256의 PKCS #1 v1.5로 서명한다 |
| 상대의 키 | 1.3과 같은 종류. 그 서명은 SHA-256, SHA-384 또는 SHA-512를 써야 한다 |
| **상대에게 요구하는 것** | 확장 마스터 시크릿(RFC 7627). 그것이 없는 상대는 `handshake_failure`로 거부된다 |
| 그 밖에 | ALPN, 서버 이름 표시, 세션 티켓(RFC 5077. 6절), 클라이언트 인증서, RFC 8446 4.1.3절의 다운그레이드 표시 |

**무엇을 뺐는가가 요점이다.** 이름이 붙은 TLS 1.2 공격은 모두 이 목록에 없는 것을 거쳐 갔다: CBC 암호
스위트(패딩 오라클), RSA 키 교환(Bleichenbacher의 공격, 그리고 순방향 비밀성 없음), 약하거나 상대가 고른
그룹의 유한체 Diffie-Hellman, 재협상, 압축, 핸드셰이크에 묶이지 않은 마스터 시크릿, 핸드셰이크 서명의
SHA-1과 MD5. 그것들만 내놓는 상대는 `handshake_failure`를 받고, 프로그램은 `PROVEN_ERR_PROTOCOL`을
받는다. 그 가운데 일부는 다른 것이 없는 상대를 위해 하나씩 켤 수 있다: 어느 것인지, 각각 무엇을 치르는지는
10절에 있다.

- **재협상은 답하지 않고 사양한다.** 수립된 연결에서 새 핸드셰이크를 청하는 상대에게는
  `no_renegotiation` 경고를 보내고 연결은 그대로 이어 간다. 그다음에 상대가 무엇을 할지는 상대의
  선택이다: 계속하는 것도 있고 닫는 것도 있다. 계속 청하는 상대는 연달아 열여섯 번 뒤에 끊는다.
- **클라이언트는 1.2로 속아 내려가지 않는다.** 1.3을 말할 수 있었으면서 1.2로 답하는 서버는 자기 난수에
  표시를 남긴다. 1.3을 내놓았던 이쪽 클라이언트가 그 표시를 보면 `illegal_parameter`로 핸드셰이크를
  끝낸다. 중간의 누군가가 더 나은 제안을 지웠기 때문이다.
- **ChangeCipherSpec은 방향마다 한 곳에서만** - 키가 바뀌는 곳에서 - 받아들이고 다른 어디에서도 받지
  않는다.
- **Ed25519 키는 TLS 1.3에서만 클라이언트를 인증한다.** 키가 Ed25519인 클라이언트는 1.2 서버에 인증서를
  내밀지 않는다(없는 것처럼). 서버의 Ed25519 키는 양쪽 버전에서 쓰인다.

**TLS 1.3만 말하려면** `min_version = PROVEN_TLS_VERSION_1_3`으로 둔다. 0.22.0 이전의 이 라이브러리가
하던 그대로다. 클라이언트를 직접 통제하는 서버, 그리고 6절의 더 약한 재개를 받아들일 수 없는 모든
곳에서 고려하라. 기본값이 1.2를 받는 까닭은, 더 새로운 것을 말하지 않는 서버에 닿지 못하는 클라이언트는
더 안전한 것이 아니라 끊긴 것일 뿐이기 때문이다 - 그리고 아주 많은 서버와 기기가 아직 더 새로운 것을
말하지 않는다.

**연결마다 붙드는 것.** 핸드셰이크 동안에는 몇 킬로바이트이고, 끝나면 해제된다. 그 뒤로 아무것도
오가지 않을 때는 연결마다 **약 1킬로바이트**: 키와 카운터. 레코드 버퍼는 레코드를 받는 동안,
읽히기를 기다리는 동안, 보내지기를 기다리는 동안에만 있고 끝나면 해제된다 - 그래서 한가한 TLS
연결 만 개는 16 KiB 버퍼 둘씩이 치를 삼백 메가바이트가 아니라 십 메가바이트쯤을 붙든다. 그것은 이
층의 몫이다. 전송 래퍼는 수십 바이트를 더하고, HTTP 서버 자신의 연결별 요청 버퍼는 11장의 것이며
별개다. (TLS 1.2 연결도 같은 만큼을 붙든다.)

**핸드셰이크 하나의 비용.** 개발 기계의 x86-64 코어 하나에서 X25519와 P-256 인증서의 전체
핸드셰이크는 서버에서 프로세서 시간 약 2 ms, 체인을 검증하는 클라이언트에서는 약 5 ms가 든다: 서버라면
코어당 초당 새 연결 500개 남짓. 여기의 공개 키 연산은 상수 시간이 되도록, 그리고 읽을 수 있도록 쓰였고
다듬어진 라이브러리보다 열 배에서 쉰 배 느리다. 재개(6절)는 서명을 없애고, 연결을 열어 두는 것은
핸드셰이크를 없앤다.

**RSA 키에서는 서명이 대부분을 차지한다.** 같은 코어에서 서명 하나는 2048비트 키로 약 9 ms, 3072비트로
27 ms, 4096비트로 55 ms가 걸린다: 2048비트 RSA 키를 가진 서버는 코어당 초당 새 연결 백 개쯤을 마치고,
P-256으로는 오백 개다. [15장](manual-15-event-loop-ko.md)의 이벤트 구동 서버에서 그 시간은 루프의
스레드에서 쓰이고, 그동안 다른 것은 서비스되지 않는다. 서명은 두 소수를 법으로 하여 상수 시간 지수
연산으로 하고, 입력을 블라인딩하며, 결과를 내보내기 전에 공개 키로 검사한다. 작업용 수들은 스택에
둔다 - x86-64의 `-O2`에서 컴파일러 자신의 계산으로 약 20 KiB - 작은 타깃은 그것을 감안해야 한다. 상대는
`rsa_pss_rsae_sha256`을 받아들여야 한다. TLS 1.3이 모든 구현에 요구하는 것이며, 거기서 이쪽이 서명에 쓰는 단
하나의 방식이다.

**상대에 대한 한계.** 핸드셰이크 메시지는 최대 `max_handshake_bytes`이고, 레코드는 프로토콜이 허락하는 크기까지이며 헤더에서 거부된다. 아무것도 나르지 않는 레코드 - 빈 레코드, 반복되는 호환 메시지 - 는 연달아 열여섯 개까지 봐준다. 키 갱신 요청에는 보내지 않은 답이 64 KiB 쌓일 때까지 답한다. 어느 것이든 넘으면 연결은 `PROVEN_ERR_PROTOCOL`로 끝난다.

**여기에 없는 것:**

- **설정이 요구하지 않는 한(10절):** TLS 1.1과 1.0, CBC 암호 스위트, RSA 및 유한체 Diffie-Hellman 키
  교환, 확장 마스터 시크릿이 없는 상대.
- **전혀 없는 것:** SSL 3.0과 그 이전. RC4, 3DES, 수출용, 익명, DSS 스위트. 재협상. 압축.
  서버에 보관하는 세션 ID. TLS 1.2의 `record_size_limit`. 그중 하나가 필요한 상대는
  `PROVEN_ERR_PROTOCOL`로 실패한다.
- 이쪽을 위한 **4096비트를 넘는 RSA 키**, 그리고 RSA-PSS 키(키가 PSS 전용으로 표시된 인증서).
- **조기 데이터(0-RTT).**
- **폐기.** CRL도, OCSP도, 스테이플링도 없다. 13장을 보라.
- **클라이언트가 요청한 이름에 따라 인증서를 고르기.** 설정 하나에 인증서 하나.
- **암호화된 ClientHello, 핸드셰이크 뒤의 클라이언트 인증, 외부 사전 공유 키.**
- **레코드 패딩.** 사용 중인 TLS 대부분이 그렇듯 레코드 길이가 데이터 길이를 드러낸다.
- **프로세스 사이의 티켓 공유**, 또는 재시작을 넘긴 보관.

**어떻게 시험했는가.** 등록된 테스트는 RFC 8448의 예시 핸드셰이크 다섯을 키 스케줄과 레코드 계층에
값 하나하나 재현하고, 그 가운데 둘은 클라이언트에 레코드 하나하나 재현한다. 메모리 안에서
클라이언트와 서버를 서로 맞붙여 모든 스위트와 키 종류, 7절과 8절의 모든 거부, 재개, 그리고
핸드셰이크의 단일 비트 변경 스윕을 돌린다. 그리고 실제 소켓 위에서 HTTPS와 `wss`를 돌린다. 등록된
테스트 밖에서는, 양쪽 역할을 OpenSSL과, 서버를 GnuTLS와 대조해 모든 스위트와 그룹, 재시도, 재개,
클라이언트 인증서를 돌렸고, 클라이언트로 공개 서버의 페이지를 가져왔다. 이쪽의 RSA 키에 대해서는: 같은 두
구현이, OpenSSL이 만든 2048·3072·4096비트 키로 서명한 핸드셰이크를 받아들였다. 그 키들에 대한 이
라이브러리의 PKCS #1 v1.5 서명은 OpenSSL의 것과 같은 바이트였고, PSS 서명은 OpenSSL이 받아들였다. TLS 1.2에
대해서는: 그 키 유도를 PRF의 공개된 시험 벡터 둘과, 이 라이브러리 밖에서 계산한 값에 대조한다(1.3과 달리
1.2 핸드셰이크 전체의 공개된 기록은 없으므로, 핸드셰이크에 대해서는 다른 구현들이 증거다). 양쪽 역할을
OpenSSL과 대조해 여섯 스위트 모두와 세 종류의 키 모두, 티켓, 클라이언트 인증서, PKCS #1과 PSS 서명, 확장
마스터 시크릿이 없는 상대(거부), 뺀 것만 내놓는 상대(거부), 재협상을 청하는 상대(사양)를 돌렸고, GnuTLS의
클라이언트를 서버에 대조해 돌렸다. 레거시 묶음에 대해서는: 그 조각들을 이 라이브러리 밖에서 계산한 값과,
존재하는 공개 값(10절에 나열)에 대조한다. 양쪽 역할을 OpenSSL과, 서버를 GnuTLS의 클라이언트와 대조해 TLS
1.0, 1.1, 1.2에서 추가된 키 교환과 암호 각각, encrypt_then_mac이 있을 때와 없을 때, 클라이언트 인증서,
확장 마스터 시크릿이 없는 상대를 돌렸다. 그리고 한 걸음만 잘못 디뎌도 키를 내주는 두 곳 - CBC 레코드를
여는 것과 RSA 프리마스터를 되찾는 것 - 을, 복호화된 것을 비밀로 표시하고 아래와 같은 검사기 아래에서
돌렸으며, 아무것도 보고되지 않았다. 그리고 서명 경로와 두 버전의 키 유도 및 레코드 보호를, 비밀 - 소수, 비밀
지수, 블라인딩 값, 공유 키와 거기서 유도된 모든 것 - 에 의존하는 분기나 메모리 인덱스를 보고하는 검사기 아래에서
돌렸고, 한 컴파일러와 한 프로세서 계열의 두 최적화 수준에서 아무것도 보고되지 않았다. **하지 않은 것:** 커버리지
유도 퍼징 없음, 프로토콜 수준 퍼징 스위트(tlsfuzzer, BoGo) 없음, 하드웨어에서의 시간 측정 없음,
외부 검토 없음. 밑의 기본 연산은 13장의 것이며 거기 적힌 한계가 그대로 있다.

## 10. 레거시 묶음: 설정 하나하나가 무엇에 노출시키는가

이 절의 모든 것은 **설정이 이름을 들지 않는 한 꺼져 있고**, 그 어느 것도 당신이 원하는 것이 아니다. 이것이
있는 까닭은, 프로그램이 때로는 더 나은 것이 없는 기계 - 아무도 갱신할 수 없는 장비, 남의 네트워크에 있는
서버 - 와 말해야 하고, 그 대안이 연결 없음이나 두 번째 TLS 라이브러리이기 때문이다. RFC 8996은 TLS 1.0과
1.1을 퇴역시켰고 RFC 9325는 나머지를 협상하지 말라고 한다. 양 끝을 모두 통제한다면 저쪽 끝을 바꿔라.

**비트 하나가 결정 하나다.** 옵션의 `legacy`에 뜻하는 것들을 둔다:

| 비트 | 가능해지는 것 | 포기하는 것 |
|---|---|---|
| `PROVEN_TLS_LEGACY_CBC` | AES의 CBC 모드로 암호화하고 HMAC-SHA1, HMAC-SHA256 또는 HMAC-SHA384로 인증하는 암호 스위트. 1.2 미만의 모든 버전에 필요하다 - 거기에는 다른 것이 없다 | 이십 년의 패딩 공격 이력을 가진 구성 |
| `PROVEN_TLS_LEGACY_RSA_KEY_EXCHANGE` | 양쪽이 합의하는 대신 클라이언트가 세션의 시크릿을 서버의 RSA 키로 암호화한다 | **순방향 비밀성**: 나중에 서버의 키를 얻는 자는 녹음된 모든 연결을 읽는다. 그리고 키 자체가 서버 쪽 실수 하나의 사정거리에 놓인다(아래) |
| `PROVEN_TLS_LEGACY_DHE` | RSA 인증서와 함께 하는, 유한체의 Diffie-Hellman 키 교환 | 원리상으로는 없다. 실제로는 시간 - 타원곡선 교환보다 몇 배 느리다 - 그리고 대개 다른 면에서도 오래된 상대에게만 있다 |
| `PROVEN_TLS_LEGACY_NO_EXTENDED_MASTER_SECRET` | RFC 7627이 없는 상대와의 연결이 진행된다 | 세션의 키를 그것을 만든 핸드셰이크에 묶는 것 |

그리고 그 버전들을 허락하려면 `min_version`을 `PROVEN_TLS_VERSION_1_1` 또는 `PROVEN_TLS_VERSION_1_0`으로
둔다 - 이것에도 CBC 비트가 필요하고, 없으면 거부된다. 이 가운데 아무것도 두지 않는 설정에는 달라지는 것이
없다: 이것들이 있기 전과 같은 hello를 보내고, 같은 답을 하고, 같은 메모리를 붙든다.

**핸드셰이크 안에서도 마지막 수단이다.** 레거시 스위트는 9절의 모든 스위트 뒤에 내놓고 뒤에 고른다. 더
오래된 버전은 상대에게 더 새 것이 없을 때에만 합의된다. 모든 것을 허락하는 양쪽도 결국 TLS 1.3에
이른다. 그리고 9절의 다운그레이드 검사는 오래된 버전도 덮는다: 1.2를 말할 수 있었으면서 1.0으로 답하게 된
서버는 클라이언트가 확인하는 방식으로 그것을 밝힌다.

### CBC 스위트

`ECDHE-ECDSA-`와 `ECDHE-RSA-`에 `AES256-SHA384`, `AES128-SHA256`, `AES128-SHA`, `AES256-SHA`. 그리고
아래의 비트들과 함께라면 `DHE-RSA-` 뒤와 순수 RSA 뒤에 `AES128-SHA256`, `AES256-SHA256`, `AES128-SHA`,
`AES256-SHA`. MAC이 SHA-1인 스위트가
TLS 1.0과 1.1이 가진 전부다.

약점은 연산의 순서다. 보내는 쪽은 평문을 인증하고, 패딩하고, 그다음에 암호화한다. 받는 쪽은 복호화하고,
패딩을 벗기고, 그제야 MAC을 확인해야 한다. 한 종류의 나쁜 레코드에 다른 종류보다 조금 빠르거나 조금
다르게 반응하는 수신자는 공격자에게 평문이 무엇이었는지 한 바이트씩 알려 준다(Vaudenay 2002, POODLE,
Lucky Thirteen). 이 라이브러리가 하는 일:

- **먼저 해결책을 청한다.** `encrypt_then_mac`(RFC 7366)은 암호문을 인증하므로, 상대가 보내지 않은 것은
  복호화되지 않는다. CBC를 내놓을 때마다 내놓고 상대가 동의할 때마다 쓴다. 아직 CBC가 필요한 상대
  대부분은 이것이 없다.
- **그것이 없으면, 레코드를 여는 일은 안에 무엇이 있든 같은 일을 한다.** 복호화 뒤의 모든 것 - 패딩을
  찾고, 확인하고, MAC을 찾고, 패딩에 따라 달라지는 길이에 대해 계산하고, 비교하는 것 - 은 마스크로 하며,
  복호화된 바이트에 의존하는 분기도 메모리 인덱스도 없고, 계산하는 해시 블록의 수는 레코드의 길이에만
  의존한다. 모든 실패는 하나의 경고 `bad_record_mac`이다.
- **복호화는 어느 쪽이든 데이터에 따라 달라지는 시간이 걸리지 않는다**: x86-64에서는 프로세서의 AES
  명령으로 - 그러면 CBC 연결은 개발 기계에서 약 100 MiB/s로, 보내는 만큼 빠르게 받는다 - 그 밖의 모든
  곳에서는 포터블한 비트슬라이스 암호로, 약 6 MiB/s로.
- **TLS 1.0에서는** 각 레코드의 IV가 앞 레코드의 마지막 블록이므로, 당신이 고른 데이터를 보내게 만들 수
  있는 공격자는 IV를 미리 안다(BEAST). 그래서 애플리케이션 데이터는 한 바이트, 그다음 나머지로 보낸다 -
  쓰기 한 번에 레코드 둘.

### RSA에 의한 키 교환

`AES128-GCM-SHA256`과 `AES256-GCM-SHA384`, 그리고 CBC 비트와 함께 `AES128-SHA256`, `AES256-SHA256`,
`AES128-SHA`, `AES256-SHA`. 서버의 키는 RSA여야 한다.

두 가지이고, 둘째는 보기보다 나쁘다:

- **순방향 비밀성이 없다.** 모든 키가 유도되는 시크릿이 서버의 장기 키로 암호화되어 지나간다. 연결의
  녹음은 그 키만큼만, 영원히, 안전하다.
- **서버가 실수 하나를 하면 복호화 오라클이 된다.** 서버는 클라이언트가 고른 값을 복호화한다. 그 뒤에
  하는 어떤 일 - 경고, 지연 - 이 그 값이 제대로 된 형태로 복호화되었는지에 의존하면, 공격자는 그것을
  써서, 한 번에 수천 연결로, 서버의 키로 무엇이든 복호화하거나 *서명*할 수 있다(Bleichenbacher 1998,
  ROBOT 2017). 이것은 이 키 교환에 한정되지 않는다: 이렇게 위조한 서명은 TLS 1.3에서도 똑같이 통한다.
  **서버에서 이 비트를 켜면 그 키는 그것이 쓰이는 모든 프로토콜에 대해 그 위험에 놓인다.** 여기서 하는
  일: 복호화 전에 난수 48바이트를 뽑는다. 복호화는 9절의 블라인딩되고 검사되는 연산이다. 결과는 마스크로
  - 모든 바이트를, 고정된 순서로 - 검사하고, 그것이든 난수 바이트든 마스크로 골라 시크릿이 된다. 그러면
  핸드셰이크는 똑같이 이어지고, 실패한다면 틀린 키의 어느 핸드셰이크와도 같이 Finished 메시지에서
  실패한다. 이 경로는 복호화된 바이트에 의존하는 분기나 메모리 인덱스를 보고하는 검사기 아래에서
  돌렸고, 아무것도 보고되지 않았다. 하드웨어에서 시간을 재지는 않았다.

### 유한체 Diffie-Hellman

`DHE-RSA-`에 `AES128-GCM-SHA256`, `AES256-GCM-SHA384`, `CHACHA20-POLY1305`, 그리고 CBC 비트와 함께
`AES128-SHA256`, `AES256-SHA256`, `AES128-SHA`, `AES256-SHA`.

DHE에서 잘못된 것은 그룹이었다. 서버가 고르는데 클라이언트는 핸드셰이크가 걸리는 시간 안에 그것을 검사할
수 없다: 너무 작거나(Logjam), 소수가 아니거나, 새도록 만들어졌거나. 그래서 여기서는 어떤 그룹도 검사하지
않는다:

- **클라이언트는 소수가 바이트 하나까지 여섯 중 하나일 때에만 받는다** - RFC 7919와 RFC 3526의 2048,
  3072, 4096비트 그룹 - 생성원은 2. 그 밖의 것은 `insufficient_security`로 핸드셰이크를 끝낸다: 스스로
  만든 그룹을 쓰는 서버에는, 그 그룹이 아무리 좋아도, 닿을 수 없다.
- **서버는 하나를 내놓는다**: RFC 7919의 2048비트 그룹.
- 상대의 값이 0, 1, 또는 소수에서 1을 뺀 값이면 거부한다. 모든 연결이 새 지수를 쓰며, 그것이 키 유도의
  알려진 약점(Raccoon, 2020)을 닿지 않게 한다.
- 개발 기계에서 서명에 더해 한쪽에 약 7 ms의 프로세서 시간이 든다: 2048비트 RSA 키를 가진 서버는
  코어당 초당 그런 핸드셰이크 예순다섯 개쯤을 마친다. 그리고 작업용 수들을 스택에 둔다: 핸드셰이크의
  가장 깊은 지점에서 약 16 KiB - x86-64의 `-O2`에서 컴파일러 자신의 계산으로.

### TLS 1.0과 1.1

`min_version = PROVEN_TLS_VERSION_1_1` 또는 `PROVEN_TLS_VERSION_1_0`, CBC 비트와 함께.

- 핸드셰이크의 무결성이 MD5와 SHA-1 둘에 함께 기대고, 서명은 그것들 위에 만든다: RSA 키는 두 해시를,
  ECDSA 키는 SHA-1을 서명한다. 어느 해시도 이 라이브러리의 다른 어디에서도 이 용도로 받아들여지지 않는다.
- AEAD 암호가 없다: 모든 스위트가 HMAC-SHA1의 CBC이고, 위에서 말한 모든 것이 따른다.
- **Ed25519 키는 이 버전들을 서비스할 수 없다** - 그 서명을 실을 방법이 없다. 키가 Ed25519인 서버는 더
  새 것을 말하지 않는 클라이언트를 거부한다.
- 세션은 1.2에서처럼 동작한다(6절): 티켓 하나, **만들어진 그 버전에서만** 재개된다 - TLS 1.0에서 만든
  세션은 1.2에 합의하는 핸드셰이크가 재개하지 않는다.
- 클라이언트 인증서는 P-256과 RSA 키로 동작한다.

### 확장 마스터 시크릿이 없는 상대

RFC 7627(2015)은 세션의 키를 그것을 만든 핸드셰이크에 묶는다. 그것이 없으면 중간의 누군가가 서로 다른
두 연결이 마스터 시크릿을 공유하도록 꾸미고, 한쪽에서 다른 쪽으로 클라이언트의 인증서 증명을 넘길 수
있다(트리플 핸드셰이크 공격). 기본 설정은 그것이 없는 상대를 거부한다. TLS 1.0이나 1.1만 말하는 상대
대부분은 그것이 없다.

`PROVEN_TLS_LEGACY_NO_EXTENDED_MASTER_SECRET`이 있으면 그런 상대와 말하고, 그 연결에서는 공격에 필요한
일들을 하지 않는다: **티켓을 발급하지도 받지도 않고**, 클라이언트는 **인증서를 보이지 않으며**, 서버는
**요청하지 않는다** - 클라이언트 인증서를 *요구하는* 서버는 그런 클라이언트를 곧바로 거부한다. 그 확장이
있는 상대는 여전히 그것을 쓴다. 이 비트는 그것의 부재를 허락할 뿐이다.

### 이 가운데 아무것도 쓰지 않는 프로그램이 치르는 것

없다: CBC 스위트에 합의하지 않는 연결은 이 가운데 어느 것도 있기 전에 붙들던 만큼을 붙든다. 한가한
CBC 연결은 약 1 KiB 대신 약 1.1 KiB를 붙든다.

### 어떻게 확인했고, 무엇을 하지 않았는가

등록된 테스트는 모든 조각을 이 라이브러리 밖에서 계산한 값에 대조한다 - AES-CBC는 SP 800-38A에, HMAC은
RFC 2202와 RFC 4231에, 나머지(옛 PRF, 모든 버전과 두 MAC 순서의 CBC 레코드, Diffie-Hellman)는 독립된
구현에 - 그리고 모든 스위트를 그것이 존재하는 모든 버전에서, 양쪽 역할로, 위에서 말한 거부 각각과 함께
돌린다. Diffie-Hellman 소수 여섯은 추가할 때 다른 구현 둘과 비교했다. 등록된 테스트 밖에서는 양쪽 역할을
OpenSSL과, 서버를 GnuTLS와 대조해 돌렸다. 그리고 CBC 레코드를 여는 것과 RSA 프리마스터를 되찾는 것을
각각, 복호화된 바이트를 비밀로 표시하고 검사기 아래에서 돌렸으며, 검사기가 잡아야 하는 일부러 틀린 판도
함께 돌렸다(잡았다).

**그리고 시간을 쟀다.** 개발 기계의 프로세서에서, 이런 누출을 찾는 방식대로: 같은 연산을 두 종류의
입력에 대해 무작위 순서로 수십만 번 돌리고, 한 번 한 번을 사이클 카운터로 재고, 두 시간 집합을
통계적으로 비교했다. CBC 레코드에 대해 - 세 MAC 각각, 프로세서의 AES와 포터블 AES로 - 패딩이 없는
레코드 대 255바이트인 레코드, 각 패딩 뒤의 틀린 MAC, 틀린 MAC 대 틀린 패딩, 그리고 불가능한 패딩 길이.
RSA 프리마스터에 대해 제대로 된 것 대 잡음, 대 틀린 버전, 대 틀린 길이. 두 번 했다. **되풀이되는
차이를 보인 쌍은 없었다.** 잰 그대로의 전체 실행에 대해서는 검정 통계량이 모든 쌍에서 3 미만에
머물렀다(통상의 기준선은 4.5). 끼어든 방해를 걸러 내려고 관례대로 가장 느린 십분의 일을 빼면, 열 쌍에서
한쪽 실행에서만 4.5에서 7.7 사이로 올랐고 두 실행 모두에서 오른 쌍은 없었다 - 잡음이 하는 일이고, 실제
차이는 하지 않는 일이다. 대조군으로 같은 방식으로 잰 일부러 순진한 레코드 검사는 수천의 통계량을 냈다.
레코드가 열렸는지 자체는, 답에서 그렇듯이, 시간에도 *보인다*.

**그것이 보이지 않는 것.** 프로세서 하나, 컴파일러 하나, 다른 일도 하고 있던 기계. 레코드는 쌍마다
400,000회, RSA는 6,000회 - 몇 사이클의 차이는 놓칠 것이다. 측정이지 증명이 아니며, 더 조용한 기계나
다른 프로세서는 이 기계가 보지 못한 것을 볼 수 있다. 이 경로들의 퍼징 없음. 외부 검토 없음.
