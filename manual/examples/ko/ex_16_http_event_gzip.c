#include "example.h"
#include <stdio.h>
#include <string.h>

/*
 * 같은 일을 루프 위에서: 조각조각 쓰는 응답을 압축하는 이벤트 구동 서버와, 도착하는 대로
 * 풀어 읽는 이벤트 구동 클라이언트.
 *
 * 어느 쪽도 본문을 쥐고 있지 않는다. 서버는 받은 것을 압축해 출력 큐에 넣되 큐의 한도를
 * 지키고, 클라이언트는 풀린 조각을 최대 16 KiB씩 넘겨준다.
 */

#define REPORT_BYTES ((proven_size_t)300000)

typedef struct {
    proven_loop_t *loop;
    proven_http_stream_t *stream;      /* 쓰고 있는 응답 하나 */
    proven_size_t written;
    proven_size_t received, pieces, largest;
    unsigned long sum_sent, sum_received;
    bool gzip, intact, done;
    proven_err_t why;
} app_t;

/* 보고서는 쓰면서 만들어진다: 전체를 담은 버퍼는 없다. */
static proven_byte_t report_byte(proven_size_t i) { return (proven_byte_t)("measured value \n"[i % 16]); }

// ---- 서버 ------------------------------------------------------------------

/* 서버가 더 받지 않을 때까지 쓴다. 자리가 나면 on_writable이 이것을 다시 부른다. */
static void pump(app_t *app) {
    proven_byte_t piece[4096];
    while (app->written < REPORT_BYTES) {
        proven_size_t n = REPORT_BYTES - app->written < sizeof piece ? REPORT_BYTES - app->written : sizeof piece;
        for (proven_size_t i = 0; i < n; ++i) piece[i] = report_byte(app->written + i);
        proven_result_size_t took = proven_http_stream_write(app->stream, (proven_mem_view_t){ piece, n });
        if (took.err != PROVEN_OK) return;
        for (proven_size_t i = 0; i < took.value; ++i) app->sum_sent += piece[i];
        app->written += took.value;
        if (took.value < n) return;        /* 압축된 출력이 한도에 닿았다: 기다린다 */
    }
    (void)proven_http_stream_end(app->stream);
}

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    (void)head;
    app_t *app = ctx;
    app->stream = stream;
    /* 응답을 시작하기 전에. 여기서부터 본문은 이것이 없을 때와 똑같이 쓴다:
     * 클라이언트에게는 gzip 청크가 가지만, 알려 준 길이는 여전히 써야 할 길이다.
     * 쓴 것은 뒤따르는 것이 올 때까지 압축기 안에서 기다릴 수 있다. */
    proven_http_stream_compress(stream);
    if (proven_http_stream_begin(stream, 200, NULL, 0, REPORT_BYTES) == PROVEN_OK) pump(app);
}
static void on_writable(void *ctx, proven_http_stream_t *stream) { (void)stream; pump(ctx); }

// ---- 클라이언트 ------------------------------------------------------------

static void on_response(void *ctx, proven_http_event_request_t *request, const proven_http_response_t *head) {
    (void)request;
    app_t *app = ctx;
    app->gzip = proven_http_content_coding(head->headers, head->header_count) == PROVEN_HTTP_CODING_GZIP;
}

/* 풀린 조각들. 조각은 이 함수가 돌아갈 때까지만 유효한 뷰다. */
static void on_body(void *ctx, proven_http_event_request_t *request, proven_mem_view_t piece, bool last) {
    (void)request; (void)last;
    app_t *app = ctx;
    for (proven_size_t i = 0; i < piece.size; ++i) {
        if (piece.ptr[i] != report_byte(app->received + i)) app->intact = false;
        app->sum_received += piece.ptr[i];
    }
    app->received += piece.size;
    app->pieces++;
    if (piece.size > app->largest) app->largest = piece.size;
}

static void on_done(void *ctx, proven_http_event_request_t *request, proven_err_t why) {
    (void)request;
    app_t *app = ctx;
    app->why = why;
    app->done = true;
    proven_loop_stop(app->loop);
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    app_t app = { .intact = true };
    EXAMPLE_REQUIRE(proven_loop_create(heap, &app.loop) == PROVEN_OK, "a loop");

    /* 창 크기가 압축 응답 하나가 쓰이는 동안 쥐는 메모리를 정한다: 12비트에서 약 53 KiB,
     * 최대인 15비트에서 325 KiB. 한꺼번에 많이 다룬다면 골라야 할 숫자가 이것이다. */
    proven_http_event_server_config_t server_config = { .on = { .on_request = on_request, .on_writable = on_writable }, .ctx = &app, .compress_window_bits = 12 };
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

    /* `decompress`는 gzip을 요청하고 풀어 준다. 그러면 `max_body_bytes`는 풀린 크기에도
     * 걸리므로, 작은 본문이 거대하게 펼쳐지더라도 한도에서 끊긴다. */
    proven_http_event_client_config_t client_config = { .decompress = true, .max_body_bytes = 1024 * 1024 };
    proven_http_event_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_event_client_create(app.loop, &client_config, &client) == PROVEN_OK, "a client that decodes");

    char url[64];
    int n = snprintf(url, sizeof url, "http://127.0.0.1:%u/report", (unsigned)at.port);
    proven_http_event_request_options_t options = {
        .url = { (const proven_byte_t *)url, (proven_size_t)n },
        .on = { on_response, on_body, NULL, on_done },
        .ctx = &app,
    };
    EXAMPLE_REQUIRE(proven_http_event_client_start(client, &options, NULL) == PROVEN_OK, "the request is started");
    EXAMPLE_REQUIRE(proven_loop_run(app.loop) == PROVEN_OK && app.done, "the loop ran until it was done");

    EXAMPLE_REQUIRE(app.why == PROVEN_OK && app.gzip, "the response came as gzip");
    EXAMPLE_REQUIRE(app.received == REPORT_BYTES && app.intact && app.sum_received == app.sum_sent, "and was delivered as the report that was written");
    EXAMPLE_REQUIRE(app.largest <= 16384 && app.pieces >= REPORT_BYTES / 16384, "in pieces of at most 16 KiB");
    printf("%u bytes written, compressed, sent, decoded and received\n", (unsigned)app.received);

    proven_http_event_client_destroy(client);
    proven_http_event_server_destroy(server);
    proven_loop_destroy(app.loop);
    return EXAMPLE_OK();
}
