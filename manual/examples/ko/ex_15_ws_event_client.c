#include "example.h"
#include <stdio.h>
#include <string.h>

/*
 * 루프 위의 WebSocket 클라이언트: 이쪽에서도 스레드가 들지 않는 연결.
 *
 * proven_ws_event_connect는 연결을 시작하고 곧바로 돌아온다. 돌려주는 것은
 * proven_ws_stream_t - 서버의 연결과 같은 타입 - 이므로 호출과 콜백이 위의 서버가 쓰는
 * 것과 같다. 새로운 것은 시작이다: on_open이 서버가 수락했음을 알리고, 그때까지는 아무것도
 * 보낼 수 없다.
 *
 * 여기서는 루프 하나가 양쪽을 다 싣는다: 요청받는 서버와 요청하는 클라이언트.
 */

typedef struct {
    proven_loop_t *loop;
    int opened, answers, closed, refused;
    proven_u16 code;
    char last[64];
} ex_app_t;

// ---- 서버: 무엇을 받았는지 말해 준다 ----

static void served(void *ctx, proven_ws_stream_t *ws, bool text, proven_mem_view_t piece, bool first, bool last) {
    (void)ctx; (void)first;
    if (!last) return;                      /* 이 예제의 메시지는 작다: 각각 한 조각 */
    char reply[48];
    int n = snprintf(reply, sizeof reply, "%s, %u bytes", text ? "text" : "binary", (unsigned)piece.size);
    (void)proven_ws_stream_send(ws, true, (proven_mem_view_t){ (const proven_byte_t *)reply, (proven_size_t)n });
}

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    (void)ctx;
    proven_ws_event_config_t config = { .on = { .on_message = served } };
    proven_ws_stream_t *ws = NULL;
    if (proven_ws_event_accept(stream, head, &config, &ws) != PROVEN_OK) (void)proven_http_stream_respond(stream, 400, NULL, 0, (proven_mem_view_t){ 0 });
}

// ---- 클라이언트 ----

/* 서버가 수락했다. 여기서부터 - 이 호출 안에서도 - 메시지를 보낼 수 있다. */
static void on_open(void *ctx, proven_ws_stream_t *ws, proven_u8str_view_t protocol) {
    ex_app_t *app = ctx;
    (void)protocol;
    app->opened++;
    (void)proven_ws_stream_send(ws, true, proven_mem_view_from_u8(PROVEN_LIT("hello")));
}

/* 서버가 갖는 것과 같은 콜백. 이쪽은 두 번 묻고 작별한다. */
static void on_message(void *ctx, proven_ws_stream_t *ws, bool text, proven_mem_view_t piece, bool first, bool last) {
    ex_app_t *app = ctx;
    (void)text; (void)first;
    if (!last) return;
    proven_size_t n = piece.size < sizeof app->last - 1 ? piece.size : sizeof app->last - 1;
    memcpy(app->last, piece.ptr, n);
    app->last[n] = '\0';
    printf("the server says: %s\n", app->last);
    if (++app->answers == 1) {
        static const proven_byte_t bytes[300] = { 1, 2, 3 };
        (void)proven_ws_stream_send(ws, false, (proven_mem_view_t){ bytes, sizeof bytes });
    } else {
        proven_ws_stream_close(ws, 1000, PROVEN_LIT("thank you"));
    }
}

/* 시작한 연결마다 정확히 한 번 불린다 - 끝내 열리지 않은 연결에도:
 * 그때 코드는 0이고 `why`가 무엇이 잘못되었는지 말한다. */
static void on_closed(void *ctx, proven_ws_stream_t *ws, proven_u16 code, proven_err_t why) {
    ex_app_t *app = ctx;
    (void)ws;
    if (code == 0) {
        /* WebSocket이 되지 못했다: 아무도 듣지 않았거나, 서버가 다른 답을 했거나, 시간이 다 됐다. */
        printf("no connection: error %d\n", (int)why);
        app->refused++;
    } else {
        app->code = code;
        app->closed++;
    }
    /* 이 프로그램의 두 연결이 모두 끝났다: 루프가 할 일이 남지 않았다. */
    if (app->closed == 1 && app->refused == 1) proven_loop_stop(app->loop);
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    ex_app_t app = { 0 };
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

    /* 호스트가 주소이므로 따로 줄 것이 없다: 루프 위에서는 이름을 풀지 않는다. */
    char url[64];
    int n = snprintf(url, sizeof url, "ws://127.0.0.1:%u/chat", (unsigned)at.port);
    proven_ws_event_connect_options_t options = {
        .url = { (const proven_byte_t *)url, (proven_size_t)n },
        .on_open = on_open,
        .ws = { .on = { .on_message = on_message, .on_closed = on_closed }, .ctx = &app },
    };
    proven_ws_stream_t *ws = NULL;
    EXAMPLE_REQUIRE(proven_ws_event_connect(app.loop, &options, &ws) == PROVEN_OK && ws != NULL && app.opened == 0, "a connection is begun; nothing has happened yet");
    /* 호출은 돌아왔고 연결은 만들어지는 중이다: 말하기에는 아직 이르다. */
    EXAMPLE_REQUIRE(proven_ws_stream_send(ws, true, proven_mem_view_from_u8(PROVEN_LIT("too early"))) == PROVEN_ERR_INVALID_STATE, "nothing can be sent before the server has accepted");

    /* 그리고 실패할 연결 하나. 그것이 어떻게 전해지는지 보려고: 방금 닫은 포트. */
    proven_net_listener_t unused;
    proven_net_addr_t nobody;
    EXAMPLE_REQUIRE(proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 1, &unused, &nobody) == PROVEN_OK && proven_net_listener_close(&unused) == PROVEN_OK, "a port nobody listens on");
    n = snprintf(url, sizeof url, "ws://127.0.0.1:%u/chat", (unsigned)nobody.port);
    options.url.size = (proven_size_t)n;
    proven_ws_stream_t *lost = NULL;
    err = proven_ws_event_connect(app.loop, &options, &lost);
    EXAMPLE_REQUIRE(err == PROVEN_OK || err == PROVEN_ERR_REFUSED, "an attempt that cannot succeed: refused now, or told later");
    if (err != PROVEN_OK) app.refused++;

    /* 나머지는 모두 여기서 일어난다. */
    if (!(app.closed == 1 && app.refused == 1)) EXAMPLE_REQUIRE(proven_loop_run(app.loop) == PROVEN_OK, "the loop runs until both have ended");
    EXAMPLE_REQUIRE(app.opened == 1 && app.answers == 2 && strcmp(app.last, "binary, 300 bytes") == 0, "it opened once, and two messages were answered");
    EXAMPLE_REQUIRE(app.closed == 1 && app.code == 1000 && app.refused == 1, "it closed once, with this side's code - and the other attempt was reported once");

    proven_http_event_server_destroy(server);
    proven_loop_destroy(app.loop);
    return EXAMPLE_OK();
}
