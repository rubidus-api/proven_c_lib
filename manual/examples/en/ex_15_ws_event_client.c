#include "example.h"
#include <stdio.h>
#include <string.h>

/*
 * A WebSocket client on the loop: a connection that costs no thread on this side either.
 *
 * proven_ws_event_connect begins a connection and returns at once. What it gives back is a
 * proven_ws_stream_t - the type a server's connections are - so the calls and the callbacks
 * are the ones the server above uses. What is new is the beginning: on_open says when the
 * server has accepted, and until then nothing may be sent.
 *
 * One loop carries both ends here: the server that is asked, and the client that asks.
 */

typedef struct {
    proven_loop_t *loop;
    int opened, answers, closed, refused;
    proven_u16 code;
    char last[64];
} app_t;

// ---- the server: it says what it was sent ----

static void served(void *ctx, proven_ws_stream_t *ws, bool text, proven_mem_view_t piece, bool first, bool last) {
    (void)ctx; (void)first;
    if (!last) return;                      /* this example's messages are small: one piece each */
    char reply[48];
    int n = snprintf(reply, sizeof reply, "%s, %u bytes", text ? "text" : "binary", (unsigned)piece.size);
    (void)proven_ws_stream_send(ws, true, (proven_mem_view_t){ (const proven_byte_t *)reply, (proven_size_t)n });
}

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    (void)ctx;
    proven_ws_event_config_t config = { .on = { .on_message = served } };
    proven_ws_stream_t *ws = NULL;
    if (proven_ws_event_accept(stream, head, &config, &ws) != PROVEN_OK) (void)proven_http_stream_respond(stream, 400, NULL, 0, (proven_mem_view_t){ 0 });
}

// ---- the client ----

/* The server accepted. From here on - and from inside this call - messages may be sent. */
static void on_open(void *ctx, proven_ws_stream_t *ws, proven_u8str_view_t protocol) {
    app_t *app = ctx;
    (void)protocol;
    app->opened++;
    (void)proven_ws_stream_send(ws, true, proven_mem_view_from_u8(PROVEN_LIT("hello")));
}

/* The same callback a server has. This side asks twice and then says goodbye. */
static void on_message(void *ctx, proven_ws_stream_t *ws, bool text, proven_mem_view_t piece, bool first, bool last) {
    app_t *app = ctx;
    (void)text; (void)first;
    if (!last) return;
    proven_size_t n = piece.size < sizeof app->last - 1 ? piece.size : sizeof app->last - 1;
    memcpy(app->last, piece.ptr, n);
    app->last[n] = '\0';
    printf("the server says: %s\n", app->last);
    if (++app->answers == 1) {
        static const proven_byte_t bytes[300] = { 1, 2, 3 };
        (void)proven_ws_stream_send(ws, false, (proven_mem_view_t){ bytes, sizeof bytes });
    } else {
        proven_ws_stream_close(ws, 1000, PROVEN_LIT("thank you"));
    }
}

/* Called exactly once for every connection that was begun - also for one that never opened:
 * its code is then 0, and `why` says what went wrong. */
static void on_closed(void *ctx, proven_ws_stream_t *ws, proven_u16 code, proven_err_t why) {
    app_t *app = ctx;
    (void)ws;
    if (code == 0) {
        /* Never a WebSocket: nothing listened, the server answered something else, or time ran out. */
        printf("no connection: error %d\n", (int)why);
        app->refused++;
    } else {
        app->code = code;
        app->closed++;
    }
    /* Both connections of this program have ended: there is nothing left for the loop to do. */
    if (app->closed == 1 && app->refused == 1) proven_loop_stop(app->loop);
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    app_t app = { 0 };
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

    /* The host is an address, so none has to be given: names are not resolved on a loop. */
    char url[64];
    int n = snprintf(url, sizeof url, "ws://127.0.0.1:%u/chat", (unsigned)at.port);
    proven_ws_event_connect_options_t options = {
        .url = { (const proven_byte_t *)url, (proven_size_t)n },
        .on_open = on_open,
        .ws = { .on = { .on_message = on_message, .on_closed = on_closed }, .ctx = &app },
    };
    proven_ws_stream_t *ws = NULL;
    EXAMPLE_REQUIRE(proven_ws_event_connect(app.loop, &options, &ws) == PROVEN_OK && ws != NULL && app.opened == 0, "a connection is begun; nothing has happened yet");
    /* The call has returned and the connection is being made: this is too early to speak. */
    EXAMPLE_REQUIRE(proven_ws_stream_send(ws, true, proven_mem_view_from_u8(PROVEN_LIT("too early"))) == PROVEN_ERR_INVALID_STATE, "nothing can be sent before the server has accepted");

    /* And one that will fail, to see how that is told: a port that was just closed. */
    proven_net_listener_t unused;
    proven_net_addr_t nobody;
    EXAMPLE_REQUIRE(proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 1, &unused, &nobody) == PROVEN_OK && proven_net_listener_close(&unused) == PROVEN_OK, "a port nobody listens on");
    n = snprintf(url, sizeof url, "ws://127.0.0.1:%u/chat", (unsigned)nobody.port);
    options.url.size = (proven_size_t)n;
    proven_ws_stream_t *lost = NULL;
    err = proven_ws_event_connect(app.loop, &options, &lost);
    EXAMPLE_REQUIRE(err == PROVEN_OK || err == PROVEN_ERR_REFUSED, "an attempt that cannot succeed: refused now, or told later");
    if (err != PROVEN_OK) app.refused++;

    /* Everything else happens here. */
    if (!(app.closed == 1 && app.refused == 1)) EXAMPLE_REQUIRE(proven_loop_run(app.loop) == PROVEN_OK, "the loop runs until both have ended");
    EXAMPLE_REQUIRE(app.opened == 1 && app.answers == 2 && strcmp(app.last, "binary, 300 bytes") == 0, "it opened once, and two messages were answered");
    EXAMPLE_REQUIRE(app.closed == 1 && app.code == 1000 && app.refused == 1, "it closed once, with this side's code - and the other attempt was reported once");

    proven_http_event_server_destroy(server);
    proven_loop_destroy(app.loop);
    return EXAMPLE_OK();
}
