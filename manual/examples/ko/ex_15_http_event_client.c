#include "example.h"
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
