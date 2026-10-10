#include "example.h"
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
