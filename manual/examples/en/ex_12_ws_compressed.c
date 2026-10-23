#include "example.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Compressed WebSocket messages, the way a program uses them: one field in a configuration.
 *
 * The server here is the event-driven one, the client the blocking one. Each sets `compress`;
 * the handshake agrees; after that every call is used exactly as before, and what crosses
 * the network is a fraction of what was said.
 */

typedef struct {
    proven_loop_t *loop;
    bool compressed;                   /* what the server found agreed */
    proven_size_t received, largest, pieces;
} ex_app_t;

/* A compressed message arrives decoded, in pieces of at most 16 KiB. Nothing holds the whole
 * of it unless you do. This handler counts, and answers when the message is complete. */
static void on_message(void *ctx, proven_ws_stream_t *ws, bool text, proven_mem_view_t piece, bool first, bool last) {
    ex_app_t *app = ctx;
    (void)text;
    if (first) { app->received = 0; app->pieces = 0; app->largest = 0; }
    app->received += piece.size;
    app->pieces++;
    if (piece.size > app->largest) app->largest = piece.size;
    if (!last) return;
    char answer[64];
    int n = snprintf(answer, sizeof answer, "%u bytes", (unsigned)app->received);
    /* Sent like any message. Short ones go as they are: under 64 bytes there is nothing to gain. */
    (void)proven_ws_stream_send(ws, true, (proven_mem_view_t){ (const proven_byte_t *)answer, (proven_size_t)n });
}

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    ex_app_t *app = ctx;
    proven_ws_event_config_t config = {
        .on = { .on_message = on_message },
        .ctx = app,
        .compress = true,              /* accept the extension when the client offers it */
        .max_message_bytes = 256 * 1024,   /* the limit on a message AS DECODED: a few bytes cannot become a gigabyte */
    };
    proven_ws_stream_t *ws = NULL;
    if (proven_ws_event_accept(stream, head, &config, &ws) != PROVEN_OK) {
        (void)proven_http_stream_respond(stream, 400, NULL, 0, (proven_mem_view_t){ 0 });
        return;
    }
    /* Whether it was agreed depends on the client too. Nothing else has to be done either way. */
    app->compressed = proven_ws_stream_compressed(ws);
    /* The same switch on this side, for a server whose answers must not be compressed:
     * proven_ws_stream_set_compress(ws, false). Here they may be. */
    proven_ws_stream_set_compress(ws, true);
}

static void run(void *arg) { (void)proven_loop_run(((ex_app_t *)arg)->loop); }

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    ex_app_t app = { 0 };
    EXAMPLE_REQUIRE(proven_loop_create(heap, &app.loop) == PROVEN_OK, "a loop");
    proven_http_event_server_config_t server_config = { .on = { .on_request = on_request }, .ctx = &app };
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
    proven_job_sys_t *threads = NULL;
    proven_job_group_t running;
    proven_job_group_init(&running);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &threads) == PROVEN_OK &&
                    proven_job_group_submit(threads, &running, run, &app) == PROVEN_OK, "the loop runs on another thread");

    // ---- a client that asks for compression ---------------------------------------
    proven_http_client_config_t client_config = { .alloc = heap };
    proven_http_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&client_config, &client) == PROVEN_OK, "an HTTP client");
    char url[64];
    int n = snprintf(url, sizeof url, "ws://127.0.0.1:%u/", (unsigned)at.port);
    proven_ws_conn_config_t ws_config = { .alloc = heap, .compress = true };       /* offer the extension */
    proven_ws_conn_t *conn = NULL;
    EXAMPLE_REQUIRE(proven_ws_conn_connect(client, (proven_u8str_view_t){ (const proven_byte_t *)url, (proven_size_t)n }, NULL, 0, PROVEN_LIT(""),
                                           &ws_config, &conn, NULL) == PROVEN_OK, "connected");
    EXAMPLE_REQUIRE(proven_ws_conn_compressed(conn), "and the server agreed to compress");

    /* A hundred thousand bytes of a report. On the wire it is a few hundred. */
    static char report[100000];
    for (proven_size_t i = 0; i < sizeof report; ++i) report[i] = "temperature 21.5, humidity 40\n"[i % 30];
    proven_ws_message_t msg;
    EXAMPLE_REQUIRE(proven_ws_conn_send_text(conn, (proven_u8str_view_t){ (const proven_byte_t *)report, sizeof report }) == PROVEN_OK &&
                    proven_ws_conn_receive(conn, proven_net_deadline_in(5000), &msg) == PROVEN_OK &&
                    msg.data.size == 12 && memcmp(msg.data.ptr, "100000 bytes", 12) == 0, "the server received all of it");

    /* The limit is on the decoded size. Three hundred thousand of one letter are a few hundred
     * bytes compressed - and still too much: the server ends the connection with 1009. */
    /* One message kept out of the compressor - what a message that carries a secret beside
     * text the other side chose needs. The switch is read when a message begins. */
    proven_ws_conn_set_compress(conn, false);
    EXAMPLE_REQUIRE(proven_ws_conn_send_text(conn, (proven_u8str_view_t){ (const proven_byte_t *)report, 1000 }) == PROVEN_OK &&
                    proven_ws_conn_receive(conn, proven_net_deadline_in(5000), &msg) == PROVEN_OK &&
                    msg.data.size == 10 && memcmp(msg.data.ptr, "1000 bytes", 10) == 0, "a thousand bytes, sent as they are");
    proven_ws_conn_set_compress(conn, true);

    static char flood[300000];
    memset(flood, 'x', sizeof flood);
    EXAMPLE_REQUIRE(proven_ws_conn_send_text(conn, (proven_u8str_view_t){ (const proven_byte_t *)flood, sizeof flood }) == PROVEN_OK, "a message that inflates past the server's limit");
    EXAMPLE_REQUIRE(proven_ws_conn_receive(conn, proven_net_deadline_in(5000), &msg) == PROVEN_ERR_EOF && proven_ws_conn_close_code(conn) == 1009, "is answered with 1009, however small it was on the wire");
    proven_ws_conn_destroy(conn);
    proven_http_client_destroy(client);

    proven_loop_stop(app.loop);
    proven_job_group_wait(threads, &running);
    EXAMPLE_REQUIRE(app.compressed, "the server found the extension agreed as well");
    proven_http_event_server_destroy(server);
    proven_loop_destroy(app.loop);
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    return EXAMPLE_OK();
}
