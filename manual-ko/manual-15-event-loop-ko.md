# 15장: 이벤트 루프와 이벤트 구동 서버

**5부 — 운영체제와 대화하기. 선행 조건: 소켓, 데드라인, 셀렉터를 다루는
[9장](manual-09-networking-ko.md). 이 장의 서버가 그 다른 형태인 HTTP 서버를 다루는
[11장](manual-11-http-client-server-ko.md). TLS를 쓰려면 [14장](manual-14-tls-ko.md).**
**이 장을 마치면** 타이머와 다른 스레드에서 돌아오는 일을 가진 루프를 돌리고, 기다리는 호출 하나
없이 그 위에서 HTTP를 서비스하고, 요청에 나중에 답하고, 프로그램보다 느린 클라이언트에게 응답을
스트리밍하고, 연결마다 스레드를 두지 않고 WebSocket 연결을 열어 두고, 한 스레드에서 많은 HTTP
요청을 한꺼번에 보내고, 서버를 여러 프로세서 코어에 펼치고, 아무 일도 하지 않는 연결 하나가 무엇을
차지하는지 - 그리고 무엇을 측정했는지 - 말할 수 있다.

이 장은 `loop.h`, `http_event.h`, `ws_event.h`, `http_event_client.h`를 다룬다. 모두
호스티드(hosted) 전용이고 `PROVEN_NO_NET`으로 빠진다.

## 목차

1. [서비스하는 두 가지 방식](#1-서비스하는-두-가지-방식)
2. [루프](#2-루프)
3. [타이머](#3-타이머)
4. [다른 곳에서 하는 일](#4-다른-곳에서-하는-일)
5. [여러분 자신의 소켓 지켜보기](#5-여러분-자신의-소켓-지켜보기)
6. [이벤트 구동 서버](#6-이벤트-구동-서버)
7. [답하기](#7-답하기)
8. [요청 본문](#8-요청-본문)
9. [한도, 시간, 거절](#9-한도-시간-거절)
10. [스레드, TLS, 종료](#10-스레드-tls-종료)
11. [루프 위의 WebSocket](#11-루프-위의-websocket)
12. [이벤트 구동 클라이언트](#12-이벤트-구동-클라이언트)
13. [여러 루프](#13-여러-루프)
14. [연결 하나가 붙드는 것, 측정한 것, 그리고 여기 없는 것](#14-연결-하나가-붙드는-것-측정한-것-그리고-여기-없는-것)

## 1. 서비스하는 두 가지 방식

11장의 서버는 핸들러에게 요청을 건네고, 기다리는 호출로 읽고 쓰게 한다. 이보다 쓰기 쉬운 것은
없다: 핸들러는 요청에서 응답까지 이어지는 한 줄기다. 그 값은, 처리 중인 요청 하나가 걸리는 시간
내내 - 클라이언트가 느린 시간까지 전부 - 스레드 하나를 차지한다는 것이다.

이 장의 서버는 그것을 뒤집는다. **그 안의 무엇도 기다리지 않는다.** 서버는 이벤트 루프 위에
앉는다. 루프는 어느 소켓에 할 말이 있는지 시스템에 묻고, 그것들을 처리하고, 다시 묻는 스레드
하나다. 여러분의 함수들은 무슨 일이 일어났는지 - 요청이 도착했다, 본문 한 조각이 도착했다, 더 쓸
자리가 생겼다 - 전해 듣고 곧바로 돌아온다. 그러면 아무 일도 하지 않는 연결은 작은 구조체 하나일
뿐 다른 것이 아니다: 스레드도, 스택도, 버퍼도 없다.

그 대신 떠맡는 것은 실제로 있고, 그것이 이 장의 전부다:

- **여러분의 함수는 기다려서는 안 된다** - 소켓도, 파일도, 누군가 오래 쥘 수 있는 락도. 시간이
  걸리는 일은 다른 스레드로 가고 그 결과가 게시되어 돌아온다(4절).
- **쓰기는 거절될 수 있다.** 클라이언트가 보낸 것을 가져가지 않으면 서버는 여러분에게서 더 받기를
  멈추고, 언제 계속할지 알려 준다(7절).
- **보여 주는 것은 수명이 짧다.** 요청 헤드와 본문 조각은 여러분의 함수가 돌아올 때까지만 유효한
  뷰(view)다. 간직할 것은 복사한다.

어느 쪽을 고를까: 동시에 열려 있는 연결 수가 스레드 풀(pool)이 넉넉히 감당하는 정도이고 핸들러를
한 줄기로 쓰는 편이 쉬운 동안에는 블로킹 서버. 대부분의 연결이 대부분의 시간 동안 한가하고 그런
연결이 많을 때 - 롱 폴링, 이벤트 스트림, 느린 클라이언트, 아주 많은 유지 연결 - 는 이 서버. 둘은
[10장](manual-10-http-ko.md)의 같은 코드로 HTTP를 파싱하고 같은 잘못된 요청을 거절한다.

## 2. 루프

```c
proven_err_t proven_loop_create(proven_allocator_t alloc, proven_loop_t **out);
void proven_loop_destroy(proven_loop_t *loop);
proven_err_t proven_loop_run(proven_loop_t *loop);
proven_err_t proven_loop_poll(proven_loop_t *loop, proven_net_deadline_t until);
void proven_loop_stop(proven_loop_t *loop);
```

`proven_loop_t`는 셀렉터([9장](manual-09-networking-ko.md) 9절), 타이머 묶음, 다른 스레드가 게시할
수 있는 큐, 그리고 임시 작업용(scratch) 버퍼다. 만들 때 할당자(allocator)를 하나 받는다.

| 호출 | 하는 일 | 반환 |
|---|---|---|
| `proven_loop_create(alloc, &loop)` | 루프를 만든다. | `proven_err_t`: `INVALID_ARG`. `NOMEM`. 셀렉터를 여는 호출이 돌려준 것. |
| `proven_loop_run(loop)` | 기다리고, 준비된 것을 부르고, 때가 된 것을 울리고, 반복한다 - `proven_loop_stop`까지. | 멈춘 뒤 `PROVEN_OK`. 기다리는 일 자체가 실패하면 오류. |
| `proven_loop_poll(loop, until)` | 한 바퀴: 무언가 준비되거나 `until`이 지날 때까지 기다리고 처리한다. `PROVEN_NET_DONT_WAIT`는 지금 준비된 것만 처리한다. | `proven_err_t`. |
| `proven_loop_stop(loop)` | `proven_loop_run`이 돌아오게 한다. 어느 스레드에서든, 콜백 안에서든 안전하다. | 없음. |
| `proven_loop_destroy(loop)` | 루프를 해제한다. 아직 등록되었거나 맞춰진 것은 잊힌다. 아무것도 불리지 않는다. 널을 받는다. | 없음. |

`proven_loop_run`은 다른 일을 하지 않는 스레드를 위한 것이다. `proven_loop_poll`은 이미 자신의 메인
루프가 있어서 그 안에서 이 루프에 차례를 주는 프로그램을 위한 것이다.

**스레드 규칙.** 이 장에서 `proven_loop_stop`과 `proven_loop_post`를 뺀 모든 것은 루프 자신의
스레드 - `proven_loop_run`이나 `proven_loop_poll` 안에 있는 스레드 - 에서, 또는 루프가 시작하기 전에
불러야 한다. 서버와 뒤 절들의 모든 스트림 함수가 여기에 든다. 대신 검사해 주는 것은 없다.

## 3. 타이머

```c
void proven_loop_timer_set(proven_loop_t *loop, proven_loop_timer_t *timer, proven_u32 ms, proven_loop_fn fn, void *ctx);
void proven_loop_timer_cancel(proven_loop_t *loop, proven_loop_timer_t *timer);
bool proven_loop_timer_is_set(const proven_loop_timer_t *timer);
```

`proven_loop_timer_t`는 여러분의 것이다: 그것이 속한 구조체 안에 0으로 초기화해 두고, 맞춰져 있는
동안에는 복사하거나 옮기지 않는다. 맞추는 데 아무것도 할당하지 않는다.

- 타이머는 지금부터 `ms` 밀리초 뒤에 **한 번 울린다**. 루프의 틱인 16 ms로 올림된다. 일찍 울리지
  않는다. 반복해야 하는 타이머는 자기 함수 안에서 스스로 다시 맞춘다.
- 이미 맞춰진 타이머를 맞추면 **옮겨진다**.
- `proven_loop_timer_cancel`은 어느 콜백 안에서든, 다른 타이머의 콜백 안에서도 안전하고, 맞춰져
  있지 않은 타이머에는 해가 없다.
- 맞추기, 취소하기, 울리기는 상수 시간이고, 기다리는 타이머는 때가 될 때까지 들여다보지 않는다.
  서버의 모든 연결이 저마다의 타임아웃을 가질 수 있는 것은 그 덕이다.

타이머는 [5장](manual-05-hosted-services-ko.md)의 단조 시계를 잰다. 벽시계를 맞춰도 움직이지 않는다.

## 4. 다른 곳에서 하는 일

```c
proven_err_t proven_loop_post(proven_loop_t *loop, proven_loop_fn fn, void *ctx);
```

`proven_loop_post`는 루프에게 `fn(ctx)`를 루프 자신의 스레드에서 곧 불러 달라고 한다. **어느
스레드에서든 안전하고**, 결과가 돌아오는 길이 이것이다: 콜백은 느린 부분을 워커([6장](manual-06-execution-and-platform-ko.md)의
job system)에게 넘기고 돌아오며, 워커는 루프의 데이터를 만져도 되는 곳에서 일을 마무리할 함수를
게시한다.

- 함수들은 게시된 순서대로 불린다.
- 큐 항목은 루프의 할당자에서 나오므로, 다른 스레드가 게시한다면 그 할당자는 여러 스레드에서
  불러도 되는 것이어야 한다. 힙(heap) 할당자는 그렇다.
- 그 할당이 실패하면 `PROVEN_ERR_NOMEM`을, 그리고 `PROVEN_ERR_INVALID_ARG`를 돌려준다.

아래 프로그램은 타이머들과 일 하나만 올라가 있는 루프다. 테스트 스위트가 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_15_loop.c -->
```c
#include <stdatomic.h>

/*
 * 이벤트 루프 그 자체: 타이머, 그리고 다른 스레드에서 하고 돌아오는 일.
 *
 * 루프의 스레드는 "다음은 무엇인가" 말고는 아무것도 기다리지 않는다. 시간이 걸리는 것 - 여기서는
 * 파일이나 데이터베이스를 대신해 잠을 자는 워커 - 은 다른 곳에서 일어나고, 그 결과는 루프에
 * 게시되어 다른 이벤트들 사이에서 실행된다.
 */

typedef struct {
    proven_loop_t *loop;
    proven_loop_timer_t tick, late;
    int ticks;
    int order[8];
    int count;
    atomic_int from_worker;
} app_t;

static void note(app_t *app, int what) { if (app->count < 8) app->order[app->count++] = what; }

/* 타이머는 한 번 울린다. 반복해야 하는 타이머는 스스로 다시 맞춘다. */
static void on_tick(void *ctx) {
    app_t *app = ctx;
    note(app, 1);
    if (++app->ticks < 3) proven_loop_timer_set(app->loop, &app->tick, 20, on_tick, app);
}

static void on_late(void *ctx) { note(ctx, 9); }

/* 워커 스레드가 요청했지만, 이것은 루프의 스레드에서 실행된다. */
static void on_result(void *ctx) {
    app_t *app = ctx;
    note(app, 5);
    proven_loop_timer_cancel(app->loop, &app->late);      /* 오 초 뒤에 울렸을 것이다. 이제는 영영 울리지 않는다 */
    proven_loop_stop(app->loop);
}

/* 이것은 워커 스레드에서 실행된다. 얼마든지 오래 걸려도 된다: 루프는 기다리지 않는다. */
static void slow_work(void *ctx) {
    app_t *app = ctx;
    proven_time_sleep(400);
    atomic_store(&app->from_worker, 1);
    if (proven_loop_post(app->loop, on_result, app) != PROVEN_OK) proven_loop_stop(app->loop);   /* 게시는 할당 실패일 때만 거절된다. 그때는 그냥 실행을 끝낸다 */
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    app_t app = { 0 };

    // ---- 루프 ---------------------------------------------------------------------
    /* 루프는 셀렉터, 타이머 묶음, 그리고 다른 스레드가 게시할 수 있는 큐다. */
    EXAMPLE_REQUIRE(proven_loop_create(heap, &app.loop) == PROVEN_OK, "루프");

    // ---- 타이머 --------------------------------------------------------------------
    /* 타이머 구조체는 여러분의 것이다: 여러분의 상태 안에 살고, 맞추는 데 아무것도 할당하지
     * 않는다. 울리기를 기다리는 타이머도 비용이 없다 - 때가 될 때까지 들여다보지 않는다. */
    proven_loop_timer_set(app.loop, &app.tick, 20, on_tick, &app);
    proven_loop_timer_set(app.loop, &app.late, 5000, on_late, &app);
    EXAMPLE_REQUIRE(proven_loop_timer_is_set(&app.tick) && proven_loop_timer_is_set(&app.late), "타이머 둘이 맞춰졌다");

    // ---- 다른 곳에서 하는 일 --------------------------------------------------------
    /* job system이 slow_work를 워커 스레드에서 돌린다. 끝나면 on_result를 루프로 게시한다. */
    proven_job_sys_t *workers = NULL;
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &workers) == PROVEN_OK && proven_job_submit(workers, slow_work, &app), "워커가 느린 일을 맡았다");

    // ---- 실행 ----------------------------------------------------------------------
    /* proven_loop_run은 무언가가 proven_loop_stop을 부르면 돌아온다 - 여기서는 on_result가 부른다. */
    proven_time_t began = proven_time_monotonic_now();
    EXAMPLE_REQUIRE(proven_loop_run(app.loop) == PROVEN_OK, "루프가 돌았고 멈춰졌다");
    proven_time_t took = proven_time_monotonic_now() - began;

    EXAMPLE_REQUIRE(app.ticks == 3 && app.count == 4 && app.order[0] == 1 && app.order[1] == 1 && app.order[2] == 1 && app.order[3] == 5,
                    "틱 세 번, 그다음 워커의 결과, 그 순서로");
    EXAMPLE_REQUIRE(atomic_load(&app.from_worker) == 1 && took < 2000000000, "결과는 워커에게서 왔고, 오 초 타이머보다 훨씬 먼저 왔다");
    EXAMPLE_REQUIRE(!proven_loop_timer_is_set(&app.late), "취소된 타이머는 더는 맞춰져 있지 않다");

    proven_job_system_close(workers);
    proven_job_system_destroy(workers);
    proven_loop_destroy(app.loop);
    return EXAMPLE_OK();
}
```

## 5. 여러분 자신의 소켓 지켜보기

다음 절의 서버는 이것을 대신 해 준다. 이 절은 HTTP가 아닌 프로토콜을 위한 것이다.

```c
proven_err_t proven_loop_io_add(proven_loop_t *loop, proven_loop_io_t *io, proven_net_handle_t handle,
                                proven_u8 want, proven_loop_io_fn fn, void *ctx);
proven_err_t proven_loop_io_set(proven_loop_t *loop, proven_loop_io_t *io, proven_u8 want);
void proven_loop_io_remove(proven_loop_t *loop, proven_loop_io_t *io);
proven_mem_mut_t proven_loop_scratch(proven_loop_t *loop);
proven_allocator_t proven_loop_allocator(const proven_loop_t *loop);
proven_size_t proven_loop_io_count(const proven_loop_t *loop);
```

| 호출 | 하는 일 | 반환 |
|---|---|---|
| `proven_loop_io_add(loop, &io, handle, want, fn, ctx)` | 소켓을 지켜본다: `want`가 묻는 것(`PROVEN_NET_READABLE`, `PROVEN_NET_WRITABLE`, 둘 다, 또는 아직 아무것도 아닌 0)에 준비되면 `fn(ctx, got)`가 불린다. | `proven_err_t`: `INVALID_ARG`. `io`가 이미 등록되어 있으면 `INVALID_STATE`. 셀렉터가 돌려준 것. |
| `proven_loop_io_set(loop, &io, want)` | 무엇을 지켜볼지 바꾼다. | `proven_err_t`. |
| `proven_loop_io_remove(loop, &io)` | 지켜보기를 그만둔다. | 없음. |
| `proven_loop_scratch(loop)` | 루프의 임시 작업용 버퍼, 64 KiB. | `proven_mem_mut_t`. |
| `proven_loop_allocator(loop)` | 루프를 만들 때 준 할당자. | `proven_allocator_t`. |
| `proven_loop_io_count(loop)` | 지켜보는 소켓의 수. | `proven_size_t`. |

`proven_loop_io_t`는 타이머와 같은 뜻에서 여러분의 것이다: 여러분의 연결 구조체 안에, 0으로
초기화해서, 등록되어 있는 동안 옮기지 않는다. 알아 둘 것 네 가지:

- **준비 상태는 레벨 트리거다.** 읽을 것이 있는 동안 함수는 바퀴마다 불린다.
  `PROVEN_NET_DONT_WAIT`로 호출이 기다려야 한다고 할 때까지 읽거나, `PROVEN_NET_READABLE`을 그만
  묻는다.
- **쓸 것이 있는 동안에만 `PROVEN_NET_WRITABLE`을 묻는다.** 자리가 있는 소켓은 언제나 쓸 수 있고,
  바퀴마다 그 말을 듣는 루프는 다른 일을 하지 못한다.
- **닫기 전에 뺀다.** `proven_loop_io_remove`는 어느 콜백 안에서든, 그 소켓 자신의 콜백 안에서도
  안전하고, 그 뒤로는 그 소켓에 대해 아무것도 전달되지 않는다 - 같은 바퀴에서 이미 거둔
  이벤트조차도.
- **임시 작업용 버퍼는 수천 개의 연결이 읽기 버퍼 하나를 나눠 쓰는 방법이다.** 거기에 읽어 들이고,
  도착한 것을 쓰고, 아직 쓸 수 없는 것만 간직한다. 콜백이 돌아오거나, 그 버퍼를 쓸 수도 있는 다른
  것을 부르고 나면 내용은 아무 뜻이 없다.

## 6. 이벤트 구동 서버

```c
proven_err_t proven_http_event_server_create(proven_loop_t *loop, const proven_http_event_server_config_t *config,
                                             proven_http_event_server_t **out);
proven_err_t proven_http_event_server_listen(proven_http_event_server_t *server, proven_net_addr_t at, proven_net_addr_t *bound);
void proven_http_event_server_destroy(proven_http_event_server_t *server);
proven_size_t proven_http_event_server_connections(const proven_http_event_server_t *server);
void proven_http_event_server_stop_listening(proven_http_event_server_t *server);
```

서버는 루프 위에 만들어지고, 어디서 들을지 지시받고, 루프가 돌 때까지는 아무 일도 하지 않는다.

| 호출 | 하는 일 | 반환 |
|---|---|---|
| `proven_http_event_server_create(loop, &config, &server)` | 서버를 만든다. | `proven_err_t`: 루프가 없거나, `on_request`가 없거나, TLS 설정에 인증서가 없으면 `INVALID_ARG`. `NOMEM`. |
| `proven_http_event_server_listen(server, at, &bound)` | `at`에서 듣는다. 서버 하나에 주소 넷까지. `bound`(선택)는 쓰이는 주소를 받는다 - `at`이 포트 0을 청했다면 시스템이 고른 포트와 함께. | `proven_err_t`. |
| `proven_http_event_server_connections(server)` | 지금 열려 있는 연결. | `proven_size_t`. |
| `proven_http_event_server_stop_listening(server)` | 받아들이기를 멈춘다. 열린 연결은 계속된다. | 없음. |
| `proven_http_event_server_destroy(server)` | 모든 연결과 리스너를 닫고 서버를 해제한다. 진행 중인 교환은 `PROVEN_ERR_RESET`과 함께 `on_done`을 받는다. 루프는 멈추지도 해제되지도 않는다. 널을 받는다. | 없음. |

**함수 넷.** 설정이 `on` 안에 싣고, `ctx`가 각각에 전달된다:

| 함수 | 언제 | 비고 |
|---|---|---|
| `on_request(ctx, stream, head)` | 요청 헤드가 도착해 서버의 검사를 통과했다. | 필수. 여기서 답하거나, `stream`을 기억해 두고 나중에 답한다. `head`는 이 함수가 돌아올 때까지만 유효하다. |
| `on_body(ctx, stream, piece, last)` | 요청 본문의 한 조각. 마지막 조각에서 `last`가 참이고, 그 조각은 비어 있을 수 있다. | 선택: 없으면 본문은 읽혀서 버려진다. `piece`는 이 함수가 돌아올 때까지만 유효하다. |
| `on_writable(ctx, stream)` | 보류되었던 출력이 나갔고, `proven_http_stream_write`가 더 받는다. | 선택. |
| `on_done(ctx, stream, why)` | 교환이 끝났다. | 선택. **`on_request`마다 정확히 한 번** 불린다: 응답 전체가 연결에 넘겨졌으면 `PROVEN_OK`, 아니면 그러지 못한 이유. |

**스트림은 요청 하나와 그 응답이다.** `proven_http_stream_t *`는 `on_request`부터 `on_done`이
돌아올 때까지 유효하고, 그보다 한순간도 더 길지 않다. 그러므로 `on_done`은 스트림에 붙여 둔 것을
해제하고, 아직 스트림을 쥐고 있는 타이머를 취소하거나 일을 잊게 할 단 하나의 자리다: 끝난 스트림을
들고 나중에 도착하는 함수에게는 부를 수 있는 유효한 것이 없다.

```c
void proven_http_stream_set_user(proven_http_stream_t *stream, void *user);
void *proven_http_stream_user(const proven_http_stream_t *stream);
proven_net_addr_t proven_http_stream_peer(const proven_http_stream_t *stream);
```

`proven_http_stream_set_user`는 여러분의 포인터를 스트림에 붙이고 `proven_http_stream_user`가 그것을
돌려준다 - 아무것도 받지 않은 스트림은 널을 돌려주며, 해제하는 `on_done`은 그 경우를 받아들여야
한다. `proven_http_stream_peer`는 클라이언트의 주소다.

## 7. 답하기

```c
proven_err_t proven_http_stream_respond(proven_http_stream_t *stream, proven_u16 status,
                                        const proven_http_header_t *headers, proven_size_t header_count,
                                        proven_mem_view_t body);
proven_err_t proven_http_stream_begin(proven_http_stream_t *stream, proven_u16 status,
                                      const proven_http_header_t *headers, proven_size_t header_count,
                                      proven_u64 content_length);
proven_result_size_t proven_http_stream_write(proven_http_stream_t *stream, proven_mem_view_t data);
proven_err_t proven_http_stream_end(proven_http_stream_t *stream);
void proven_http_stream_abort(proven_http_stream_t *stream);
proven_size_t proven_http_stream_buffered(const proven_http_stream_t *stream);
```

**전부 한 번에.** `proven_http_stream_respond`는 상태, 여러분의 헤더, 메모리에 든 본문을 보낸다.
본문은 크기가 얼마든 복사되므로 작은 본문을 위한 것이다. `Content-Length`, `Date`, `Connection`은
대신 써 주며 `headers`에 들어 있어서는 안 된다. `on_request` 안에서, 또는 스트림이 유효한 동안 그
뒤 어느 때든 루프의 스레드에서 부를 수 있다.

**조각조각.** `proven_http_stream_begin`은 헤드를 보내고 `content_length` 바이트의 본문을, 또는 -
`PROVEN_HTTP_EVENT_LENGTH_UNKNOWN`을 주면 - 아직 아무도 길이를 모르는, chunked로 보내는 본문을
약속한다. 그다음 `proven_http_stream_write`가 바이트를 내놓고 `proven_http_stream_end`가 끝낸다.

**`proven_http_stream_write`는 내놓은 것보다 적게 받을 수 있다** - `max_buffered_output` 아래에
들어가는 만큼, 어쩌면 하나도 - 그리고 몇 바이트를 받았는지 돌려준다. 이것은 오류가 아니다. 이미 보낸
것을 클라이언트가 가져가지 않았다는 뜻이고, 규칙은 이렇다: **나머지를 간직하고, 돌아오고,
`on_writable`이 불리면 다시 내놓는다.** 그렇게 쓴 생산자는 가장 느린 클라이언트와 정확히 같은
속도로 가고, 그 연결에 쓰이는 서버의 메모리는 응답 크기와 무관하게 한도 아래에 머문다.

| 호출 | 반환 |
|---|---|
| `proven_http_stream_respond`, `proven_http_stream_begin` | `proven_err_t`: 응답이 이미 시작되었거나 스트림이 끝났으면 `INVALID_STATE`. 상태가 200-999 밖이거나 서버가 직접 쓰는 헤더가 있으면 `INVALID_ARG`. 응답 헤드가 `max_head_bytes`에 들어가지 않으면 `OUT_OF_BOUNDS`. `NOMEM`. |
| `proven_http_stream_write` | `proven_result_size_t`: 받은 바이트 수. `begin` 전, `end` 후, 또는 스트림이 끝났으면 `INVALID_STATE`. 약속한 `Content-Length`를 넘으면 `OUT_OF_BOUNDS`. |
| `proven_http_stream_end` | `proven_err_t`: 응답이 시작되지 않았거나 이미 끝났으면 `INVALID_STATE`. 약속보다 적게 썼으면 `INVALID_FORMAT` - 그때 연결은 닫힌다. 클라이언트가 모자란 본문을 온전한 것으로 받아들이지 못하게 하기 위해서다. |
| `proven_http_stream_abort` | 없음. 연결은 곧바로 닫히고 `on_done`이 `PROVEN_ERR_RESET`과 함께 불린다. |
| `proven_http_stream_buffered` | `proven_size_t`: 이 연결을 위해 붙들고 있고 클라이언트가 아직 받지 않은 응답 바이트. |

프로토콜에서 따라 나오고 여러분에게 아무것도 요구하지 않는 세 가지: `HEAD`에 대한 응답과 상태 204,
304의 응답은 무엇을 넘기든 본문 없이 나간다. `Expect: 100-continue`라고 한 요청은 본문이 처음 필요해질
때 계속하라는 말을 듣는다. 그리고 닫기를 청한 요청이나 끝까지 읽을 수 없었던 요청에 응답한 뒤에는
연결을 유지하지 않고 닫는다.

모든 것이 한 번에 소켓에 들어갈 때, `on_done`은 `proven_http_stream_respond`나
`proven_http_stream_end` **안에서** 불릴 수 있다. 둘 중 하나가 돌아온 뒤에는 스트림도, `on_done`이
해제한 것도 건드리지 않는다.

아래 프로그램은 네 가지 방식으로 답한다 - 즉시, 나중에 타이머에서, 거절당하면 물러서는 펌프로
스트리밍해서, 그리고 조각조각 받은 본문 뒤에. 테스트 스위트가 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_15_http_event.c -->
```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 이벤트 루프 위의 HTTP 서버: 스레드 하나, 그리고 그 안에 기다리는 호출은 없다.
 *
 * 서버가 이 함수들에게 무슨 일이 일어났는지 알려 주고, 함수들은 곧바로 돌아온다. 답하는 방식
 * 네 가지를 보인다: 즉시. 나중에 타이머에서. 스트리밍하되 클라이언트가 느리면 물러서며. 그리고
 * 요청 본문이 조각조각 도착한 뒤에.
 *
 * 클라이언트는 11장의 블로킹 클라이언트이고, 예제의 메인 스레드에서 돈다.
 */

typedef struct {
    proven_loop_t *loop;
    int served;
} app_t;

/* 요청 하나에 답하는 동안 이 프로그램이 간직하는 것. */
typedef struct {
    app_t *app;
    proven_http_stream_t *stream;
    proven_loop_timer_t timer;
    proven_size_t sent, total;         /* /count: how far the body has got */
    proven_size_t received;            /* /size: body bytes seen */
} exchange_t;

/* 서버가 "지금은 그만"이라고 할 때까지 줄을 쓴다. 이미 보낸 것을 클라이언트가 가져가지
 * 않았을 때 그렇게 말한다. 나머지는 on_writable을 기다린다. */
static void count_pump(exchange_t *x) {
    while (x->sent < x->total) {
        char line[32];
        int n = snprintf(line, sizeof line, "%u\n", (unsigned)x->sent);
        proven_result_size_t w = proven_http_stream_write(x->stream, (proven_mem_view_t){ (const proven_byte_t *)line, (proven_size_t)n });
        if (w.err != PROVEN_OK) return;                   /* 클라이언트가 떠났다 */
        if (w.value < (proven_size_t)n) return;           /* 보류됨: 언제 되는지는 on_writable이 알려 준다 */
        x->sent++;
    }
    (void)proven_http_stream_end(x->stream);
}

static void answer_later(void *ctx) {
    exchange_t *x = ctx;
    (void)proven_http_stream_respond(x->stream, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("worth the wait\n")));
}

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    app_t *app = ctx;
    exchange_t *x = calloc(1, sizeof *x);
    if (!x) { proven_http_stream_abort(stream); return; }
    x->app = app;
    x->stream = stream;
    proven_http_stream_set_user(stream, x);

    /* `head`와 그 안의 모든 것은 이 함수가 돌아올 때까지만 유효하다. */
    if (proven_u8str_view_eq(head->target, PROVEN_LIT("/"))) {
        /* 가장 단순한 답: 전부, 지금. */
        (void)proven_http_stream_respond(stream, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("hello\n")));
    } else if (proven_u8str_view_eq(head->target, PROVEN_LIT("/slow"))) {
        /* 지금은 아니다: 스트림을 기억해 두고 타이머에서 답한다. 그동안 루프는 계속 돈다. */
        proven_loop_timer_set(app->loop, &x->timer, 50, answer_later, x);
    } else if (proven_u8str_view_eq(head->target, PROVEN_LIT("/count"))) {
        /* 만들어 가며 보내는 본문. 길이를 미리 알 수 없으므로 chunked로 보낸다. */
        x->total = 20000;
        if (proven_http_stream_begin(stream, 200, NULL, 0, PROVEN_HTTP_EVENT_LENGTH_UNKNOWN) == PROVEN_OK) count_pump(x);
    } else if (proven_u8str_view_eq(head->target, PROVEN_LIT("/size"))) {
        /* 아직 없다: 본문이 오는 중이고, 다 오면 on_body가 답한다. */
    } else {
        (void)proven_http_stream_respond(stream, 404, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("not here\n")));
    }
}

static void on_body(void *ctx, proven_http_stream_t *stream, proven_mem_view_t piece, bool last) {
    (void)ctx;
    exchange_t *x = proven_http_stream_user(stream);
    x->received += piece.size;
    if (last) {
        char text[32];
        int n = snprintf(text, sizeof text, "%u bytes\n", (unsigned)x->received);
        (void)proven_http_stream_respond(stream, 200, NULL, 0, (proven_mem_view_t){ (const proven_byte_t *)text, (proven_size_t)n });
    }
}

/* 보류되었던 출력이 나갔다: 쓰기가 멈춘 자리에서 이어 간다. */
static void on_writable(void *ctx, proven_http_stream_t *stream) {
    (void)ctx;
    exchange_t *x = proven_http_stream_user(stream);
    if (x->total > 0) count_pump(x);
}

/* 무슨 일이 있었든 on_request마다 한 번 불린다. 붙여 둔 것을 해제할 자리. */
static void on_done(void *ctx, proven_http_stream_t *stream, proven_err_t why) {
    app_t *app = ctx;
    exchange_t *x = proven_http_stream_user(stream);
    if (!x) return;
    proven_loop_timer_cancel(app->loop, &x->timer);
    free(x);
    if (why == PROVEN_OK) app->served++;
}

static void run(void *arg) { (void)proven_loop_run(((app_t *)arg)->loop); }

static char g_url[64];
static proven_u8str_view_t url_for(proven_u16 port, const char *path) {
    int n = snprintf(g_url, sizeof g_url, "http://127.0.0.1:%u%s", (unsigned)port, path);
    return (proven_u8str_view_t){ (const proven_byte_t *)g_url, (proven_size_t)n };
}

static bool body_equals(proven_http_client_response_t *resp, proven_allocator_t alloc, const char *want) {
    proven_u8str_t body = { 0 };
    bool ok = proven_http_client_read_all(resp, alloc, &body, 1024 * 1024) == PROVEN_OK &&
              proven_u8str_view_eq(proven_u8str_as_view(&body), (proven_u8str_view_t){ (const proven_byte_t *)want, strlen(want) });
    proven_u8str_destroy(alloc, &body);
    return ok;
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    app_t app = { 0 };

    // ---- 서버 ----------------------------------------------------------------------
    /* 서버는 루프 위에 만들어지고 듣는다. 루프가 돌 때까지는 아무 일도 하지 않는다. */
    EXAMPLE_REQUIRE(proven_loop_create(heap, &app.loop) == PROVEN_OK, "루프");
    proven_http_event_server_config_t config = {
        .on = { on_request, on_body, on_writable, on_done },
        .ctx = &app,
        .max_buffered_output = 16 * 1024,        /* 클라이언트가 가져가지 않은 것을 연결마다 최대 이만큼만 붙든다 */
    };
    proven_http_event_server_t *server = NULL;
    proven_net_addr_t at;
    EXAMPLE_REQUIRE(proven_http_event_server_create(app.loop, &config, &server) == PROVEN_OK &&
                    proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "빈 포트의 이벤트 구동 서버");

    /* 이 예제는 메인 스레드가 클라이언트 노릇을 할 수 있도록 루프에 따로 스레드를 준다. */
    proven_job_sys_t *threads = NULL;
    proven_job_group_t running;
    proven_job_group_init(&running);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &threads) == PROVEN_OK &&
                    proven_job_group_submit(threads, &running, run, &app) == PROVEN_OK, "루프가 다른 스레드에서 돈다");

    // ---- 요청 넷 -------------------------------------------------------------------
    proven_http_client_config_t client_config = { .alloc = heap, .max_idle_connections = 1 };
    proven_http_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&client_config, &client) == PROVEN_OK, "클라이언트");
    proven_http_client_response_t resp;

    EXAMPLE_REQUIRE(proven_http_client_get(client, url_for(at.port, "/"), &resp) == PROVEN_OK && body_equals(&resp, heap, "hello\n"), "즉시 답했다");
    proven_http_client_finish(&resp);
    EXAMPLE_REQUIRE(proven_http_client_get(client, url_for(at.port, "/slow"), &resp) == PROVEN_OK && body_equals(&resp, heap, "worth the wait\n"), "50 ms 뒤에 타이머에서 답했다");
    proven_http_client_finish(&resp);

    /* 이만 줄. 서버가 보류할 때마다 핸들러는 멈추고 on_writable이 재개시킨다. */
    EXAMPLE_REQUIRE(proven_http_client_get(client, url_for(at.port, "/count"), &resp) == PROVEN_OK && resp.status == 200, "스트리밍 응답이 시작된다");
    proven_u8str_t counted = { 0 };
    EXAMPLE_REQUIRE(proven_http_client_read_all(&resp, heap, &counted, 1024 * 1024) == PROVEN_OK, "끝까지 읽힌다");
    proven_u8str_view_t all = proven_u8str_as_view(&counted);
    EXAMPLE_REQUIRE(all.size == 108890 && memcmp(all.ptr, "0\n1\n2\n", 6) == 0 && memcmp(all.ptr + all.size - 6, "19999\n", 6) == 0, "모든 줄이 순서대로 도착했다");
    proven_u8str_destroy(heap, &counted);
    proven_http_client_finish(&resp);

    static proven_byte_t upload[50000];
    proven_http_client_request_t post = { .method = PROVEN_LIT("POST"), .url = url_for(at.port, "/size"), .body = { upload, sizeof upload } };
    EXAMPLE_REQUIRE(proven_http_client_send(client, &post, &resp) == PROVEN_OK && body_equals(&resp, heap, "50000 bytes\n"), "조각조각 센 본문");
    proven_http_client_finish(&resp);
    proven_http_client_destroy(client);

    // ---- 끝 ------------------------------------------------------------------------
    /* 루프를 멈춘 다음 서버를 해체한다. 그때는 다른 무엇도 서버를 건드리지 않는다. */
    proven_loop_stop(app.loop);
    proven_job_group_wait(threads, &running);
    EXAMPLE_REQUIRE(app.served == 4, "교환 넷이 on_done(PROVEN_OK)로 끝났다");
    proven_http_event_server_destroy(server);
    proven_loop_destroy(app.loop);
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    return EXAMPLE_OK();
}
```

`count_pump`를 보라: 스트리밍되는 모든 응답이 갖는 모양이다. 시작할 때 `on_request`에서, 이어 갈 때
`on_writable`에서 불리고, 자기 위치를 교환 자신의 상태에 간직하며, 쓰기가 내놓은 것보다 적게 받는
순간 돌아온다. 어떤 실행에서 실제로 거절당하는지는 클라이언트가 얼마나 빨리 읽느냐에 달렸다. 함수는
어느 쪽이든 옳다.

## 8. 요청 본문

```c
void proven_http_stream_pause(proven_http_stream_t *stream);
void proven_http_stream_resume(proven_http_stream_t *stream);
```

본문은 네트워크가 전달한 크기 그대로의 조각으로 `on_body`를 통해 도착하고, 프레이밍 -
`Content-Length`나 청크 - 은 이미 벗겨져 있다. 본문이 없는 요청은 `on_body` 호출을 아예 받지 않는다.
본문이 있는 요청은 `last`가 참인 마지막 호출을 받는다.

핸들러는 본문이 도착하기 전에, 또는 본문을 읽지 않고 답해도 된다. 그때 본문의 나머지는 비용이 작으면
읽혀서 버려지고, 그렇지 않으면 응답 뒤에 연결이 닫힌다.

`proven_http_stream_pause`는 반대 방향의 backpressure다: `proven_http_stream_resume`까지 조각이 더
전달되지 않고 연결도 읽히지 않는다. 받은 것을 네트워크보다 느린 무언가에 쓰는 핸들러는 이 쌍을 써서,
본문이 메모리에 쌓이는 대신 클라이언트가 느려지게 한다. 기다리던 조각들은
`proven_http_stream_resume` 호출 안에서 도착할 수 있다.

## 9. 한도, 시간, 거절

`proven_http_event_server_config_t`를 0으로 초기화하고, `on.on_request`를 정하고, 필요한 것을 정한다.
0인 필드마다 기본값이 있다:

| 필드 | 뜻 | 기본값 |
|---|---|---|
| `alloc` | 연결과 버퍼를 위한 할당자. | 루프의 것 |
| `max_connections` | 동시에 열려 있는 연결. 그 이상은 받아들여지지 않은 채 시스템의 백로그에서 기다린다. | 10,000 |
| `max_head_bytes` | 가장 큰 요청 헤드. 가장 큰 응답 헤드이기도 하다. | 16 KiB |
| `max_headers` | 요청 하나의 헤더 필드 최대 수. | 64 |
| `max_body_bytes` | 가장 큰 요청 본문. | 1 MiB |
| `max_buffered_output` | 쓰기가 거절되기 전까지 연결 하나를 위해 붙드는 응답 바이트. | 64 KiB |
| `head_timeout_ms` | 요청의 첫 바이트부터 헤드의 끝까지. TLS에서는 핸드셰이크도 포함한다. | 10초 |
| `body_timeout_ms` | 요청 본문의 조각과 조각 사이. | 30초 |
| `write_timeout_ms` | 붙든 출력을 클라이언트가 하나도 받지 않는 시간. | 30초 |
| `idle_timeout_ms` | 요청이 없는 열린 연결. | 60초 |
| `tls` | 인증서가 있는 TLS 설정, 또는 평문 HTTP를 뜻하는 널. | 널 |

서버가 서비스하지 않을 요청은 `on_request`에 닿지 않는다. 답을 받고 연결이 닫힌다:

| 상태 | 무엇에 |
|---|---|
| 400 | HTTP가 아닌 헤드. `Host`가 정확히 하나가 아닌 HTTP/1.1 요청. 스스로 모순되는 본문 프레이밍. 청크가 아닌 청크. |
| 408 | 시작되었으나 `head_timeout_ms` 안에 끝나지 않은 헤드. |
| 413 | `max_body_bytes`보다 긴 본문. |
| 431 | `max_head_bytes`보다 큰 헤드, 또는 `max_headers`보다 많은 필드. |
| 501 | `chunked`가 아닌 전송 코딩. |
| 505 | HTTP/1.0도 HTTP/1.1도 아닌 버전. |

이런 일이 `on_request` 뒤에 일어나면 - 너무 긴 것으로 드러나는 본문, 도중에 잘못되는 본문 - 교환은
`on_done`과 그 이유로 끝나고, 응답이 아직 시작되지 않았다면 클라이언트는 그 상태를 받는다.
`body_timeout_ms`, `write_timeout_ms`, `idle_timeout_ms`를 넘긴 연결은 닫힌다. 그 위에서 진행 중이던
교환은 `on_done(PROVEN_ERR_TIMEOUT)`으로 끝난다. 사라진 클라이언트는 자기 교환을 `PROVEN_ERR_RESET`으로
끝낸다.

기다리지 않고 한 연결에 잇달아 보낸 요청 - 파이프라이닝 - 은 순서대로, 한 번에 스트림 하나씩
서비스된다.

## 10. 스레드, TLS, 종료

**스레드.** 루프 하나는 스레드 하나이고, 서버는 그 스레드에 속한다. 계산하거나 무언가를 기다려야 하는
핸들러는 일을 제출하고, 돌아오고, 그 일이 답을 게시해 돌려보낸다:

1. `on_request`가 헤드에서 필요한 것을 복사하고, 스트림을 기억하고, 일을 제출한다.
2. 일은 다른 스레드에서, 서버의 것은 아무것도 건드리지 않고 돈다.
3. 일은 `proven_loop_post`로, 루프의 스레드에서 `proven_http_stream_respond`를 부를 함수를 게시한다.
4. `on_done`이 먼저 왔다면 - 클라이언트가 떠났다 - 그 함수는 그것을 알아채고 아무것도 하지 않아야
   한다. 흔한 방법은 `on_done`이 교환 자신의 상태에 끝났다고 표시하고, 그 해제는 둘 중 나중에 도는
   쪽에 맡기는 것이다.

**TLS.** 설정에 인증서가 있는 `proven_tls_config_t`([14장](manual-14-tls-ko.md) 2절)를 주면 모든
연결이 TLS다. 여러분의 코드에서 다른 것은 바뀌지 않는다. 핸드셰이크는 다른 읽기와 쓰기처럼 루프가
나르고 `head_timeout_ms`의 적용을 받는다. 설정은 서버보다 오래 살아야 한다. 핸드셰이크의 공개 키
연산은 루프의 스레드에서 이루어지고, 그동안 다른 것은 서비스되지 않는다: 하나의 비용은 14장 9절에
있다.

**종료.** `proven_http_event_server_stop_listening`은 이미 연결된 누구도 거절하지 않고 새로운 누구도
받아들이지 않는다. `proven_http_event_server_connections`가 줄어들 때까지, 또는 기다릴 만큼 기다린
다음 `proven_http_event_server_destroy`, 그다음 루프를 멈추고 해제한다. 서버는 루프의 스레드에서,
또는 루프가 멈춘 뒤에 해제한다 - 다른 스레드가 `proven_loop_run` 안에 있는 동안은 아니다.

## 11. 루프 위의 WebSocket

[12장](manual-12-websocket-ko.md)의 WebSocket 연결은 기다리는 호출을 가지며, 그래서 열려 있는 동안
스레드 하나를 차지한다. `ws_event.h`는 다른 형태다: 이 장의 서버로 온 요청이 `on_request` 안에서
WebSocket이 되고, 그때부터 루프가 여러분의 함수에게 무엇이 도착했는지 알려 준다. 조용한 연결은 자기
상태를 붙들 뿐 버퍼는 붙들지 않는다.

```c
proven_err_t proven_ws_event_accept(proven_http_stream_t *stream, const proven_http_request_t *head,
                                    const proven_ws_event_config_t *config, proven_ws_stream_t **out);
proven_err_t proven_ws_stream_send(proven_ws_stream_t *ws, bool text, proven_mem_view_t data);
proven_err_t proven_ws_stream_send_piece(proven_ws_stream_t *ws, bool text, proven_mem_view_t data, bool first, bool last);
proven_err_t proven_ws_stream_ping(proven_ws_stream_t *ws, proven_mem_view_t data);
void proven_ws_stream_close(proven_ws_stream_t *ws, proven_u16 code, proven_u8str_view_t reason);
void proven_ws_stream_abort(proven_ws_stream_t *ws);
void proven_ws_stream_pause(proven_ws_stream_t *ws);
void proven_ws_stream_resume(proven_ws_stream_t *ws);
void proven_ws_stream_set_user(proven_ws_stream_t *ws, void *user);
void *proven_ws_stream_user(const proven_ws_stream_t *ws);
proven_net_addr_t proven_ws_stream_peer(const proven_ws_stream_t *ws);
proven_size_t proven_ws_stream_buffered(const proven_ws_stream_t *ws);
```

**받아들이기.** `proven_ws_event_accept`는 `head`가 유효한 동안, `on_request` 안에서 부른다. 요청을
검사하고, 101로 답하고, 연결을 WebSocket으로 바꾼다. **HTTP 교환은 그 호출 안에서 끝난다**: 그
스트림에 대해 `on_done(PROVEN_OK)`가 불리고 스트림은 끝나며, `on_closed`가 돌아올 때까지 여러분이
쥐는 것은 `*out`이다.

| 반환 | 뜻 | 핸들러가 할 일 |
|---|---|---|
| `PROVEN_OK` | 연결이 WebSocket이 되었다. | `*out`에 상태를 붙이고 돌아온다. |
| `PROVEN_ERR_NOT_FOUND` | 요청이 WebSocket을 청하지 않았다. 아무것도 보내지 않았다. | 보통 요청으로서 답한다. |
| `PROVEN_ERR_UNSUPPORTED` | 13이 아닌 버전의 업그레이드. 아무것도 보내지 않았다. | `Sec-WebSocket-Version: 13`과 함께 426으로 답한다. |
| `PROVEN_ERR_INVALID_FORMAT` | 업그레이드이지만 형식이 잘못되었다. 아무것도 보내지 않았다. | 400으로 답한다. |
| `PROVEN_ERR_INVALID_ARG` | `on_message`가 없거나, 토큰이 아닌 서브프로토콜. | 프로그램의 결함이다. |
| `PROVEN_ERR_INVALID_STATE` | 이 스트림의 `on_request` 안이 아니거나, 응답이 이미 시작되었다. | 프로그램의 결함이다. |
| `PROVEN_ERR_NOMEM` | | 503으로 답하거나 abort한다. |
| `PROVEN_ERR_RESET` | 바로 그 순간 클라이언트가 사라졌다. `on_done`은 이미 불렸다. | 없음: 스트림은 끝났다. |

`proven_ws_event_config_t`는 0으로 초기화하고, `on.on_message`를 주고, 그 밖에 필요한 것을 준다:

| 필드 | 뜻 | 기본값 |
|---|---|---|
| `on.on_message(ctx, ws, text, piece, first, last)` | 메시지의 한 조각. 필수. | |
| `on.on_writable(ctx, ws)` | 거절되었던 보내기가 이제 받아들여진다. | 없음 |
| `on.on_closed(ctx, ws, code, why)` | 연결이 끝났다. 정확히 한 번 불린다. | 없음 |
| `ctx` | 모든 콜백에 전달된다. | |
| `subprotocol` | 비어 있지 않으면 답에 적힌다. 클라이언트가 제안했을 때에만 적는다(`proven_ws_request_offers`). | 없음 |
| `max_message_bytes` | 받아들이는 가장 큰 메시지, 그 조각들을 합쳐서. | 1 MiB |
| `max_buffered_output` | 보내기가 거절되기 전까지 붙드는 출력. | 64 KiB |
| `ping_interval_ms` | 클라이언트가 이만큼 조용하면 ping을 보낸다. | 30초 |
| `pong_timeout_ms` | 그 뒤로도 이만큼 조용하면 연결을 끝낸다. | 10초 |
| `close_timeout_ms` | close에 대한 답을 기다리는 시간. 클라이언트가 마지막으로 출력을 가져간 때부터 센다. | 5초 |

**메시지는 조각으로 도착한다.** `on_message`는 각 메시지를 네트워크가 전달한 조각대로 받고,
`first`와 `last`가 그 양 끝을 표시한다: 어떤 크기의 메시지든 조립되거나 붙들리지 않고 지나가며,
`piece`는 함수가 돌아올 때까지만 유효하다. 텍스트는 메시지 전체에 걸쳐 UTF-8인지 검사되고, 조각은
문자 중간에서 끝날 수 있다. 온전한 메시지를 원하는 핸들러는 스스로 정한 한도까지 조각을 직접
모은다 - 아래 프로그램이 그렇게 한다. `max_message_bytes`는 서버의 한도이지 버퍼가 아니다: 그것을
넘는 메시지는 close 코드 1009로 거절된다.

**보내기는 통째로 받아들여지거나 전혀 받아들여지지 않는다.** `proven_ws_stream_send`는 붙든 것이
`max_buffered_output`보다 적을 때 메시지 하나를 큐에 넣고, 그렇지 않으면 아무것도 받지 않고
`PROVEN_ERR_AGAIN`을 돌려준다: 메시지를 간직하고, `on_writable`이 불리면 다시 보낸다. 이것이 7절의
HTTP 본문과 다른 점이다. 거기의 쓰기는 몇 바이트를 받았는지 말하지만, 프레임은 라이브러리가 여러분의
메시지를 어디서 자를지 정하지 않고서는 반만 보낼 수 없다. 그러므로 연결 하나가 붙드는 최대는 한도에
여러분이 고른 메시지 하나를 더한 것이다. 만들어 가며 보내는 큰 메시지는
`proven_ws_stream_send_piece`가 같은 규칙으로 한 번에 한 프래그먼트씩 보낸다. 되돌려 보내기만 하는
핸들러는 기다리는 동안 `proven_ws_stream_pause`로 읽기를 멈출 수 있고, 그러면 읽는 것보다 빨리 보내는
클라이언트는 버퍼에 쌓이는 대신 느려진다.

| 호출 | 반환 |
|---|---|
| `proven_ws_stream_send`, `proven_ws_stream_send_piece` | `proven_err_t`: 출력 한도에 닿았으면 `AGAIN`, 아무것도 받지 않는다. close를 보낸 뒤이거나, 메시지 안에서의 `first`이거나, 메시지 밖에서 `first`가 아닌 조각이면 `INVALID_STATE`. 연결이 사라졌으면 `RESET`. |
| `proven_ws_stream_ping` | 같다. 125바이트를 넘으면 `OUT_OF_BOUNDS`. |
| `proven_ws_stream_close` | 없음. `code`와 `reason`으로 close를 보내고, 클라이언트가 답하거나 close 타임아웃이 지나면 연결을 끝낸다. `on_closed`는 나중에 따른다. 보낼 수 없는 코드는 1000이 된다. |
| `proven_ws_stream_abort` | 없음. close 프레임 없이 곧바로 연결을 끝낸다. `on_closed(1006, PROVEN_ERR_RESET)`이 호출 안에서 불린다. |
| `proven_ws_stream_pause`, `proven_ws_stream_resume` | 없음. 메시지 전달과 연결 읽기를 멈추고 다시 시작한다. |
| `proven_ws_stream_buffered` | `proven_size_t`: 클라이언트를 위해 큐에 있고 아직 받아 가지 않은 바이트. |

**어떻게 끝나는가.** `on_closed`는 정확히 한 번 불리고, 받는 값이 경위를 말한다:

| `code` | `why` | 일어난 일 |
|---|---|---|
| 클라이언트의 코드 (close에 코드가 없었으면 1005) | `PROVEN_OK` | 클라이언트가 닫았고, 답을 받았다. |
| 이쪽이 보낸 코드 | `PROVEN_OK` | 이쪽이 닫았고, 클라이언트가 답했다. |
| 이쪽이 보낸 코드 | `PROVEN_ERR_TIMEOUT` | 이쪽이 닫았고, 클라이언트는 끝내 답하지 않았다. |
| 1002, 1007, 1009 | `PROVEN_ERR_INVALID_FORMAT`, `PROVEN_ERR_INVALID_ENCODING`, `PROVEN_ERR_OUT_OF_BOUNDS` | 클라이언트가 프로토콜을 어겼거나, UTF-8이 아닌 텍스트를 보냈거나, 한도를 넘는 메시지를 보냈다. 그 코드를 받았다. |
| 1006 | `PROVEN_ERR_TIMEOUT` | 두 생존 한도 안에 데이터로도 ping에도 답하지 않았다. |
| 1006 | `PROVEN_ERR_RESET` | close 프레임 없이 사라졌다. 또는 `proven_ws_stream_abort`. 또는 서버가 해제되었다. |

클라이언트의 ping에는 대신 답해 주고, 그 pong은 여러분에게 아무것도 요구하지 않는다. 생존은 받는
것으로 판단한다: 데이터를 받기만 하고 읽지 않는 클라이언트는 그것만으로는 발견되지 않는다 - 다만 그
클라이언트를 위해 출력 한도 이상은 큐에 쌓이지 않고, ping에 답하기를 멈추면 끊긴다.

테스트 스위트가 이 프로그램을 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_15_ws_event.c -->
```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 이벤트 구동 서버 위의 WebSocket: 스레드를 쓰지 않는 연결.
 *
 * 요청은 on_request 안에서 WebSocket이 된다. 그때부터 루프가 이 함수들에게 무엇이 도착했는지
 * 알려 주고, 함수들은 곧바로 돌아온다. 이 서버는 되받아 외친다: 텍스트 메시지마다 대문자로
 * 답한다.
 *
 * 클라이언트는 12장의 블로킹 클라이언트이고, 예제의 메인 스레드에서 돈다.
 */

typedef struct {
    proven_loop_t *loop;
    int opened, closed;
    proven_u16 last_code;
} app_t;

/* 연결 하나에 대해 이 프로그램이 간직하는 것: 모으고 있는 메시지. */
typedef struct {
    app_t *app;
    char text[4096];
    proven_size_t len;
    bool waiting;                 /* 서버가 아직 받아 주지 않은 답 */
} peer_t;

/* 답을 보낸다. 클라이언트가 앞서 보낸 것을 가져가지 않고 있으면 서버가 거절한다:
 * 답을 간직하고, 이 클라이언트에게서 읽기를 멈추고, on_writable이 알려 줄 때 다시 시도한다. */
static void reply(proven_ws_stream_t *ws, peer_t *peer) {
    proven_err_t err = proven_ws_stream_send(ws, true, (proven_mem_view_t){ (const proven_byte_t *)peer->text, peer->len });
    if (err == PROVEN_ERR_AGAIN) {
        peer->waiting = true;
        proven_ws_stream_pause(ws);
        return;
    }
    peer->waiting = false;
    peer->len = 0;
}

/* 메시지는 네트워크가 전달한 조각대로 도착한다. 이 핸들러는 온전한 메시지를 원하므로
 * 직접 모은다 - 스스로 정한 한도까지. 손으로 하는 까닭이 바로 그 한도다. */
static void on_message(void *ctx, proven_ws_stream_t *ws, bool text, proven_mem_view_t piece, bool first, bool last) {
    (void)ctx;
    peer_t *peer = proven_ws_stream_user(ws);
    if (first) peer->len = 0;
    if (!text || peer->len + piece.size > sizeof peer->text) {
        proven_ws_stream_close(ws, 1009, PROVEN_LIT("short text only"));       /* 1009: 너무 크다 */
        return;
    }
    for (proven_size_t i = 0; i < piece.size; ++i) {
        proven_byte_t ch = piece.ptr[i];
        peer->text[peer->len++] = (char)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
    }
    if (last) reply(ws, peer);
}

/* 다시 자리가 생겼다. */
static void on_writable(void *ctx, proven_ws_stream_t *ws) {
    (void)ctx;
    peer_t *peer = proven_ws_stream_user(ws);
    if (!peer->waiting) return;
    reply(ws, peer);
    if (!peer->waiting) proven_ws_stream_resume(ws);
}

/* 연결이 어떻게 끝났든 한 번 불린다. 붙여 둔 것을 해제할 자리. */
static void on_closed(void *ctx, proven_ws_stream_t *ws, proven_u16 code, proven_err_t why) {
    app_t *app = ctx;
    (void)why;
    free(proven_ws_stream_user(ws));
    app->last_code = code;
    app->closed++;
}

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    app_t *app = ctx;
    peer_t *peer = calloc(1, sizeof *peer);
    if (!peer) { proven_http_stream_abort(stream); return; }
    peer->app = app;

    proven_ws_event_config_t config = {
        .on = { on_message, on_writable, on_closed },
        .ctx = app,
        .max_message_bytes = sizeof peer->text,
    };
    proven_ws_stream_t *ws = NULL;
    /* 요청을 검사하고, 101로 답하고, 연결을 WebSocket으로 바꾼다. HTTP 교환은 이 호출
     * 안에서 끝난다(on_done이 있으면 불린다). */
    proven_err_t err = proven_ws_event_accept(stream, head, &config, &ws);
    if (err != PROVEN_OK) {
        free(peer);
        /* 아무것도 보내지 않았다: 요청은 여전히 보통 요청이고, 그렇게 답한다. */
        if (err == PROVEN_ERR_UNSUPPORTED) {
            proven_http_header_t version = { PROVEN_LIT("Sec-WebSocket-Version"), PROVEN_LIT("13") };
            (void)proven_http_stream_respond(stream, 426, &version, 1, (proven_mem_view_t){ 0 });
        } else if (err != PROVEN_ERR_RESET) {
            (void)proven_http_stream_respond(stream, err == PROVEN_ERR_NOT_FOUND ? 404 : 400, NULL, 0, (proven_mem_view_t){ 0 });
        }
        return;
    }
    /* `stream`은 이제 끝났다. `ws`는 on_closed가 돌아올 때까지 유효하다. */
    proven_ws_stream_set_user(ws, peer);
    app->opened++;
}

static void run(void *arg) { (void)proven_loop_run(((app_t *)arg)->loop); }

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    app_t app = { 0 };

    // ---- 루프 위의 서버, 자기 스레드에서 --------------------------------------------
    EXAMPLE_REQUIRE(proven_loop_create(heap, &app.loop) == PROVEN_OK, "루프");
    proven_http_event_server_config_t config = { .on = { .on_request = on_request }, .ctx = &app };
    proven_http_event_server_t *server = NULL;
    proven_net_addr_t at;
    EXAMPLE_REQUIRE(proven_http_event_server_create(app.loop, &config, &server) == PROVEN_OK &&
                    proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "빈 포트의 이벤트 구동 서버");
    proven_job_sys_t *threads = NULL;
    proven_job_group_t running;
    proven_job_group_init(&running);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &threads) == PROVEN_OK &&
                    proven_job_group_submit(threads, &running, run, &app) == PROVEN_OK, "루프가 다른 스레드에서 돈다");

    // ---- 클라이언트 ----------------------------------------------------------------
    /* 반대쪽 끝을 보이는 가장 간단한 방법은 블로킹 클라이언트다. */
    proven_http_client_config_t client_config = { .alloc = heap };
    proven_http_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&client_config, &client) == PROVEN_OK, "HTTP 클라이언트");
    char url[64];
    int n = snprintf(url, sizeof url, "ws://127.0.0.1:%u/shout", (unsigned)at.port);
    proven_ws_conn_config_t ws_config = { .alloc = heap };
    proven_ws_conn_t *conn = NULL;
    EXAMPLE_REQUIRE(proven_ws_conn_connect(client, (proven_u8str_view_t){ (const proven_byte_t *)url, (proven_size_t)n }, NULL, 0, PROVEN_LIT(""),
                                           &ws_config, &conn, NULL) == PROVEN_OK, "서버가 WebSocket을 받아들인다");

    proven_ws_message_t msg;
    EXAMPLE_REQUIRE(proven_ws_conn_send_text(conn, PROVEN_LIT("hello, loop")) == PROVEN_OK &&
                    proven_ws_conn_receive(conn, proven_net_deadline_in(5000), &msg) == PROVEN_OK &&
                    msg.text && msg.data.size == 11 && memcmp(msg.data.ptr, "HELLO, LOOP", 11) == 0, "메시지가 대문자로 돌아온다");
    /* 클라이언트가 메시지를 어떻게 자르든 핸들러는 메시지 하나를 본다. */
    EXAMPLE_REQUIRE(proven_ws_conn_send_part(conn, true, proven_mem_view_from_u8(PROVEN_LIT("in three ")), false) == PROVEN_OK &&
                    proven_ws_conn_send_part(conn, true, proven_mem_view_from_u8(PROVEN_LIT("pieces, ")), false) == PROVEN_OK &&
                    proven_ws_conn_send_part(conn, true, proven_mem_view_from_u8(PROVEN_LIT("one answer")), true) == PROVEN_OK &&
                    proven_ws_conn_receive(conn, proven_net_deadline_in(5000), &msg) == PROVEN_OK &&
                    msg.data.size == 27 && memcmp(msg.data.ptr, "IN THREE PIECES, ONE ANSWER", 27) == 0, "조각 셋이 핸들러에게는 메시지 하나다");

    // ---- 닫기 ----------------------------------------------------------------------
    EXAMPLE_REQUIRE(proven_ws_conn_close(conn, 1000, PROVEN_LIT("done")) == PROVEN_OK, "클라이언트의 close에 답이 온다");
    proven_ws_conn_destroy(conn);
    proven_http_client_destroy(client);

    /* 루프의 스레드가 쓴 것을 보기 전에 루프를 멈춘다. */
    proven_loop_stop(app.loop);
    proven_job_group_wait(threads, &running);
    EXAMPLE_REQUIRE(app.opened == 1 && app.closed == 1 && app.last_code == 1000, "연결 하나가 열렸고, 클라이언트의 코드로 한 번 닫혔다");
    proven_http_event_server_destroy(server);
    proven_loop_destroy(app.loop);
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    return EXAMPLE_OK();
}
```

## 12. 이벤트 구동 클라이언트

```c
proven_err_t proven_http_event_client_create(proven_loop_t *loop, const proven_http_event_client_config_t *config,
                                             proven_http_event_client_t **out);
void proven_http_event_client_destroy(proven_http_event_client_t *client);
proven_size_t proven_http_event_client_requests(const proven_http_event_client_t *client);
proven_err_t proven_http_event_client_start(proven_http_event_client_t *client, const proven_http_event_request_options_t *options,
                                            proven_http_event_request_t **out);
proven_result_size_t proven_http_event_request_write(proven_http_event_request_t *request, proven_mem_view_t data);
proven_err_t proven_http_event_request_end(proven_http_event_request_t *request);
void proven_http_event_request_abort(proven_http_event_request_t *request);
void proven_http_event_request_pause(proven_http_event_request_t *request);
void proven_http_event_request_resume(proven_http_event_request_t *request);
void proven_http_event_request_set_user(proven_http_event_request_t *request, void *user);
void *proven_http_event_request_user(const proven_http_event_request_t *request);
```

[11장](manual-11-http-client-server-ko.md)의 클라이언트는 요청을 보내고 답을 기다린다. 이
클라이언트는 요청을 시작하고 돌아온다. 응답 헤드, 본문 조각, 끝은 루프의 스레드에서 콜백으로
도착하고, 요청은 몇 개든 동시에 진행된다.

**의도적으로 좁다. 그리고 두 가지 한계는 다른 무엇보다 먼저 알아 둘 만하다.**

- **요청 하나에 연결 하나, 끝나면 닫힌다.** 연결 재사용이 없고, 리다이렉트, 인증 요구에 대한 답,
  쿠키, 프록시도 없다. 11장의 클라이언트에는 모두 있다. 진행 중인 요청마다 스레드 하나를 쓸 수 있는
  곳에서는 그것을 쓴다.
- **이름을 해석하지 않는다.** 이름 조회는 몇 초가 걸릴 수 있고, 루프 위의 무엇도 기다려서는 안 된다.
  요청은 `address`로 어디에 연결할지 말한다. URL의 호스트가 IP 주소일 때만 필요 없다. 이름이라면:
  일(job)에서 `proven_net_resolve`를 부르고, 결과를 루프에 게시하고, 거기서 요청을 시작한다 - 이 절
  끝의 프로그램이 그렇게 한다. URL은 이름을 그대로 둔다 - `Host`에 들어가는 것이 그것이고, TLS에서는
  서버의 인증서가 위해야 하는 이름이다.

요청은 `proven_http_event_request_options_t`로 기술한다. 그것이 가리키는 모든 것은
`proven_http_event_client_start`가 돌아오기 전에 복사되거나 쓰인다:

| 필드 | 뜻 |
|---|---|
| `method` | 비어 있으면 `GET`. `CONNECT`는 거절된다. |
| `url` | 절대 URL, `http` 또는 `https`, 자격 증명이 들어 있지 않은 것. |
| `address` | 연결할 곳. 그 포트를 그대로 쓴다. URL의 호스트가 IP 주소일 때에만 널. |
| `headers`, `header_count` | 여러분의 헤더. `Host`, `Content-Length`, `Transfer-Encoding`, `Connection`은 대신 써 주며 여기 있어서는 안 된다. |
| `body` | 메모리에 든 본문, 통째로 보낸다. |
| `body_length` | `body`가 없을 때: 0이면 본문 없음. 길이를 주면 나중에 쓰는 본문. `PROVEN_HTTP_EVENT_LENGTH_UNKNOWN`이면 chunked로 쓴다. |
| `on.on_response(ctx, request, head)` | 응답 헤드. 선택. 중간 응답(100, 103)은 지나치고 알리지 않는다. |
| `on.on_body(ctx, request, piece, last)` | 본문 한 조각. 마지막 조각에서 `last`이고 그 조각은 비어 있을 수 있다. 본문 없는 응답은 호출을 받지 않는다. 선택. |
| `on.on_writable(ctx, request)` | 보류되었던 요청 본문 바이트가 나갔다. 선택. |
| `on.on_done(ctx, request, why)` | 끝. **필수**이고, 시작된 요청마다 정확히 한 번 불린다. |
| `ctx` | 모든 콜백에 전달된다. |

**시작.** `proven_http_event_client_start`가 오류를 돌려주면 요청은 시작되지 않았고 콜백은 지금도
나중에도 불리지 않는다. `PROVEN_OK`를 돌려주면 `on_done`이 정확히 한 번 불린다 - **그 호출 안에서는
결코 불리지 않으므로**, 요청을 시작하는 코드는 그 요청에 무슨 일이 일어나기 전에 하던 일을 마칠 수
있다. 요청은 시작부터 `on_done`이 돌아올 때까지 유효하다.

| `start`의 반환 | 무엇에 |
|---|---|
| `PROVEN_ERR_INVALID_ARG` | `on_done`이 없다. 절대 `http`/`https`가 아니거나 자격 증명을 실은 URL. TLS 설정이 없는 `https` - 거절되며 평문으로 보내지 않는다. `address` 없는 이름. 클라이언트가 직접 쓰는 헤더. 토큰이 아닌 메서드, 또는 `CONNECT`. |
| `PROVEN_ERR_OUT_OF_BOUNDS` | `max_head_bytes`에 들어가지 않는 요청 헤드. |
| `PROVEN_ERR_NOMEM` | |
| `PROVEN_ERR_REFUSED`, `PROVEN_ERR_UNREACHABLE` | 연결이 곧바로 실패했다. 나중에 실패할 수도 있고, 그때는 `on_done`이 알린다. |

**어떻게 끝나는가.** `on_done`이 말한다:

| `why` | 일어난 일 |
|---|---|
| `PROVEN_OK` | 응답 전체가 도착했다 - 상태가 무엇이든: 404도 응답이다. |
| `PROVEN_ERR_REFUSED`, `PROVEN_ERR_UNREACHABLE` | 아무것도 듣고 있지 않았거나 경로가 없었다. |
| `PROVEN_ERR_TIMEOUT` | 네 시간 한도 가운데 하나가 지났다. |
| `PROVEN_ERR_RESET` | 응답이 온전해지기 전에 서버가 닫았거나 사라졌다 - 길이에 못 미쳐 잘린 본문은 결코 완전하다고 보고되지 않는다. 또는 `proven_http_event_request_abort`. 또는 클라이언트가 해제되었다. |
| `PROVEN_ERR_INVALID_FORMAT` | 답이 HTTP가 아니었거나, 프레이밍이 스스로 모순되었거나, 중간 응답이 여덟 번 넘게 잇달아 왔다. |
| `PROVEN_ERR_OUT_OF_BOUNDS` | 헤드가 `max_head_bytes`보다 컸거나 본문이 `max_body_bytes`보다 컸다. |
| `PROVEN_ERR_UNSUPPORTED` | 서버가 101로 답했다: 이 클라이언트는 프로토콜 변경을 청하지 않는다. |
| `PROVEN_ERR_UNTRUSTED`, `PROVEN_ERR_NAME_MISMATCH`, `PROVEN_ERR_EXPIRED`, ... | TLS가 서버를 거절했다([14장](manual-14-tls-ko.md) 8절). 요청은 보내지지 않았다. |

**만들어 가며 쓰는 요청 본문**은 7절의 규칙을 따른다. `proven_http_event_request_write`는
`max_buffered_output` 아래에 들어가는 만큼, 어쩌면 하나도 받지 않고, 몇 바이트를 받았는지 말한다.
나머지는 `on_writable`이 불리면 다시 내놓는다. `start` 직후, 연결이 생기기 전에 불러도 된다: 받은 것은
기다린다. `proven_http_event_request_end`가 본문을 끝낸다. 약속한 길이에 못 미쳐 끝내면 요청은
`PROVEN_ERR_INVALID_FORMAT`으로 끝난다.

| 호출 | 반환 |
|---|---|
| `proven_http_event_client_create(loop, &config, &client)` | `proven_err_t`: 루프가 없으면 `INVALID_ARG`. `NOMEM`. `config`는 널이어도 된다. |
| `proven_http_event_client_requests(client)` | `proven_size_t`: 진행 중인 요청 수. |
| `proven_http_event_client_destroy(client)` | 없음. 진행 중인 요청마다 `on_done(PROVEN_ERR_RESET)`을 받는다. 널을 받는다. |
| `proven_http_event_request_write(request, data)` | `proven_result_size_t`: 받은 바이트 수. 쓸 본문이 없는 요청이거나 끝낸 뒤면 `INVALID_STATE`. 약속한 길이를 넘으면 `OUT_OF_BOUNDS`. |
| `proven_http_event_request_end(request)` | `proven_err_t`: 끝낼 것이 없었으면 `INVALID_STATE`. 약속보다 적게 썼으면 `INVALID_FORMAT` - 그때 `on_done`이 호출 안에서 불린다. |
| `proven_http_event_request_abort(request)` | 없음. `on_done(PROVEN_ERR_RESET)`이 호출 안에서 불린다. |
| `proven_http_event_request_pause`, `proven_http_event_request_resume` | 없음. 응답 본문 전달을 멈추고 다시 시작한다. |

설정의 필드. 0이면 각각 기본값이 쓰인다:

| 필드 | 뜻 | 기본값 |
|---|---|---|
| `alloc` | 요청과 버퍼를 위한 할당자. | 루프의 것 |
| `tls` | 서버를 검증할 수 있는 TLS 설정, `https`용. 클라이언트보다 오래 살아야 한다. | 없음: `https`는 거절된다 |
| `max_head_bytes` | 가장 큰 응답 헤드, 그리고 요청 헤드. | 16 KiB |
| `max_headers` | 응답 하나의 헤더 필드 최대 수. | 64 |
| `max_body_bytes` | 가장 큰 응답 본문. | 한도 없음: 조각으로 전달되고 붙들지 않는다 |
| `max_buffered_output` | 쓰기가 거절되기 전까지 붙드는 요청 본문 바이트. | 64 KiB |
| `connect_timeout_ms` | 연결되기까지, TLS 핸드셰이크 포함. | 10초 |
| `response_timeout_ms` | 요청을 다 보낸 때부터 응답 헤드의 끝까지. | 30초 |
| `body_timeout_ms` | 응답 본문의 조각과 조각 사이. | 30초 |
| `write_timeout_ms` | 붙든 출력을 서버가 하나도 받지 않는 시간. | 30초 |

테스트 스위트가 이 프로그램을 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_15_http_event_client.c -->
```c
#include <stdio.h>
#include <string.h>

/*
 * 이벤트 구동 HTTP 클라이언트: 한 스레드에서 동시에 진행되는 여러 요청.
 *
 * 요청을 시작하면 호출은 곧바로 돌아온다. 그 요청에 일어나는 일은 루프의 스레드에서 콜백으로
 * 도착한다. 여기의 서버는 이 장의 이벤트 구동 서버이고 같은 루프 위에 있다 - 이 프로그램
 * 전체가 스레드 하나, 그리고 기다리는 단 하나의 일을 위한 워커 하나다.
 *
 * 그 하나의 일은 이름을 찾는 것이다. 클라이언트는 이름을 해석하지 않는다: 조회는 몇 초가 걸릴
 * 수 있고, 루프 위의 무엇도 기다려서는 안 된다. 그래서 세 번째 요청이 그 패턴을 보인다 -
 * 워커에서 해석하고, 답을 게시해 돌려보내고, 요청을 시작한다.
 */

typedef struct app app_t;

/* 요청 하나에 대해 이 프로그램이 간직하는 것. */
typedef struct {
    app_t *app;
    const char *path;
    int status;
    char body[64];
    proven_size_t len;
    proven_err_t why;
    bool done;
} fetch_t;

struct app {
    proven_loop_t *loop;
    proven_http_event_client_t *client;
    proven_job_sys_t *workers;
    proven_u16 port;
    fetch_t fetch[3];
    int finished;
    /* 워커가 쓰고, 게시된 뒤 루프에서 읽는다 */
    proven_net_addr_t found;
    proven_err_t lookup;
};

// ---- 요청이 가는 서버 --------------------------------------------------------

static void serve(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    (void)ctx;
    if (proven_u8str_view_eq(head->target, PROVEN_LIT("/a"))) (void)proven_http_stream_respond(stream, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("first")));
    else if (proven_u8str_view_eq(head->target, PROVEN_LIT("/b"))) (void)proven_http_stream_respond(stream, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("second")));
    else (void)proven_http_stream_respond(stream, 404, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("nothing here")));
}

// ---- 클라이언트의 콜백 -------------------------------------------------------

/* 헤드: 이 함수가 돌아올 때까지만 유효하므로, 필요한 것을 여기서 꺼낸다. */
static void on_response(void *ctx, proven_http_event_request_t *request, const proven_http_response_t *head) {
    (void)request;
    ((fetch_t *)ctx)->status = head->status;
}

/* 본문, 한 번에 한 조각. 대신 모아 주지 않는다. 원하는 것을 직접 모은다. */
static void on_body(void *ctx, proven_http_event_request_t *request, proven_mem_view_t piece, bool last) {
    (void)request; (void)last;
    fetch_t *f = ctx;
    for (proven_size_t i = 0; i < piece.size && f->len < sizeof f->body - 1; ++i) f->body[f->len++] = (char)piece.ptr[i];
}

/* 시작된 요청마다 한 번, 무슨 일이 있었든. */
static void on_done(void *ctx, proven_http_event_request_t *request, proven_err_t why) {
    (void)request;
    fetch_t *f = ctx;
    f->why = why;
    f->done = true;
    if (++f->app->finished == 3) proven_loop_stop(f->app->loop);
}

static proven_err_t fetch(app_t *app, fetch_t *f, const char *path, const proven_net_addr_t *address, const char *host) {
    char url[96];
    int n = snprintf(url, sizeof url, "http://%s:%u%s", host, (unsigned)app->port, path);
    f->app = app;
    f->path = path;
    proven_http_event_request_options_t options = {
        .url = { (const proven_byte_t *)url, (proven_size_t)n },
        .address = address,                               /* URL의 호스트가 IP 주소일 때에만 널을 쓸 수 있다 */
        .on = { on_response, on_body, NULL, on_done },
        .ctx = f,
    };
    return proven_http_event_client_start(app->client, &options, NULL);     /* 곧바로 돌아온다 */
}

// ---- 다른 곳에서 해석한 이름 --------------------------------------------------

/* 다시 루프의 스레드: 이제 주소가 있으니 요청을 시작할 수 있다.
 * URL은 이름을 그대로 둔다 - Host에 들어가는 것이 그것이다 - 그리고 주소가 어디로 연결할지 말한다. */
static void resolved(void *ctx) {
    app_t *app = ctx;
    if (app->lookup != PROVEN_OK || fetch(app, &app->fetch[2], "/missing", &app->found, "localhost") != PROVEN_OK) {
        app->fetch[2].why = PROVEN_ERR_NOT_FOUND;
        if (++app->finished == 3) proven_loop_stop(app->loop);
    }
}

/* 워커 스레드에서: 이 호출은 리졸버가 걸리는 만큼 기다려도 된다. */
static void resolve(void *ctx) {
    app_t *app = ctx;
    proven_net_addr_t all[8];
    proven_size_t count = 0;
    app->lookup = proven_net_resolve(PROVEN_LIT("localhost"), app->port, all, 8, &count);
    /* 여기의 서버는 IPv4로만 듣는다. 실제 프로그램이라면 주소를 차례로 시도할 것이다. */
    bool have = false;
    for (proven_size_t i = 0; i < count && !have; ++i) {
        if (all[i].family == PROVEN_NET_FAMILY_IPV4) { app->found = all[i]; have = true; }
    }
    if (app->lookup == PROVEN_OK && !have) app->lookup = PROVEN_ERR_NOT_FOUND;
    if (proven_loop_post(app->loop, resolved, app) != PROVEN_OK) proven_loop_stop(app->loop);
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    app_t app = { 0 };

    // ---- 루프 하나, 그 위의 서버와 클라이언트 --------------------------------------
    /* 아직 아무것도 돌지 않는다: 루프는 돌릴 때까지 아무 일도 하지 않는다. */
    EXAMPLE_REQUIRE(proven_loop_create(heap, &app.loop) == PROVEN_OK, "루프");
    proven_http_event_server_config_t server_config = { .on = { .on_request = serve } };
    proven_http_event_server_t *server = NULL;
    proven_net_addr_t at;
    EXAMPLE_REQUIRE(proven_http_event_server_create(app.loop, &server_config, &server) == PROVEN_OK &&
                    proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "물어볼 서버");
    app.port = at.port;
    EXAMPLE_REQUIRE(proven_http_event_client_create(app.loop, NULL, &app.client) == PROVEN_OK, "이벤트 구동 클라이언트");
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &app.workers) == PROVEN_OK, "조회를 맡을 워커 하나");

    // ---- 요청 셋 -------------------------------------------------------------------
    /* 둘은 주소로. 어느 것도 보내지기 전에 둘 다 시작된다. */
    EXAMPLE_REQUIRE(fetch(&app, &app.fetch[0], "/a", NULL, "127.0.0.1") == PROVEN_OK &&
                    fetch(&app, &app.fetch[1], "/b", NULL, "127.0.0.1") == PROVEN_OK, "요청 둘이 시작되었다");
    EXAMPLE_REQUIRE(proven_http_event_client_requests(app.client) == 2 && !app.fetch[0].done && !app.fetch[1].done, "둘 다 진행 중이고 답은 없다: 아직 아무것도 돌지 않았다");

    /* 하나는 이름으로. 주소가 없으면 거절된다 - 그러니 먼저 워커에서 해석한다. */
    fetch_t unresolved = { 0 };
    EXAMPLE_REQUIRE(fetch(&app, &unresolved, "/missing", NULL, "localhost") == PROVEN_ERR_INVALID_ARG, "주소 없는 이름은 거절된다");
    EXAMPLE_REQUIRE(proven_job_submit(app.workers, resolve, &app), "조회를 워커에 넘겼다");

    // ---- 실행 ----------------------------------------------------------------------
    /* 세 번째 요청이 끝나면 on_done이 루프를 멈춘다. */
    EXAMPLE_REQUIRE(proven_loop_run(app.loop) == PROVEN_OK, "셋이 모두 끝날 때까지 루프가 돌았다");

    EXAMPLE_REQUIRE(app.fetch[0].why == PROVEN_OK && app.fetch[0].status == 200 && strcmp(app.fetch[0].body, "first") == 0, "첫 번째 답");
    EXAMPLE_REQUIRE(app.fetch[1].why == PROVEN_OK && app.fetch[1].status == 200 && strcmp(app.fetch[1].body, "second") == 0, "두 번째 답");
    EXAMPLE_REQUIRE(app.fetch[2].why == PROVEN_OK && app.fetch[2].status == 404 && strcmp(app.fetch[2].body, "nothing here") == 0, "세 번째, 이름으로: 404는 오류가 아니라 응답이다");

    proven_job_system_close(app.workers);
    proven_job_system_destroy(app.workers);
    proven_http_event_client_destroy(app.client);
    proven_http_event_server_destroy(server);
    proven_loop_destroy(app.loop);
    return EXAMPLE_OK();
}
```

## 13. 여러 루프

루프 하나는 스레드 하나이고, 연결은 평생 루프 하나에 속한다: 그 연결의 어떤 것도 다른 스레드가
건드리지 않으며, 이 장의 무엇도 락을 잡지 않는 까닭이 그것이다. 그러므로 프로세서 코어가 둘 이상
필요한 서버는 **각자 서버를 가진 여러 루프**이고, 들어오는 연결을 그들 사이에 나눠 줄 무언가다.

```c
proven_err_t proven_http_event_server_adopt(proven_http_event_server_t *server, proven_net_conn_t *conn);
```

`proven_http_event_server_adopt`는 다른 곳에서 받아들인 연결을 서버에 준다. 그 연결은 서버가 직접
받아들인 것과 똑같이 서버의 것이 된다. TLS라면 핸드셰이크가 시작된다. 그래서 스레드 하나가 듣고
받아들이며, 연결마다 서버들에게 차례로 나눠 준다. 세 가지 규칙이 이것을 올바르게 만든다:

- **`adopt`는 서버의 루프 스레드에서 부른다** - 다른 모든 것처럼. 받아들이는 스레드는 그것을 부르지
  않는다: 그 루프에 함수를 게시하고(`proven_loop_post`), 그 함수가 부른다.
- **연결은 자기 메모리에 담겨 간다.** `proven_net_conn_t`는 열려 있는 동안 복사해서는 안 되므로,
  게시하는 것은 그것을 담은 작은 블록이고, adopt하는 함수가 해제한다.
- **해체는 순서대로:** 받아들이는 스레드가 먼저다. 그래야 더는 게시되지 않는다. 그다음 루프들. 그다음 -
  게시되었으나 아직 돌지 않은 것을 위해 각 루프를 마지막으로 한 바퀴 돌린 뒤 - 서버들.

| `adopt`의 반환 | 뜻 |
|---|---|
| `PROVEN_OK` | 서버가 연결을 소유한다. 호출자의 값은 더는 열려 있지 않다. |
| `PROVEN_ERR_BUSY` | 서버가 `max_connections`에 닿았다. 연결은 여전히 호출자의 것이고, 호출자가 닫는다. |
| `PROVEN_ERR_INVALID_STATE` | 연결이 열려 있지 않거나, 서버가 `proven_http_event_server_stop_listening`을 들었다. 여전히 호출자의 것. |
| `PROVEN_ERR_INVALID_ARG`, `PROVEN_ERR_NOMEM` | 여전히 호출자의 것. |

같은 목적에 이르는 다른 길은 호출이 전혀 필요 없다: 루프마다 서버에 다른 주소나 포트의 리스너를 따로
주고, 앞에 있는 무언가 - 로드 밸런서나 DNS - 가 클라이언트를 퍼뜨리게 한다. 다음 절의 부하 측정은 그
방식으로 했다.

루프를 늘리는 것이 도움이 되는지는 시간이 어디에 쓰이느냐에 달렸다. 루프는 핸들러가 계산할 수 있는
양을 곱해 준다. 기다리는 핸들러에게는 아무 도움이 되지 않으며, 그런 핸들러는 애초에 루프 위에 있어서는
안 된다(10절).

테스트 스위트가 이 프로그램을 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_15_loops.c -->
```c
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 포트 하나 뒤의 여러 루프: 스레드 하나가 받아들이고, 각자의 루프 위에 있는 서버들에게
 * 연결을 나눠 준다.
 *
 * 연결은 루프 하나에 속하고, 그 연결의 어떤 것도 다른 스레드가 건드리지 않는다. 그래서
 * 프로세서에 걸친 확장은 각자 서버를 가진 여러 루프다 - 그리고 들어오는 연결을 그들 사이에
 * 나눠 줄 무언가가 있어야 한다. 여기서는 받아들이고 차례로 나눠 주는 스레드가 그 일을 하며,
 * 이 방식은 라이브러리가 도는 곳이면 어디서나 된다.
 */

enum { LOOPS = 2 };

/* 루프 하나, 그 서버, 그리고 그 핸들러가 세는 것. 그 루프의 스레드만 건드린다. */
typedef struct {
    int index;
    proven_loop_t *loop;
    proven_http_event_server_t *server;
    int served;                       /* 루프가 멈춘 뒤에만 메인 스레드가 읽는다 */
} worker_t;

static worker_t g_workers[LOOPS];
static proven_net_listener_t g_listener;
static atomic_bool g_stop;

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    worker_t *me = ctx;
    (void)head;
    char text[16];
    int n = snprintf(text, sizeof text, "loop %d", me->index);
    me->served++;
    (void)proven_http_stream_respond(stream, 200, NULL, 0, (proven_mem_view_t){ (const proven_byte_t *)text, (proven_size_t)n });
}

/* 받아들이는 스레드에서 루프로 건너가는 것. 열린 연결은 복사해서는 안 되므로
 * 자기 메모리에 담겨 간다. */
typedef struct { worker_t *to; proven_net_conn_t conn; } handoff_t;

/* 루프의 스레드에서 - 서버에 연결을 줄 수 있는 유일한 곳. */
static void adopt_here(void *ctx) {
    handoff_t *h = ctx;
    if (proven_http_event_server_adopt(h->to->server, &h->conn) != PROVEN_OK) (void)proven_net_close(&h->conn);   /* 거절됨: 여전히 우리 것이니 닫는다 */
    free(h);
}

/* 받아들이는 스레드. 어떤 서버도 건드리지 않는다: 받아들이고, 고르고, 게시한다. */
static void acceptor(void *arg) {
    (void)arg;
    int next = 0;
    while (!atomic_load(&g_stop)) {
        handoff_t *h = malloc(sizeof *h);
        if (!h) break;
        /* 한 번에 잠깐씩만 기다려서, 멈추라는 말을 스레드가 알아채게 한다. */
        if (proven_net_accept(&g_listener, proven_net_deadline_in(20), &h->conn, NULL) != PROVEN_OK) { free(h); continue; }
        h->to = &g_workers[next];
        next = (next + 1) % LOOPS;
        if (proven_loop_post(h->to->loop, adopt_here, h) != PROVEN_OK) { (void)proven_net_close(&h->conn); free(h); }
    }
}

static void run(void *arg) { (void)proven_loop_run(((worker_t *)arg)->loop); }

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    // ---- 루프 둘, 각각 리스너 없는 서버 하나 ---------------------------------------
    /* 리스너 없는 서버는 주어진 것만 서비스한다. */
    proven_job_sys_t *threads = NULL;
    proven_job_group_t loops, accepting;
    proven_job_group_init(&loops);
    proven_job_group_init(&accepting);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, LOOPS + 1, 8, &threads) == PROVEN_OK, "스레드 셋");
    for (int i = 0; i < LOOPS; ++i) {
        worker_t *w = &g_workers[i];
        w->index = i;
        proven_http_event_server_config_t config = { .on = { .on_request = on_request }, .ctx = w };
        EXAMPLE_REQUIRE(proven_loop_create(heap, &w->loop) == PROVEN_OK &&
                        proven_http_event_server_create(w->loop, &config, &w->server) == PROVEN_OK &&
                        proven_job_group_submit(threads, &loops, run, w) == PROVEN_OK, "자기 스레드의 루프와 서버");
    }

    // ---- 리스너 하나, 그리고 나눠 주는 스레드 --------------------------------------
    proven_net_addr_t at;
    EXAMPLE_REQUIRE(proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 64, &g_listener, &at) == PROVEN_OK &&
                    proven_job_group_submit(threads, &accepting, acceptor, NULL) == PROVEN_OK, "리스너와 받아들이는 스레드");

    // ---- 연결 여덟 -----------------------------------------------------------------
    /* 연결을 열어 두지 않는 클라이언트: 요청 여덟은 연결 여덟이다. */
    proven_http_client_config_t client_config = { .alloc = heap, .max_idle_connections = 0 };
    proven_http_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&client_config, &client) == PROVEN_OK, "클라이언트");
    char url[64];
    int n = snprintf(url, sizeof url, "http://127.0.0.1:%u/", (unsigned)at.port);
    int answers[LOOPS] = { 0 };
    for (int i = 0; i < 8; ++i) {
        proven_http_client_response_t response;
        proven_u8str_t body = { 0 };
        EXAMPLE_REQUIRE(proven_http_client_get(client, (proven_u8str_view_t){ (const proven_byte_t *)url, (proven_size_t)n }, &response) == PROVEN_OK &&
                        proven_http_client_read_all(&response, heap, &body, 64) == PROVEN_OK, "요청에 답이 온다");
        proven_u8str_view_t text = proven_u8str_as_view(&body);
        if (text.size == 6 && memcmp(text.ptr, "loop ", 5) == 0 && text.ptr[5] - '0' < LOOPS) answers[text.ptr[5] - '0']++;
        proven_u8str_destroy(heap, &body);
        proven_http_client_finish(&response);
    }
    proven_http_client_destroy(client);
    EXAMPLE_REQUIRE(answers[0] == 4 && answers[1] == 4, "차례로 나눠 줬다: 루프마다 연결 넷을 서비스했다");

    // ---- 해체, 이 순서로 -----------------------------------------------------------
    /* 받아들이는 쪽이 먼저다. 그래야 더는 게시되지 않는다. 그다음 루프들. 그다음 서버들. */
    atomic_store(&g_stop, true);
    proven_job_group_wait(threads, &accepting);
    (void)proven_net_listener_close(&g_listener);
    for (int i = 0; i < LOOPS; ++i) proven_loop_stop(g_workers[i].loop);
    proven_job_group_wait(threads, &loops);
    int served = 0;
    for (int i = 0; i < LOOPS; ++i) {
        /* 게시되었으나 아직 돌지 않은 것은 여기서, 서버가 사라지기 전에 돈다. */
        EXAMPLE_REQUIRE(proven_loop_poll(g_workers[i].loop, PROVEN_NET_DONT_WAIT) == PROVEN_OK, "마지막 한 바퀴");
        served += g_workers[i].served;
        proven_http_event_server_destroy(g_workers[i].server);
        proven_loop_destroy(g_workers[i].loop);
    }
    EXAMPLE_REQUIRE(served == 8, "모두 여덟 요청이 서비스되었다");
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    return EXAMPLE_OK();
}
```

## 14. 연결 하나가 붙드는 것, 측정한 것, 그리고 여기 없는 것

**연결 하나가 붙드는 것.** 요청이 없는 연결은 자기 구조체를 붙들 뿐 버퍼는 붙들지 않는다: 요청은
루프의 임시 작업용 버퍼로 읽히고, 바이트는 요청이 아직 완전하지 않을 때에만 따로 복사된다. x86-64
Linux에서 등록된 테스트는 한가한 평문 HTTP 연결 하나에 힙 448바이트를 재고 512를 넘으면 실패하며,
한가한 평문 WebSocket 연결 하나에 928바이트를 재고 1 KiB를 넘으면 실패한다. TLS 연결은 엔진의 상태를
더한다. 한가할 때 약 1 KiB다(14장 9절).

**측정한 것.** 테스트 스위트에 들어 있지 않은 부하 프로그램을, 기계 한 대에서, 한 번 돌린 결과다:
3.5 GHz의 16스레드 x86-64 데스크톱 프로세서, 62 GB, Linux 6.12, GCC `-O2`. 서버와 클라이언트는 그
기계의 서로 다른 프로세스이고 루프백 인터페이스로, 평문 HTTP로 말한다. 모든 연결이 요청 하나를 보낸
뒤 열린 채 남는다. 그다음 정해진 비율의 연결이 10초 동안, 답을 기다렸다가 곧바로 다음 요청을 보내는
식으로 요청을 잇는다. 핸들러는 두 바이트로 답한다. **루프백은 네트워크가 아니다** - 여기에는 손실도
느린 상대도 없다 - 그리고 실제 일을 하는 핸들러는 "ok"라고 답하는 핸들러보다 느릴 것이다.

루프 하나:

| 유지한 연결 | 요청을 보내는 연결 | 초당 요청 | 왕복 시간: 중앙값, 99번째 백분위 | 연결당 서버 상주 메모리 |
|---|---|---|---|---|
| 1,000 | 10 | 169,000 | 0.05 ms, 0.14 ms | 479 B |
| 50,000 | 10 | 175,000 | 0.04 ms, 0.11 ms | 481 B |
| 10,000 | 100 | 184,000 | 0.5 ms, 0.9 ms | 486 B |
| 50,000 | 500 | 171,000 | 2.8 ms, 5.2 ms | 481 B |
| 100,000 | 1,000 | 155,000 | 6.4 ms, 9.7 ms | 480 B |

표가 말하는 것: **아무 일도 하지 않는 연결은 바쁜 연결에게 아무 비용도 지우지 않는다** - 연결 열 개는
오만 개 사이에서도 천 개 사이에서와 같은 빠르기로 서비스된다 - 그리고 메모리는 비례해서 든다. 서버에서
하나에 약 480바이트다. 루프 하나는 초당 정해진 양의 일을 하므로, 더 많은 연결이 한꺼번에 물으면 각자
더 오래 기다린다: 왕복 시간은 유지한 수가 아니라 묻는 수에 따라 늘어난다. 어느 행에서도 연결은 하나도
잃지 않았다. 100,000개를 여는 데 96초가 걸렸고 50,000개는 1.7초였다. 그것은 포트 범위가 바닥나면서
클라이언트 쪽 시스템이 빈 포트를 찾느라 쓴 시간으로 보이며 서버의 시간이 아니다: 블로킹 서버도 같은
단계에 그만큼 또는 그 이상 걸렸다.

소켓에 대한 시스템 자신의 비용은 별개이고 더 크다: 같은 실행들에서 커널의 slab 메모리는 연결 하나에,
그 양 끝을 합쳐, 약 7.5 KiB씩 늘었다.

여러 루프. 클라이언트 프로세스 넷이 각각 연결 12,500개를 유지하고 그 1%가 요청을 보낸다:

| 루프 | 초당 요청, 모든 클라이언트 합계 | 왕복 시간: 중앙값 |
|---|---|---|
| 1 | 167,000 | 2.9 ms |
| 2 | 337,000 | 1.4 ms |
| 4 | 514,000 | 1.0 ms |

**비교를 위한 11장의 블로킹 서버.** 같은 프로그램에서, 핸들러를 자기 루프의 스레드에 둔 채: 같은
100,000개의 연결을 유지했고 조금 더 빨리 답했다 - 50,000개를 유지할 때 초당 186,000 요청, 이쪽은
171,000. 그 값은 메모리다: 연결당 39 KiB를 할당하고 그중 10.9 KiB가 상주한다. 이쪽은 480바이트다.
그리고 그것은 핸들러가 기다리는 스레드 하나다. 이 장의 서버를 고를 이유는 1절의 것들이지, 사소한
핸들러에서의 순수한 속도가 아니다.

**Windows에서는 한계가 낮고, 그 자리는 여기다.** 거기서 루프는 `WSAPoll`로 기다리는데, 그것은 호출마다
모든 소켓을 들여다본다. 같은 프로그램의 양쪽 끝을 Windows 11 가상 머신 하나에 두고, 연결 열 개가 요청을
보낼 때:

| 유지한 연결 | 초당 요청 | 왕복 시간: 중앙값 |
|---|---|---|
| 100 | 40,000 | 0.2 ms |
| 1,000 | 8,100 | 1.0 ms |
| 2,000 | 2,700 | 3.2 ms |
| 5,000 | 480 | 18 ms |
| 10,000 | 150 | 66 ms |

클라이언트도 같은 루프 구현 위에 있으므로 각 수치는 그 비용을 두 번 싣는다. 20,000개의 연결을
청했을 때 그 기계는 포트가 바닥나기 전에 16,369개를 열었다. Windows에서 이 서버는 수백 개의 연결,
많아야 몇천 개를 위한 것이다. 이 릴리스의 무엇도 그것을 바꾸지 않는다.

**여기 없는 것.**

- **이름값을 하는 부하 시험.** 위의 수치는 기계 한 대가 자기 자신과 말한 것이다: 네트워크도, 느리거나
  적대적인 클라이언트도, 부하 속의 TLS도 없고, 아무 일도 하지 않는 핸들러 하나뿐이다.
- **루프 위의 WebSocket 클라이언트**, 그리고 이 서버 위의 WebSocket 아닌 업그레이드. 12장의
  클라이언트는 블로킹 쪽이다.
- **이벤트 구동 클라이언트에서:** 연결 재사용, 리다이렉트, 인증, 쿠키, 프록시, 이름 해석(대신 무엇을
  할지는 12절에 있다).
- **루프의 스레드 밖에서 하는 TLS 핸드셰이크.** 하나마다 루프는 약 2 ms 동안 다른 아무것도 서비스하지
  못한다(14장 9절).
- Windows에서 `WSAPoll`보다 **빠른 기다림**.
- **파일을 복사 없이 보내기**, 응답 압축, HTTP/2.
- 루프로 **파일, 시그널, 자식 프로세스 지켜보기**. 루프는 소켓을 지켜본다.

**어떻게 시험했나.** 등록된 테스트는 루프의 타이머, 게시, 소켓 관심을 한 스레드에서, 그리고 여러
스레드에서 몬다 - 해당 콜백 안에서의 제거와 취소를 포함해서. 그리고 서버를 루프백 위에서, 평문과
TLS로, 7절과 9절 표의 모든 행에 걸쳐 돌린다: 즉시 하는 답, 타이머에서 하는 답, 다른 스레드에서 하는
답, pause와 resume이 있는 업로드, 쓰기가 한 번 거절될 때까지 읽지 않는 클라이언트로의 16 MB 다운로드,
파이프라이닝, 각 거절, 각 타임아웃, 사라지는 클라이언트, 답하지 않은 요청을 둔 채 해제되는 서버 -
`on_request`마다 `on_done`이 한 번 불렸는지 확인하면서. WebSocket은 12장의 블로킹 클라이언트와 날
소켓을 상대로 11절 표들의 행에 걸쳐 돌린다 - `PROVEN_ERR_NOMEM`, 그리고 `proven_ws_event_accept`와
보내기 호출의 `PROVEN_ERR_RESET`만 빼고. 이것들은 일으키지 않는다. 클라이언트는 같은 루프 위의 이
장의 서버와, 테스트가 직접 써 주는 답을 상대로 12절 표들의 행에 걸쳐 돌린다 -
`PROVEN_ERR_UNREACHABLE`, `PROVEN_ERR_NOMEM`, 만료된 인증서, 그리고 네 시간 한도 가운데 셋만 빼고:
지나가게 만드는 것은 응답 타임아웃뿐이다. 이 모두를 기계에 부하를 건 채 주소 검사와 미정의 동작 검사
아래에서 수백 번, 그리고 스레드 검사 아래에서 돌렸다. 여러 루프는 받아들이는 스레드 하나 뒤의 세 스레드 위 세 서버로, 평문과
TLS로, 스레드 검사 아래에서 돌린다. 루프의 휠 한 바퀴(16.4초)보다 먼 타이머는 테스트가 움직이는 시계로
시험한다. **하지 않은 것:** 나열한 경우를 넘어서는 적대적 상대와의 실행, 공개된 WebSocket 적합성
스위트를 이 서버에 돌리는 것, 그리고 위에 적은 한 번의 실행을 넘어서는 부하.
