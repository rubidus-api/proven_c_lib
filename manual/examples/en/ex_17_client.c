#include "example.h"
#include <stdio.h>
#include <string.h>

/*
 * The event-driven client speaking HTTP/2: three requests, one connection.
 *
 * Client and server sit on one loop here, driven by hand, so the example needs no thread. The
 * client's callbacks are the ones an HTTP/1.1 program has; what differs is underneath - the
 * client keeps a connection for the origin and puts every request to it on that connection.
 */

typedef struct { int answered, over_http2; char text[3][32]; bool done[3]; } seen_t;
typedef struct { seen_t *seen; int which; } call_t;

static void serve(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    (void)ctx;
    (void)proven_http_stream_respond(stream, 200, NULL, 0, (proven_mem_view_t){ head->target.ptr, head->target.size });     /* its own path, back */
}

static void on_response(void *ctx, proven_http_event_request_t *request, const proven_http_response_t *head) {
    call_t *call = ctx;
    /* 2 here; 1 for a request that is still waiting for its connection, or whose server
     * turned out to speak only HTTP/1.1. */
    if (head->status == 200 && proven_http_event_request_protocol(request) == 2) call->seen->over_http2++;
}

static void on_body(void *ctx, proven_http_event_request_t *request, proven_mem_view_t piece, bool last) {
    (void)request; (void)last;
    call_t *call = ctx;
    snprintf(call->seen->text[call->which], sizeof call->seen->text[0], "%.*s", (int)piece.size, (const char *)piece.ptr);
}

static void on_done(void *ctx, proven_http_event_request_t *request, proven_err_t why) {
    (void)request;
    call_t *call = ctx;
    call->seen->done[call->which] = true;
    if (why == PROVEN_OK) call->seen->answered++;
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    proven_loop_t *loop = NULL;
    EXAMPLE_REQUIRE(proven_loop_create(heap, &loop) == PROVEN_OK, "a loop");

    /* A server that speaks both protocols (section 6). */
    proven_http_event_server_config_t server_config = { .on = { .on_request = serve }, .http2 = true };
    proven_http_event_server_t *server = NULL;
    proven_net_addr_t at;
    EXAMPLE_REQUIRE(proven_http_event_server_create(loop, &server_config, &server) == PROVEN_OK &&
                    proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "a server");

    /* In the clear there is no handshake to ask in, so the client is told: every `http`
     * request of this client is HTTP/2. Over TLS it is `http2` instead, with a TLS
     * configuration that offers "h2", and the handshake decides for each origin. */
    proven_http_event_client_config_t client_config = { .alloc = heap, .http2_cleartext = true };
    proven_http_event_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_event_client_create(loop, &client_config, &client) == PROVEN_OK, "a client");

    seen_t seen = { 0 };
    call_t calls[3] = { { &seen, 0 }, { &seen, 1 }, { &seen, 2 } };
    static const char *const paths[3] = { "/first", "/second", "/third" };
    for (int i = 0; i < 3; i++) {
        char url[64];
        snprintf(url, sizeof url, "http://127.0.0.1:%u%s", (unsigned)at.port, paths[i]);
        proven_http_event_request_options_t request = {
            .url = proven_u8str_view_from_cstr(url),
            .on = { .on_response = on_response, .on_body = on_body, .on_done = on_done }, .ctx = &calls[i],
        };
        /* Started at once, before there is a connection: the first makes it, the others wait
         * for it, and all three go out together when the server has said what it allows. */
        EXAMPLE_REQUIRE(proven_http_event_client_start(client, &request, NULL) == PROVEN_OK, "a request is started");
    }
    EXAMPLE_REQUIRE(proven_http_event_client_requests(client) == 3, "three are under way");

    proven_net_deadline_t until = proven_net_deadline_in(5000);
    while (!(seen.done[0] && seen.done[1] && seen.done[2]) && proven_time_monotonic_now() < until)
        EXAMPLE_REQUIRE(proven_loop_poll(loop, proven_net_deadline_in(10)) == PROVEN_OK, "a round of the loop");

    for (int i = 0; i < 3; i++) printf("%s\n", seen.text[i]);
    EXAMPLE_REQUIRE(seen.answered == 3 && seen.over_http2 == 3, "all three were answered, over HTTP/2");
    EXAMPLE_REQUIRE(strcmp(seen.text[0], "/first") == 0 && strcmp(seen.text[1], "/second") == 0 && strcmp(seen.text[2], "/third") == 0, "each with its own answer");
    EXAMPLE_REQUIRE(proven_http_event_server_connections(server) == 1, "and the server saw one connection, not three");

    proven_http_event_client_destroy(client);
    proven_http_event_server_destroy(server);
    proven_loop_destroy(loop);
    return EXAMPLE_OK();
}
