#include "example.h"

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
    EXAMPLE_REQUIRE(err == PROVEN_ERR_UNSUPPORTED, "https without a tls_wrap: the client says so rather than sending in the clear");
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
