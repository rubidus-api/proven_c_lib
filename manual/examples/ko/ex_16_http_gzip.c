#include "example.h"
#include <stdio.h>
#include <string.h>

/*
 * HTTP로 보내는 압축된 응답: 핸들러가 요청하고 클라이언트가 받아들이면 압축하는 서버와,
 * 압축을 요청하고 풀어 읽는 클라이언트.
 *
 * 프로그램이 말하기 전에는 둘 다 꺼져 있다. 서버 쪽은 핸들러에서 응답마다 부르는 호출 하나이고,
 * 클라이언트 쪽은 설정의 플래그 하나다.
 */

static proven_byte_t g_page[20000];        /* 제공할 페이지: 텍스트라서 압축이 잘 된다 */

static void fill_page(void) {
    proven_size_t n = 0;
    for (unsigned k = 0; n < sizeof g_page; ++k) {
        char line[48];
        int len = snprintf(line, sizeof line, "row %u of a table that says much the same\n", k);
        for (int i = 0; i < len && n < sizeof g_page; ++i) g_page[n++] = (proven_byte_t)line[i];
    }
}

static void handle(void *ctx, proven_http_exchange_t *x) {
    proven_http_server_t **server = ctx;
    const proven_http_request_t *req = proven_http_exchange_request(x);
    proven_mem_view_t page = { g_page, sizeof g_page };

    if (proven_u8str_view_eq(req->target, PROVEN_LIT("/page"))) {
        /* 응답을 시작하기 전에 호출 하나. 나머지는 서버가 한다: 요청의 Accept-Encoding을
         * 읽고, gzip이 허용되면 압축하고, Content-Encoding과 Vary와 새 Content-Length를
         * 쓴다. */
        proven_http_exchange_compress(x);
        proven_http_header_t type = { PROVEN_LIT("Content-Type"), PROVEN_LIT("text/plain") };
        (void)proven_http_exchange_respond(x, 200, &type, 1, page);

    } else if (proven_u8str_view_eq(req->target, PROVEN_LIT("/account"))) {
        /* 여기서는 일부러 요청하지 않는다: 이 응답은 비밀을 클라이언트가 고른 글자 옆에
         * 싣게 되는데, 압축된 본문의 크기는 그 비밀을 한 바이트씩 새어 나가게 한다.
         * 압축을 응답마다 요청하게 한 것은 핸들러만이 이를 알기 때문이다. */
        (void)proven_http_exchange_respond(x, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("token=... you searched for: ...\n")));

    } else if (proven_u8str_view_eq(req->target, PROVEN_LIT("/accepts"))) {
        /* 서버가 묻는 것과 같은 질문을, 스스로 고르고 싶은 핸들러가 묻는다 -
         * 예컨대 파일과 미리 압축해 둔 그 짝 가운데 하나를. */
        bool gzip = proven_http_accepts_coding(req->headers, req->header_count, PROVEN_HTTP_CODING_GZIP);
        (void)proven_http_exchange_respond(x, 200, NULL, 0, proven_mem_view_from_u8(gzip ? PROVEN_LIT("gzip") : PROVEN_LIT("identity")));

    } else {
        (void)proven_http_exchange_respond(x, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("bye\n")));
        proven_http_server_stop(*server);
    }
}

static void serve(void *arg) { (void)proven_http_server_run(*(proven_http_server_t **)arg); }

/* `path`를 GET한다. 읽은 본문의 크기, Content-Length, 헤더가 말하는 코딩을 알려 준다. */
static bool get(proven_http_client_t *client, proven_u16 port, const char *path,
                proven_size_t *read_size, proven_u64 *content_length, proven_http_coding_t *coding, proven_u8str_t *body) {
    char url[96];
    int n = snprintf(url, sizeof url, "http://127.0.0.1:%u%s", (unsigned)port, path);
    proven_http_client_response_t resp;
    bool ok = proven_http_client_get(client, (proven_u8str_view_t){ (const proven_byte_t *)url, (proven_size_t)n }, &resp) == PROVEN_OK && resp.status == 200;
    if (ok) {
        /* 헤더는 서버가 보낸 그대로다: 무엇이 전송됐는지는 이렇게 본다. */
        *coding = proven_http_content_coding(resp.headers, resp.header_count);
        proven_u8str_view_t length = { 0 };
        *content_length = 0;
        if (proven_http_header_find(resp.headers, resp.header_count, PROVEN_LIT("Content-Length"), &length)) {
            for (proven_size_t i = 0; i < length.size; ++i) *content_length = *content_length * 10 + (proven_u64)(length.ptr[i] - '0');
        }
        /* 한도는 여기에 도착하는 것에 걸린다 - 클라이언트가 푼다면 풀린 바이트에. */
        (void)proven_u8str_reset(body);
        ok = proven_http_client_read_all(&resp, proven_heap_allocator(), body, 1024 * 1024) == PROVEN_OK;
        *read_size = proven_u8str_as_view(body).size;
    }
    proven_http_client_finish(&resp);
    return ok;
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    fill_page();

    static proven_http_server_t *server;
    proven_http_server_config_t config = { .alloc = heap, .handler = handle, .handler_ctx = &server };
    config.compress_level = 6;              /* 1이 가장 빠르고 9가 가장 작다. 0은 이 값을 뜻한다 */
    EXAMPLE_REQUIRE(proven_http_server_create(&config, &server) == PROVEN_OK, "a server");
    proven_net_addr_t at;
    proven_err_t err = proven_http_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        proven_http_server_destroy(server);
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "it listens");
    proven_job_sys_t *threads = NULL;
    proven_job_group_t running;
    proven_job_group_init(&running);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &threads) == PROVEN_OK &&
                    proven_job_group_submit(threads, &running, serve, &server) == PROVEN_OK, "the server loop is started");

    /* 클라이언트 둘: 하나는 늘 하던 대로, 하나는 압축된 응답을 요청한다. */
    proven_http_client_config_t plain_config = { .alloc = heap, .max_idle_connections = 2 };
    proven_http_client_config_t decoding_config = { .alloc = heap, .max_idle_connections = 2, .decompress = true };
    proven_http_client_t *plain = NULL, *decoding = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&plain_config, &plain) == PROVEN_OK &&
                    proven_http_client_create(&decoding_config, &decoding) == PROVEN_OK, "two clients");

    proven_u8str_t body = { 0 };
    proven_size_t read_size = 0;
    proven_u64 sent = 0;
    proven_http_coding_t coding = PROVEN_HTTP_CODING_IDENTITY;

    /* 아무것도 설정하지 않은 클라이언트는 Accept-Encoding을 보내지 않고, 달라지는 것이 없다. */
    EXAMPLE_REQUIRE(get(plain, at.port, "/page", &read_size, &sent, &coding, &body), "the page, to a client that did not ask");
    EXAMPLE_REQUIRE(coding == PROVEN_HTTP_CODING_IDENTITY && sent == sizeof g_page && read_size == sizeof g_page, "arrives as it is");

    /* 요청한 쪽은 같은 바이트를 그 일부만으로 받는다. */
    EXAMPLE_REQUIRE(get(decoding, at.port, "/page", &read_size, &sent, &coding, &body), "the page, to a client that asked");
    EXAMPLE_REQUIRE(coding == PROVEN_HTTP_CODING_GZIP && read_size == sizeof g_page &&
                    memcmp(proven_u8str_as_view(&body).ptr, g_page, sizeof g_page) == 0, "was sent as gzip and read as the page");
    EXAMPLE_REQUIRE(sent < sizeof g_page / 5, "in less than a fifth of the bytes");
    printf("the page: %u bytes, sent as %u\n", (unsigned)sizeof g_page, (unsigned)sent);

    /* 핸들러가 요청하지 않은 응답은, 누가 달라고 하든 압축되지 않는다. */
    EXAMPLE_REQUIRE(get(decoding, at.port, "/account", &read_size, &sent, &coding, &body) && coding == PROVEN_HTTP_CODING_IDENTITY, "the account page is sent as it is");

    EXAMPLE_REQUIRE(get(decoding, at.port, "/accepts", &read_size, &sent, &coding, &body) &&
                    proven_u8str_view_eq(proven_u8str_as_view(&body), PROVEN_LIT("gzip")), "the handler can see that this client accepts gzip");
    EXAMPLE_REQUIRE(get(plain, at.port, "/accepts", &read_size, &sent, &coding, &body) &&
                    proven_u8str_view_eq(proven_u8str_as_view(&body), PROVEN_LIT("identity")), "and that the other does not");

    EXAMPLE_REQUIRE(get(plain, at.port, "/quit", &read_size, &sent, &coding, &body), "stop");
    proven_job_group_wait(threads, &running);
    proven_u8str_destroy(heap, &body);
    proven_http_client_destroy(plain);
    proven_http_client_destroy(decoding);
    proven_http_server_destroy(server);
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    return EXAMPLE_OK();
}
