#include "example.h"
#include <stdio.h>
#include <string.h>

/*
 * Trailers on the loop: fields after a body. A client uploads with a checksum it knows only at
 * the end; the server checks it after the body and answers with a trailer of its own. The same
 * calls work over HTTP/2 and HTTP/3 - there the trailers are a HEADERS frame that ends the stream.
 */

static unsigned sum_of(const proven_byte_t *p, proven_size_t n, unsigned s) { for (proven_size_t i = 0; i < n; ++i) s = s * 31 + p[i]; return s; }

// ---- the server -------------------------------------------------------------

typedef struct { unsigned sum; } ex_upload_t;
static ex_upload_t g_upload;                    /* one request here; a real server keeps one per stream (proven_http_stream_set_user) */

static void serve(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    (void)ctx; (void)stream; (void)head;
    g_upload.sum = 0;
}

static void serve_body(void *ctx, proven_http_stream_t *stream, proven_mem_view_t piece, bool last) {
    (void)ctx;
    g_upload.sum = sum_of(piece.ptr, piece.size, g_upload.sum);
    if (!last) return;
    /* The body is over: its trailers are here, until on_done returns. */
    proven_http_header_t trailers[4];
    proven_size_t count = 0;
    proven_u8str_view_t claimed = { 0 };
    char mine[16];
    const int ml = snprintf(mine, sizeof mine, "%u", g_upload.sum);
    const bool good = proven_http_stream_trailers(stream, trailers, 4, &count) == PROVEN_OK &&
                      proven_http_header_find(trailers, count, PROVEN_LIT("X-Checksum"), &claimed) &&
                      proven_u8str_view_eq(claimed, (proven_u8str_view_t){ (const proven_byte_t *)mine, (proven_size_t)ml });
    /* Trailers follow a body of no stated length. */
    if (proven_http_stream_begin(stream, 200, NULL, 0, PROVEN_HTTP_EVENT_LENGTH_UNKNOWN) != PROVEN_OK) return;
    (void)proven_http_stream_write(stream, proven_mem_view_from_u8(PROVEN_LIT("stored\n")));
    const proven_http_header_t status = { PROVEN_LIT("X-Upload"), good ? PROVEN_LIT("verified") : PROVEN_LIT("corrupt") };
    (void)proven_http_stream_end_trailers(stream, &status, 1);
}

// ---- the client -------------------------------------------------------------

typedef struct {
    proven_loop_t *loop;
    proven_http_event_request_t *request;
    proven_mem_view_t data;
    proven_size_t sent;
    bool ended;
    char verdict[16];
    proven_err_t why;
} ex_put_t;

/* Write what the client takes; when it is all written, end with the checksum. */
static void put_more(ex_put_t *p) {
    while (p->sent < p->data.size) {
        proven_result_size_t w = proven_http_event_request_write(p->request, (proven_mem_view_t){ p->data.ptr + p->sent, p->data.size - p->sent });
        if (w.err != PROVEN_OK || w.value == 0) return;            /* on_writable says when to go on */
        p->sent += w.value;
    }
    if (p->ended) return;
    char sum[16];
    const int sl = snprintf(sum, sizeof sum, "%u", sum_of(p->data.ptr, p->data.size, 0));
    const proven_http_header_t trailer = { PROVEN_LIT("X-Checksum"), { (const proven_byte_t *)sum, (proven_size_t)sl } };
    p->ended = proven_http_event_request_end_trailers(p->request, &trailer, 1) == PROVEN_OK;    /* the fields are copied */
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

    /* A field that frames the message may not be a trailer; a checksum may. */
    EXAMPLE_REQUIRE(!proven_http_trailer_allowed(PROVEN_LIT("Content-Length")) && proven_http_trailer_allowed(PROVEN_LIT("X-Checksum")), "what may be a trailer");

    char url[64];
    const int n = snprintf(url, sizeof url, "http://127.0.0.1:%u/upload", (unsigned)at.port);
    ex_put_t put = { .loop = loop, .data = proven_mem_view_from_u8(PROVEN_LIT("the bytes of a file, sent as they are read")) };
    proven_http_event_request_options_t options = {
        .method = PROVEN_LIT("PUT"), .url = { (const proven_byte_t *)url, (proven_size_t)n },
        .body_length = PROVEN_HTTP_EVENT_LENGTH_UNKNOWN,         /* written afterwards, with no length: so trailers can follow */
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
