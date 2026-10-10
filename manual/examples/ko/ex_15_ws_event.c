#include "example.h"
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
