#include "example.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * One thread waits for messages while another decides what to send - a chat client's shape.
 *
 * A blocking connection (Chapter 12) cannot do it: it is one thread's, and a lock around
 * proven_ws_conn_receive would hold every send for as long as the receive waits. The loop's
 * connection can. It waits for messages without a thread of its own, and another thread hands
 * it what to send with proven_loop_post: the send itself happens on the loop's thread, the
 * only one that touches the connection.
 *
 * One loop carries both ends here: a server that answers each message, and the client.
 */

#define LINES 3

typedef struct {
    proven_loop_t *loop;
    proven_ws_stream_t *ws;           /* the client's connection: the loop's thread only */
    atomic_bool open;                 /* what the typing thread may read */
    int answers, closed;
} ex_app_t;

// ---- the server: it answers each message ------------------------------------

static void served(void *ctx, proven_ws_stream_t *ws, bool text, proven_mem_view_t piece, bool first, bool last) {
    (void)ctx; (void)text; (void)first;
    if (!last) return;                      /* this example's messages are small: one piece each */
    char reply[80];
    int n = snprintf(reply, sizeof reply, "got: %.*s", (int)piece.size, (const char *)piece.ptr);
    (void)proven_ws_stream_send(ws, true, (proven_mem_view_t){ (const proven_byte_t *)reply, (proven_size_t)n });
}

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    (void)ctx;
    proven_ws_event_config_t config = { .on = { .on_message = served } };
    proven_ws_stream_t *ws = NULL;
    if (proven_ws_event_accept(stream, head, &config, &ws) != PROVEN_OK) (void)proven_http_stream_respond(stream, 400, NULL, 0, (proven_mem_view_t){ 0 });
}

// ---- the client, on the loop's thread -----------------------------------------

static void on_open(void *ctx, proven_ws_stream_t *ws, proven_u8str_view_t protocol) {
    ex_app_t *app = ctx;
    (void)protocol;
    app->ws = ws;
    atomic_store(&app->open, true);
}

static void on_message(void *ctx, proven_ws_stream_t *ws, bool text, proven_mem_view_t piece, bool first, bool last) {
    ex_app_t *app = ctx;
    (void)text; (void)first;
    if (!last) return;
    printf("the server says: %.*s\n", (int)piece.size, (const char *)piece.ptr);
    if (++app->answers == LINES) proven_ws_stream_close(ws, 1000, PROVEN_LIT("bye"));
}

static void on_closed(void *ctx, proven_ws_stream_t *ws, proven_u16 code, proven_err_t why) {
    ex_app_t *app = ctx;
    (void)ws; (void)code; (void)why;
    app->ws = NULL;
    app->closed++;
    proven_loop_stop(app->loop);
}

// ---- what crosses from the typing thread ---------------------------------------

/* A line to send, in memory of its own: the typing thread has moved on by the time it is sent. */
typedef struct { ex_app_t *app; proven_size_t len; char text[64]; } ex_line_t;

/* On the loop's thread. */
static void send_here(void *ctx) {
    ex_line_t *line = ctx;
    if (line->app->ws) (void)proven_ws_stream_send(line->app->ws, true, (proven_mem_view_t){ (const proven_byte_t *)line->text, line->len });
    free(line);
}

/* The typing thread. It never touches the connection: it posts. */
static void typist(void *arg) {
    ex_app_t *app = arg;
    static const char *const lines[LINES] = { "hello", "how are you", "goodbye" };
    while (!atomic_load(&app->open)) proven_time_sleep(1);
    for (int i = 0; i < LINES; ++i) {
        ex_line_t *line = malloc(sizeof *line);
        if (!line) return;
        line->app = app;
        line->len = strlen(lines[i]);
        memcpy(line->text, lines[i], line->len);
        if (proven_loop_post(app->loop, send_here, line) != PROVEN_OK) free(line);
        proven_time_sleep(5);               /* someone typing */
    }
}

static void run(void *arg) { (void)proven_loop_run(((ex_app_t *)arg)->loop); }

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    static ex_app_t app;
    atomic_init(&app.open, false);
    EXAMPLE_REQUIRE(proven_loop_create(heap, &app.loop) == PROVEN_OK, "a loop");
    proven_http_event_server_config_t server_config = { .on = { .on_request = on_request } };
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

    char url[64];
    int n = snprintf(url, sizeof url, "ws://127.0.0.1:%u/chat", (unsigned)at.port);
    proven_ws_event_connect_options_t options = {
        .url = { (const proven_byte_t *)url, (proven_size_t)n },
        .on_open = on_open,
        .ws = { .on = { .on_message = on_message, .on_closed = on_closed }, .ctx = &app },
    };
    proven_ws_stream_t *ws = NULL;
    EXAMPLE_REQUIRE(proven_ws_event_connect(app.loop, &options, &ws) == PROVEN_OK, "a connection is begun");

    // ---- two threads: the loop, and someone typing --------------------------------
    proven_job_sys_t *threads = NULL;
    proven_job_group_t looping, typing;
    proven_job_group_init(&looping);
    proven_job_group_init(&typing);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 2, 4, &threads) == PROVEN_OK &&
                    proven_job_group_submit(threads, &looping, run, &app) == PROVEN_OK &&
                    proven_job_group_submit(threads, &typing, typist, &app) == PROVEN_OK, "the loop on one thread, the typist on another");

    /* The loop stops when the connection has closed, after the third answer. */
    proven_job_group_wait(threads, &typing);
    proven_job_group_wait(threads, &looping);
    EXAMPLE_REQUIRE(app.answers == LINES && app.closed == 1, "three lines sent from the typing thread, three answers received on the loop's");

    /* Anything posted and not yet run is run here, before the loop goes. */
    EXAMPLE_REQUIRE(proven_loop_poll(app.loop, PROVEN_NET_DONT_WAIT) == PROVEN_OK, "a last round");
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    proven_http_event_server_destroy(server);
    proven_loop_destroy(app.loop);
    return EXAMPLE_OK();
}
