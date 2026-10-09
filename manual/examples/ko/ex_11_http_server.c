#include "example.h"

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
