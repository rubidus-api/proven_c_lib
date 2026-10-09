#include "example.h"

/*
 * Changing protocol: a request asks to stop speaking HTTP on this connection, the server
 * agrees with a 101, and from then on the same connection carries something else.
 *
 * The "something else" here is a toy - lines of text, sent back in capitals - because the
 * point is the hand-over. WebSocket is the protocol people usually mean, and chapter 12's
 * connection does these steps for you.
 */

static proven_http_server_t *g_server;

static void handle(void *ctx, proven_http_exchange_t *x) {
    (void)ctx;
    const proven_http_request_t *request = proven_http_exchange_request(x);
    /* Is this a request to change to OUR protocol? Checking is the handler's job. */
    if (!proven_http_header_has_token(request->headers, request->header_count, PROVEN_LIT("Upgrade"), PROVEN_LIT("shout")) ||
        !proven_http_header_has_token(request->headers, request->header_count, PROVEN_LIT("Connection"), PROVEN_LIT("Upgrade"))) {
        (void)proven_http_exchange_respond(x, 426, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("this address speaks only shout\n")));
        return;
    }

    /* The server writes the 101, and the connection is ours: a transport that owns the socket. */
    proven_transport_t t;
    proven_mem_view_t early;                /* bytes the client sent without waiting for the 101 */
    if (proven_http_exchange_upgrade(x, PROVEN_LIT("shout"), NULL, 0, &t, &early) != PROVEN_OK) return;

    /* From here on this is not HTTP and none of the server's limits apply: every wait needs
     * a deadline of our own. `early` dies when the handler returns, so use it first. */
    proven_byte_t line[64];
    proven_size_t n = early.size < sizeof line ? early.size : sizeof line;
    for (proven_size_t i = 0; i < n; ++i) line[i] = early.ptr[i];
    while (n < sizeof line && (n == 0 || line[n - 1] != '\n')) {
        proven_result_size_t got = proven_transport_read(t, (proven_mem_mut_t){ line + n, sizeof line - n }, proven_net_deadline_in(5000));
        if (got.err != PROVEN_OK) break;
        n += got.value;
    }
    for (proven_size_t i = 0; i < n; ++i) if (line[i] >= 'a' && line[i] <= 'z') line[i] = (proven_byte_t)(line[i] - 32);
    (void)proven_transport_write_all(t, (proven_mem_view_t){ line, n }, proven_net_deadline_in(5000));
    /* It could also be kept and used after the handler returns. Either way, closing it is ours. */
    (void)proven_transport_close(t);
}

static void serve(void *arg) {
    (void)arg;
    (void)proven_http_server_run(g_server);
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    proven_http_server_config_t server_config = {0};
    server_config.alloc = heap;
    server_config.handler = handle;
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

    proven_http_client_config_t client_config = { .alloc = heap, .max_idle_connections = 2 };
    proven_http_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&client_config, &client) == PROVEN_OK, "a client");
    char url[64];
    int url_len = snprintf(url, sizeof url, "http://127.0.0.1:%u/", (unsigned)at.port);

    /* `upgrade` names the protocol; the client writes `Connection: Upgrade` and `Upgrade`. */
    proven_http_client_request_t request = { .url = { (const proven_byte_t *)url, (proven_size_t)url_len }, .upgrade = PROVEN_LIT("shout") };
    proven_http_client_response_t response;
    err = proven_http_client_send(client, &request, &response);
    EXAMPLE_REQUIRE(err == PROVEN_OK && response.status == 101, "the server agreed: 101 Switching Protocols");

    /* A 101 is a response like any other until you take its connection. */
    proven_transport_t t;
    proven_mem_view_t early;
    EXAMPLE_REQUIRE(proven_http_client_upgrade(&response, &t, &early) == PROVEN_OK, "the connection is ours now");
    proven_http_client_finish(&response);       /* still required; it no longer touches the connection */

    proven_byte_t reply[16];
    proven_size_t have = 0;
    EXAMPLE_REQUIRE(proven_transport_write_all(t, proven_mem_view_from_u8(PROVEN_LIT("hello\n")), proven_net_deadline_in(5000)).err == PROVEN_OK, "a line, in the new protocol");
    while (have < 6) {
        proven_result_size_t got = proven_transport_read(t, (proven_mem_mut_t){ reply + have, sizeof reply - have }, proven_net_deadline_in(5000));
        if (got.err != PROVEN_OK) break;
        have += got.value;
    }
    EXAMPLE_REQUIRE(have == 6 && proven_u8str_view_eq((proven_u8str_view_t){ reply, 6 }, PROVEN_LIT("HELLO\n")), "and the answer, in capitals");
    (void)proven_transport_close(t);            /* closes the socket and frees the transport */

    /* Without `upgrade` the same address answers as HTTP, and there is nothing to take. */
    err = proven_http_client_get(client, (proven_u8str_view_t){ (const proven_byte_t *)url, (proven_size_t)url_len }, &response);
    EXAMPLE_REQUIRE(err == PROVEN_OK && response.status == 426, "an ordinary request is told what this address speaks");
    EXAMPLE_REQUIRE(proven_http_client_upgrade(&response, &t, &early) == PROVEN_ERR_INVALID_STATE, "a response that is not a 101 has no connection to give");
    proven_http_client_finish(&response);

    proven_http_client_destroy(client);
    proven_http_server_stop(g_server);
    proven_job_group_wait(threads, &running);
    proven_http_server_destroy(g_server);
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    return EXAMPLE_OK();
}
