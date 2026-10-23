#include "example.h"
#include <stdio.h>
#include <string.h>

/*
 * HTTP/2로 말하는 이벤트 구동 클라이언트: 요청 셋, 연결 하나.
 *
 * 여기서는 클라이언트와 서버가 한 루프 위에 있고 손으로 돌리므로 스레드가 필요 없다.
 * 클라이언트의 콜백은 HTTP/1.1 프로그램의 것과 같다. 다른 것은 그 아래다 - 클라이언트가
 * 오리진마다 연결 하나를 간직하고, 그 오리진으로 가는 모든 요청을 그 연결에 올린다.
 */

typedef struct { int answered, over_http2; char text[3][32]; bool done[3]; } ex_seen_t;
typedef struct { ex_seen_t *seen; int which; } ex_call_t;

static void serve(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    (void)ctx;
    (void)proven_http_stream_respond(stream, 200, NULL, 0, (proven_mem_view_t){ head->target.ptr, head->target.size });     /* 자기 경로를 그대로 돌려준다 */
}

static void on_response(void *ctx, proven_http_event_request_t *request, const proven_http_response_t *head) {
    ex_call_t *call = ctx;
    /* 여기서는 2다. 아직 연결을 기다리는 요청이나, 서버가 HTTP/1.1만 말하는 것으로 드러난
     * 요청은 1이다. */
    if (head->status == 200 && proven_http_event_request_protocol(request) == 2) call->seen->over_http2++;
}

static void on_body(void *ctx, proven_http_event_request_t *request, proven_mem_view_t piece, bool last) {
    (void)request; (void)last;
    ex_call_t *call = ctx;
    snprintf(call->seen->text[call->which], sizeof call->seen->text[0], "%.*s", (int)piece.size, (const char *)piece.ptr);
}

static void on_done(void *ctx, proven_http_event_request_t *request, proven_err_t why) {
    (void)request;
    ex_call_t *call = ctx;
    call->seen->done[call->which] = true;
    if (why == PROVEN_OK) call->seen->answered++;
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    proven_loop_t *loop = NULL;
    EXAMPLE_REQUIRE(proven_loop_create(heap, &loop) == PROVEN_OK, "a loop");

    /* 두 프로토콜을 모두 말하는 서버(6절). */
    proven_http_event_server_config_t server_config = { .on = { .on_request = serve }, .http2 = true };
    proven_http_event_server_t *server = NULL;
    proven_net_addr_t at;
    EXAMPLE_REQUIRE(proven_http_event_server_create(loop, &server_config, &server) == PROVEN_OK &&
                    proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "a server");

    /* 평문에는 물어볼 핸드셰이크가 없으므로 클라이언트에게 알려 준다: 이 클라이언트의 모든
     * `http` 요청은 HTTP/2다. TLS에서는 대신 `http2`이고, "h2"를 내놓는 TLS 설정과 함께 쓰며,
     * 오리진마다 핸드셰이크가 정한다. */
    proven_http_event_client_config_t client_config = { .alloc = heap, .http2_cleartext = true };
    proven_http_event_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_event_client_create(loop, &client_config, &client) == PROVEN_OK, "a client");

    ex_seen_t seen = { 0 };
    ex_call_t calls[3] = { { &seen, 0 }, { &seen, 1 }, { &seen, 2 } };
    static const char *const paths[3] = { "/first", "/second", "/third" };
    for (int i = 0; i < 3; i++) {
        char url[64];
        snprintf(url, sizeof url, "http://127.0.0.1:%u%s", (unsigned)at.port, paths[i]);
        proven_http_event_request_options_t request = {
            .url = proven_u8str_view_from_cstr(url),
            .on = { .on_response = on_response, .on_body = on_body, .on_done = on_done }, .ctx = &calls[i],
        };
        /* 연결이 생기기 전에 한꺼번에 시작한다: 첫 요청이 연결을 만들고 나머지는 그것을
         * 기다리며, 서버가 무엇을 허락하는지 말한 뒤 셋이 함께 나간다. */
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
