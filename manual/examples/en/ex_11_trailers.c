#include "example.h"
#include <string.h>

/*
 * Trailers: fields that come after a body, for what the sender knows only at the end - a checksum
 * of an upload, a status after a stream. Here a client uploads with a checksum as a trailer, the
 * server checks it and answers with a trailer of its own; then the same thing in the codec alone.
 */

static proven_http_server_t *g_server;

static unsigned sum_of(const proven_byte_t *p, proven_size_t n) { unsigned s = 0; for (proven_size_t i = 0; i < n; ++i) s = s * 31 + p[i]; return s; }

static void handle(void *ctx, proven_http_exchange_t *x) {
    (void)ctx;
    static proven_byte_t body[4096];
    proven_size_t n = 0;
    for (;;) {
        proven_result_size_t got = proven_http_exchange_read(x, (proven_mem_mut_t){ body + n, sizeof body - n });
        if (got.err != PROVEN_OK) break;           /* PROVEN_ERR_EOF: the body is over, and its trailers are here */
        n += got.value;
    }
    /* The trailers, once the body has been read to its end. */
    proven_http_header_t trailers[4];
    proven_size_t count = 0;
    proven_u8str_view_t claimed = { 0 };
    bool good = proven_http_exchange_trailers(x, trailers, 4, &count) == PROVEN_OK &&
                proven_http_header_find(trailers, count, PROVEN_LIT("X-Checksum"), &claimed);
    char mine[16];
    const int ml = snprintf(mine, sizeof mine, "%u", sum_of(body, n));
    good = good && claimed.size == (proven_size_t)ml && memcmp(claimed.ptr, mine, claimed.size) == 0;
    /* A response with trailers is written in pieces with no length, so that it goes chunked. */
    if (proven_http_exchange_begin(x, 200, NULL, 0, PROVEN_HTTP_LENGTH_UNKNOWN) != PROVEN_OK) return;
    (void)proven_http_exchange_write(x, proven_mem_view_from_u8(PROVEN_LIT("stored\n")));
    const proven_http_header_t status = { PROVEN_LIT("X-Upload"), good ? PROVEN_LIT("verified") : PROVEN_LIT("corrupt") };
    (void)proven_http_exchange_end_trailers(x, &status, 1);
}

static void serve(void *arg) {
    (void)arg;
    (void)proven_http_server_run(g_server);
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    proven_http_server_config_t server_config = { .alloc = heap, .handler = handle };
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
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &threads) == PROVEN_OK && proven_job_group_submit(threads, &running, serve, NULL) == PROVEN_OK,
                    "its loop runs on another thread");

    proven_http_client_config_t client_config = { .alloc = heap };
    proven_http_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&client_config, &client) == PROVEN_OK, "a client");
    char url[64];
    const int url_len = snprintf(url, sizeof url, "http://127.0.0.1:%u/upload", (unsigned)at.port);

    /* The upload: a body from a stream (so chunked), its checksum as a trailer after it. */
    static const char data[] = "the bytes of a file, sent as they are read";
    char sum[16];
    const int sl = snprintf(sum, sizeof sum, "%u", sum_of((const proven_byte_t *)data, sizeof data - 1));
    const proven_http_header_t trailer = { PROVEN_LIT("X-Checksum"), { (const proven_byte_t *)sum, (proven_size_t)sl } };
    EXAMPLE_REQUIRE(proven_http_trailer_allowed(trailer.name) && !proven_http_trailer_allowed(PROVEN_LIT("Content-Length")),
                    "a checksum may be a trailer; a field that frames the message may not");
    proven_reader_view_t rv;
    proven_http_client_request_t request = { .method = PROVEN_LIT("PUT"), .url = { (const proven_byte_t *)url, (proven_size_t)url_len },
                                             .body_stream = proven_reader_from_view(&rv, (proven_u8str_view_t){ (const proven_byte_t *)data, sizeof data - 1 }), .trailers = &trailer, .trailer_count = 1 };
    proven_http_client_response_t response;
    EXAMPLE_REQUIRE(proven_http_client_send(client, &request, &response) == PROVEN_OK && response.status == 200, "the upload is answered");
    proven_byte_t text[64];
    proven_size_t got = 0;
    for (;;) {
        proven_result_size_t r = proven_http_client_read(&response, (proven_mem_mut_t){ text + got, sizeof text - got });
        if (r.err != PROVEN_OK) break;
        got += r.value;
    }
    /* The response's trailers, after its body. */
    proven_http_header_t back[4];
    proven_size_t count = 0;
    proven_u8str_view_t verdict = { 0 };
    EXAMPLE_REQUIRE(proven_http_client_trailers(&response, back, 4, &count) == PROVEN_OK && proven_http_header_find(back, count, PROVEN_LIT("X-Upload"), &verdict) &&
                    proven_u8str_view_eq(verdict, PROVEN_LIT("verified")), "the server says, after its body, that the checksum matched");
    proven_http_client_finish(&response);
    proven_http_client_destroy(client);
    proven_http_server_stop(g_server);
    proven_job_group_wait(threads, &running);
    proven_http_server_destroy(g_server);
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);

    /* The codec alone: a trailer section written after the last chunk, and read back. */
    proven_byte_t wire[128];
    proven_size_t len = 0;
    const proven_http_header_t done = { PROVEN_LIT("grpc-status"), PROVEN_LIT("0") };
    EXAMPLE_REQUIRE(proven_http_write_chunk_begin((proven_mem_mut_t){ wire, sizeof wire }, &len, 2) == PROVEN_OK && (memcpy(wire + len, "ok", 2), len += 2, true) &&
                    proven_http_write_chunk_end((proven_mem_mut_t){ wire, sizeof wire }, &len) == PROVEN_OK &&
                    proven_http_write_trailers((proven_mem_mut_t){ wire, sizeof wire }, &len, &done, 1) == PROVEN_OK, "a chunked body of two bytes and a trailer");
    proven_http_body_t body;
    proven_byte_t keep[PROVEN_HTTP_MAX_TRAILER_BYTES];
    proven_size_t used = 0;
    proven_mem_view_t payload;
    bool ended = false;
    EXAMPLE_REQUIRE(proven_http_body_init(&body, (proven_http_framing_t){ .kind = PROVEN_HTTP_BODY_CHUNKED }, 1024) == PROVEN_OK &&
                    proven_http_body_keep_trailers(&body, (proven_mem_mut_t){ keep, sizeof keep }) == PROVEN_OK, "a decoder that keeps the trailers");
    for (proven_size_t at2 = 0; !ended && at2 < len; at2 += used)
        EXAMPLE_REQUIRE(proven_http_body_feed(&body, (proven_mem_view_t){ wire + at2, len - at2 }, &used, &payload, &ended) == PROVEN_OK, "the bytes");
    EXAMPLE_REQUIRE(ended && proven_http_body_trailers(&body, back, 4, &count) == PROVEN_OK && count == 1 && proven_u8str_view_eq(back[0].value, PROVEN_LIT("0")),
                    "and its trailer read back");
    return EXAMPLE_OK();
}
