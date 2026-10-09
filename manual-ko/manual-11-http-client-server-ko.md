# 11장: HTTP 클라이언트와 서버

**5부 — 운영체제와 대화하기. 선행 조건: [9장](manual-09-networking-ko.md)의 주소·기한·전송과
[10장](manual-10-http-ko.md)의 요청·응답·헤더. 핸들러를 job system에서 돌리려면
[6장](manual-06-execution-and-platform-ko.md)의 §1.**
**이 장을 마치면** 여러분의 함수 하나로 HTTP를 서비스할 수 있다. 모든 기다림에는 한도가 있고, 형식이
어긋난 요청은 여러분에게 닿기 전에 거절된다. 그리고 URL을 가져올 수 있다 - 리다이렉트와 로그인과
프록시를 거쳐서, 본문은 여러분의 속도로 읽으면서.

이 장은 `http_server.h`와 `http_client.h`를 다룬다. 둘 다 소켓이 필요하다.
[프리스탠딩(freestanding)](manual-freestanding-ko.md) 빌드에는 들어가지 않고, 호스티드(hosted)
빌드에서는 `PROVEN_NO_NET`으로 빠진다.

## 목차

1. [이것이 무엇이고, 무엇 하나가 아닌가](#1-이것이-무엇이고-무엇-하나가-아닌가)
2. [서버](#2-서버)
3. [핸들러 안에서](#3-핸들러-안에서)
4. [서버가 묻지 않고 하는 일](#4-서버가-묻지-않고-하는-일)
5. [핸들러를 돌리는 두 가지 방법](#5-핸들러를-돌리는-두-가지-방법)
6. [클라이언트](#6-클라이언트)
7. [요청 하나, 응답 하나](#7-요청-하나-응답-하나)
8. [리다이렉트, 인증 요구, 쿠키](#8-리다이렉트-인증-요구-쿠키)
9. [연결과 프록시](#9-연결과-프록시)
10. [TLS가 붙을 자리](#10-tls가-붙을-자리)
11. [프로토콜 바꾸기](#11-프로토콜-바꾸기)
12. [여기에 없는 것](#12-여기에-없는-것)

## 1. 이것이 무엇이고, 무엇 하나가 아닌가

10장은 코덱이다. 바이트를 메시지로, 메시지를 바이트로 바꾸며 소켓은 건드리지 않는다. 9장은 소켓이고
HTTP를 모른다. 이 두 헤더는 그 사이의 부분이다 - 연결을 받고, 읽고, 파싱하고, 시간 제한을 걸고,
연결을 열어 두는 루프와, 클라이언트 쪽에서 그것을 거울처럼 뒤집은 것.

이것은 드라이버이고 그 위의 것은 아무것도 아니다. 서버에는 라우터도, 정적 파일 핸들러도, 세션도,
템플릿도 없다. 요청 하나로 함수 하나를 부른다. 클라이언트에는 재시도 정책도, 캐시도, JSON도 없다.
요청 하나를 보내고 응답 하나를 준다. 그 위에 프로그램이 짓는 것은 프로그램의 것이다.

**TLS가 없다.** 이 라이브러리에는 아직 없다. 서버는 평문 HTTP만 말한다. 클라이언트는 `https` URL을
`PROVEN_ERR_UNSUPPORTED`로 거절한다 - 슬그머니 `http`로 대신 가져오지 않는다 - . 여러분이 암호화를
직접 공급하는 경우만 예외다(§10). TLS가 들어오기 전까지 이 둘이 보내고 받는 모든 것은 경로 위의
누구든 읽을 수 있고 바꿀 수 있다.

- 서버는 TLS를 끝내 주는 것 - 리버스 프록시, 로드 밸런서 - 뒤에 두거나, loopback 인터페이스나
  믿을 수 있는 네트워크 안에 두어라.
- 클라이언트로 다른 기계에 비밀번호나 토큰이나 세션 쿠키를 보내지 마라.

이것은 코드가 오늘 하는 일을 적은 것이지, 따져 보고 받아들일 권고가 아니다.

## 2. 서버

```text
static void handle(void *ctx, proven_http_exchange_t *x) {
    (void)proven_http_exchange_respond(x, 200, NULL, 0, body);
}

proven_http_server_config_t config = {0};
config.alloc = proven_heap_allocator();
config.handler = handle;
proven_http_server_create(&config, &server);
proven_http_server_listen(server, proven_net_addr_any(PROVEN_NET_FAMILY_IPV4, 8080), NULL);
proven_http_server_run(server);          /* until proven_http_server_stop */
proven_http_server_destroy(server);
```

### 참조

| API | 의도 | 반환 |
|---|---|---|
| `proven_http_server_create(&config, &server)` | 서버를 만든다. 아직 listen하지 않는다. | `proven_err_t`: 할당자(allocator)나 핸들러가 없으면 `INVALID_ARG`. `NOMEM`. 소켓이 모자라면 `BUSY`. |
| `proven_http_server_listen(server, at, &bound)` | 주소 하나에서 listen한다. 넷까지 - 이를테면 IPv4 하나와 IPv6 하나. `bound`(선택)는 실제로 묶인 주소를 받으므로 포트 0을 쓸 수 있다. | `proven_net_listen`의 오류들. 다섯 번째에는 `OUT_OF_BOUNDS`. |
| `proven_http_server_run(server)` | 멈출 때까지 서비스한다. | `proven_err_t`: stop 뒤에는 `PROVEN_OK`. |
| `proven_http_server_poll(server, until)` | 한 바퀴: 무언가 준비되거나 `until`이 될 때까지 기다리고, 준비된 것을 처리한다. 자기 루프가 있는 프로그램을 위한 것이다. | `proven_err_t`: 처리한 것이 있으면 `PROVEN_OK`, 없으면 `TIMEOUT`. |
| `proven_http_server_stop(server)` | `run`이 돌아오게 한다. 어느 스레드에서든, 핸들러 안에서든 안전하다. | 없음. |
| `proven_http_server_destroy(server)` | 모든 연결과 리스너를 닫고 서버를 해제한다. 핸들러 안에서는 부르지 않는다. | 없음. |
| `proven_http_server_connection_count(server)` | 지금 열려 있는 연결 수. 루프의 스레드에서 쓴다. | `proven_size_t`. |

```text
typedef struct {
    proven_allocator_t alloc;          /* required */
    proven_http_handler_fn handler;    /* required: void (*)(void *ctx, proven_http_exchange_t *) */
    void *handler_ctx;
    proven_job_sys_t *jobs;            /* NULL: handlers run on the loop's thread (section 5) */
    proven_size_t max_connections;     /* 0: 64 */
    proven_size_t max_head_bytes;      /* 0: 16 KiB - request head, and response head */
    proven_size_t max_headers;         /* 0: 64 fields */
    proven_u64 max_body_bytes;         /* 0: 1 MiB */
    proven_u32 head_timeout_ms;        /* 0: 10 s */
    proven_u32 body_timeout_ms;        /* 0: 30 s */
    proven_u32 write_timeout_ms;       /* 0: 30 s */
    proven_u32 idle_timeout_ms;        /* 0: 60 s */
} proven_http_server_config_t;
```

구조체를 0으로 채우고 필요한 것만 정한다. 모든 한도에는 기본값이 있고, 어느 것도 끌 수 없다.
"무제한"은 없다.

### 각 한도가 막는 것

| 한도 | 막는 것 |
|---|---|
| `head_timeout_ms` | 연결을 열고 요청을 1분에 한 바이트씩 보내는 클라이언트. 첫 바이트부터 헤드의 끝까지 이만큼의 시간이 주어지고, 넘기면 `408`을 받고 닫힌다. 아무것도 보내지 않는 새 연결에도 같은 시간이 주어진다. |
| `body_timeout_ms` | 같은 수법을 본문으로 쓰는 것. 본문 읽기 한 번이 이만큼 기다릴 수 있다. |
| `write_timeout_ms` | 큰 응답을 요청해 놓고 읽지 않는 클라이언트. 쓰기 한 번이 이만큼 기다릴 수 있다. |
| `idle_timeout_ms` | 열어 둔 채 쓰지 않는 연결. HTTP가 허락하는 대로 말없이 닫는다. |
| `max_head_bytes`, `max_headers` | 끝나지 않는 헤드, 필드가 만 개인 헤드: `431`. |
| `max_body_bytes` | 받을 준비가 된 것보다 큰 본문: `413`. 길이를 미리 알렸으면 읽기 전에, 청크였으면 읽는 도중에. |
| `max_connections` | 메모리가 감당할 수 있는 것보다 많은 연결. 다음 클라이언트는 listen 대기열에서 기다린다. 거절되지 않는다. |

연결 하나의 메모리는 받아들일 때 정해진다. `2 * max_head_bytes`에 4 KiB와 헤더 배열을 더한 것으로,
기본값이면 약 38 KiB이고 연결 64개면 약 2.4 MiB다. 클라이언트가 무엇을 보내든 연결 하나의 비용은
늘지 않는다.

### 주의사항, 그리고 무엇이 잘못되는가

**`proven_net_addr_any`는 모든 인터페이스다.** 거기서 listen하는 서버는 그 기계가 닿는 곳이면 어디서든
평문으로 접근할 수 있다. 같은 기계를 위한 서비스라면 `proven_net_addr_loopback`에서 listen하라.

**IPv6 리스너는 IPv4를 받지 않는다.** 9장이 말하듯 여기서 IPv6 소켓은 모든 플랫폼에서 IPv6 전용이다.
둘 다 받으려면 `proven_http_server_listen`을 두 번 불러라.

**`destroy`는 `stop`이 아니다.** `stop`은 `run`이 돌아오게 하고, 서버는 멈춘 채로 남는다. `run`을 다시
부르면 곧장 돌아온다. `destroy`는 서버를 해제한다. 핸들러 안에서는 `stop`을 부르고 `destroy`는 절대
부르지 마라.

준비 상태 확인은 `proven_net_poll`이다. 연결 수백 개는 잘 처리하고 수만 개는 잘 처리하지 못한다.
열린 인터넷을 큰 규모로 상대하는 서버에는 `epoll`이나 `kqueue`가 필요한데, 이 라이브러리에는 없다.

## 3. 핸들러 안에서

핸들러는 헤드를 읽고 파싱하고 검사한 요청과 함께 불린다. 본문은 읽지 않은 상태다. 읽을지는 핸들러가
정한다.

### 참조

| API | 의도 | 반환 |
|---|---|---|
| `proven_http_exchange_request(x)` | 요청: 메서드, 타깃, 헤더(10장의 `proven_http_request_t`). | 포인터. 뷰(view)는 핸들러가 돌아갈 때까지 유효하다. |
| `proven_http_exchange_peer(x)` | 클라이언트의 주소. | `proven_net_addr_t`. |
| `proven_http_exchange_read(x, dest)` | 요청 본문을 더 읽는다. | `proven_result_size_t`: 본문이 끝나면 `.err`가 `EOF`. `TIMEOUT`. `max_body_bytes`를 넘으면 `OUT_OF_BOUNDS`. 청크 틀이 잘못되면 `INVALID_FORMAT`. 클라이언트가 떠났으면 `RESET`. |
| `proven_http_exchange_respond(x, status, headers, count, body)` | 메모리에 있는 본문과 함께 응답 전체를 보낸다. | `proven_err_t`: 응답을 이미 시작했으면 `INVALID_STATE`. 서버가 맡은 헤더나 쓰기 함수가 거절하는 값이면 `INVALID_ARG`. 헤드가 들어가지 않으면 `OUT_OF_BOUNDS`. `TIMEOUT`, `RESET`. |
| `proven_http_exchange_begin(x, status, headers, count, content_length)` | 본문이 조각조각 뒤따르는 응답을 시작한다. 길이를 모르면 `PROVEN_HTTP_LENGTH_UNKNOWN`: 본문은 청크로 나간다. | `respond`와 같다. |
| `proven_http_exchange_write(x, data)` | 그 본문을 더 쓴다. | `proven_err_t`: 알린 길이를 넘으면 `OUT_OF_BOUNDS`. `INVALID_STATE`. `TIMEOUT`, `RESET`. |
| `proven_http_exchange_end(x)` | 끝낸다. 선택 사항이다: 핸들러에서 돌아가도 같은 일이 일어난다. | `proven_err_t`. |

### 주의사항, 그리고 무엇이 잘못되는가

**타깃은 클라이언트가 쓴 텍스트다.** `request->target`은 보낸 그대로다 - `/a/b?x=1`이거나 절대 URL이고,
디코딩되지 않았다. 비교하기 전에 `proven_url_split_target`으로 나누고, `proven_url_path_resolve`
(10장 §3) 없이 그것으로 파일 이름을 만들지 마라.

잘못된 예:

```text
snprintf(file, sizeof file, "/var/www%.*s", (int)req->target.size, req->target.ptr);   /* wrong: "/../../etc/passwd" */
```

올바른 예 — 경로는 `proven_url_split_target`으로 얻고, 거기에 `proven_url_path_resolve`를 적용하고,
`PROVEN_OK` 결과만 루트에 잇는다.

**`Content-Length`, `Transfer-Encoding`, `Connection`, `Date`를 직접 쓰지 마라.** 서버가 여러분이
실제로 보내는 것에 맞춰 쓴다. 하나라도 주면 `PROVEN_ERR_INVALID_ARG`이고 아무것도 보내지지 않는다.
서로 어긋나는 `Content-Length` 필드 두 개가 바로 요청 스머글링의 재료가 되는 모호함이고, 여기서는
그것을 만들어 낼 수 없다.

**요청에서 가져온 헤더 값이 응답을 쪼갤 수 없다.** 줄바꿈이 든 값은 쓰기 함수가 거절하므로,
`Location: ` 뒤에 클라이언트가 고른 텍스트를 붙여도 헤더나 두 번째 응답이 끼어들 수 없다. 그렇게
거절된 뒤에는 아무것도 보내지지 않았으니 다른 응답 - 오류 페이지 - 을 대신 보낼 수 있다.

**쓰기의 결과를 확인하고, 실패하면 멈춰라.** `PROVEN_ERR_RESET`과 `PROVEN_ERR_TIMEOUT`은 클라이언트가
사라졌거나 읽지 않는다는 뜻이다. 아무도 받지 않는 큰 본문을 계속 만들어 내는 핸들러는 연결 하나를 -
스레드 하나짜리 모델에서는 서버 전체를 - 아무 보람 없이 붙잡는다.

**아무것도 보내지 않고 돌아가면 클라이언트는 `500`을 받는다.** 이것은 모든 요청이 답을 받는다는 서버의
약속을 지키는 것이지, 오류를 보내는 방법이 아니다. 본문 읽기가 실패했다면 상태 코드는 그 실패에 맞는
것 - `408`, `413`, `400` - 이 된다.

**exchange에서 나온 모든 것은 핸들러가 돌아갈 때 끝난다** - 요청, 그 뷰, exchange 자체. exchange를 다른
스레드에 넘기고 돌아가는 핸들러는 허공을 가리키는 포인터를 넘긴 것이다. 나중에 답하려면 돌아가지
마라. 핸들러 안에서 기다려라(핸들러를 job system에서 돌리면(§5) 작업 스레드 하나의 비용이 든다).

**쓸 바이트 수만큼만 길이를 알려라.** 더 쓰면 거절된다. 덜 쓰면 고칠 수 없다 - 클라이언트가 나머지를
기다리고 있다 - . 그래서 그 지점에서 연결을 닫고, 클라이언트는 잘린 응답을 본다. 그것이 사실이다.

## 4. 서버가 묻지 않고 하는 일

형식이 어긋나거나, 모호하거나, 너무 큰 요청은 핸들러에 닿지 않는다.

| 요청 | 답 |
|---|---|
| HTTP/1.x로 파싱되지 않는다: LF만 있는 줄 끝, 콜론 앞의 공백, 제어 바이트, 접힌 헤더 | `400` |
| `Host`가 정확히 하나가 아닌 HTTP/1.1 | `400` |
| `Content-Length`와 `Transfer-Encoding`이 함께 있다. 서로 다른 길이 둘. 숫자가 아닌 길이 | `400` |
| `max_head_bytes`보다 큰 헤드, `max_headers`보다 많은 필드 | `431` |
| 다른 HTTP 버전 | `505` |
| `chunked`가 아닌 전송 코딩 | `501` |
| `max_body_bytes`보다 크다고 알린 본문 | `413` |
| `head_timeout_ms` 안에 끝나지 않은 헤드 | `408` |
| job system이 요청을 더 받을 수 없다(§5) | `503` |

이들은 모두 이 절의 끝에 적은 방식으로 연결을 닫는다. 일부러 엄격하다. 두 서버가 요청 하나를 다르게 읽는 곳에서 공격자는 요청
안에 요청을 숨긴다. 치료법은 한 가지 읽기를 고르는 것이 아니라 모호한 것을 거절하는 것이다.

그리고 핸들러에 닿은 요청의 둘레에서 하는 일:

- **Keep-alive.** 연결은 다음 요청을 위해 열려 있다. 클라이언트가 닫아 달라고 했거나, 요청이
  `keep-alive` 없는 HTTP/1.0이었거나, 응답의 틀을 잡을 수 없었던 경우는 예외다.
- **파이프라이닝.** 잇달아 보낸 요청에는 순서대로 답한다.
- **읽지 않은 본문.** 핸들러가 요청 본문을 읽지 않았다면 서버가 읽어서 버린다 - 64 KiB까지 - . 그래야
  다음 요청이 제자리에서 시작한다. 그보다 크면 닫는다.
- **`Expect: 100-continue`.** `100 Continue`는 핸들러가 본문을 처음 읽을 때 보낸다. 헤더만 보고
  거절하는 핸들러는 본문을 청하지 않으며, 그때는 연결을 닫는다. 본문이 뒤따를지는 클라이언트만 알기
  때문이다.
- **`HEAD`.** 핸들러는 `GET`처럼 답한다. 서버는 헤드를 - `Content-Length`를 포함해서 - 보내고 본문은
  보내지 않는다.
- **`204`와 `304`**는 본문도 길이도 없이 보낸다.
- **HTTP/1.0.** 길이를 모르는 응답은 1.0 클라이언트에게 청크로 보낼 수 없다. 연결이 닫힐 때까지 보낸다.
- 모든 응답의 **`Date`**.
- **답을 망가뜨리지 않고 닫기.** 서버는 연결을 닫을 때 먼저 그렇다고 알리고 - 보내기를 멈춘다 - , 그다음 클라이언트도 닫을 때까지 최대 1초 동안 클라이언트가 보내는 것을 계속 읽어서 버린다. 읽지 않은 입력을 남긴 채 닫힌 소켓은 reset으로 답하고, reset은 클라이언트의 시스템이 아직 읽지 않은 응답을 버리게 만든다. 그러면 올리는 도중에 `413`을 받은 클라이언트가 그 대신 끊어진 연결을 보게 된다. 그 1초는 고정이며 설정 항목이 아니다.

## 5. 핸들러를 돌리는 두 가지 방법

설정의 `jobs`가 고른다.

**`jobs == NULL`: 루프의 스레드에서.** 스레드 하나가 모든 것을 한다. 핸들러가 도는 동안 - 또는 느린
클라이언트가 본문을 보내기를, 응답을 읽기를 기다리는 동안 - 다른 연결은 서비스받지 못한다. 계산하고
답하는 핸들러에는 괜찮다. 단순하고, 잠금이 필요한 것이 없다. 느린 무언가를 기다리는 핸들러에는
틀렸다.

**`jobs` 지정: job system의 작업 스레드에서.** 요청 하나하나가
[6장](manual-06-execution-and-platform-ko.md)의 job system에 넘겨지고, 루프는 곧장 연결을 받고 읽는
일로 돌아간다. 핸들러는 자기 작업 스레드에서 본문을 읽고 응답을 쓴다. 핸들러가 돌아갈 때까지 연결은
그 핸들러의 것이다. 핸들러들이 동시에 돌므로:

- 핸들러들이 공유하는 것에는, 공유 데이터에 언제나 필요한 보호가 필요하다.
- 설정의 `alloc`은 여러 스레드에서 불리므로 스레드 안전해야 한다 - 힙(heap) 할당자는 그렇고,
  아레나(arena)는 그렇지 않다.
- job 큐가 차면 그 요청에는 `503`으로 답하고 연결을 닫는다. 감당하려는 부하에 맞춰 큐 크기를 정하고,
  `503`은 서버가 이만하면 충분하다고 말하는 것으로 여겨라.

핸들러는 두 경우에 똑같이 쓴다. 어느 쪽에서도 해서는 안 되는 것은 영원히 막히는 것이다. 돌아가지 않는
핸들러는 연결 하나를 영원히 붙잡고, 그와 함께 작업 스레드 하나 - 또는 서버 - 를 붙잡는다.

**느린 클라이언트가 치르게 하는 값은 루프가 아니라 작업 스레드다. 그래도 작업 스레드 하나는 든다.**
`jobs`를 지정하면, 본문을 느리게 보내는 클라이언트는 그 핸들러를 돌리는 작업 스레드를
`body_timeout_ms`까지 차지한다. 그런 클라이언트가 충분히 많으면 모든 작업 스레드를 차지한다. 타임아웃이
그것을 한정할 뿐 공짜로 만들어 주지는 않는다. 그것이 문제라면 핸들러에서 요청 본문은 꼭 필요할 때에만
읽고, 타임아웃을 짧게 잡아라.

`proven_http_server_destroy`는 작업 스레드에서 돌고 있는 핸들러를 기다린다. job system은 서버를
destroy한 뒤에 닫아라. 앞이 아니다.

테스트 스위트가 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_11_http_server.c -->
```c
/*
 * 온전한 HTTP 서버: 요청에 답하는 함수 하나와, 나머지를 모두 맡는 루프.
 *
 * 핸들러가 보는 요청은 이미 읽고 검사하고 한도를 적용한 것이다. 핸들러는 원하면 본문을
 * 읽고, 응답 하나를 보낸다 - 통째로 또는 조각조각.
 */

typedef struct {
    proven_http_server_t *server;
    int served;
} app_t;

static void handle(void *ctx, proven_http_exchange_t *x) {
    app_t *app = ctx;
    const proven_http_request_t *req = proven_http_exchange_request(x);
    app->served++;

    /* 타깃은 경로이고 쿼리가 붙을 수도 있다. 비교하기 전에 나눈다. */
    proven_u8str_view_t path, query;
    bool has_query;
    if (proven_url_split_target(req->target, &path, &query, &has_query) != PROVEN_OK) {
        (void)proven_http_exchange_respond(x, 400, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("bad target\n")));
        return;
    }

    if (req->method == PROVEN_HTTP_GET && proven_u8str_view_eq(path, PROVEN_LIT("/"))) {
        /* 가장 단순한 답: 상태 코드, 여러분의 헤더, 메모리에 있는 본문. Content-Length와
         * Date와 Connection은 대신 써 준다. */
        proven_http_header_t type = { PROVEN_LIT("Content-Type"), PROVEN_LIT("text/plain; charset=utf-8") };
        (void)proven_http_exchange_respond(x, 200, &type, 1, proven_mem_view_from_u8(PROVEN_LIT("hello\n")));

    } else if (req->method == PROVEN_HTTP_POST && proven_u8str_view_eq(path, PROVEN_LIT("/shout"))) {
        /* 본문을 오는 대로 읽고, 길이를 미리 알지 못한 채 조각조각 답한다:
         * 응답은 청크로 나간다. 여기서는 어디에도 본문 전체를 쥐고 있지 않다. */
        if (proven_http_exchange_begin(x, 200, NULL, 0, PROVEN_HTTP_LENGTH_UNKNOWN) != PROVEN_OK) return;
        proven_byte_t piece[256];
        for (;;) {
            proven_result_size_t got = proven_http_exchange_read(x, (proven_mem_mut_t){ piece, sizeof piece });
            if (got.err != PROVEN_OK) break;          /* PROVEN_ERR_EOF: 본문이 끝났다 */
            for (proven_size_t i = 0; i < got.value; ++i) {
                if (piece[i] >= 'a' && piece[i] <= 'z') piece[i] = (proven_byte_t)(piece[i] - 32);
            }
            if (proven_http_exchange_write(x, (proven_mem_view_t){ piece, got.value }) != PROVEN_OK) return;   /* 클라이언트가 떠났다 */
        }
        (void)proven_http_exchange_end(x);

    } else if (proven_u8str_view_eq(path, PROVEN_LIT("/whoami"))) {
        proven_net_addr_t peer = proven_http_exchange_peer(x);
        proven_byte_t text[PROVEN_NET_ADDR_TEXT_MAX];
        proven_size_t n = 0;
        if (proven_net_addr_format(&peer, (proven_mem_mut_t){ text, sizeof text }, &n) != PROVEN_OK) return;   /* 아무것도 보내지 않고 돌아가면: 500 */
        (void)proven_http_exchange_respond(x, 200, NULL, 0, (proven_mem_view_t){ text, n });

    } else if (proven_u8str_view_eq(path, PROVEN_LIT("/quit"))) {
        (void)proven_http_exchange_respond(x, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("bye\n")));
        proven_http_server_stop(app->server);         /* 이 요청이 끝나면 run()이 돌아온다 */

    } else {
        (void)proven_http_exchange_respond(x, 404, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("no such page\n")));
    }
}

static void serve(void *arg) {
    app_t *app = arg;
    (void)proven_http_server_run(app->server);        /* stop될 때까지 */
}

/* `path`를 GET 또는 POST하고 본문 전체를 비교한다. */
static bool fetch_is(proven_http_client_t *client, proven_u16 port, const char *method, const char *path, const char *body, proven_u16 status, const char *expect) {
    char url[128];
    int n = snprintf(url, sizeof url, "http://127.0.0.1:%u%s", (unsigned)port, path);
    proven_http_client_request_t req = {
        .method = proven_u8str_view_from_cstr(method),
        .url = { (const proven_byte_t *)url, (proven_size_t)n },
        .body = proven_mem_view_from_u8(proven_u8str_view_from_cstr(body)),
    };
    proven_http_client_response_t resp;
    bool ok = proven_http_client_send(client, &req, &resp) == PROVEN_OK && resp.status == status;
    proven_u8str_t text = {0};
    ok = ok && proven_http_client_read_all(&resp, proven_heap_allocator(), &text, 4096) == PROVEN_OK;
    ok = ok && (expect == NULL || proven_u8str_view_eq(proven_u8str_as_view(&text), proven_u8str_view_from_cstr(expect)));
    proven_u8str_destroy(proven_heap_allocator(), &text);
    proven_http_client_finish(&resp);
    return ok;
}

int main(void) {
    static app_t app;
    proven_allocator_t heap = proven_heap_allocator();

    /* 설정을 0으로 채우고 필요한 것만 정한다. 0인 필드에는 모두 기본값이 있다. */
    proven_http_server_config_t config = {0};
    config.alloc = heap;
    config.handler = handle;
    config.handler_ctx = &app;
    config.max_body_bytes = 64 * 1024;        /* 이보다 큰 요청 본문에는 413으로 답한다 */
    config.head_timeout_ms = 5000;            /* 클라이언트가 요청 헤드를 보내는 데 허락된 시간 */
    proven_err_t err = proven_http_server_create(&config, &app.server);
    EXAMPLE_REQUIRE(err == PROVEN_OK, "a server");

    /* 포트 0은 시스템이 고르게 한다. 고른 값은 `at`에 담긴다. */
    proven_net_addr_t at;
    err = proven_http_server_listen(app.server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        proven_http_server_destroy(app.server);
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "it listens");

    /* 자기 루프가 있는 프로그램은 poll을 부른다: 마감 시각으로 묶인 한 바퀴. */
    EXAMPLE_REQUIRE(proven_http_server_poll(app.server, proven_net_deadline_in(10)) == PROVEN_ERR_TIMEOUT, "nobody has connected yet");

    /* 대부분의 프로그램은 run을 부른다. 여기서는 다른 스레드에서 돌려서, 이 스레드가 클라이언트 노릇을 한다. */
    proven_job_sys_t *threads = NULL;
    proven_job_group_t running;
    proven_job_group_init(&running);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &threads) == PROVEN_OK &&
                    proven_job_group_submit(threads, &running, serve, &app) == PROVEN_OK, "the server loop is started");

    proven_http_client_config_t client_config = {0};
    client_config.alloc = heap;
    client_config.max_idle_connections = 2;
    proven_http_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&client_config, &client) == PROVEN_OK, "a client");

    EXAMPLE_REQUIRE(fetch_is(client, at.port, "GET", "/", "", 200, "hello\n"), "GET /");
    EXAMPLE_REQUIRE(fetch_is(client, at.port, "POST", "/shout", "a body, read and answered in pieces", 200, "A BODY, READ AND ANSWERED IN PIECES"), "POST /shout");
    EXAMPLE_REQUIRE(fetch_is(client, at.port, "GET", "/whoami?verbose=1", "", 200, NULL), "GET /whoami");
    EXAMPLE_REQUIRE(fetch_is(client, at.port, "GET", "/nothing-here", "", 404, "no such page\n"), "a 404 is the handler's to send");
    EXAMPLE_REQUIRE(fetch_is(client, at.port, "GET", "/quit", "", 200, "bye\n"), "GET /quit");

    /* 핸들러가 stop을 불렀으므로 run이 돌아온다. 그러면 서버를 해체할 수 있다. */
    proven_job_group_wait(threads, &running);
    EXAMPLE_REQUIRE(app.served == 5, "five requests reached the handler");
    EXAMPLE_REQUIRE(proven_http_server_connection_count(app.server) <= 1, "all on one connection, which is still open or just closed");

    proven_http_client_destroy(client);
    proven_http_server_destroy(app.server);   /* 남은 것을 닫는다 */
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    return EXAMPLE_OK();
}
```

## 6. 클라이언트

```text
proven_http_client_config_t config = {0};
config.alloc = proven_heap_allocator();
proven_http_client_create(&config, &client);

proven_http_client_response_t resp;
if (proven_http_client_get(client, PROVEN_LIT("http://example.com/"), &resp) == PROVEN_OK) {
    /* resp.status, resp.headers; then proven_http_client_read until PROVEN_ERR_EOF */
}
proven_http_client_finish(&resp);
proven_http_client_destroy(client);
```

### 참조

| API | 의도 | 반환 |
|---|---|---|
| `proven_http_client_create(&config, &client)` | 클라이언트를 만든다. 설정의 문자열은 복사된다. | `proven_err_t`: 할당자가 유효하지 않거나, 프록시가 `http`나 `socks5`가 아니거나, 자격 증명에 제어 문자가 있으면 `INVALID_ARG`. `NOMEM`. |
| `proven_http_client_destroy(client)` | 한가한 연결을 닫고 해제한다. 모든 응답을 먼저 finish하라. | 없음. |
| `proven_http_client_send(client, &request, &response)` | 요청을 보내고 응답의 헤드를 읽는다. | `proven_err_t`. §7을 보라. |
| `proven_http_client_get(client, url, &response)` | URL만으로 하는 `send`. | 같다. |
| `proven_http_client_read(&response, dest)` | 본문을 더 읽는다. | `proven_result_size_t`: 끝에서 `.err`가 `EOF`. 본문이 중간에 끊겼으면 `RESET`. `TIMEOUT`. `INVALID_FORMAT`. |
| `proven_http_client_read_all(&response, alloc, &out, max_bytes)` | 본문의 나머지를 읽어 문자열에 덧붙인다. | `proven_err_t`: 본문이 `max_bytes`보다 길면 `OUT_OF_BOUNDS` - 그때 `out`에는 그만큼이 들어 있다. |
| `proven_http_client_finish(&response)` | 응답을 놓아준다. 연결은 남겨 두거나 닫는다. send가 실패한 뒤에도 안전하다. | 없음. |

```text
typedef struct {
    proven_allocator_t alloc;               /* required */
    proven_u32 connect_timeout_ms;          /* 0: 30 s */
    proven_u32 io_timeout_ms;               /* 0: 30 s, for each read and each write */
    proven_size_t max_head_bytes;           /* 0: 16 KiB */
    proven_u32 max_redirects;               /* 0: none are followed */
    proven_size_t max_idle_connections;     /* 0: every connection is closed after use */
    proven_u8str_view_t proxy;              /* "http://host:port" or "socks5://host:port" */
    proven_u8str_view_t username, password; /* offered when a server answers 401 */
    proven_u8str_view_t user_agent;         /* empty: none is sent */
    proven_http_cookie_jar_t *cookies;      /* NULL: cookies are ignored */
    proven_http_tls_wrap_fn tls_wrap;       /* NULL: an https URL is PROVEN_ERR_UNSUPPORTED */
    void *tls_ctx;
} proven_http_client_config_t;
```

**0으로 채운 설정은 가장 적게 한다.** `alloc`만 정하면 클라이언트는 리다이렉트를 따르지 않고, 연결을
남겨 두지 않고, 자격 증명을 내놓지 않고, 쿠키를 저장하지 않고, `User-Agent`를 보내지 않는다. 그
하나하나는 여러분이 알고서 켜는 것이다.

**클라이언트 하나에 스레드 하나.** 클라이언트는 여러 스레드가 동시에 쓰기에 안전하지 않다. 스레드마다
하나씩 주어라. 값싸다.

## 7. 요청 하나, 응답 하나

```text
typedef struct {
    proven_u8str_view_t method;             /* empty: GET */
    proven_u8str_view_t url;                /* absolute: http://host[:port]/path?query */
    const proven_http_header_t *headers;    /* extra header fields */
    proven_size_t header_count;
    proven_mem_view_t body;                 /* a body in memory: sent with Content-Length */
    proven_reader_t body_stream;            /* or a stream: sent chunked */
} proven_http_client_request_t;

typedef struct {
    proven_u16 status;
    proven_u8str_view_t reason;
    const proven_http_header_t *headers;
    proven_size_t header_count;
    proven_u8str_view_t url;                /* where this response came from, after redirects */
    proven_u32 redirects;
    void *internal;
} proven_http_client_response_t;
```

`proven_http_client_send`는 상태 코드와 헤더가 도착하면 돌아온다. 본문은 아직 연결 위에 있고,
여러분이 자기 버퍼에 조각조각 읽는다. 그래서 응답이 얼마나 크든 드는 메모리는 여러분이 고른 만큼이고,
내려받은 것을 곧장 파일로 보낼 수 있다. 스트림에서 오는 요청 본문은 `proven_reader_t`, 곧 5장의
읽기 스트림(reader)이다.

**오류 상태 코드는 오류가 아니다.** `404`나 `500`은 응답이다. `send`는 `PROVEN_OK`를 돌려주고 여러분이
`status`를 본다. `send`가 실패하는 것은 들여다볼 응답이 *없을* 때다.

| `send`의 반환 | 이유 |
|---|---|
| `PROVEN_ERR_INVALID_FORMAT` | URL이 절대 URL이 아니거나, 응답이 HTTP가 아니었다 - 또는 최종 응답 없이 중간(`1xx`) 응답만 여덟 개를 넘었다. |
| `PROVEN_ERR_UNSUPPORTED` | `http`가 아닌 스킴 - 또는 `tls_wrap` 없는 `https`. |
| `PROVEN_ERR_INVALID_ARG` | 클라이언트가 직접 쓰는 헤더를 주었다: `Host`, `Content-Length`, `Transfer-Encoding`, `Connection`. 또는 본문과 스트림을 함께 주었다. |
| `PROVEN_ERR_NOT_FOUND` | 호스트 이름이 해석되지 않는다. |
| `PROVEN_ERR_REFUSED`, `PROVEN_ERR_UNREACHABLE`, `PROVEN_ERR_TIMEOUT`, `PROVEN_ERR_RESET` | 연결: 9장에서의 뜻 그대로. |
| `PROVEN_ERR_OUT_OF_BOUNDS` | 요청이나 응답 헤드가 `max_head_bytes`보다 크다. |
| `PROVEN_ERR_PERMISSION` | 프록시가 거절했다. |
| `PROVEN_ERR_UNTRUSTED` | `tls_wrap`이 서버를 검증하지 못했다. |

### 주의사항, 그리고 무엇이 잘못되는가

**언제나 `finish`를 불러라.** 응답은 finish할 때까지 연결과 메모리를 붙잡는다. `finish`는 `send`가
실패한 응답에도 안전하므로, 단순한 모양이 옳은 모양이다: `send`하고, 성공했으면 쓰고, 어느 쪽이든
`finish`한다.

**`PROVEN_ERR_EOF`는 본문의 끝이다. `PROVEN_ERR_RESET`은 아니다.** 알린 길이보다 먼저 멈춘 본문 -
서버가 죽었거나 네트워크가 끊겼다 - 은 `PROVEN_ERR_RESET`으로 보고되고 결코 `EOF`로 보고되지 않는다.
`EOF`만을 온전한 본문으로 다뤄라.

잘못된 예:

```text
while (proven_http_client_read(&resp, buf).err == PROVEN_OK) save(buf);   /* wrong: stops on RESET and calls the file done */
commit_download();
```

올바른 예 — 결과를 간직하고, 그것이 `PROVEN_ERR_EOF`일 때에만 확정한다.

**`read_all`에는 한도가 필요하고, 그것은 여러분이 고른다.** 얼마나 보낼지는 서버가 정한다. `max_bytes`는
여러분이 쥘 최대치다. 그보다 긴 본문은 `PROVEN_ERR_OUT_OF_BOUNDS`이지, 기계에 메모리가 남지 않을
때까지 자라는 할당이 아니다.

**응답의 뷰는 응답과 함께 죽는다.** `headers`, `reason`, `url`은 `finish`가 해제하는 메모리를 가리킨다.
간직할 것은 복사하라.

**스트림 본문은 한 번만 보낸다.** `body_stream`에서 읽은 본문은 다시 읽을 수 없으므로, 그런 요청은
되풀이되지 않는다 - 본문이 필요한 리다이렉트에도, 인증 요구에도, 남겨 둔 연결이 죽은 것으로 드러났을
때에도. 되풀이를 일으켰을 그 응답을 여러분이 받고, 여러분이 정한다.

**`io_timeout_ms`는 기다림 하나하나를 한정하지, 요청 전체를 한정하지 않는다.** 10초마다 한 바이트를
보내는 서버는 30초 타임아웃에 걸리지 않는다. 교환 전체에 한도를 두려면 `send` 전에 시각을 적어 두고,
충분히 기다렸으면 읽기를 멈춰라.

이름 해석에는 9장에서처럼 기한이 없다. `connect_timeout_ms`는 이름이 해석된 뒤부터 센다.

## 8. 리다이렉트, 인증 요구, 쿠키

**리다이렉트**(`301`, `302`, `303`, `307`, `308`)는 `max_redirects`까지 따르고, 각 `Location`은 그것을
보낸 URL에 대해 해소한다. `response.url`과 `response.redirects`가 어디에 어떻게 도착했는지 말해 준다.

| 상태 코드 | 다음 요청 |
|---|---|
| `303` | 본문 없는 `GET` - 메서드가 무엇이었든 |
| `301`, `302` | 메서드가 `POST`였으면 본문 없는 `GET`. 아니면 같은 요청을 다시 |
| `307`, `308` | 본문을 포함해 같은 요청을 다시 |

다음 경우에는 리다이렉트를 따르지 *않고* 그 `3xx` 응답을 여러분에게 넘긴다.

- 한도에 닿았다.
- `https`에서 `http`로 이어진다 - 클라이언트는 서버가 시켰다고 암호화된 연결에서 내려오지 않는다.
- `http`와 `https`가 아닌 스킴으로 이어진다.
- 요청을 다시 보내야 하는데 본문이 스트림이었다.

**자격 증명은 그것이 주어진 호스트에 남는다.** 리다이렉트가 다른 호스트나 포트나 스킴으로 이어지면,
여러분이 준 `Authorization`, `Cookie`, `Proxy-Authorization` 헤더는 두고 가고, 새 곳에서 온 `401`에는
설정된 사용자 이름과 비밀번호로 답하지 않는다. 토큰을 맡길 만큼 믿는 서버라도, 그 토큰을 자기가 고른
서버로 보내게 할 수는 없어야 한다.

**클라이언트가 확인하지 않는 것은 리다이렉트가 어디로 이어지는가다.** 서버는 `http://127.0.0.1/`이나
여러분의 네트워크 안쪽 주소로 리다이렉트할 수 있다. URL이 믿지 못할 사람에게서 온다면 - "이 링크를
가져와" 기능 - 모든 홉의 목적지를 확인하는 것은 여러분 몫이다. `max_redirects`를 0으로 두고, `Location`을
직접 읽고, 정하라.

**인증 요구.** `username`을 정해 두면 Digest나 Basic 요구가 담긴 `401`에 한 번 답한다. Digest가 있으면
Digest로, 아니면 Basic으로. `stale`이라고 보고된 Digest nonce에는 한 번 더 답한다. 그래서 틀린
비밀번호의 값은 요청 두 번이지 무한 반복이 아니다. 자격 증명은 서버가 요구하기 전에는 결코 보내지
않는다 - 그리고 평문 HTTP에서 Basic으로 요구받으면 읽을 수 있는 채로 보낸다. §1이 그대로 적용된다.

**쿠키.** `cookies`에 보관함을 주면 모든 응답의 `Set-Cookie` 헤더가 저장되고 모든 요청이 일치하는
쿠키를 싣는다. 요청에 여러분이 직접 넣은 `Cookie` 헤더는 그 요청에서 보관함의 것을 대신한다. 보관함은
10장 §12의 호스트 전용 보관함이고, 거기 적힌 것이 모두 따라온다.

## 9. 연결과 프록시

**재사용.** `max_idle_connections`가 0보다 크면, 응답을 끝까지 읽은 연결은 클라이언트에게 돌아가 같은
스킴·호스트·포트로 가는 다음 요청을 실어 나른다. 클라이언트가 한가한 연결을 모아 두는 이곳을 흔히
연결 풀(pool)이라 부른다. 본문을 읽기 *전에* finish한 응답의 연결은 재사용할 수 없고 - 본문의 나머지가
아직 그 위에 있다 - 닫힌다. 연결을 남기려면 `finish` 전에 `PROVEN_ERR_EOF`까지 읽어라.

**남겨 둔 연결은 죽어 있을 수 있다.** 서버는 한가한 연결을 예고 없이 닫는다. 재사용한 연결에서 답의
바이트가 하나도 오기 전에 요청이 실패하면 클라이언트는 새 연결로 한 번 다시 보낸다. 아무것도 돌아오지
않았기 때문에 안전한 것이다 - 그리고 두 번 보낼 수 없는 스트림 본문에는 하지 않는다.

**프록시.** `proxy`는 `http://[user:password@]host:port` 또는 `socks5://[user:password@]host:port`다.

| 프록시 | `http` URL일 때 | `https` URL일 때 |
|---|---|---|
| HTTP | 프록시에 URL 전체를 보내고, 프록시가 가져온다. | 프록시에 `host:port`로의 `CONNECT`를 요청한다. 요청은 그 터널을 지나간다. |
| SOCKS5 | `host:port`로의 터널, 그다음 평범한 요청. | 같다. 그 안에 TLS가 들어간다. |

- 대상의 이름은 여기서 해석하지 않고 프록시에 넘긴다 - SOCKS5에는 이름으로 넘기며, 다른 곳에서
  `socks5h`라 부르는 것이 이것이다. 클라이언트를 돌리는 기계는 그 이름을 조회하지 않는다.
- 프록시 URL의 자격 증명은 프록시에게 - HTTP 프록시에는 `Proxy-Authorization: Basic`으로, SOCKS5에는
  RFC 1929의 사용자/비밀번호 방식으로 - 가고 다른 누구에게도 가지 않는다. 터널 안으로 들어가 원 서버까지
  가지 않는다. 두 형태 모두 프록시까지는 읽을 수 있는 채로 네트워크를 건넌다.
- 거절하는 프록시(`407`, `403`, SOCKS5의 "허용되지 않음")는 `PROVEN_ERR_PERMISSION`이다.
- HTTP 프록시는 평문 `http` 교환의 모든 것을 보고, 바꿀 수 있다. 프록시란 그런 것이다.

프록시 설정을 환경에서 읽지 않는다. `HTTP_PROXY`와 그 친척들은 뜻밖의 일을 많이 일으켜 온 관례다.
여러분의 프로그램이 그것을 따른다면, 프로그램이 읽어서 `proxy`를 직접 정한다.

## 10. TLS가 붙을 자리

```text
typedef proven_err_t (*proven_http_tls_wrap_fn)(void *ctx, proven_transport_t plain,
                                                proven_u8str_view_t host,
                                                proven_net_deadline_t until,
                                                proven_transport_t *out);
```

`https` URL이면 클라이언트는 연결하고 - 프록시 터널이 있으면 그것을 거쳐서 - , 연결된 전송과 호스트
이름으로 `tls_wrap`을 부른다. 그 함수가 핸드셰이크를 하고 암호화하는 전송을 돌려준다. 클라이언트는
그 위에서 HTTP를 말하고 차이를 알지 못한다. 9장의 전송 인터페이스가 그것이 만들어진 목적대로 일하는
것이다.

라이브러리는 아직 그런 함수를 제공하지 않는다. 이 이음매가 여기 있는 까닭은, 그 둘레의 클라이언트
동작 - 리다이렉트에서 내려가지 않기, 자격 증명을 원래 자리에 두기, 핸드셰이크보다 터널이 먼저 - 이
이미 정해지고 시험되어 있도록 하고, TLS 구현을 가진 프로그램이 오늘 그것을 붙일 수 있도록 하기
위해서다.

**직접 쓴다면 반드시 서버를 검증해야 한다.** `host`를 넘기는 것은 인증서를 그것에 견주어 확인하라는
뜻이다. 검증 없이 암호화만 하는 wrap은, 그 연결 한가운데 있는 누군가만 빼고 모두에게서 비밀인 연결을
준다 - 그리고 `PROVEN_OK`를 돌려주므로 그 뒤의 어느 것도 알아챌 수 없다. 상대를 검증할 수 없으면
`PROVEN_ERR_UNTRUSTED`를 돌려줘라. 클라이언트는 그것을 위로 넘기고 아무것도 보내지 않는다.

테스트 스위트가 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_11_http_client.c -->
```c
/*
 * HTTP 클라이언트: URL에서 바이트까지. 그 사이에 놓인 것 - 리다이렉트, 로그인, 쿠키 - 은
 * 클라이언트가 처리하고 응답에 드러난다.
 *
 * 상대 서버는 같은 프로그램 안, 다른 스레드에 있어서 이 예제에는 네트워크가 필요 없다.
 * 파일의 앞 절반이 그 서버의 핸들러이고, 클라이언트는 main에 있다.
 */

static proven_http_server_t *g_server;

static void site(void *ctx, proven_http_exchange_t *x) {
    (void)ctx;
    const proven_http_request_t *req = proven_http_exchange_request(x);
    proven_u8str_view_t value;

    if (proven_u8str_view_eq(req->target, PROVEN_LIT("/old-home"))) {
        proven_http_header_t h = { PROVEN_LIT("Location"), PROVEN_LIT("/home") };
        (void)proven_http_exchange_respond(x, 301, &h, 1, (proven_mem_view_t){0});
    } else if (proven_u8str_view_eq(req->target, PROVEN_LIT("/home"))) {
        proven_http_header_t h = { PROVEN_LIT("Set-Cookie"), PROVEN_LIT("visited=yes; Path=/") };
        (void)proven_http_exchange_respond(x, 200, &h, 1, proven_mem_view_from_u8(PROVEN_LIT("welcome home")));
    } else if (proven_u8str_view_eq(req->target, PROVEN_LIT("/cookie"))) {
        bool has = proven_http_header_find(req->headers, req->header_count, PROVEN_LIT("Cookie"), &value);
        (void)proven_http_exchange_respond(x, 200, NULL, 0, proven_mem_view_from_u8(has ? value : PROVEN_LIT("none")));
    } else if (proven_u8str_view_eq(req->target, PROVEN_LIT("/private"))) {
        /* "Basic" + base64("ada:lovelace") */
        if (proven_http_header_find(req->headers, req->header_count, PROVEN_LIT("Authorization"), &value) &&
            proven_u8str_view_eq(value, PROVEN_LIT("Basic YWRhOmxvdmVsYWNl"))) {
            (void)proven_http_exchange_respond(x, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("the private page")));
        } else {
            proven_http_header_t h = { PROVEN_LIT("WWW-Authenticate"), PROVEN_LIT("Basic realm=\"example\"") };
            (void)proven_http_exchange_respond(x, 401, &h, 1, proven_mem_view_from_u8(PROVEN_LIT("who are you?")));
        }
    } else if (proven_u8str_view_eq(req->target, PROVEN_LIT("/count"))) {
        /* 요청 본문이 몇 바이트였는지로 답한다. */
        proven_byte_t piece[512];
        proven_u64 total = 0;
        for (;;) {
            proven_result_size_t got = proven_http_exchange_read(x, (proven_mem_mut_t){ piece, sizeof piece });
            if (got.err != PROVEN_OK) break;
            total += got.value;
        }
        char text[32];
        int n = snprintf(text, sizeof text, "%llu bytes", (unsigned long long)total);
        (void)proven_http_exchange_respond(x, 200, NULL, 0, (proven_mem_view_t){ (const proven_byte_t *)text, (proven_size_t)n });
    } else {
        (void)proven_http_exchange_respond(x, 404, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("not found")));
    }
}

static void serve(void *arg) {
    (void)arg;
    (void)proven_http_server_run(g_server);
}

static char g_url[128];
static proven_u8str_view_t url_for(proven_u16 port, const char *path) {
    int n = snprintf(g_url, sizeof g_url, "http://127.0.0.1:%u%s", (unsigned)port, path);
    return (proven_u8str_view_t){ (const proven_byte_t *)g_url, (proven_size_t)n };
}

static bool view_is(proven_u8str_view_t v, const char *text) {
    return proven_u8str_view_eq(v, proven_u8str_view_from_cstr(text));
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    // ---- 말을 걸 상대가 되어 줄 서버 ----------------------------------------
    proven_http_server_config_t server_config = {0};
    server_config.alloc = heap;
    server_config.handler = site;
    EXAMPLE_REQUIRE(proven_http_server_create(&server_config, &g_server) == PROVEN_OK, "a server");
    proven_net_addr_t at;
    proven_err_t err = proven_http_server_listen(g_server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        proven_http_server_destroy(g_server);
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "it listens");
    proven_job_sys_t *threads = NULL;
    proven_job_group_t running;
    proven_job_group_init(&running);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &threads) == PROVEN_OK &&
                    proven_job_group_submit(threads, &running, serve, NULL) == PROVEN_OK, "its loop runs on another thread");

    // ---- 클라이언트 ----------------------------------------------------------
    proven_http_cookie_jar_t jar;
    EXAMPLE_REQUIRE(proven_http_cookie_jar_init(&jar, heap, 32) == PROVEN_OK, "a cookie jar");

    /* 0으로 채우고 필요한 것만 정한다. 할당자 말고 모든 필드가 0이면 클라이언트는
     * 리다이렉트를 따르지 않고, 연결을 남겨 두지 않고, 자격 증명을 보내지 않고, 쿠키를 갖지 않는다. */
    proven_http_client_config_t config = {0};
    config.alloc = heap;
    config.max_redirects = 5;
    config.max_idle_connections = 4;          /* 다음 요청을 위해 연결을 남겨 둔다 */
    config.io_timeout_ms = 5000;              /* 어떤 읽기도 쓰기도 이보다 오래 기다리지 않는다 */
    config.user_agent = PROVEN_LIT("manual-example/1.0");
    config.username = PROVEN_LIT("ada");      /* 서버가 요구할 때에만 내놓는다 */
    config.password = PROVEN_LIT("lovelace");
    config.cookies = &jar;
    proven_http_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&config, &client) == PROVEN_OK, "a client");

    /* GET. 상태 코드와 헤더가 도착하면 호출이 돌아온다. 본문은 아직 회선 위에 있고,
     * 여러분의 속도로 여러분의 버퍼에 읽는다. */
    proven_http_client_response_t resp;
    err = proven_http_client_get(client, url_for(at.port, "/old-home"), &resp);
    EXAMPLE_REQUIRE(err == PROVEN_OK && resp.status == 200, "the 301 was followed to a 200");
    EXAMPLE_REQUIRE(resp.redirects == 1 && resp.url.size > 5 && view_is((proven_u8str_view_t){ resp.url.ptr + resp.url.size - 5, 5 }, "/home"),
                    "and the response says where it ended up");
    proven_byte_t buf[64];
    proven_size_t have = 0;
    for (;;) {
        proven_result_size_t got = proven_http_client_read(&resp, (proven_mem_mut_t){ buf + have, sizeof buf - have });
        if (got.err == PROVEN_ERR_EOF) break;             /* 본문을 끝까지 읽었다 */
        EXAMPLE_REQUIRE(got.err == PROVEN_OK, "a piece of the body");
        if (got.err != PROVEN_OK) break;                  /* PROVEN_ERR_RESET: 중간에 끊겼다 */
        have += got.value;
    }
    EXAMPLE_REQUIRE(view_is((proven_u8str_view_t){ buf, have }, "welcome home"), "the body");
    /* 반드시 finish한다. 끝까지 읽었다면 연결은 다시 쓰이도록 클라이언트에게 돌아간다. */
    proven_http_client_finish(&resp);

    /* 그 응답이 쿠키를 심었다. 보관함이 그것을 갖고 있고 다음 요청이 실어 보낸다. */
    EXAMPLE_REQUIRE(proven_http_cookie_jar_count(&jar) == 1, "one cookie in the jar");
    proven_u8str_t text = {0};
    err = proven_http_client_get(client, url_for(at.port, "/cookie"), &resp);
    EXAMPLE_REQUIRE(err == PROVEN_OK && proven_http_client_read_all(&resp, heap, &text, 1024) == PROVEN_OK, "read_all: the whole body, up to a limit you name");
    EXAMPLE_REQUIRE(view_is(proven_u8str_as_view(&text), "visited=yes"), "the server saw the cookie");
    proven_http_client_finish(&resp);

    /* 로그인 뒤에 있는 페이지. 첫 답은 인증 요구가 담긴 401이다. 클라이언트가 설정된
     * 자격 증명으로 그에 답하고, 여러분에게는 결과만 보인다. */
    EXAMPLE_REQUIRE(proven_u8str_reset(&text) == PROVEN_OK, "reuse the string");
    err = proven_http_client_get(client, url_for(at.port, "/private"), &resp);
    EXAMPLE_REQUIRE(err == PROVEN_OK && resp.status == 200 && proven_http_client_read_all(&resp, heap, &text, 1024) == PROVEN_OK, "let in");
    EXAMPLE_REQUIRE(view_is(proven_u8str_as_view(&text), "the private page"), "the page");
    proven_http_client_finish(&resp);

    /* 모든 것을 정한 요청: 메서드, 추가 헤더, 그리고 스트림에서 오는 본문. 스트림 본문은
     * 청크로 나간다 - 그래서 크기를 모르는 파일이나 파이프도 올릴 수 있다. */
    proven_reader_view_t source;
    proven_http_header_t headers[1] = { { PROVEN_LIT("Content-Type"), PROVEN_LIT("text/plain") } };
    proven_http_client_request_t req = {
        .method = PROVEN_LIT("POST"),
        .url = url_for(at.port, "/count"),
        .headers = headers,
        .header_count = 1,
        .body_stream = proven_reader_from_view(&source, PROVEN_LIT("nineteen bytes here")),
    };
    EXAMPLE_REQUIRE(proven_u8str_reset(&text) == PROVEN_OK, "reuse the string");
    err = proven_http_client_send(client, &req, &resp);
    EXAMPLE_REQUIRE(err == PROVEN_OK && proven_http_client_read_all(&resp, heap, &text, 1024) == PROVEN_OK, "a POST from a stream");
    EXAMPLE_REQUIRE(view_is(proven_u8str_as_view(&text), "19 bytes"), "the server counted them");
    proven_http_client_finish(&resp);

    /* 오류 상태 코드는 응답이지 오류가 아니다: 호출은 성공하고 여러분이 status를 본다. */
    err = proven_http_client_get(client, url_for(at.port, "/missing"), &resp);
    EXAMPLE_REQUIRE(err == PROVEN_OK && resp.status == 404, "a 404 arrives as a response");
    proven_http_client_finish(&resp);

    /* 오류란 들여다볼 응답이 없을 때다. */
    err = proven_http_client_get(client, PROVEN_LIT("https://127.0.0.1/"), &resp);
    EXAMPLE_REQUIRE(err == PROVEN_ERR_UNSUPPORTED, "https: this library has no TLS yet, and says so rather than sending in the clear");
    proven_http_client_finish(&resp);         /* 실패한 뒤에 불러도 해롭지 않다 */

    proven_u8str_destroy(heap, &text);
    proven_http_client_destroy(client);
    proven_http_cookie_jar_destroy(&jar);

    proven_http_server_stop(g_server);
    proven_job_group_wait(threads, &running);
    proven_http_server_destroy(g_server);
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    return EXAMPLE_OK();
}
```

## 11. 프로토콜 바꾸기

요청은 연결이 HTTP를 그만 실어 나르게 해 달라고 청할 수 있다: `Connection: Upgrade`와
`Upgrade: <protocol>`. 서버가 동의하면 `101 Switching Protocols`로 답하고, 그 뒤의 모든 바이트는 다른
프로토콜의 것이다. 두 드라이버 모두 그 시점에 연결을 넘겨줄 수 있다.

| API | 의도 | 반환 |
|---|---|---|
| `proven_http_exchange_upgrade(x, protocol, headers, count, &transport, &early)` | 핸들러 안에서: `Upgrade: protocol`과 `Connection: Upgrade`를 단 `101`로 답하고, 연결을 서버에서 꺼낸다. | `proven_err_t`: 응답을 이미 시작했거나, 요청에 읽지 않은 본문이 있거나, HTTP/1.0이면 `INVALID_STATE`. 서버가 맡은 헤더(`Upgrade` 포함)면 `INVALID_ARG`. `NOMEM`. `OUT_OF_BOUNDS`. `TIMEOUT`, `RESET`. |
| `request.upgrade` | 클라이언트 요청에서: 요청할 프로토콜. `Connection: Upgrade`와 `Upgrade`는 클라이언트가 직접 쓴다. | - |
| `proven_http_client_upgrade(&response, &transport, &early)` | `101`을 받은 뒤: 연결을 클라이언트에서 꺼낸다. | `proven_err_t`: 응답이 `101`이 아니거나 이미 꺼냈으면 `INVALID_STATE`. |

어느 쪽이든 연결을 **소유(owned)**하는 `proven_transport_t`를 받는다. 그것은 더 이상 서버나 클라이언트의
것이 아니다. `max_connections`에 세어지지 않고, 그들의 타임아웃이 적용되지 않고, 재사용되지 않으며,
여러분이 `proven_transport_close`를 부를 때까지 열려 있다. 서버 핸들러는 돌아간 뒤에도 그것을 갖고
있어도 된다.

**`early`는 다른 프로토콜의 시작이다.** 상대는 핸드셰이크가 끝나기를 기다렸다가 보낼 의무가 없으므로,
새 프로토콜의 첫 바이트들이 HTTP 헤드와 같은 읽기에 도착할 수 있다. 그것이 `early`에 있다 - exchange와
함께, 또는 `proven_http_client_finish`에서 죽는 뷰다 - . 이것을 무시하는 프로그램은 대화의 첫머리를
잃는데, 어떤 연결에서는 잃고 어떤 연결에서는 잃지 않는다.

**동의하기 전에 확인하라.** `proven_http_exchange_upgrade`는 `101`을 쓴다. 요청이 여러분이 뜻하는
프로토콜에 대한 올바른 요청이었는지는 판단하지 않는다. WebSocket이라면 그 확인과 거기 필요한 응답
헤더가 `proven_ws_check_request`와 `proven_ws_accept_key`다 - 아니면 이 모두를 해 주는
[12장](manual-12-websocket-ko.md)의 `proven_ws_conn_accept`를 써라.

**넘겨받은 뒤에는 한도가 여러분 몫이다.** 모든 기다림에 한도가 있다는 서버의 약속은 `101`에서 끝난다.
그 전송에 `PROVEN_NET_NO_DEADLINE`으로 읽으면 영원히 기다린다.

테스트 스위트가 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_11_http_upgrade.c -->
```c
/*
 * 프로토콜 바꾸기: 요청이 이 연결에서 HTTP를 그만 말하자고 청하고, 서버가 101로 동의하면,
 * 그때부터 같은 연결이 다른 것을 실어 나른다.
 *
 * 여기서 "다른 것"은 장난감이다 - 텍스트 줄을 받아 대문자로 돌려보낸다 - . 요점은 넘겨받는
 * 과정이기 때문이다. 사람들이 보통 뜻하는 프로토콜은 WebSocket이고, 12장의 연결이 이 단계들을
 * 대신 해 준다.
 */

static proven_http_server_t *g_server;

static void handle(void *ctx, proven_http_exchange_t *x) {
    (void)ctx;
    const proven_http_request_t *request = proven_http_exchange_request(x);
    /* 이것이 "우리" 프로토콜로 바꾸자는 요청인가? 확인은 핸들러의 일이다. */
    if (!proven_http_header_has_token(request->headers, request->header_count, PROVEN_LIT("Upgrade"), PROVEN_LIT("shout")) ||
        !proven_http_header_has_token(request->headers, request->header_count, PROVEN_LIT("Connection"), PROVEN_LIT("Upgrade"))) {
        (void)proven_http_exchange_respond(x, 426, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("this address speaks only shout\n")));
        return;
    }

    /* 서버가 101을 쓰고, 연결은 우리 것이 된다: 소켓을 소유한 전송. */
    proven_transport_t t;
    proven_mem_view_t early;                /* 클라이언트가 101을 기다리지 않고 보낸 바이트 */
    if (proven_http_exchange_upgrade(x, PROVEN_LIT("shout"), NULL, 0, &t, &early) != PROVEN_OK) return;

    /* 여기서부터는 HTTP가 아니고 서버의 한도는 하나도 적용되지 않는다: 모든 기다림에 우리의
     * 기한이 필요하다. `early`는 핸들러가 돌아가면 죽으므로 먼저 쓴다. */
    proven_byte_t line[64];
    proven_size_t n = early.size < sizeof line ? early.size : sizeof line;
    for (proven_size_t i = 0; i < n; ++i) line[i] = early.ptr[i];
    while (n < sizeof line && (n == 0 || line[n - 1] != '\n')) {
        proven_result_size_t got = proven_transport_read(t, (proven_mem_mut_t){ line + n, sizeof line - n }, proven_net_deadline_in(5000));
        if (got.err != PROVEN_OK) break;
        n += got.value;
    }
    for (proven_size_t i = 0; i < n; ++i) if (line[i] >= 'a' && line[i] <= 'z') line[i] = (proven_byte_t)(line[i] - 32);
    (void)proven_transport_write_all(t, (proven_mem_view_t){ line, n }, proven_net_deadline_in(5000));
    /* 붙들어 두었다가 핸들러가 돌아간 뒤에 써도 된다. 어느 쪽이든 닫는 것은 우리 몫이다. */
    (void)proven_transport_close(t);
}

static void serve(void *arg) {
    (void)arg;
    (void)proven_http_server_run(g_server);
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
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
    proven_job_sys_t *threads = NULL;
    proven_job_group_t running;
    proven_job_group_init(&running);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &threads) == PROVEN_OK &&
                    proven_job_group_submit(threads, &running, serve, NULL) == PROVEN_OK, "its loop runs on another thread");

    proven_http_client_config_t client_config = { .alloc = heap, .max_idle_connections = 2 };
    proven_http_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&client_config, &client) == PROVEN_OK, "a client");
    char url[64];
    int url_len = snprintf(url, sizeof url, "http://127.0.0.1:%u/", (unsigned)at.port);

    /* `upgrade`가 프로토콜의 이름을 정한다. `Connection: Upgrade`와 `Upgrade`는 클라이언트가 쓴다. */
    proven_http_client_request_t request = { .url = { (const proven_byte_t *)url, (proven_size_t)url_len }, .upgrade = PROVEN_LIT("shout") };
    proven_http_client_response_t response;
    err = proven_http_client_send(client, &request, &response);
    EXAMPLE_REQUIRE(err == PROVEN_OK && response.status == 101, "the server agreed: 101 Switching Protocols");

    /* 101은 그 연결을 가져가기 전까지는 다른 응답과 다를 바 없는 응답이다. */
    proven_transport_t t;
    proven_mem_view_t early;
    EXAMPLE_REQUIRE(proven_http_client_upgrade(&response, &t, &early) == PROVEN_OK, "the connection is ours now");
    proven_http_client_finish(&response);       /* 여전히 필요하다. 이제 연결은 건드리지 않는다 */

    proven_byte_t reply[16];
    proven_size_t have = 0;
    EXAMPLE_REQUIRE(proven_transport_write_all(t, proven_mem_view_from_u8(PROVEN_LIT("hello\n")), proven_net_deadline_in(5000)).err == PROVEN_OK, "a line, in the new protocol");
    while (have < 6) {
        proven_result_size_t got = proven_transport_read(t, (proven_mem_mut_t){ reply + have, sizeof reply - have }, proven_net_deadline_in(5000));
        if (got.err != PROVEN_OK) break;
        have += got.value;
    }
    EXAMPLE_REQUIRE(have == 6 && proven_u8str_view_eq((proven_u8str_view_t){ reply, 6 }, PROVEN_LIT("HELLO\n")), "and the answer, in capitals");
    (void)proven_transport_close(t);            /* 소켓을 닫고 전송을 해제한다 */

    /* `upgrade`가 없으면 같은 주소가 HTTP로 답하고, 가져갈 것이 없다. */
    err = proven_http_client_get(client, (proven_u8str_view_t){ (const proven_byte_t *)url, (proven_size_t)url_len }, &response);
    EXAMPLE_REQUIRE(err == PROVEN_OK && response.status == 426, "an ordinary request is told what this address speaks");
    EXAMPLE_REQUIRE(proven_http_client_upgrade(&response, &t, &early) == PROVEN_ERR_INVALID_STATE, "a response that is not a 101 has no connection to give");
    proven_http_client_finish(&response);

    proven_http_client_destroy(client);
    proven_http_server_stop(g_server);
    proven_job_group_wait(threads, &running);
    proven_http_server_destroy(g_server);
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    return EXAMPLE_OK();
}
```

## 12. 여기에 없는 것

- 양쪽 방향의 **TLS**. §1.
- **HTTP/2와 HTTP/3.**
- **압축.** `Accept-Encoding`을 보내지 않고 콘텐츠 코딩을 풀지 않는다. 그래도 압축해서 보내는 서버는
  압축된 바이트를 건넨다.
- **WebSocket**은 이 두 헤더에 없다. §11 위에 지은 [12장](manual-12-websocket-ko.md)에 있다.
- 서버의 **라우터, 정적 파일, 세션, 미들웨어**. 클라이언트의 **재시도, 캐시, 호스트별 연결 한도**.
- 양쪽 방향의 **트레일러**.
- **클라이언트가 보내는 `Expect: 100-continue`.** 본문은 먼저 묻지 않고 보낸다.
- **프록시 자동 설정, `NO_PROXY`, 프록시에 대한 Digest 인증.**
- **유예 기간을 둔 우아한 종료.** `stop`은 루프를 끝낸다. 요청을 기다리던 연결은 `destroy`가 닫는다.
