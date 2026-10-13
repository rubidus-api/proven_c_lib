#include "example.h"
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
    /* 이쪽에도 같은 스위치가 있다. 답을 압축하면 안 되는 서버라면
     * proven_ws_stream_set_compress(ws, false). 여기서는 압축해도 된다. */
    proven_ws_stream_set_compress(ws, true);
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
    /* 압축기를 거치지 않는 메시지 하나 - 상대가 고른 텍스트 옆에 비밀을 실은 메시지에 필요한 것.
     * 스위치는 메시지가 시작될 때 읽힌다. */
    proven_ws_conn_set_compress(conn, false);
    EXAMPLE_REQUIRE(proven_ws_conn_send_text(conn, (proven_u8str_view_t){ (const proven_byte_t *)report, 1000 }) == PROVEN_OK &&
                    proven_ws_conn_receive(conn, proven_net_deadline_in(5000), &msg) == PROVEN_OK &&
                    msg.data.size == 10 && memcmp(msg.data.ptr, "1000 bytes", 10) == 0, "a thousand bytes, sent as they are");
    proven_ws_conn_set_compress(conn, true);

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
