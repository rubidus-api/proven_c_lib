# 12장: WebSocket

**5부 — 운영체제와 대화하기. 선행 조건: [11장](manual-11-http-client-server-ko.md)의 HTTP 클라이언트와
서버, 그리고 그 장의 프로토콜 바꾸기 절. 코덱 절반만 볼 것이면 [10장](manual-10-http-ko.md).**
**이 장을 마치면** 어느 쪽에서든 WebSocket 연결을 열고, 텍스트와 바이너리 메시지를 주고받고, 제대로
닫고, 상대가 규칙을 어겼을 때 여러분의 프로그램이 무엇을 하는지 말할 수 있다 - 또는 소켓 없이
프레임을 직접 뜯어볼 수 있다.

이 장은 `ws.h`와 `ws_conn.h`를 다루고, 9절에서는 그 둘과 15장의 이벤트 구동 서버에서 쓰는 압축된 메시지를 다룬다. `ws.h`는 순수한 텍스트·바이트 처리로,
[프리스탠딩(freestanding)](manual-freestanding-ko.md) 빌드에서 쓸 수 있고 `PROVEN_NO_NET`의 영향을 받지
않는다. `ws_conn.h`는 소켓이 필요하다. 호스티드(hosted) 전용이고 `PROVEN_NO_NET`으로 빠진다.

## 목차

1. [WebSocket이 무엇인지, 한 쪽으로](#1-websocket이-무엇인지-한-쪽으로)
2. [연결](#2-연결)
3. [보내기와 받기](#3-보내기와-받기)
4. [닫기, 그리고 닫히기](#4-닫기-그리고-닫히기)
5. [서버의 연결은 어디에 사는가](#5-서버의-연결은-어디에-사는가)
6. [코덱: 핸드셰이크](#6-코덱-핸드셰이크)
7. [코덱: 프레임](#7-코덱-프레임)
8. [코덱: 디코더](#8-코덱-디코더)
9. [압축된 메시지](#9-압축된-메시지)
10. [여기에 없는 것](#10-여기에-없는-것)

## 1. WebSocket이 무엇인지, 한 쪽으로

HTTP는 질문 하나에 답 하나다. WebSocket(RFC 6455)은 양쪽이 할 말이 생길 때마다 말하고 싶을 때 연결이
되는 것이다. HTTP 요청 한 번과 `101` 응답 뒤에, 같은 TCP 연결이 한쪽이 닫을 때까지 양방향으로
*메시지*를 실어 나른다.

```text
client                                         server
  | GET /chat HTTP/1.1                           |
  | Upgrade: websocket                           |
  | Sec-WebSocket-Key: <16 random bytes>         |
  |--------------------------------------------->|
  |            HTTP/1.1 101 Switching Protocols  |
  |            Sec-WebSocket-Accept: <the answer>|
  |<---------------------------------------------|
  |  message  --->         <---  message         |      any number, either way, any time
  |  ping     --->         <---  pong            |
  |  close    --->         <---  close           |      then the connection ends
```

메시지는 **텍스트**(UTF-8이고, 검사된다)이거나 **바이너리**(아무 바이트)다. 회선 위에서 메시지는
하나 이상의 *프레임*이다. 길거나 흘려보내는 메시지는 프래그먼트로 나뉘고 받는 쪽이 이어 붙인다.
그 사이로 작은 *제어* 프레임 세 가지가 다닌다. 반대쪽이 아직 있는지 알아보는 **ping**과 **pong**,
그리고 코드와 이유를 싣는 **close**다.

두 가지 규칙은 까닭을 알기 전에는 이상해 보인다. 클라이언트는 모든 프레임을 **마스킹**한다 -
페이로드를 무작위 4바이트 키와 XOR한다 - . 서버는 결코 하지 않는다. 마스크는 아무것도 숨기지 않는다.
웹 페이지의 스크립트가 네트워크를 건너는 바이트를 정확히 고르지 못하게 막을 뿐이고, 한때는 그것만으로
캐싱 프록시를 오염시킬 수 있었다. 그리고 핸드셰이크의 키와 답은 인증이 아니다. 서버가 정말
WebSocket을 말하며, 받은 것을 되돌려 보내는 HTTP 서버가 아니라는 것만 증명한다.

**`ws://`는 보호되지 않는다.** `ws://` 연결은 11장이 `http://`에 대해 말한 그대로, 경로 위의 누구든
읽고 바꿀 수 있다. `wss://`는 TLS 위의 같은 프로토콜이다. HTTP 클라이언트에 TLS 랩을, 또는 HTTP
서버에 TLS 설정을 주면([14장](manual-14-tls-ko.md)) 이 장의 나머지는 아무것도 바뀌지 않는다.

## 2. 연결

```text
/* a client */
proven_ws_conn_config_t config = { .alloc = proven_heap_allocator() };
proven_ws_conn_t *ws;
if (proven_ws_conn_connect(http_client, PROVEN_LIT("ws://example.com/feed"), NULL, 0,
                           PROVEN_LIT(""), &config, &ws, NULL) == PROVEN_OK) {
    proven_ws_conn_send_text(ws, PROVEN_LIT("hello"));
    ...
    proven_ws_conn_close(ws, PROVEN_WS_CLOSE_NORMAL, PROVEN_LIT(""));
    proven_ws_conn_destroy(ws);
}
```

### 참조

| API | 의도 | 반환 |
|---|---|---|
| `proven_ws_conn_connect(client, url, headers, count, protocols, &config, &ws, &http_status)` | HTTP 클라이언트를 거쳐 클라이언트로 연결한다 - 그래서 그 클라이언트의 프록시, 타임아웃, `tls_wrap`이 적용된다. `url`은 `ws://`나 `wss://`다. `http_status`(선택)는 서버가 답한 상태 코드를 받는다. | `proven_err_t`: 답이 `101`이 아니면 `REFUSED`(`http_status`를 보라). `101`이 이 핸드셰이크에 대한 답이 아니면 `INVALID_FORMAT`. `tls_wrap` 없는 `wss`는 `UNSUPPORTED`. 무작위 바이트를 얻지 못하면 `IO`. 그리고 `proven_http_client_send`의 오류들. |
| `proven_ws_conn_accept(x, protocol, &config, &ws)` | HTTP 핸들러 안에서 서버로 받아들인다: 요청을 확인하고, `101`로 답하고, 연결을 가져온다. `protocol`은 고를 서브프로토콜이거나 비어 있다. | `proven_err_t`: `NOT_FOUND` - 업그레이드가 아니다, 아무것도 보내지 않았다, HTTP로 답하라. `UNSUPPORTED` - 다른 버전이다, `426`을 보냈다. `INVALID_FORMAT` - 형식이 어긋난 업그레이드다, `400`을 보냈다. `INVALID_ARG` - 클라이언트가 내놓지 않은 `protocol`이다, 아무것도 보내지 않았다. |
| `proven_ws_conn_open(transport, is_server, early, &config, &ws)` | 핸드셰이크를 이미 마친 전송 위에 연결을 만든다. 성공하면 연결이 전송을 소유한다. | `proven_err_t`: `INVALID_ARG`. `NOMEM`. `early`가 16 KiB를 넘으면 `OUT_OF_BOUNDS`. `IO`. |
| `proven_ws_conn_destroy(ws)` | 전송을 닫고 연결을 해제한다. | 없음. |
| `proven_ws_conn_protocol(ws)` | 합의된 서브프로토콜. 없으면 비어 있다. | `proven_u8str_view_t`. |

```text
typedef struct {
    proven_allocator_t alloc;           /* required */
    proven_size_t max_message_bytes;    /* 0: 1 MiB - the largest message accepted */
    proven_u32 io_timeout_ms;           /* 0: 30 s - for each write */
    proven_u32 close_timeout_ms;        /* 0: 5 s - how long close waits for the peer */
} proven_ws_conn_config_t;
```

연결 하나는 만들 때 약 21 KiB가 든다 - 16 KiB 읽기 버퍼와 4 KiB 보내기 버퍼 - . 여기에 지금까지
조립해야 했던 가장 큰 메시지만큼이 더해지는데, 다음 메시지를 위해 간직되며 `max_message_bytes`를
넘지 않는다. 메모리는 모두 `alloc`에 준 할당자(allocator)에서 온다.

### 주의사항, 그리고 무엇이 잘못되는가

**연결은 한 번에 스레드 하나의 것이다.** "한 스레드가 보내는 동안 다른 스레드가 받는다"도 안 된다.
받기는 ping에 답하고, 그것은 보내기다. 한 연결을 두 스레드가 쓰면 두 프레임의 바이트가 섞이고,
상대는 원인과 동떨어진 곳을 가리키는 프로토콜 오류로 닫는다. 연결을 한 스레드에 맡기거나, 모든 호출을
잠금 하나로 감싸라.

**`max_message_bytes`는 상대가 여러분에게 쥐게 할 수 있는 양이다.** 메시지는 여러분이 보기 전에 메모리
안에서 조립된다. 이 한도는 무엇이 오든 연결 하나가 그 일에 할당할 최대치다. 넘는 메시지는 코드
1009로 연결을 끝낸다. 상상할 수 있는 가장 큰 메시지가 아니라 기대하는 메시지에 맞춰 정하라.

**서브프로토콜은 장식이 아니라 합의다.** 클라이언트가 이름들을 내놓고, 서버가 그중 하나를 고르거나
아무것도 고르지 않는다. `proven_ws_conn_accept`는 클라이언트가 내놓지 않은 이름을 거절하고,
`proven_ws_conn_connect`는 그런 이름을 고른 서버를 거절한다. 여러분의 프로토콜에 버전이 있다면, 양쪽이
어느 버전을 공유하는지 알게 되는 곳이 여기다. `proven_ws_conn_protocol`을 확인하고, 짐작하지 마라.

**`Origin` 헤더를 확인하는 것은 서버 몫이다.** 브라우저는 모든 WebSocket 핸드셰이크에 `Origin`을 실어
보내고, 거기에 동일 출처 정책을 적용하지 *않는다*. 어떤 웹 페이지든 여러분의 서버에 WebSocket을 열
수 있고, 브라우저는 사용자의 쿠키를 붙여 준다. 서버가 브라우저에서 닿을 수 있고 쿠키를 쓴다면,
받아들이기 전에 `Origin`을 예상하는 사이트와 비교하라. 그러지 않으면 사용자가 방문한 아무 페이지나
그 사용자 행세를 할 수 있다.

## 3. 보내기와 받기

| API | 의도 | 반환 |
|---|---|---|
| `proven_ws_conn_send_text(ws, text)` | 텍스트 메시지를 보낸다. | `proven_err_t`: UTF-8이 아니면 `INVALID_ENCODING` - 아무것도 보내지 않는다. close 뒤이거나 `_send_part`로 시작한 메시지 안이면 `INVALID_STATE`. `TIMEOUT`, `RESET`. |
| `proven_ws_conn_send_binary(ws, data)` | 바이너리 메시지를 보낸다. | 같다. 인코딩 검사만 없다. |
| `proven_ws_conn_send_part(ws, text, data, last)` | 메시지의 프래그먼트 하나를 보낸다. `last`가 메시지를 끝낸다. `text`는 첫 호출에서만 읽는다. | 같다. 이렇게 보내는 텍스트는 검사하지 **않는다**. |
| `proven_ws_conn_ping(ws, data)` | 최대 125바이트를 실은 ping을 보낸다. | `proven_err_t`: 더 길면 `INVALID_ARG`. |
| `proven_ws_conn_receive(ws, until, &message)` | 다음 온전한 메시지를, 또는 `until`까지 기다린다. | `proven_err_t`: `TIMEOUT` - 잃은 것은 없다, 다시 불러라. `EOF` - 상대가 닫았다. `RESET` - 연결이 끊겼다. `INVALID_FORMAT`, `INVALID_ENCODING`, `OUT_OF_BOUNDS` - 상대가 규칙을 어겼고, 그렇다고 알렸다. |
| `proven_ws_conn_pong_count(ws)` | 도착한 pong의 수. | `proven_u64`. |

```text
typedef struct {
    bool text;                  /* text (UTF-8, checked) rather than binary */
    proven_mem_view_t data;     /* owned by the connection; good until the next receive */
} proven_ws_message_t;
```

`proven_ws_conn_receive`는 기다리는 동안 프로토콜의 살림을 한다. **ping에는 pong으로 답하고** 여러분은
그것을 보지 못한다. **pong은 센다.** **프래그먼트는 이어 붙이고**, 그 사이에 낀 제어 프레임은 도착한
자리에서 처리한다. 돌려주는 것은 언제나 온전한 메시지다.

### 주의사항, 그리고 무엇이 잘못되는가

**거절에는 최대 1초가 걸린다.** 상대가 규칙을 어기면 연결은 close를 보낸 다음, `receive`가 돌아오기 전에 상대가 아직 보내고 있는 것을 최대 1초 동안 읽어서 버린다 - 그러지 않으면 소켓을 닫는 순간 연결이 reset되어, 상대가 읽기도 전에 그 close가 사라진다.

**`PROVEN_ERR_TIMEOUT`은 그 뒤에도 받을 수 있는 유일한 오류다.** 아직 온전한 메시지가 도착하지 않았다는
뜻이다. 도착한 일부는 간직되고, 다음 호출이 이어 간다. 다른 결과는 모두 연결을 끝낸다. 그 뒤의 모든
호출에서 같은 오류가 돌아오고, 남은 일은 `proven_ws_conn_destroy`다.

**메시지의 바이트는 다음 receive까지만 유효하다.** `message.data`는 연결이 다시 쓰는 메모리를 가리키는
뷰(view)다. 간직할 것은 복사하라.

잘못된 예:

```text
proven_ws_conn_receive(ws, until, &first);
proven_ws_conn_receive(ws, until, &second);
use(first.data);                     /* wrong: first.data now holds the second message, or freed memory */
```

올바른 예 — 다시 받기 전에 `first.data`를 쓰거나 복사한다.

**ping에는 받는 동안에만 답한다.** 연결에는 자기 스레드가 없다. `proven_ws_conn_receive`를 부르지 않고
오랫동안 보내기만 하는 프로그램은 ping에 답하지 않고, ping으로 죽은 연결을 찾는 상대는 이 연결이
죽었다고 결론짓는다. 많이 보낸다면 이따금 받아라 - 아무것도 없을 수 있으면 `PROVEN_NET_DONT_WAIT`로.

**ping을 대신 보내 주는 것도 없다.** 말없이 죽은 연결 - 덮인 노트북, 잊어버린 NAT - 을 찾으려면
`proven_ws_conn_ping`을 보내고, 기다릴 만큼 받아 보고, `proven_ws_conn_pong_count`가 움직였는지 보라.
반대쪽이 한마디 없이 사라진 TCP 연결은 그러지 않으면 몇 시간이고 열려 있는 것처럼 보인다.

**`_send_part`는 텍스트를 검사하지 않는다.** `proven_ws_conn_send_text`는 UTF-8이 아닌 텍스트를
거절한다. 그런 텍스트를 받으면 상대는 연결을 끊어야 하기 때문이다. 프래그먼트는 하나씩 검사할 수
없다 - 문자 하나가 두 프래그먼트 사이에서 잘릴 수 있다 - . 그래서 거기서는 조각들이 *합쳐서* UTF-8이어야
하고, 그것을 확실히 하는 것은 여러분 몫이다.

**타임아웃된 쓰기는 연결을 끝낸다.** `io_timeout_ms`는 쓰기 하나하나를 한정한다. 그것이 다하면 프레임의
일부가 나갔을 수 있고, 프레임 중간에서 이어 갈 방법은 없다. 연결은 끝난 것이다.

## 4. 닫기, 그리고 닫히기

| API | 의도 | 반환 |
|---|---|---|
| `proven_ws_conn_close(ws, code, reason)` | close 프레임을 보내고 상대의 close를 기다린다. 그 사이에 도착한 메시지는 버린다. | `proven_err_t`: 양쪽이 모두 닫았으면 `PROVEN_OK`. 상대가 `close_timeout_ms` 안에 답하지 않으면 `TIMEOUT`. 보낼 수 없는 코드나 이유면 `INVALID_ARG`. |
| `proven_ws_conn_close_code(ws)` | 상대의 close 코드. close 없이 연결이 끝났으면 1006. 열려 있는 동안은 0. | `proven_u16`. |
| `proven_ws_conn_close_reason(ws)` | 함께 온 이유. | `proven_u8str_view_t`. |

닫기는 작은 핸드셰이크다. 한쪽이 close 프레임을 보내고, 다른 쪽이 close로 답하고, 그다음 TCP 연결이
끝난다. 이것을 건너뛰어도 - 닫지 않은 연결을 destroy해도 - 동작은 한다. 상대는 코드 1006 "비정상
종료"를 보는데, 네트워크가 고장 났을 때 보는 것과 같다. 1006에 로그를 남기거나 재시도하는 상대는
그렇게 할 것이다. 먼저 닫아라.

| 코드 | 뜻 | 보내는 쪽 |
|---|---|---|
| 1000 | 정상: 목적을 이뤘다. | 여러분 |
| 1001 | 떠남: 서버가 내려가거나 페이지를 떠난다. | 여러분 |
| 1002 | 프로토콜 오류. | 연결 - 프로토콜이 금지한 프레임에 |
| 1003 | 지원하지 않는 데이터: 바이너리를 원했는데 텍스트, 또는 그 반대. | 여러분 |
| 1007 | 잘못된 데이터: UTF-8이 아닌 텍스트. | 연결 |
| 1008 | 정책 위반. | 여러분 |
| 1009 | 메시지가 너무 크다. | 연결 - `max_message_bytes`를 넘으면 |
| 1011 | 내부 오류. | 여러분 |
| 3000-4999 | 여러분과 여러분의 응용이 정의한다. | 여러분 |
| 1005, 1006 | 결코 보내지 않는다: "코드가 없었다"와 "close 프레임이 오지 않았다". | 아무도. 읽기만 한다 |

**상대가 닫으면 `receive`는 `PROVEN_ERR_EOF`를 돌려주고, 그 close에는 이미 답했다.** 원하면 코드와
이유를 읽고, destroy하라. 더는 아무것도 보낼 수 없다.

**이유는 로그를 읽는 사람을 위한 것이다.** UTF-8 최대 123바이트. 프로그램이 파싱해야 하는 것을 넣지
말고, 읽을 수 있는 채로 네트워크를 건넌다는 것을 기억하라.

**close는 반대 방향을 비워 주지 않는다.** 상대가 여러분의 close를 보기 전에 보낸 메시지는
`proven_ws_conn_close`가 버린다. 그것이 필요하면 먼저 닫지 말고, 보내기를 멈춘 채 상대가 닫을 때까지
계속 받아라.

## 5. 서버의 연결은 어디에 사는가

`proven_ws_conn_accept`는 연결을 HTTP 서버에서 꺼낸다(11장 §11). 그 순간부터 서버의 루프, 연결 수 한도,
타임아웃은 그 연결과 아무 상관이 없고, 남는 질문은 어느 스레드가 `proven_ws_conn_receive`를 부를
것인가다.

**핸들러가 넘겨주고 돌아간다.** 핸들러를 어떻게 돌리든 통하는 모양이고, 예제가 쓰는 모양이다. 연결을
여러분의 스레드나 job에 주고 돌아간다. 서버는 계속 서비스한다.

**핸들러가 남아서 대화한다.** 쓰기는 간단하다. 핸들러를 job system에서 돌리면 연결이 이어지는 동안
작업 스레드 하나를 차지한다 - 그래서 작업 스레드 수가 곧 가질 수 있는 WebSocket 연결 수이고, 다음
클라이언트의 핸드셰이크는 하나가 닫힐 때까지 큐에서 기다린다. 핸들러를 루프의 스레드에서 돌리면 그
시간 동안 서버 전체가 멈추는데, 그것이 의도인 경우는 거의 없다.

어느 쪽이든 **여기서 WebSocket 연결 하나는 스레드 하나의 값이 든다.** `proven_ws_conn_receive`는 연결
하나를 기다린다. 여럿을 기다리는 호출은 없다. 연결 백 개는 receive에 막힌 스레드 백 개다. 연결 수십
개나 수백 개에는 쓸 만한 설계이고 수만 개에는 틀린 설계다 - 그런 경우에는 §6-§8의 코덱을 여러분의 준비
상태 루프(9장의 `proven_net_poll`)에서, 연결마다 디코더 하나씩 두고 구동해야 한다.

**대신 걸어 주는 한도가 없다.** HTTP 서버의 `max_connections`는 연결이 업그레이드되면 그것을 세지
않는다. 클라이언트가 WebSocket을 끝없이 열 수 있다면, 여러분이 세어서 충분할 때 핸드셰이크에 `503`으로
답하라.

테스트 스위트가 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_12_ws_echo.c -->
```c
/*
 * WebSocket 서버와 클라이언트를 한 프로그램에.
 *
 * 서버는 핸들러가 업그레이드를 받아들이는 평범한 HTTP 서버다. 핸들러는 남아서 대화하지
 * 않는다: 연결을 작업 스레드에 넘기고 돌아간다. 그래서 서버의 루프는 다음 요청을 받을 수 있다.
 * 클라이언트는 HTTP 클라이언트를 거쳐 연결하고, 그때부터 두 끝은 같은 종류의 것이다.
 */

static proven_http_server_t *g_server;
static proven_job_sys_t *g_workers;

/* 작업 스레드에서 보내는 연결 하나의 일생: 상대가 닫을 때까지 메시지를 되돌려 보낸다. */
static void talk(void *arg) {
    proven_ws_conn_t *ws = arg;
    for (;;) {
        proven_ws_message_t msg;
        /* ping에는 receive 안에서 답한다. 30초 동안 말이 없는 상대는 포기한다. */
        proven_err_t err = proven_ws_conn_receive(ws, proven_net_deadline_in(30000), &msg);
        if (err != PROVEN_OK) break;        /* PROVEN_ERR_EOF: 상대가 닫았고, 그에 답했다 */
        err = msg.text ? proven_ws_conn_send_text(ws, (proven_u8str_view_t){ msg.data.ptr, msg.data.size })
                       : proven_ws_conn_send_binary(ws, msg.data);
        if (err != PROVEN_OK) break;
    }
    proven_ws_conn_destroy(ws);             /* 반드시: 이제 이것이 소켓을 소유한다 */
}

static void handle(void *ctx, proven_http_exchange_t *x) {
    (void)ctx;
    proven_ws_conn_config_t config = { .alloc = proven_heap_allocator(), .max_message_bytes = 64 * 1024 };
    proven_ws_conn_t *ws = NULL;
    /* 클라이언트가 내놓았을 때에만 서브프로토콜을 고른다. */
    const proven_http_request_t *request = proven_http_exchange_request(x);
    proven_u8str_view_t protocol = proven_ws_request_offers(request, PROVEN_LIT("echo.v1")) ? PROVEN_LIT("echo.v1") : PROVEN_LIT("");

    proven_err_t err = proven_ws_conn_accept(x, protocol, &config, &ws);
    if (err == PROVEN_ERR_NOT_FOUND) {
        /* 업그레이드가 아예 아니다 - 이 주소로 온 평범한 요청이다. 아무것도 보내지 않았다. */
        (void)proven_http_exchange_respond(x, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("this is a WebSocket endpoint\n")));
        return;
    }
    if (err != PROVEN_OK) return;           /* 잘못된 핸드셰이크에는 이미 400이나 426으로 답했다 */

    /* 연결은 더 이상 서버의 것이 아니다. 스레드를 주고 돌아간다. */
    if (proven_job_submit_ex(g_workers, talk, ws) != PROVEN_OK) proven_ws_conn_destroy(ws);
}

static void serve(void *arg) {
    (void)arg;
    (void)proven_http_server_run(g_server);
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    // ---- 서버 ----------------------------------------------------------------
    proven_http_server_config_t server_config = {0};
    server_config.alloc = heap;
    server_config.handler = handle;
    EXAMPLE_REQUIRE(proven_http_server_create(&server_config, &g_server) == PROVEN_OK, "a server");
    proven_net_addr_t at;
    proven_err_t err = proven_http_server_listen(g_server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        proven_http_server_destroy(g_server);
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "it listens");
    /* 서버의 루프에 스레드 하나, 그리고 연결을 맡을 작업 스레드들: 열려 있는 WebSocket
     * 하나가 그것이 이어지는 동안 작업 스레드 하나를 차지한다. */
    proven_job_sys_t *loop_thread = NULL;
    proven_job_group_t running;
    proven_job_group_init(&running);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &loop_thread) == PROVEN_OK &&
                    proven_job_system_init(heap, 4, 16, &g_workers) == PROVEN_OK &&
                    proven_job_group_submit(loop_thread, &running, serve, NULL) == PROVEN_OK, "the loop and the workers are started");

    // ---- 클라이언트 ----------------------------------------------------------
    proven_http_client_config_t http_config = { .alloc = heap };
    proven_http_client_t *http = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&http_config, &http) == PROVEN_OK, "an HTTP client to connect through");

    char url[64];
    int n = snprintf(url, sizeof url, "ws://127.0.0.1:%u/talk", (unsigned)at.port);
    proven_ws_conn_config_t config = { .alloc = heap };
    proven_ws_conn_t *ws = NULL;
    proven_u16 status = 0;
    err = proven_ws_conn_connect(http, (proven_u8str_view_t){ (const proven_byte_t *)url, (proven_size_t)n }, NULL, 0,
                                 PROVEN_LIT("echo.v1, echo.v0"), &config, &ws, &status);
    EXAMPLE_REQUIRE(err == PROVEN_OK && status == 101, "connected: the server answered 101 and the answer checked out");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_ws_conn_protocol(ws), PROVEN_LIT("echo.v1")), "the subprotocol the server chose");
    /* 연결은 이제 독립된 것이다. 쓰는 데 HTTP 클라이언트는 필요 없다. */
    proven_http_client_destroy(http);

    proven_ws_message_t msg;
    EXAMPLE_REQUIRE(proven_ws_conn_send_text(ws, PROVEN_LIT("hello")) == PROVEN_OK, "a text message");
    EXAMPLE_REQUIRE(proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &msg) == PROVEN_OK && msg.text &&
                    proven_u8str_view_eq((proven_u8str_view_t){ msg.data.ptr, msg.data.size }, PROVEN_LIT("hello")), "comes back, whole");

    proven_byte_t bytes[4] = { 0x00, 0xff, 0x10, 0x80 };
    EXAMPLE_REQUIRE(proven_ws_conn_send_binary(ws, (proven_mem_view_t){ bytes, sizeof bytes }) == PROVEN_OK &&
                    proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &msg) == PROVEN_OK && !msg.text && msg.data.size == 4, "binary is any bytes");

    /* 텍스트는 보내기 전에 검사한다: 이것을 받으면 상대는 연결을 끊어야 한다. */
    EXAMPLE_REQUIRE(proven_ws_conn_send_text(ws, PROVEN_LIT("not \xff text")) == PROVEN_ERR_INVALID_ENCODING, "not UTF-8: refused here, nothing sent");

    /* 조각조각 만들어지는 메시지는 프래그먼트로 나간다. 반대쪽에는 여전히 메시지 하나로 보인다. */
    EXAMPLE_REQUIRE(proven_ws_conn_send_part(ws, true, proven_mem_view_from_u8(PROVEN_LIT("one, ")), false) == PROVEN_OK &&
                    proven_ws_conn_send_part(ws, true, proven_mem_view_from_u8(PROVEN_LIT("two, ")), false) == PROVEN_OK &&
                    proven_ws_conn_send_part(ws, true, proven_mem_view_from_u8(PROVEN_LIT("three")), true) == PROVEN_OK, "three fragments");
    EXAMPLE_REQUIRE(proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &msg) == PROVEN_OK && msg.data.size == 15, "one message of fifteen bytes");

    /* 반대쪽이 살아 있는가? ping을 보내고 잠시 receive한다: pong은 그 사이에 세어진다. */
    EXAMPLE_REQUIRE(proven_ws_conn_ping(ws, proven_mem_view_from_u8(PROVEN_LIT("still there?"))) == PROVEN_OK, "a ping");
    err = proven_ws_conn_receive(ws, proven_net_deadline_in(200), &msg);
    EXAMPLE_REQUIRE(err == PROVEN_ERR_TIMEOUT && proven_ws_conn_pong_count(ws) == 1, "no message came, but the pong did: the peer is alive");

    /* 닫기는 핸드셰이크다: 우리의 close, 그다음 상대의 close. */
    EXAMPLE_REQUIRE(proven_ws_conn_close(ws, PROVEN_WS_CLOSE_NORMAL, PROVEN_LIT("done")) == PROVEN_OK, "closed, both ways");
    EXAMPLE_REQUIRE(proven_ws_conn_close_code(ws) == PROVEN_WS_CLOSE_NORMAL && proven_ws_conn_close_reason(ws).size == 0, "the peer echoed the code, with no reason of its own");
    proven_ws_conn_destroy(ws);

    // ---- 어떤 전송 위에서든 되는 연결 ----------------------------------------
    /* 핸드셰이크는 HTTP의 일이고, 그 뒤는 전송만 있으면 된다. 여기서는 소켓 짝이다. */
    proven_net_conn_t a, b;
    proven_ws_conn_t *left = NULL, *right = NULL;
    EXAMPLE_REQUIRE(proven_net_pair(&a, &b) == PROVEN_OK, "two joined sockets");
    EXAMPLE_REQUIRE(proven_ws_conn_open(proven_net_conn_transport(&a), false, (proven_mem_view_t){0}, &config, &left) == PROVEN_OK &&
                    proven_ws_conn_open(proven_net_conn_transport(&b), true, (proven_mem_view_t){0}, &config, &right) == PROVEN_OK, "a client end and a server end");
    EXAMPLE_REQUIRE(proven_ws_conn_send_text(left, PROVEN_LIT("no handshake needed")) == PROVEN_OK &&
                    proven_ws_conn_receive(right, proven_net_deadline_in(1000), &msg) == PROVEN_OK && msg.data.size == 19, "a message across");
    proven_ws_conn_destroy(left);
    proven_ws_conn_destroy(right);

    proven_http_server_stop(g_server);
    proven_job_group_wait(loop_thread, &running);
    proven_http_server_destroy(g_server);
    proven_job_system_close(g_workers);         /* 대화를 마무리하고 있는 작업 스레드를 기다린다 */
    proven_job_system_destroy(g_workers);
    proven_job_system_close(loop_thread);
    proven_job_system_destroy(loop_thread);
    return EXAMPLE_OK();
}
```

## 6. 코덱: 핸드셰이크

`ws.h`는 연결 없는 프로토콜이다. 바이트를 주면 그것이 무슨 뜻인지 말해 준다. 전송이 이 라이브러리가 연
소켓이 아닐 때, 한 루프에서 많은 연결을 서비스할 때, 소켓이 아예 없는 타깃에서 써라.

| API | 의도 | 반환 |
|---|---|---|
| `proven_ws_make_key(random, out)` | 여러분이 준 무작위 16바이트로 클라이언트의 `Sec-WebSocket-Key`(24글자)를 만든다. | 없음. |
| `proven_ws_accept_key(key, out)` | 키에 답하는 `Sec-WebSocket-Accept` 값(28글자). | `proven_err_t`: 키가 16바이트를 나타내는 Base64 24글자가 아니면 `INVALID_FORMAT`. |
| `proven_ws_check_request(&request, &key)` | 파싱된 요청에 대한 서버의 확인. | `proven_err_t`: `PROVEN_OK` - 101로 답하라. `NOT_FOUND` - WebSocket 업그레이드가 아니다. `UNSUPPORTED` - 다른 버전이다, `Sec-WebSocket-Version: 13`을 단 426으로 답하라. `INVALID_FORMAT` - 400으로 답하라. |
| `proven_ws_request_offers(&request, name)` | 요청이 서브프로토콜을 내놓는지. 정확히 비교한다. | `bool`. |
| `proven_ws_check_response(&response, key, offered, &protocol)` | 답에 대한 클라이언트의 확인: 101, 맞는 accept 값, 내놓았던 서브프로토콜, 확장 없음. | `proven_err_t`: 아니면 `INVALID_FORMAT`. |

accept 값은 키 뒤에 RFC가 정한 고정 문자열을 붙인 것의 SHA-1을 Base64로 적은 것이다. SHA-1은 서명
용도로는 깨졌지만 이 용도는 그것을 개의치 않는다. 여기에 비밀은 없고, 이 값은 WebSocket 서버가 *아닌*
HTTP 서버가 반사적으로 만들어 내지 않을 무언가이기만 하면 된다. 증명하는 것도 그것이 전부다. 서버를
인증하지 않고 클라이언트를 인증하지 않는다. 누구든 계산할 수 있다.

**확장의 이름을 댄 응답은** `proven_ws_check_response`가 **거절한다**: 아무것도 제안하지 않은 클라이언트가
요청하지 않은 프레임을 예고받는 것이기 때문이다. 압축을 제안한 클라이언트는 그 대신
`proven_ws_check_response_deflate`(9절)로 확인한다. 이 함수는 그 확장 하나를 제안한 형태 그대로만 받아들인다.

## 7. 코덱: 프레임

```text
typedef struct {
    bool fin;                   /* the last frame of its message */
    proven_u8 opcode;           /* PROVEN_WS_TEXT, _BINARY, _CONTINUATION, _CLOSE, _PING, _PONG */
    bool masked;
    proven_byte_t mask[4];
    proven_u64 length;          /* payload bytes that follow the header */
} proven_ws_frame_t;
```

| API | 의도 | 반환 |
|---|---|---|
| `proven_ws_frame_parse(data, &frame, &header_size)` | `data` 앞에서 프레임 헤더 - 2~14바이트 - 를 파싱한다. | `proven_err_t`: `NEED_MORE`. 예약 비트(첫째 비트는 메시지가 시작하는 곳에서는 `rsv1`로 알려 준다: 9절), 정의되지 않은 opcode, 가장 짧은 형태가 아닌 길이, 프래그먼트로 나뉘었거나 125바이트를 넘는 제어 프레임은 `INVALID_FORMAT`. |
| `proven_ws_frame_write(out, &len, &frame)` | 프레임 헤더를 덧붙인다. 페이로드는 그 뒤에 여러분이 쓴다. | `proven_err_t`: 파서가 거절할 프레임이면 `INVALID_ARG`. `OUT_OF_BOUNDS`. |
| `proven_ws_mask(data, mask, offset)` | 페이로드 바이트를 제자리에서 마스킹하거나 푼다. `offset`은 `data`가 페이로드 안에서 시작하는 위치다. | 없음. |
| `proven_ws_close_write(out, &len, code, reason)` | close 프레임의 페이로드를 덧붙인다. | `proven_err_t`: 보낼 수 없는 코드나 123바이트를 넘는 이유는 `INVALID_ARG`. `INVALID_ENCODING`. `OUT_OF_BOUNDS`. |
| `proven_ws_close_parse(payload, &code, &reason)` | 그것을 읽는다. 빈 페이로드는 코드 1005다. | `proven_err_t`: `INVALID_FORMAT`. `INVALID_ENCODING`. |
| `proven_ws_close_code_is_valid(code)` | 코드가 close 프레임에 실릴 수 있는지. | `bool`. |
| `proven_ws_close_code_for(err)` | 디코더 오류를 알리는 close 코드: 1002, 1007, 1009, 또는 1011. | `proven_u16`. |

**길이의 표기는 하나뿐이다.** 125까지는 둘째 바이트에, 65535까지는 두 바이트를 더, 그 너머는 여덟
바이트에 적는다. `proven_ws_frame_write`는 언제나 가장 짧은 것을 쓰고, `proven_ws_frame_parse`는 그
밖의 것을 거절한다 - 한 프레임에 두 가지 인코딩이 있으면 필터와 그 뒤의 프로그램이 프레임이 어디서
끝나는지를 두고 어긋나게 된다.

**클라이언트는 응용이 예측할 수 없는 키로 마스킹해야 한다.** 4바이트는 `proven_random_bytes`나 그것으로
시드한 생성기에서 얻어라 - 카운터나 상수는 목적을 무너뜨린다. 서버는 `masked`를 false로 두고 페이로드를
그대로 쓴다.

**코덱에는 무작위 원천이 없다.** 키와 마스크는 10장의 multipart 경계처럼 인자다. 그래서 같은 코드가
물어볼 운영체제가 없는 곳에서도 돈다.

## 8. 코덱: 디코더

헤더 파싱은 작은 부분이다. 받는 쪽이 제대로 해야 하는 것은 스트림이다. 제멋대로인 조각으로 도착하는
프레임, 이어 붙일 프래그먼트, 프래그먼트 사이의 제어 프레임, 프래그먼트 경계를 넘어 검사해야 하는
텍스트, 그리고 거절할 것들의 목록. `proven_ws_decoder_t`가 그것이다.

| API | 의도 | 반환 |
|---|---|---|
| `proven_ws_decoder_init(&decoder, from_client, max_message_bytes)` | 시작한다. `from_client`가 true면 모든 프레임이 마스킹되어 있어야 한다(서버의 디코더). false면 어느 것도 마스킹되어 있으면 안 된다. | 없음. |
| `proven_ws_decoder_feed(&decoder, in, &consumed, &event)` | 보고할 것이 생기거나 `in`을 다 쓸 때까지 `in`의 바이트를 소비한다. | `proven_err_t`: `INVALID_FORMAT`(close 1002). `INVALID_ENCODING`(1007). `OUT_OF_BOUNDS`(1009). close 프레임 뒤의 바이트는 `EOF`. |

```text
typedef struct {
    proven_ws_event_kind_t kind;   /* PROVEN_WS_EVENT_NONE, _DATA, _PING, _PONG, _CLOSE */
    bool text, first, last;        /* DATA: text or binary; the ends of the message */
    proven_mem_view_t data;        /* DATA: a piece, unmasked, inside `in`. PING, PONG: the payload. CLOSE: the reason. */
    proven_u16 close_code;         /* CLOSE */
} proven_ws_event_t;
```

루프는 10장의 본문 디코더에서 본 그것이다. 읽은 것을 넣고, 이벤트에 따라 행동하고, 나머지를 넣고,
이벤트가 `NONE`이면 더 읽는다.

**메시지는 붙잡히지 않고 지나간다.** `DATA` 이벤트는 메시지의 *조각*이고, 여러분이 넣은 버퍼 안에,
있던 자리에서 마스크가 풀린 채로 있다. `first`와 `last`가 메시지의 양 끝을 표시한다. 기가바이트짜리
메시지는 그만큼 많은 이벤트일 뿐 할당은 없다 - 조각을 잇고 싶다면 잇는 것은 여러분 몫이고, 한도도
여러분이 고른다(`ws_conn.h`가 `max_message_bytes`까지 대신 이어 준다).

**`in`에는 쓰기가 일어난다.** 마스크 풀기가 제자리에서 일어나므로 `in`은 `const`가 아니다. 바뀌면 안
되는 메모리를 넣지 마라.

**디코더가 거절하는 것**, 각각 그 close 코드를 알려 주는 오류와 함께:

| 스트림 | 오류 |
|---|---|
| 클라이언트가 보냈는데 마스킹되지 않은 프레임, 서버가 보냈는데 마스킹된 프레임 | `INVALID_FORMAT` |
| 예약 비트(첫째 비트는 있어서는 안 되는 곳에 있을 때, 또는 압축에 합의하지 않았을 때만: 9절), 정의되지 않은 opcode, 가장 짧은 형태가 아닌 길이 | `INVALID_FORMAT` |
| 125바이트를 넘거나 프래그먼트로 나뉜 제어 프레임 | `INVALID_FORMAT` |
| 열린 메시지가 없는데 온 continuation. 열린 메시지 안에서 시작한 새 메시지 | `INVALID_FORMAT` |
| 페이로드가 1바이트이거나, 보낼 수 없는 코드를 실은 close 프레임 | `INVALID_FORMAT` |
| UTF-8이 아닌 텍스트(프래그먼트를 넘어 검사한다). UTF-8이 아닌 close 이유 | `INVALID_ENCODING` |
| 프래그먼트를 합쳐 한도를 넘는 메시지 | `OUT_OF_BOUNDS` |

이들 가운데 어느 것이든 일어난 뒤에는 디코더가 같은 오류를 되풀이한다. 의도한 것이다. 프레임 하나가
틀리면 다음 프레임이 어디서 시작하는지 알 수 없고, 짐작하다가는 틀린 프레임 하나가 다른 대화가 된다.

**답하는 것은 여러분 몫이다.** 디코더는 ping을 보고할 뿐 pong을 보내지 않는다. close를 보고할 뿐 답하는
close를 보내지 않는다. 디코더에는 무엇이든 보낼 방법이 없다 - 그래서 어디서나 돌 수 있다.

테스트 스위트가 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_12_ws_frames.c -->
```c
#include <string.h>

/*
 * WebSocket 코덱만으로: 핸드셰이크의 두 값, 프레임 하나를 쓰고 읽기, 그리고 어색한 조각으로
 * 스트림을 받는 디코더. 소켓은 어디에도 없다 - "네트워크"는 배열이다.
 */

int main(void) {
    // ---- 여는 핸드셰이크 ----------------------------------------------------
    /* 클라이언트는 무작위 16바이트로 키를 만들고, 서버는 WebSocket을 말하는 것만이 계산할
     * 값으로 답한다. 이것은 RFC 6455에 실린 값이다. */
    proven_byte_t accept[PROVEN_WS_ACCEPT_SIZE];
    EXAMPLE_REQUIRE(proven_ws_accept_key(PROVEN_LIT("dGhlIHNhbXBsZSBub25jZQ=="), accept) == PROVEN_OK &&
                    memcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", sizeof accept) == 0, "the RFC's key and its answer");

    proven_byte_t random[16], key[PROVEN_WS_KEY_SIZE];
    EXAMPLE_REQUIRE(proven_random_bytes(random, sizeof random), "sixteen random bytes");
    proven_ws_make_key(random, key);
    EXAMPLE_REQUIRE(proven_ws_accept_key((proven_u8str_view_t){ key, sizeof key }, accept) == PROVEN_OK, "a key of our own, and its answer");

    /* 서버는 파싱된 요청을 보고 그것이 네 가지 중 무엇인지 안다. */
    static const char upgrade[] = "GET /chat HTTP/1.1\r\nHost: example.com\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                  "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Protocol: chat, superchat\r\n"
                                  "Sec-WebSocket-Version: 13\r\n\r\n";
    proven_http_header_t fields[16];
    proven_http_request_t request;
    proven_size_t head = 0;
    proven_u8str_view_t client_key;
    EXAMPLE_REQUIRE(proven_http_parse_request((proven_mem_view_t){ (const proven_byte_t *)upgrade, sizeof upgrade - 1 }, fields, 16, 0, &request, &head) == PROVEN_OK, "the request parses");
    EXAMPLE_REQUIRE(proven_ws_check_request(&request, &client_key) == PROVEN_OK, "it is a WebSocket upgrade: answer 101");
    EXAMPLE_REQUIRE(proven_ws_request_offers(&request, PROVEN_LIT("superchat")) && !proven_ws_request_offers(&request, PROVEN_LIT("mqtt")), "and it offers these subprotocols");

    /* 클라이언트는 답을 확인한다: 맞는 accept 값, 자기가 내놓은 서브프로토콜, 그 밖에는 없음. */
    static const char answer[] = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                 "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\nSec-WebSocket-Protocol: chat\r\n\r\n";
    proven_http_response_t response;
    proven_u8str_view_t chosen;
    EXAMPLE_REQUIRE(proven_http_parse_response((proven_mem_view_t){ (const proven_byte_t *)answer, sizeof answer - 1 }, fields, 16, 0, &response, &head) == PROVEN_OK, "the response parses");
    EXAMPLE_REQUIRE(proven_ws_check_response(&response, client_key, PROVEN_LIT("chat, superchat"), &chosen) == PROVEN_OK &&
                    proven_u8str_view_eq(chosen, PROVEN_LIT("chat")), "it answers this handshake, and chose chat");

    // ---- 프레임 하나 --------------------------------------------------------
    /* 클라이언트의 텍스트 프레임: 헤더, 그다음 4바이트 키와 XOR한 페이로드. */
    proven_byte_t wire[256];
    proven_mem_mut_t out = { wire, sizeof wire };
    proven_size_t len = 0;
    proven_ws_frame_t frame = { .fin = true, .opcode = PROVEN_WS_TEXT, .masked = true, .mask = { 0x37, 0xfa, 0x21, 0x3d }, .length = 5 };
    EXAMPLE_REQUIRE(proven_ws_frame_write(out, &len, &frame) == PROVEN_OK && len == 6, "a six-byte header");
    memcpy(wire + len, "Hello", 5);
    proven_ws_mask((proven_mem_mut_t){ wire + len, 5 }, frame.mask, 0);
    len += 5;
    EXAMPLE_REQUIRE(memcmp(wire, "\x81\x85\x37\xfa\x21\x3d\x7f\x9f\x4d\x51\x58", 11) == 0, "byte for byte the masked Hello of RFC 6455");

    proven_ws_frame_t parsed;
    proven_size_t header_size = 0;
    EXAMPLE_REQUIRE(proven_ws_frame_parse((proven_mem_view_t){ wire, 3 }, &parsed, &header_size) == PROVEN_ERR_NEED_MORE, "half a header: read more");
    EXAMPLE_REQUIRE(proven_ws_frame_parse((proven_mem_view_t){ wire, len }, &parsed, &header_size) == PROVEN_OK &&
                    parsed.opcode == PROVEN_WS_TEXT && parsed.masked && parsed.length == 5 && header_size == 6, "the header, read back");

    // ---- 스트림을 디코딩하기 ------------------------------------------------
    /* 사이에 ping이 낀 두 프래그먼트짜리 텍스트 메시지, 그다음 close. */
    len = 0;
    frame = (proven_ws_frame_t){ .fin = false, .opcode = PROVEN_WS_TEXT, .length = 3 };
    EXAMPLE_REQUIRE(proven_ws_frame_write(out, &len, &frame) == PROVEN_OK, "first fragment");
    memcpy(wire + len, "Hel", 3); len += 3;
    frame = (proven_ws_frame_t){ .fin = true, .opcode = PROVEN_WS_PING, .length = 0 };
    EXAMPLE_REQUIRE(proven_ws_frame_write(out, &len, &frame) == PROVEN_OK, "a ping, in the middle of the message");
    frame = (proven_ws_frame_t){ .fin = true, .opcode = PROVEN_WS_CONTINUATION, .length = 2 };
    EXAMPLE_REQUIRE(proven_ws_frame_write(out, &len, &frame) == PROVEN_OK, "last fragment");
    memcpy(wire + len, "lo", 2); len += 2;
    proven_byte_t close_payload[PROVEN_WS_MAX_CONTROL];
    proven_size_t close_len = 0;
    EXAMPLE_REQUIRE(proven_ws_close_write((proven_mem_mut_t){ close_payload, sizeof close_payload }, &close_len, PROVEN_WS_CLOSE_NORMAL, PROVEN_LIT("bye")) == PROVEN_OK, "a close payload");
    frame = (proven_ws_frame_t){ .fin = true, .opcode = PROVEN_WS_CLOSE, .length = close_len };
    EXAMPLE_REQUIRE(proven_ws_frame_write(out, &len, &frame) == PROVEN_OK, "the close frame");
    memcpy(wire + len, close_payload, close_len); len += close_len;

    /* 클라이언트가 서버가 보낸 것을 디코딩한다: `false` - 이 프레임들은 마스킹되어 있으면
     * 안 된다. 스트림은 한 번에 4바이트씩 도착하지만 디코더는 개의치 않는다. */
    proven_ws_decoder_t decoder;
    proven_ws_decoder_init(&decoder, false, 1024);
    char message[32];
    proven_size_t message_len = 0;
    int pings = 0;
    proven_u16 close_code = 0;
    for (proven_size_t pos = 0; pos < len;) {
        proven_size_t n = len - pos < 4 ? len - pos : 4;
        proven_mem_mut_t piece = { wire + pos, n };
        while (piece.size > 0) {
            proven_size_t used = 0;
            proven_ws_event_t ev;
            EXAMPLE_REQUIRE(proven_ws_decoder_feed(&decoder, piece, &used, &ev) == PROVEN_OK, "the stream decodes");
            piece.ptr += used;
            piece.size -= used;
            if (ev.kind == PROVEN_WS_EVENT_DATA) {
                memcpy(message + message_len, ev.data.ptr, ev.data.size);      /* 메시지의 한 조각 */
                message_len += ev.data.size;
            } else if (ev.kind == PROVEN_WS_EVENT_PING) {
                pings++;                                                       /* 실제 프로그램은 pong으로 답한다 */
            } else if (ev.kind == PROVEN_WS_EVENT_CLOSE) {
                close_code = ev.close_code;
            }
        }
        pos += n;
    }
    EXAMPLE_REQUIRE(message_len == 5 && memcmp(message, "Hello", 5) == 0, "two fragments, one message");
    EXAMPLE_REQUIRE(pings == 1 && close_code == PROVEN_WS_CLOSE_NORMAL, "the ping between them, and the close");

    // ---- 받는 쪽이 거절하는 것 ----------------------------------------------
    /* 서버가 보낸 마스킹된 프레임은 프로토콜 위반이다. 오류가 보낼 close 코드를 알려 준다. */
    proven_byte_t bad[] = { 0x81, 0x81, 1, 2, 3, 4, 'x' ^ 1 };
    proven_ws_event_t ev;
    proven_size_t used = 0;
    proven_ws_decoder_init(&decoder, false, 1024);
    proven_err_t err = proven_ws_decoder_feed(&decoder, (proven_mem_mut_t){ bad, sizeof bad }, &used, &ev);
    EXAMPLE_REQUIRE(err == PROVEN_ERR_INVALID_FORMAT && proven_ws_close_code_for(err) == PROVEN_WS_CLOSE_PROTOCOL_ERROR, "refused: close with 1002");

    /* close 프레임의 페이로드만 따로 읽기. 코드 1005는 "코드가 없었다"는 뜻이고 그 자체는
     * 보낼 수 없다. */
    proven_u16 code = 0;
    proven_u8str_view_t reason;
    EXAMPLE_REQUIRE(proven_ws_close_parse((proven_mem_view_t){ close_payload, close_len }, &code, &reason) == PROVEN_OK &&
                    code == 1000 && proven_u8str_view_eq(reason, PROVEN_LIT("bye")), "code and reason");
    EXAMPLE_REQUIRE(proven_ws_close_code_is_valid(4000) && !proven_ws_close_code_is_valid(PROVEN_WS_CLOSE_NO_STATUS), "which codes may travel");

    return EXAMPLE_OK();
}
```

## 9. 압축된 메시지

```c
bool proven_ws_conn_compressed(const proven_ws_conn_t *conn);            /* ws_conn.h */
bool proven_ws_stream_compressed(const proven_ws_stream_t *ws);          /* ws_event.h, 15장 */

#define PROVEN_WS_DEFLATE_OFFER "permessage-deflate; client_no_context_takeover; server_no_context_takeover"
proven_err_t proven_ws_deflate_accept(const proven_http_request_t *request, proven_mem_mut_t out, proven_size_t *len, int *window_bits);
proven_err_t proven_ws_check_response_deflate(const proven_http_response_t *response, proven_u8str_view_t key,
                                              proven_u8str_view_t offered, proven_u8str_view_t *protocol, bool *deflate);
void proven_ws_decoder_allow_compressed(proven_ws_decoder_t *decoder);
```

WebSocket 연결은 핸드셰이크에서 메시지를 압축하기로 합의할 수 있다: 이 확장은
`permessage-deflate`(RFC 7692)라고 부르고, 모든 브라우저가 제안하며, [16장](manual-16-compression-ko.md)의
DEFLATE다. JSON 한 쪽이나 로그 여러 줄은 3분의 1 이하로 줄어든다. 수십 바이트짜리 메시지는 얻는 것이 적은데, 까닭은 아래에 있다.

**요청하기 전에는 꺼져 있다.** 연결 설정 - `proven_ws_conn_config_t`나 15장의
`proven_ws_event_config_t` - 에서 `compress`를 켠다. 그러면 클라이언트는 확장을 제안하고 서버는 제안을
받으면 수락한다. 상대가 동의했는지는 `proven_ws_conn_compressed`와 `proven_ws_stream_compressed`가
말해 준다. **그 밖에는 아무것도 달라지지 않는다**: 같은 호출로 보내고 받으며, 상대가 동의하지 않은
연결은 그저 압축 없이 이어진다.

| | 압축에 합의했을 때 |
|---|---|
| 보내는 메시지 | 64바이트 이상이면 압축된다. 더 짧으면 그대로 간다 |
| 받는 메시지 | 보기 전에 풀린다: `proven_ws_conn_receive`에서는 통째로, 이벤트 구동 서버에서는 최대 16 KiB 조각으로 |
| `max_message_bytes` | **풀린** 크기를 센다. 몇백 바이트가 그것을 넘겨 펼쳐지면, 너무 큰 평문 메시지와 마찬가지로 1009로 연결이 끝난다 |
| 텍스트 | 푼 다음 UTF-8인지 검사한다. 아니면 1007 |
| 풀리지 않는 바이트 | 1007 |
| 합의하지 않은 연결에서 압축 표시가 된 프레임 | 1002 |

**메시지는 하나하나 따로 압축된다.** 확장은 각 쪽이 32 KiB 윈도를 한 메시지에서 다음 메시지로 이어 갈
수 있게 하는데, 작고 비슷한 메시지에는 도움이 된다 - 그리고 연결이 열려 있는 내내 연결마다 압축기와
해제기를 하나씩 쥐게 된다. 이 라이브러리는 언제나 다른 쪽 형태("no context takeover", 양방향)로
협상한다: 압축기는 한 번의 전송 동안만, 해제기는 압축된 메시지의 첫 프레임부터 마지막 프레임까지만
존재하고, **유휴 연결은 압축 상태를 전혀 쥐지 않는다**. 조용한 연결 만 개는 압축이 없을 때와 같은
비용이 든다. 대가는 짧은 메시지가 자기 자신만을 참조할 수 있다는 것이다.

### 예제: 필드 하나, 그리고 그것이 바꾸는 것

<!-- example: manual/examples/ko/ex_12_ws_compressed.c -->
```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 프로그램이 쓰는 방식 그대로의 압축된 WebSocket 메시지: 설정의 필드 하나.
 *
 * 여기서 서버는 이벤트 구동 서버이고 클라이언트는 블로킹 클라이언트다. 둘 다 `compress`를
 * 켠다. 핸드셰이크가 합의한다. 그 뒤로 모든 호출은 전과 똑같이 쓰고, 네트워크를 건너는
 * 것은 말한 것의 일부분이다.
 */

typedef struct {
    proven_loop_t *loop;
    bool compressed;                   /* 서버가 합의됐다고 본 것 */
    proven_size_t received, largest, pieces;
} app_t;

/* 압축된 메시지는 풀린 채로, 최대 16 KiB 조각으로 도착한다. 여러분이 쥐지 않는 한 전체를
 * 쥐는 것은 없다. 이 핸들러는 세기만 하고, 메시지가 끝나면 답한다. */
static void on_message(void *ctx, proven_ws_stream_t *ws, bool text, proven_mem_view_t piece, bool first, bool last) {
    app_t *app = ctx;
    (void)text;
    if (first) { app->received = 0; app->pieces = 0; app->largest = 0; }
    app->received += piece.size;
    app->pieces++;
    if (piece.size > app->largest) app->largest = piece.size;
    if (!last) return;
    char answer[64];
    int n = snprintf(answer, sizeof answer, "%u bytes", (unsigned)app->received);
    /* 여느 메시지처럼 보낸다. 짧은 것은 그대로 간다: 64바이트 미만에서는 얻을 것이 없다. */
    (void)proven_ws_stream_send(ws, true, (proven_mem_view_t){ (const proven_byte_t *)answer, (proven_size_t)n });
}

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    app_t *app = ctx;
    proven_ws_event_config_t config = {
        .on = { .on_message = on_message },
        .ctx = app,
        .compress = true,              /* 클라이언트가 제안하면 확장을 받아들인다 */
        .max_message_bytes = 256 * 1024,   /* 풀린 뒤의 메시지에 걸리는 한도: 몇 바이트가 기가바이트가 될 수는 없다 */
    };
    proven_ws_stream_t *ws = NULL;
    if (proven_ws_event_accept(stream, head, &config, &ws) != PROVEN_OK) {
        (void)proven_http_stream_respond(stream, 400, NULL, 0, (proven_mem_view_t){ 0 });
        return;
    }
    /* 합의됐는지는 클라이언트에도 달렸다. 어느 쪽이든 달리 할 일은 없다. */
    app->compressed = proven_ws_stream_compressed(ws);
}

static void run(void *arg) { (void)proven_loop_run(((app_t *)arg)->loop); }

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    app_t app = { 0 };
    EXAMPLE_REQUIRE(proven_loop_create(heap, &app.loop) == PROVEN_OK, "a loop");
    proven_http_event_server_config_t server_config = { .on = { .on_request = on_request }, .ctx = &app };
    proven_http_event_server_t *server = NULL;
    proven_net_addr_t at;
    EXAMPLE_REQUIRE(proven_http_event_server_create(app.loop, &server_config, &server) == PROVEN_OK, "a server");
    proven_err_t err = proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        proven_http_event_server_destroy(server);
        proven_loop_destroy(app.loop);
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "it listens");
    proven_job_sys_t *threads = NULL;
    proven_job_group_t running;
    proven_job_group_init(&running);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &threads) == PROVEN_OK &&
                    proven_job_group_submit(threads, &running, run, &app) == PROVEN_OK, "the loop runs on another thread");

    // ---- 압축을 요청하는 클라이언트 ----------------------------------------------
    proven_http_client_config_t client_config = { .alloc = heap };
    proven_http_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&client_config, &client) == PROVEN_OK, "an HTTP client");
    char url[64];
    int n = snprintf(url, sizeof url, "ws://127.0.0.1:%u/", (unsigned)at.port);
    proven_ws_conn_config_t ws_config = { .alloc = heap, .compress = true };       /* 확장을 제안한다 */
    proven_ws_conn_t *conn = NULL;
    EXAMPLE_REQUIRE(proven_ws_conn_connect(client, (proven_u8str_view_t){ (const proven_byte_t *)url, (proven_size_t)n }, NULL, 0, PROVEN_LIT(""),
                                           &ws_config, &conn, NULL) == PROVEN_OK, "connected");
    EXAMPLE_REQUIRE(proven_ws_conn_compressed(conn), "and the server agreed to compress");

    /* 보고서 십만 바이트. 전송되는 것은 몇백 바이트다. */
    static char report[100000];
    for (proven_size_t i = 0; i < sizeof report; ++i) report[i] = "temperature 21.5, humidity 40\n"[i % 30];
    proven_ws_message_t msg;
    EXAMPLE_REQUIRE(proven_ws_conn_send_text(conn, (proven_u8str_view_t){ (const proven_byte_t *)report, sizeof report }) == PROVEN_OK &&
                    proven_ws_conn_receive(conn, proven_net_deadline_in(5000), &msg) == PROVEN_OK &&
                    msg.data.size == 12 && memcmp(msg.data.ptr, "100000 bytes", 12) == 0, "the server received all of it");

    /* 한도는 풀린 크기에 걸린다. 한 글자 삼십만 개는 압축하면 몇백 바이트다 - 그래도
     * 너무 많다: 서버는 1009로 연결을 끝낸다. */
    static char flood[300000];
    memset(flood, 'x', sizeof flood);
    EXAMPLE_REQUIRE(proven_ws_conn_send_text(conn, (proven_u8str_view_t){ (const proven_byte_t *)flood, sizeof flood }) == PROVEN_OK, "a message that inflates past the server's limit");
    EXAMPLE_REQUIRE(proven_ws_conn_receive(conn, proven_net_deadline_in(5000), &msg) == PROVEN_ERR_EOF && proven_ws_conn_close_code(conn) == 1009, "is answered with 1009, however small it was on the wire");
    proven_ws_conn_destroy(conn);
    proven_http_client_destroy(client);

    proven_loop_stop(app.loop);
    proven_job_group_wait(threads, &running);
    EXAMPLE_REQUIRE(app.compressed, "the server found the extension agreed as well");
    proven_http_event_server_destroy(server);
    proven_loop_destroy(app.loop);
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    return EXAMPLE_OK();
}
```

### 예제: 코덱만으로 다루는 확장

코덱을 직접 구동하는 프로그램(6절부터 8절)을 위한 것: 핸드셰이크 함수, 프레임의 비트, 그리고 네 바이트.

<!-- example: manual/examples/ko/ex_12_ws_deflate.c -->
```c
#include <stdio.h>
#include <string.h>

/*
 * 코덱만으로 다루는 압축된 WebSocket 메시지: 소켓도 드라이버도 없다.
 *
 * permessage-deflate라는 확장은 세 가지로 이루어진다. 핸드셰이크가 그것에 합의한다.
 * 프레임 헤더의 비트 하나가 "이 메시지는 압축되었다"고 말한다. 그리고 페이로드는 마지막
 * 네 바이트를 뗀 DEFLATE다. 앞의 둘은 코덱이 하고, 셋째는 deflate.h가 한다.
 *
 * ws_conn.h나 ws_event.h를 쓰는 프로그램은 필드 하나를 정할 뿐 이것을 볼 일이 없다.
 */

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    // ---- 핸드셰이크 ------------------------------------------------------------------
    /* 클라이언트는 헤더 하나로 요청한다. PROVEN_WS_DEFLATE_OFFER는 이 라이브러리가 말하는 형태다:
     * 양쪽 방향 모두, 메시지 하나하나를 따로 압축한다. */
    proven_http_header_t asked[] = {
        { PROVEN_LIT("Sec-WebSocket-Extensions"), PROVEN_LIT(PROVEN_WS_DEFLATE_OFFER) },
    };
    proven_http_request_t request = { .headers = asked, .header_count = 1 };

    /* 서버는 제안된 것을 보고 답을 쓴다 - 또는 맞춰 줄 수 있는 것이 없으면
     * (PROVEN_ERR_NOT_FOUND) 연결은 압축 없이 이어진다. */
    proven_byte_t answer[PROVEN_WS_DEFLATE_ANSWER_MAX];
    proven_size_t answer_len = 0;
    int window_bits = 0;
    EXAMPLE_REQUIRE(proven_ws_deflate_accept(&request, (proven_mem_mut_t){ answer, sizeof answer }, &answer_len, &window_bits) == PROVEN_OK, "the offer can be met");
    printf("the server answers: %.*s\n", (int)answer_len, (const char *)answer);
    EXAMPLE_REQUIRE(window_bits == 15, "and may compress with the full window");

    /* 브라우저의 제안은 모양이 다르지만, 그것도 맞춰 준다. */
    proven_http_header_t browser[] = { { PROVEN_LIT("Sec-WebSocket-Extensions"), PROVEN_LIT("permessage-deflate; client_max_window_bits") } };
    proven_http_request_t from_browser = { .headers = browser, .header_count = 1 };
    proven_size_t len2 = 0;
    EXAMPLE_REQUIRE(proven_ws_deflate_accept(&from_browser, (proven_mem_mut_t){ answer, sizeof answer }, &len2, &window_bits) == PROVEN_OK, "a browser's offer too");

    /* 클라이언트는 답 전체를 검사한다: 늘 보던 것들, 그리고 확장이 요청한 형태로 돌아왔는지.
     * (키와 accept 값은 RFC 6455에 실린 한 쌍이다.) */
    proven_http_header_t answered[] = {
        { PROVEN_LIT("Upgrade"), PROVEN_LIT("websocket") },
        { PROVEN_LIT("Connection"), PROVEN_LIT("Upgrade") },
        { PROVEN_LIT("Sec-WebSocket-Accept"), PROVEN_LIT("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") },
        { PROVEN_LIT("Sec-WebSocket-Extensions"), { answer, answer_len } },
    };
    proven_http_response_t response = { .status = 101, .headers = answered, .header_count = 4 };
    proven_u8str_view_t protocol;
    bool compressed = false;
    EXAMPLE_REQUIRE(proven_ws_check_response_deflate(&response, PROVEN_LIT("dGhlIHNhbXBsZSBub25jZQ=="), PROVEN_LIT(""), &protocol, &compressed) == PROVEN_OK && compressed,
                    "the client finds the extension agreed");

    // ---- 압축된 메시지가 도착한다 ----------------------------------------------------
    /* 이 프레임은 RFC 7692에 있다: 서버가 보낸, 압축된 텍스트 "Hello". 압축하지 않은 텍스트
     * 프레임이라면 81일 첫 바이트가 c1이다: 더해진 비트가 RSV1이다. */
    proven_byte_t wire[] = { 0xc1, 0x07, 0xf2, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00 };

    proven_ws_decoder_t decoder;
    proven_ws_event_t event;
    proven_size_t used = 0;
    /* 확장에 대해 듣지 못한 디코더는 그 비트를 거부한다. 그래야 한다. */
    proven_byte_t copy[sizeof wire];
    memcpy(copy, wire, sizeof wire);
    proven_ws_decoder_init(&decoder, false, 0);
    EXAMPLE_REQUIRE(proven_ws_decoder_feed(&decoder, (proven_mem_mut_t){ copy, sizeof copy }, &used, &event) == PROVEN_ERR_INVALID_FORMAT, "without the extension, the bit is an error");

    proven_ws_decoder_init(&decoder, false, 0);
    proven_ws_decoder_allow_compressed(&decoder);
    EXAMPLE_REQUIRE(proven_ws_decoder_feed(&decoder, (proven_mem_mut_t){ wire, sizeof wire }, &used, &event) == PROVEN_OK &&
                    event.kind == PROVEN_WS_EVENT_DATA && event.compressed && event.last, "with it, the message is handed over, marked compressed");

    /* 디코더는 압축을 풀지 않는다: 아무것도 할당하지 않는다. 건네주는 바이트는 raw DEFLATE이고,
     * 보낸 쪽이 마지막 네 바이트 - 언제나 00 00 ff ff - 를 떼어 냈다. 그것을 다시 붙이고
     * 푼다. 나오는 양의 한도는 여러분 버퍼의 크기다. */
    proven_inflate_t *z = NULL;
    EXAMPLE_REQUIRE(proven_inflate_create(heap, PROVEN_DEFLATE_RAW, &z) == PROVEN_OK, "a decompressor for raw DEFLATE");
    static const proven_byte_t tail[4] = { 0x00, 0x00, 0xff, 0xff };
    proven_byte_t text[64];
    proven_size_t taken = 0, made = 0, total = 0;
    bool ended = false;
    EXAMPLE_REQUIRE(proven_inflate(z, event.data, &taken, (proven_mem_mut_t){ text, sizeof text }, &made, &ended) == PROVEN_OK && taken == event.data.size, "the payload");
    total = made;
    EXAMPLE_REQUIRE(proven_inflate(z, (proven_mem_view_t){ tail, 4 }, &taken, (proven_mem_mut_t){ text + total, sizeof text - total }, &made, &ended) == PROVEN_OK && taken == 4, "then the four bytes");
    total += made;
    EXAMPLE_REQUIRE(total == 5 && memcmp(text, "Hello", 5) == 0, "and the message is Hello");
    printf("the message: %.*s\n", (int)total, (const char *)text);
    /* 이제야 - 그 전이 아니라 - 텍스트이고, 검사는 여러분 몫이다: 디코더는 할 수 없었다. */
    proven_inflate_destroy(z);

    // ---- 압축된 메시지를 보낸다 ------------------------------------------------------
    /* 반대 방향: sync flush로 압축하면 같은 네 바이트로 끝나는데, 그것을 떼어 낸다.
     * 이것을 싣는 프레임에는 `rsv1`이 켜져 있다. */
    static const char said[] = "to be, or not to be, that is the question: to be, or not to be";
    proven_deflate_options_t options = { .alloc = heap, .format = PROVEN_DEFLATE_RAW };
    proven_deflate_t *c = NULL;
    EXAMPLE_REQUIRE(proven_deflate_create(&options, &c) == PROVEN_OK, "a compressor");
    proven_byte_t packed[128];
    EXAMPLE_REQUIRE(proven_deflate(c, (proven_mem_view_t){ (const proven_byte_t *)said, sizeof said - 1 }, &taken, (proven_mem_mut_t){ packed, sizeof packed }, &made,
                                   PROVEN_DEFLATE_FLUSH_SYNC, &ended) == PROVEN_OK && taken == sizeof said - 1 && made > 4, "compressed, with a sync flush");
    EXAMPLE_REQUIRE(memcmp(packed + made - 4, tail, 4) == 0, "which ends in 00 00 ff ff");
    proven_size_t payload = made - 4;
    proven_deflate_destroy(c);

    proven_ws_frame_t frame = { .fin = true, .opcode = PROVEN_WS_TEXT, .rsv1 = true, .length = payload };
    proven_byte_t header[PROVEN_WS_MAX_FRAME_HEADER];
    proven_size_t header_len = 0;
    EXAMPLE_REQUIRE(proven_ws_frame_write((proven_mem_mut_t){ header, sizeof header }, &header_len, &frame) == PROVEN_OK && header[0] == 0xc1, "a frame header with the bit");
    printf("%u bytes of text go out as %u\n", (unsigned)(sizeof said - 1), (unsigned)(header_len + payload));
    EXAMPLE_REQUIRE(header_len + payload < sizeof said - 1, "fewer than were said");
    return EXAMPLE_OK();
}
```

### 참조

| API | 의도 | 반환 |
|---|---|---|
| `compress` (두 연결 설정) | 확장을 제안한다(클라이언트) 또는 제안받으면 수락한다(서버). | - |
| `compress_level`, `compress_window_bits` (둘 다) | 16장 4절과 같다: 1부터 9 또는 -1. 9부터 15. 0이면 6과 15. | 범위를 벗어나면 `INVALID_ARG`. |
| `proven_ws_conn_compressed(conn)`, `proven_ws_stream_compressed(ws)` | 상대가 동의했는가. | `bool`. |
| `PROVEN_WS_DEFLATE_OFFER` | 클라이언트가 보내는 `Sec-WebSocket-Extensions` 값. | 문자열 리터럴. |
| `proven_ws_deflate_accept(request, out, &len, &window_bits)` | 요청의 제안 가운데 서버가 고른다. 답을 쓴다. | `proven_err_t`: 맞춰 줄 제안이 없으면 `NOT_FOUND` - 압축 없이 간다. `OUT_OF_BOUNDS`(`PROVEN_WS_DEFLATE_ANSWER_MAX` 바이트면 충분하다). |
| `proven_ws_check_response_deflate(response, key, offered, &protocol, &deflate)` | 제안을 보낸 클라이언트를 위한 `proven_ws_check_response`. | 같고, 제안하지 않은 형태의 답에는 `INVALID_FORMAT`. |
| `proven_ws_decoder_allow_compressed(decoder)` | 디코더가 압축된 메시지를 `compressed` 표시와 함께 통과시키게 한다. 풀지는 않는다. | 없음. |
| `proven_ws_frame_t`의 `rsv1` | 압축된 메시지를 표시하는 비트. 첫 프레임에 있다. | - |

### 주의사항, 그리고 무엇이 잘못되는가

- **비밀과 상대가 고른 글자가 함께 실린 메시지는 압축하지 말 것.** 압축된 메시지의 길이는 연결을
  지켜보는 이에게 그 둘이 얼마나 겹치는지를 알려 준다. TLS가 있든 없든 그렇다 - 16장 5절에서 설명한
  누출이다. 메시지를 하나씩 따로 압축하므로 한 메시지가 다른 메시지를 새게 하지는 못한다. 한 메시지
  안의 것은 여전히 샐 수 있다. 여기서 압축은 연결 단위다: 그런 메시지가 오가는 연결은 압축을 요청하지
  말아야 한다.
- **한도는 풀린 크기에 걸리며, 정해 두어야 한다.** 킬로바이트가 기가바이트가 되는 것을 막는 것이
  `max_message_bytes`다. 기본값은 1 MiB.
- **raw DEFLATE에는 체크섬이 없다.** 전송 중 손상된 압축 메시지는 오류 없이 다른 바이트로 풀릴 수
  있다. TLS가 그것을 막는다. TLS가 없다면 중요한 것은 직접 검사한다.
- **맞춰 줄 수 없는 것만 제안하는 상대는 거절하지 않고 압축 없이 응대한다.** 그런 제안은 하나뿐이다:
  256바이트 압축 윈도(`server_max_window_bits=8`)인데, 이 압축기에는 그 크기가 없다. 클라이언트 자신의
  윈도 크기(`client_max_window_bits`)는 받아들이고 무시한다: 어떤 윈도든 읽을 수 있다.
- **다른 형태로 수락하는 서버는 클라이언트가 거부한다.** `server_no_context_takeover` 없이 제안을
  받아들이는 서버는 메시지에 걸쳐 압축할 텐데, 이쪽은 그것을 읽지 않는다. 연결은
  `PROVEN_ERR_INVALID_FORMAT`으로 실패한다. RFC 7692를 따르는 서버는 제안을 그대로 수락하거나 거절한다.
- **압축된 메시지가 오가는 동안의 메모리:** 하나를 푸는 데 약 35 KiB(이벤트 구동 서버에서는 51 KiB),
  하나를 압축하는 데 윈도에 따라 11 KiB에서 325 KiB까지이며 메시지가 필요로 하는 것보다 크지 않다.
  메시지를 위해 할당하고 메시지와 함께 해제한다 - 그래서 압축을 쓰는 바쁜 연결은, 쓰지 않는 연결이
  하지 않는 할당을 메시지마다 한다.
- **코덱만 쓸 때 텍스트 검사는 여러분 몫이다.** 디코더는 아직 텍스트가 아닌 바이트를 검사할 수 없다.
  푼 다음에 검사한다. 그리고 나오는 양을 센다: 디코더의 한도는 압축된 바이트를 센다.

## 10. 여기에 없는 것

- **윈도를 한 메시지에서 다음 메시지로 이어 가는 압축**("context takeover"), 그리고
  `permessage-deflate` 말고 다른 확장. 9절.
- **여러 연결을 한꺼번에 기다리는 호출.** §5를 보라.
- **한 연결에서 동시에 보내고 받기.**
- **자동 ping, 재연결, 백오프.** 정책이고, 여러분의 것이다.
- **`ws_conn.h`로 메모리보다 큰 메시지 다루기.** 메시지는 통째로 조립된다. 흘려 받으려면 디코더를 써라.
- **`Origin` 확인.** §2를 보라.
- **HTTP/2 WebSocket(RFC 8441).**
