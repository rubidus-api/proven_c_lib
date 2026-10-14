#include "example.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 한 스레드는 메시지를 기다리고 다른 스레드는 무엇을 보낼지 정한다 - 채팅 클라이언트의 모양.
 *
 * 블로킹 연결(12장)로는 안 된다: 연결은 한 스레드의 것이고, proven_ws_conn_receive를 감싼 락은
 * 받기가 기다리는 동안 모든 보내기를 붙잡는다. 루프의 연결로는 된다. 그 연결은 자기 스레드 없이
 * 메시지를 기다리고, 다른 스레드는 보낼 것을 proven_loop_post로 넘긴다: 보내기 자체는 연결을
 * 만지는 유일한 스레드인 루프의 스레드에서 일어난다.
 *
 * 여기서는 루프 하나가 양 끝을 모두 나른다: 메시지마다 답하는 서버, 그리고 클라이언트.
 */

#define LINES 3

typedef struct {
    proven_loop_t *loop;
    proven_ws_stream_t *ws;           /* 클라이언트의 연결: 루프의 스레드만 */
    atomic_bool open;                 /* 타이핑 스레드가 읽어도 되는 것 */
    int answers, closed;
} app_t;

// ---- 서버: 메시지마다 답한다 --------------------------------------------------

static void served(void *ctx, proven_ws_stream_t *ws, bool text, proven_mem_view_t piece, bool first, bool last) {
    (void)ctx; (void)text; (void)first;
    if (!last) return;                      /* 이 예제의 메시지는 작다: 각각 한 조각 */
    char reply[80];
    int n = snprintf(reply, sizeof reply, "got: %.*s", (int)piece.size, (const char *)piece.ptr);
    (void)proven_ws_stream_send(ws, true, (proven_mem_view_t){ (const proven_byte_t *)reply, (proven_size_t)n });
}

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    (void)ctx;
    proven_ws_event_config_t config = { .on = { .on_message = served } };
    proven_ws_stream_t *ws = NULL;
    if (proven_ws_event_accept(stream, head, &config, &ws) != PROVEN_OK) (void)proven_http_stream_respond(stream, 400, NULL, 0, (proven_mem_view_t){ 0 });
}

// ---- 클라이언트, 루프의 스레드에서 -------------------------------------------

static void on_open(void *ctx, proven_ws_stream_t *ws, proven_u8str_view_t protocol) {
    app_t *app = ctx;
    (void)protocol;
    app->ws = ws;
    atomic_store(&app->open, true);
}

static void on_message(void *ctx, proven_ws_stream_t *ws, bool text, proven_mem_view_t piece, bool first, bool last) {
    app_t *app = ctx;
    (void)text; (void)first;
    if (!last) return;
    printf("the server says: %.*s\n", (int)piece.size, (const char *)piece.ptr);
    if (++app->answers == LINES) proven_ws_stream_close(ws, 1000, PROVEN_LIT("bye"));
}

static void on_closed(void *ctx, proven_ws_stream_t *ws, proven_u16 code, proven_err_t why) {
    app_t *app = ctx;
    (void)ws; (void)code; (void)why;
    app->ws = NULL;
    app->closed++;
    proven_loop_stop(app->loop);
}

// ---- 타이핑 스레드에서 건너오는 것 --------------------------------------------

/* 보낼 한 줄, 자기 메모리에: 그것이 보내질 때쯤 타이핑 스레드는 이미 다음으로 넘어가 있다. */
typedef struct { app_t *app; proven_size_t len; char text[64]; } line_t;

/* 루프의 스레드에서. */
static void send_here(void *ctx) {
    line_t *line = ctx;
    if (line->app->ws) (void)proven_ws_stream_send(line->app->ws, true, (proven_mem_view_t){ (const proven_byte_t *)line->text, line->len });
    free(line);
}

/* 타이핑 스레드. 연결은 절대 만지지 않는다: post한다. */
static void typist(void *arg) {
    app_t *app = arg;
    static const char *const lines[LINES] = { "hello", "how are you", "goodbye" };
    while (!atomic_load(&app->open)) proven_time_sleep(1);
    for (int i = 0; i < LINES; ++i) {
        line_t *line = malloc(sizeof *line);
        if (!line) return;
        line->app = app;
        line->len = strlen(lines[i]);
        memcpy(line->text, lines[i], line->len);
        if (proven_loop_post(app->loop, send_here, line) != PROVEN_OK) free(line);
        proven_time_sleep(5);               /* 누군가 타이핑하는 중 */
    }
}

static void run(void *arg) { (void)proven_loop_run(((app_t *)arg)->loop); }

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    static app_t app;
    atomic_init(&app.open, false);
    EXAMPLE_REQUIRE(proven_loop_create(heap, &app.loop) == PROVEN_OK, "a loop");
    proven_http_event_server_config_t server_config = { .on = { .on_request = on_request } };
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

    char url[64];
    int n = snprintf(url, sizeof url, "ws://127.0.0.1:%u/chat", (unsigned)at.port);
    proven_ws_event_connect_options_t options = {
        .url = { (const proven_byte_t *)url, (proven_size_t)n },
        .on_open = on_open,
        .ws = { .on = { .on_message = on_message, .on_closed = on_closed }, .ctx = &app },
    };
    proven_ws_stream_t *ws = NULL;
    EXAMPLE_REQUIRE(proven_ws_event_connect(app.loop, &options, &ws) == PROVEN_OK, "a connection is begun");

    // ---- 두 스레드: 루프, 그리고 타이핑하는 사람 ---------------------------------
    proven_job_sys_t *threads = NULL;
    proven_job_group_t looping, typing;
    proven_job_group_init(&looping);
    proven_job_group_init(&typing);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 2, 4, &threads) == PROVEN_OK &&
                    proven_job_group_submit(threads, &looping, run, &app) == PROVEN_OK &&
                    proven_job_group_submit(threads, &typing, typist, &app) == PROVEN_OK, "the loop on one thread, the typist on another");

    /* 세 번째 답 뒤 연결이 닫히면 루프가 멈춘다. */
    proven_job_group_wait(threads, &typing);
    proven_job_group_wait(threads, &looping);
    EXAMPLE_REQUIRE(app.answers == LINES && app.closed == 1, "three lines sent from the typing thread, three answers received on the loop's");

    /* post되고 아직 돌지 않은 것은 루프가 사라지기 전에 여기서 돈다. */
    EXAMPLE_REQUIRE(proven_loop_poll(app.loop, PROVEN_NET_DONT_WAIT) == PROVEN_OK, "a last round");
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    proven_http_event_server_destroy(server);
    proven_loop_destroy(app.loop);
    return EXAMPLE_OK();
}
