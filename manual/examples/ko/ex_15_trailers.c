#include "example.h"
#include <stdio.h>
#include <string.h>

/*
 * 루프 위의 트레일러: 본문 뒤의 필드. 클라이언트가 끝에 가서야 아는 체크섬을 붙여 올리고, 서버가
 * 본문 뒤에 그것을 확인해 자기 트레일러로 답한다. 같은 호출이 HTTP/2와 HTTP/3에서도 된다 - 거기서
 * 트레일러는 스트림을 끝내는 HEADERS 프레임이다.
 */

static unsigned sum_of(const proven_byte_t *p, proven_size_t n, unsigned s) { for (proven_size_t i = 0; i < n; ++i) s = s * 31 + p[i]; return s; }

// ---- 서버 -------------------------------------------------------------

typedef struct { unsigned sum; } ex_upload_t;
static ex_upload_t g_upload;                    /* 여기서는 요청 하나; 실제 서버는 스트림마다 하나씩 둔다(proven_http_stream_set_user) */

static void serve(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    (void)ctx; (void)stream; (void)head;
    g_upload.sum = 0;
}

static void serve_body(void *ctx, proven_http_stream_t *stream, proven_mem_view_t piece, bool last) {
    (void)ctx;
    g_upload.sum = sum_of(piece.ptr, piece.size, g_upload.sum);
    if (!last) return;
    /* 본문이 끝났다: 트레일러가 여기 있다, on_done이 돌아갈 때까지. */
    proven_http_header_t trailers[4];
    proven_size_t count = 0;
    proven_u8str_view_t claimed = { 0 };
    char mine[16];
    const int ml = snprintf(mine, sizeof mine, "%u", g_upload.sum);
    const bool good = proven_http_stream_trailers(stream, trailers, 4, &count) == PROVEN_OK &&
                      proven_http_header_find(trailers, count, PROVEN_LIT("X-Checksum"), &claimed) &&
                      proven_u8str_view_eq(claimed, (proven_u8str_view_t){ (const proven_byte_t *)mine, (proven_size_t)ml });
    /* 트레일러는 길이를 밝히지 않은 본문 뒤에 온다. */
    if (proven_http_stream_begin(stream, 200, NULL, 0, PROVEN_HTTP_EVENT_LENGTH_UNKNOWN) != PROVEN_OK) return;
    (void)proven_http_stream_write(stream, proven_mem_view_from_u8(PROVEN_LIT("stored\n")));
    const proven_http_header_t status = { PROVEN_LIT("X-Upload"), good ? PROVEN_LIT("verified") : PROVEN_LIT("corrupt") };
    (void)proven_http_stream_end_trailers(stream, &status, 1);
}

// ---- 클라이언트 -------------------------------------------------------------

typedef struct {
    proven_loop_t *loop;
    proven_http_event_request_t *request;
    proven_mem_view_t data;
    proven_size_t sent;
    bool ended;
    char verdict[16];
    proven_err_t why;
} ex_put_t;

/* 클라이언트가 받는 만큼 쓰고, 다 쓰면 체크섬으로 끝낸다. */
static void put_more(ex_put_t *p) {
    while (p->sent < p->data.size) {
        proven_result_size_t w = proven_http_event_request_write(p->request, (proven_mem_view_t){ p->data.ptr + p->sent, p->data.size - p->sent });
        if (w.err != PROVEN_OK || w.value == 0) return;            /* 언제 이어 갈지는 on_writable이 알려 준다 */
        p->sent += w.value;
    }
    if (p->ended) return;
    char sum[16];
    const int sl = snprintf(sum, sizeof sum, "%u", sum_of(p->data.ptr, p->data.size, 0));
    const proven_http_header_t trailer = { PROVEN_LIT("X-Checksum"), { (const proven_byte_t *)sum, (proven_size_t)sl } };
    p->ended = proven_http_event_request_end_trailers(p->request, &trailer, 1) == PROVEN_OK;    /* 필드는 복사된다 */
}

static void on_writable(void *ctx, proven_http_event_request_t *request) { (void)request; put_more(ctx); }

static void on_body(void *ctx, proven_http_event_request_t *request, proven_mem_view_t piece, bool last) {
    (void)piece;
    ex_put_t *p = ctx;
    if (!last) return;
    proven_http_header_t trailers[4];
    proven_size_t count = 0;
    proven_u8str_view_t v = { 0 };
    if (proven_http_event_request_trailers(request, trailers, 4, &count) == PROVEN_OK && proven_http_header_find(trailers, count, PROVEN_LIT("X-Upload"), &v))
        memcpy(p->verdict, v.ptr, v.size < sizeof p->verdict - 1 ? v.size : sizeof p->verdict - 1);
}

static void on_done(void *ctx, proven_http_event_request_t *request, proven_err_t why) {
    (void)request;
    ex_put_t *p = ctx;
    p->why = why;
    proven_loop_stop(p->loop);
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    proven_loop_t *loop = NULL;
    EXAMPLE_REQUIRE(proven_loop_create(heap, &loop) == PROVEN_OK, "a loop");
    proven_http_event_server_config_t server_config = { .on = { .on_request = serve, .on_body = serve_body } };
    proven_http_event_server_t *server = NULL;
    proven_net_addr_t at;
    EXAMPLE_REQUIRE(proven_http_event_server_create(loop, &server_config, &server) == PROVEN_OK &&
                    proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "a server");
    proven_http_event_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_event_client_create(loop, NULL, &client) == PROVEN_OK, "a client");

    /* 메시지의 틀을 정하는 필드는 트레일러일 수 없다; 체크섬은 될 수 있다. */
    EXAMPLE_REQUIRE(!proven_http_trailer_allowed(PROVEN_LIT("Content-Length")) && proven_http_trailer_allowed(PROVEN_LIT("X-Checksum")), "what may be a trailer");

    char url[64];
    const int n = snprintf(url, sizeof url, "http://127.0.0.1:%u/upload", (unsigned)at.port);
    ex_put_t put = { .loop = loop, .data = proven_mem_view_from_u8(PROVEN_LIT("the bytes of a file, sent as they are read")) };
    proven_http_event_request_options_t options = {
        .method = PROVEN_LIT("PUT"), .url = { (const proven_byte_t *)url, (proven_size_t)n },
        .body_length = PROVEN_HTTP_EVENT_LENGTH_UNKNOWN,         /* 나중에, 길이 없이 쓴다: 그래서 트레일러가 뒤따를 수 있다 */
        .on = { NULL, on_body, on_writable, on_done }, .ctx = &put,
    };
    EXAMPLE_REQUIRE(proven_http_event_client_start(client, &options, &put.request) == PROVEN_OK, "the upload started");
    put_more(&put);
    EXAMPLE_REQUIRE(proven_loop_run(loop) == PROVEN_OK, "the loop ran until the upload was done");
    EXAMPLE_REQUIRE(put.why == PROVEN_OK && put.ended && strcmp(put.verdict, "verified") == 0, "the server says, after its body, that the checksum matched");

    proven_http_event_client_destroy(client);
    proven_http_event_server_destroy(server);
    proven_loop_destroy(loop);
    return EXAMPLE_OK();
}
