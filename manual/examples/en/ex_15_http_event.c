#include "example.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * An HTTP server on an event loop: one thread, and no call in it that waits.
 *
 * The server tells these functions what happened, and they return at once. Four kinds of
 * answer are shown: at once; later, from a timer; streamed, giving way when the client is
 * slow; and after a request body has arrived piece by piece.
 *
 * The client is the blocking one of chapter 11, on the example's main thread.
 */

typedef struct {
    proven_loop_t *loop;
    int served;
} app_t;

/* What this program keeps for one request while it is being answered. */
typedef struct {
    app_t *app;
    proven_http_stream_t *stream;
    proven_loop_timer_t timer;
    proven_size_t sent, total;         /* /count: how far the body has got */
    proven_size_t received;            /* /size: body bytes seen */
} exchange_t;

/* Write lines until the server says "no more for now". It says so when the client has not
 * taken what was already sent; the rest waits for on_writable. */
static void count_pump(exchange_t *x) {
    while (x->sent < x->total) {
        char line[32];
        int n = snprintf(line, sizeof line, "%u\n", (unsigned)x->sent);
        proven_result_size_t w = proven_http_stream_write(x->stream, (proven_mem_view_t){ (const proven_byte_t *)line, (proven_size_t)n });
        if (w.err != PROVEN_OK) return;                   /* the client left */
        if (w.value < (proven_size_t)n) return;           /* held back: on_writable will say when */
        x->sent++;
    }
    (void)proven_http_stream_end(x->stream);
}

static void answer_later(void *ctx) {
    exchange_t *x = ctx;
    (void)proven_http_stream_respond(x->stream, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("worth the wait\n")));
}

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    app_t *app = ctx;
    exchange_t *x = calloc(1, sizeof *x);
    if (!x) { proven_http_stream_abort(stream); return; }
    x->app = app;
    x->stream = stream;
    proven_http_stream_set_user(stream, x);

    /* `head` and everything in it are good only until this function returns. */
    if (proven_u8str_view_eq(head->target, PROVEN_LIT("/"))) {
        /* The simplest answer: all of it, now. */
        (void)proven_http_stream_respond(stream, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("hello\n")));
    } else if (proven_u8str_view_eq(head->target, PROVEN_LIT("/slow"))) {
        /* Not now: remember the stream and answer from a timer. The loop goes on meanwhile. */
        proven_loop_timer_set(app->loop, &x->timer, 50, answer_later, x);
    } else if (proven_u8str_view_eq(head->target, PROVEN_LIT("/count"))) {
        /* A body made as it goes, sent chunked because its length is not known first. */
        x->total = 20000;
        if (proven_http_stream_begin(stream, 200, NULL, 0, PROVEN_HTTP_EVENT_LENGTH_UNKNOWN) == PROVEN_OK) count_pump(x);
    } else if (proven_u8str_view_eq(head->target, PROVEN_LIT("/size"))) {
        /* Nothing yet: the body is on its way, and on_body answers when it has all come. */
    } else {
        (void)proven_http_stream_respond(stream, 404, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("not here\n")));
    }
}

static void on_body(void *ctx, proven_http_stream_t *stream, proven_mem_view_t piece, bool last) {
    (void)ctx;
    exchange_t *x = proven_http_stream_user(stream);
    x->received += piece.size;
    if (last) {
        char text[32];
        int n = snprintf(text, sizeof text, "%u bytes\n", (unsigned)x->received);
        (void)proven_http_stream_respond(stream, 200, NULL, 0, (proven_mem_view_t){ (const proven_byte_t *)text, (proven_size_t)n });
    }
}

/* The output that was held back has gone: carry on from where the writing stopped. */
static void on_writable(void *ctx, proven_http_stream_t *stream) {
    (void)ctx;
    exchange_t *x = proven_http_stream_user(stream);
    if (x->total > 0) count_pump(x);
}

/* Called once for every on_request, whatever happened. The place to free what was attached. */
static void on_done(void *ctx, proven_http_stream_t *stream, proven_err_t why) {
    app_t *app = ctx;
    exchange_t *x = proven_http_stream_user(stream);
    if (!x) return;
    proven_loop_timer_cancel(app->loop, &x->timer);
    free(x);
    if (why == PROVEN_OK) app->served++;
}

static void run(void *arg) { (void)proven_loop_run(((app_t *)arg)->loop); }

static char g_url[64];
static proven_u8str_view_t url_for(proven_u16 port, const char *path) {
    int n = snprintf(g_url, sizeof g_url, "http://127.0.0.1:%u%s", (unsigned)port, path);
    return (proven_u8str_view_t){ (const proven_byte_t *)g_url, (proven_size_t)n };
}

static bool body_equals(proven_http_client_response_t *resp, proven_allocator_t alloc, const char *want) {
    proven_u8str_t body = { 0 };
    bool ok = proven_http_client_read_all(resp, alloc, &body, 1024 * 1024) == PROVEN_OK &&
              proven_u8str_view_eq(proven_u8str_as_view(&body), (proven_u8str_view_t){ (const proven_byte_t *)want, strlen(want) });
    proven_u8str_destroy(alloc, &body);
    return ok;
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    app_t app = { 0 };

    // ---- the server ----------------------------------------------------------------
    /* A server is made on a loop and listens; it does nothing until the loop runs. */
    EXAMPLE_REQUIRE(proven_loop_create(heap, &app.loop) == PROVEN_OK, "a loop");
    proven_http_event_server_config_t config = {
        .on = { on_request, on_body, on_writable, on_done },
        .ctx = &app,
        .max_buffered_output = 16 * 1024,        /* hold at most this much per connection that the client has not taken */
    };
    proven_http_event_server_t *server = NULL;
    proven_net_addr_t at;
    EXAMPLE_REQUIRE(proven_http_event_server_create(app.loop, &config, &server) == PROVEN_OK &&
                    proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "an event-driven server on a free port");

    /* This example gives the loop a thread of its own, so that its main thread can be a client. */
    proven_job_sys_t *threads = NULL;
    proven_job_group_t running;
    proven_job_group_init(&running);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &threads) == PROVEN_OK &&
                    proven_job_group_submit(threads, &running, run, &app) == PROVEN_OK, "the loop runs on another thread");

    // ---- four requests -------------------------------------------------------------
    proven_http_client_config_t client_config = { .alloc = heap, .max_idle_connections = 1 };
    proven_http_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&client_config, &client) == PROVEN_OK, "a client");
    proven_http_client_response_t resp;

    EXAMPLE_REQUIRE(proven_http_client_get(client, url_for(at.port, "/"), &resp) == PROVEN_OK && body_equals(&resp, heap, "hello\n"), "answered at once");
    proven_http_client_finish(&resp);
    EXAMPLE_REQUIRE(proven_http_client_get(client, url_for(at.port, "/slow"), &resp) == PROVEN_OK && body_equals(&resp, heap, "worth the wait\n"), "answered 50 ms later, from a timer");
    proven_http_client_finish(&resp);

    /* Twenty thousand lines. Whenever the server holds back, the handler stops and on_writable resumes it. */
    EXAMPLE_REQUIRE(proven_http_client_get(client, url_for(at.port, "/count"), &resp) == PROVEN_OK && resp.status == 200, "a streamed response begins");
    proven_u8str_t counted = { 0 };
    EXAMPLE_REQUIRE(proven_http_client_read_all(&resp, heap, &counted, 1024 * 1024) == PROVEN_OK, "and is read to its end");
    proven_u8str_view_t all = proven_u8str_as_view(&counted);
    EXAMPLE_REQUIRE(all.size == 108890 && memcmp(all.ptr, "0\n1\n2\n", 6) == 0 && memcmp(all.ptr + all.size - 6, "19999\n", 6) == 0, "every line arrived, in order");
    proven_u8str_destroy(heap, &counted);
    proven_http_client_finish(&resp);

    static proven_byte_t upload[50000];
    proven_http_client_request_t post = { .method = PROVEN_LIT("POST"), .url = url_for(at.port, "/size"), .body = { upload, sizeof upload } };
    EXAMPLE_REQUIRE(proven_http_client_send(client, &post, &resp) == PROVEN_OK && body_equals(&resp, heap, "50000 bytes\n"), "a body counted piece by piece");
    proven_http_client_finish(&resp);
    proven_http_client_destroy(client);

    // ---- done ----------------------------------------------------------------------
    /* Stop the loop, then take the server apart. Nothing else is touching it by then. */
    proven_loop_stop(app.loop);
    proven_job_group_wait(threads, &running);
    EXAMPLE_REQUIRE(app.served == 4, "four exchanges ended with on_done(PROVEN_OK)");
    proven_http_event_server_destroy(server);
    proven_loop_destroy(app.loop);
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    return EXAMPLE_OK();
}
