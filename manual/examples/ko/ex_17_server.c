#include "example.h"
#include <stdio.h>
#include <string.h>

/*
 * 서버 하나, 포트 하나, 프로토콜 둘: `http2`를 켠 이벤트 구동 HTTP 서버.
 *
 * 아래 처리기는 평범한 처리기다 - 같은 콜백, 같은 스트림 호출 - 묻지 않는 한 요청이 어느
 * 프로토콜로 왔는지 모른다. 그런 다음 HTTP/1.1 클라이언트와 HTTP/2 클라이언트가 같은 포트로
 * 말을 건다. 여기의 HTTP/2 클라이언트는 이 장 5절의 연결 엔진에 소켓을 통과시킨 것으로, 그것이
 * 무엇으로 이루어지는지 보여 준다: 마흔 줄이다. (연결을 간직하고 함께 쓰는 라이브러리 자체의
 * 클라이언트는 7절에 있다.)
 */

typedef struct { proven_loop_t *loop; int served, over_http2; } ex_app_t;

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    ex_app_t *app = ctx;
    /* 요청은 어느 쪽이든 같아 보인다: 메서드, 대상, 필드 - 그 가운데 Host도 있다. HTTP/2는
     * 그것을 `:authority`로 싣고, 서버는 익숙한 이름으로 넘겨준다. */
    /* HTTP/3에서 TLS 설정에 `early_data`가 있으면 요청은 조기 데이터로 올 수 있고(14장 6절),
     * 공격자가 재전송한 사본일 수 있다. 이 답은 두 번 해도 괜찮다. 그렇지 않은 요청이라면 425로
     * 답하고, 클라이언트는 나중에 다시 보낸다. */
    if (proven_http_stream_early(stream) && head->method != PROVEN_HTTP_GET) {
        (void)proven_http_stream_respond(stream, 425, NULL, 0, (proven_mem_view_t){ 0 });
        return;
    }
    const int protocol = proven_http_stream_protocol(stream);
    char text[96];
    int n = snprintf(text, sizeof text, "%.*s over HTTP/%d\n", (int)head->target.size, (const char *)head->target.ptr, protocol);
    proven_http_header_t fields[] = { { PROVEN_LIT("Content-Type"), PROVEN_LIT("text/plain") } };
    if (proven_http_stream_respond(stream, 200, fields, 1, (proven_mem_view_t){ (const proven_byte_t *)text, (proven_size_t)n }) != PROVEN_OK) return;
    if (protocol == 2) app->over_http2++;
}

static void on_done(void *ctx, proven_http_stream_t *stream, proven_err_t why) {
    (void)stream;
    if (why == PROVEN_OK) ((ex_app_t *)ctx)->served++;
}

static void run(void *arg) { (void)proven_loop_run(((ex_app_t *)arg)->loop); }

/* 작은 HTTP/2 클라이언트: 엔진, 소켓, 그리고 요청한 스트림이 모두 끝날 때까지 둘 사이로
 * 바이트를 옮기는 루프. 200으로 답을 받은 수를 돌려준다. */
static int ask_three(proven_u16 port, char answers[3][64]) {
    proven_net_conn_t sock;
    if (proven_net_connect(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, port), proven_net_deadline_in(5000), &sock) != PROVEN_OK) return -1;

    /* 엔진은 스트림이 열려 있는 동안 자기가 할당한 메모리에 헤더 목록을 읽는다 - 또는 여러분의
     * 메모리에. 한 스레드의 여러 연결이 그것을 함께 쓸 수 있다. */
    static proven_u64 header_room[16384];
    proven_h2_config_t config = { .alloc = proven_heap_allocator(), .header_buffer = { (proven_byte_t *)header_room, sizeof header_room } };
    proven_h2_conn_t *conn = NULL;
    if (proven_h2_header_buffer_size(0) > sizeof header_room || proven_h2_conn_create(&config, &conn) != PROVEN_OK) { (void)proven_net_close(&sock); return -1; }

    /* 한 연결에서 요청 셋을 한꺼번에: 스트림 1, 3, 5. */
    static const char *const paths[3] = { "/first", "/second", "/third" };
    for (int i = 0; i < 3; i++) {
        proven_http_header_t request[] = {
            { PROVEN_LIT(":method"), PROVEN_LIT("GET") }, { PROVEN_LIT(":scheme"), PROVEN_LIT("http") },
            { PROVEN_LIT(":authority"), PROVEN_LIT("localhost") }, { PROVEN_LIT(":path"), proven_u8str_view_from_cstr(paths[i]) },
        };
        proven_u32 stream = 0;
        if (proven_h2_conn_submit_request(conn, request, 4, true, &stream) != PROVEN_OK) { proven_h2_conn_destroy(conn); (void)proven_net_close(&sock); return -1; }
    }

    int ended = 0, ok = 0;
    static proven_byte_t in[16384];
    while (ended < 3) {
        /* 엔진이 쌓아 둔 것 - 서문과 설정, 요청, 나중에는 승인 - 이 먼저 소켓으로 간다. */
        proven_mem_view_t out = proven_h2_conn_pending_output(conn);
        if (out.size > 0) {
            if (proven_net_write_all(&sock, out, proven_net_deadline_in(5000)).err != PROVEN_OK) break;
            proven_h2_conn_output_sent(conn, out.size);
        }
        proven_result_size_t got = proven_net_read(&sock, (proven_mem_mut_t){ in, sizeof in }, proven_net_deadline_in(5000));
        if (got.err != PROVEN_OK) break;
        proven_size_t pos = 0;
        for (;;) {
            proven_size_t used = 0;
            proven_h2_event_t ev;
            if (proven_h2_conn_feed(conn, (proven_mem_view_t){ in + pos, got.value - pos }, &used, &ev) != PROVEN_OK) { ended = 3; break; }
            pos += used;
            const proven_u32 k = ev.stream_id ? (ev.stream_id - 1) / 2 : 0;     /* 스트림 1, 3, 5 -> 0, 1, 2 */
            if (ev.kind == PROVEN_H2_EVENT_HEADERS && ev.status == 200 && k < 3) ok++;
            if (ev.kind == PROVEN_H2_EVENT_DATA && k < 3) {
                snprintf(answers[k], 64, "%.*s", (int)(ev.data.size < 63 ? ev.data.size : 63), (const char *)ev.data.ptr);
                proven_h2_conn_consumed(conn, ev.stream_id, ev.data.size);
            }
            if ((ev.kind == PROVEN_H2_EVENT_DATA || ev.kind == PROVEN_H2_EVENT_HEADERS) && ev.end_stream) ended++;
            if (ev.kind == PROVEN_H2_EVENT_STREAM_RESET || ev.kind == PROVEN_H2_EVENT_CLOSED) { ended = 3; break; }
            if (ev.kind == PROVEN_H2_EVENT_NONE && pos == got.value) break;
        }
    }
    proven_h2_conn_destroy(conn);
    (void)proven_net_close(&sock);
    return ok;
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    ex_app_t app = { 0 };
    EXAMPLE_REQUIRE(proven_loop_create(heap, &app.loop) == PROVEN_OK, "a loop");

    /* `http2`가 전부다. 평문에서는 HTTP/2 서문으로 시작하는 클라이언트가 HTTP/2이고 나머지는
     * 모두 HTTP/1.1이다. TLS에서는 핸드셰이크가 정하며(ALPN), 그때는 TLS 설정이 "h2"를
     * 내놓아야 한다. */
    proven_http_event_server_config_t config = { .on = { .on_request = on_request, .on_done = on_done }, .ctx = &app, .http2 = true };
    proven_http_event_server_t *server = NULL;
    proven_net_addr_t at;
    EXAMPLE_REQUIRE(proven_http_event_server_create(app.loop, &config, &server) == PROVEN_OK &&
                    proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "a server that speaks both");
    proven_job_sys_t *threads = NULL;
    proven_job_group_t running;
    proven_job_group_init(&running);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &threads) == PROVEN_OK &&
                    proven_job_group_submit(threads, &running, run, &app) == PROVEN_OK, "the loop runs on another thread");

    // ---- HTTP/1.1 클라이언트 ------------------------------------------------------------------
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%u/hello", (unsigned)at.port);
    proven_http_client_config_t client_config = { .alloc = heap, .max_idle_connections = 1 };
    proven_http_client_t *client = NULL;
    proven_http_client_response_t resp;
    proven_u8str_t body = { 0 };
    EXAMPLE_REQUIRE(proven_http_client_create(&client_config, &client) == PROVEN_OK &&
                    proven_http_client_get(client, proven_u8str_view_from_cstr(url), &resp) == PROVEN_OK && resp.status == 200 &&
                    proven_http_client_read_all(&resp, heap, &body, 4096) == PROVEN_OK, "the blocking client of chapter 11 is answered");
    printf("HTTP/1.1 client: %.*s", (int)proven_u8str_as_view(&body).size, (const char *)proven_u8str_as_view(&body).ptr);
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&body), PROVEN_LIT("/hello over HTTP/1\n")), "over HTTP/1.1");
    proven_u8str_destroy(heap, &body);
    proven_http_client_finish(&resp);
    proven_http_client_destroy(client);

    // ---- 같은 포트의 HTTP/2 클라이언트 ---------------------------------------------------------
    char answers[3][64] = { "", "", "" };
    EXAMPLE_REQUIRE(ask_three(at.port, answers) == 3, "three requests on one HTTP/2 connection are each answered");
    for (int i = 0; i < 3; i++) printf("HTTP/2 client:   %s", answers[i]);
    EXAMPLE_REQUIRE(strcmp(answers[0], "/first over HTTP/2\n") == 0 && strcmp(answers[1], "/second over HTTP/2\n") == 0 && strcmp(answers[2], "/third over HTTP/2\n") == 0,
                    "by the same handler, which was told the protocol was 2");

    // ---- 끝 ------------------------------------------------------------------------------------
    /* 루프를 멈춘 다음 처리기가 센 것을 보고 서버를 해체한다: 그때는 다른 무엇도 둘을 건드리지
     * 않는다. */
    proven_loop_stop(app.loop);
    proven_job_group_wait(threads, &running);
    EXAMPLE_REQUIRE(app.served == 4 && app.over_http2 == 3, "four exchanges ended with on_done(PROVEN_OK), three of them over HTTP/2");
    proven_http_event_server_destroy(server);
    proven_loop_destroy(app.loop);
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    return EXAMPLE_OK();
}
