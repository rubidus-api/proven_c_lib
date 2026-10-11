#include "example.h"
#include <stdio.h>
#include <string.h>

/*
 * The same on a loop: the event-driven server compressing a response that is written in
 * pieces, and the event-driven client decoding it as it arrives.
 *
 * Neither holds the body. The server compresses what it is given into its output queue, under
 * the queue's limit; the client hands on decoded pieces of at most 16 KiB.
 */

#define REPORT_BYTES ((proven_size_t)300000)

typedef struct {
    proven_loop_t *loop;
    proven_http_stream_t *stream;      /* the one response being written */
    proven_size_t written;
    proven_size_t received, pieces, largest;
    unsigned long sum_sent, sum_received;
    bool gzip, intact, done;
    proven_err_t why;
} app_t;

/* The report is generated as it is written: there is no buffer holding all of it. */
static proven_byte_t report_byte(proven_size_t i) { return (proven_byte_t)("measured value \n"[i % 16]); }

// ---- the server --------------------------------------------------------------

/* Write until the server takes no more; on_writable calls this again when there is room. */
static void pump(app_t *app) {
    proven_byte_t piece[4096];
    while (app->written < REPORT_BYTES) {
        proven_size_t n = REPORT_BYTES - app->written < sizeof piece ? REPORT_BYTES - app->written : sizeof piece;
        for (proven_size_t i = 0; i < n; ++i) piece[i] = report_byte(app->written + i);
        proven_result_size_t took = proven_http_stream_write(app->stream, (proven_mem_view_t){ piece, n });
        if (took.err != PROVEN_OK) return;
        for (proven_size_t i = 0; i < took.value; ++i) app->sum_sent += piece[i];
        app->written += took.value;
        if (took.value < n) return;        /* the compressed output is at its limit: wait */
    }
    (void)proven_http_stream_end(app->stream);
}

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    (void)head;
    app_t *app = ctx;
    app->stream = stream;
    /* Before the response begins. From here on the body is written exactly as it would be
     * without it: the length given is still the length to write, though the client is sent
     * chunks of gzip. What is written may wait in the compressor until more follows. */
    proven_http_stream_compress(stream);
    if (proven_http_stream_begin(stream, 200, NULL, 0, REPORT_BYTES) == PROVEN_OK) pump(app);
}
static void on_writable(void *ctx, proven_http_stream_t *stream) { (void)stream; pump(ctx); }

// ---- the client --------------------------------------------------------------

static void on_response(void *ctx, proven_http_event_request_t *request, const proven_http_response_t *head) {
    (void)request;
    app_t *app = ctx;
    app->gzip = proven_http_content_coding(head->headers, head->header_count) == PROVEN_HTTP_CODING_GZIP;
}

/* Decoded pieces. A piece is a view that is good until this function returns. */
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

    /* The window sets what each compressed response holds while it is being written: about
     * 53 KiB at 12 bits, 325 KiB at the full 15. With many at once, that is the number to choose. */
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

    /* `decompress` asks for gzip and decodes it; `max_body_bytes` then also bounds the decoded
     * size, so a small body that unfolds into a huge one is cut off at the limit. */
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
