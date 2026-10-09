#include "example.h"

/*
 * A complete HTTP server: one function that answers requests, and a loop that does the rest.
 *
 * The handler sees a request that has already been read, checked and limited. It reads the
 * body if it wants it, and sends one response - whole, or in pieces.
 */

typedef struct {
    proven_http_server_t *server;
    int served;
} app_t;

static void handle(void *ctx, proven_http_exchange_t *x) {
    app_t *app = ctx;
    const proven_http_request_t *req = proven_http_exchange_request(x);
    app->served++;

    /* The target is a path and perhaps a query; split it before comparing. */
    proven_u8str_view_t path, query;
    bool has_query;
    if (proven_url_split_target(req->target, &path, &query, &has_query) != PROVEN_OK) {
        (void)proven_http_exchange_respond(x, 400, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("bad target\n")));
        return;
    }

    if (req->method == PROVEN_HTTP_GET && proven_u8str_view_eq(path, PROVEN_LIT("/"))) {
        /* The simplest answer: a status, your headers, a body in memory. Content-Length,
         * Date and Connection are written for you. */
        proven_http_header_t type = { PROVEN_LIT("Content-Type"), PROVEN_LIT("text/plain; charset=utf-8") };
        (void)proven_http_exchange_respond(x, 200, &type, 1, proven_mem_view_from_u8(PROVEN_LIT("hello\n")));

    } else if (req->method == PROVEN_HTTP_POST && proven_u8str_view_eq(path, PROVEN_LIT("/shout"))) {
        /* Read the body as it comes and answer in pieces, without knowing the length first:
         * the response goes out chunked. Nothing here holds the whole body. */
        if (proven_http_exchange_begin(x, 200, NULL, 0, PROVEN_HTTP_LENGTH_UNKNOWN) != PROVEN_OK) return;
        proven_byte_t piece[256];
        for (;;) {
            proven_result_size_t got = proven_http_exchange_read(x, (proven_mem_mut_t){ piece, sizeof piece });
            if (got.err != PROVEN_OK) break;          /* PROVEN_ERR_EOF: the body is complete */
            for (proven_size_t i = 0; i < got.value; ++i) {
                if (piece[i] >= 'a' && piece[i] <= 'z') piece[i] = (proven_byte_t)(piece[i] - 32);
            }
            if (proven_http_exchange_write(x, (proven_mem_view_t){ piece, got.value }) != PROVEN_OK) return;   /* the client left */
        }
        (void)proven_http_exchange_end(x);

    } else if (proven_u8str_view_eq(path, PROVEN_LIT("/whoami"))) {
        proven_net_addr_t peer = proven_http_exchange_peer(x);
        proven_byte_t text[PROVEN_NET_ADDR_TEXT_MAX];
        proven_size_t n = 0;
        if (proven_net_addr_format(&peer, (proven_mem_mut_t){ text, sizeof text }, &n) != PROVEN_OK) return;   /* returning with nothing sent: a 500 */
        (void)proven_http_exchange_respond(x, 200, NULL, 0, (proven_mem_view_t){ text, n });

    } else if (proven_u8str_view_eq(path, PROVEN_LIT("/quit"))) {
        (void)proven_http_exchange_respond(x, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("bye\n")));
        proven_http_server_stop(app->server);         /* run() returns after this request */

    } else {
        (void)proven_http_exchange_respond(x, 404, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("no such page\n")));
    }
}

static void serve(void *arg) {
    app_t *app = arg;
    (void)proven_http_server_run(app->server);        /* until stop */
}

/* GET or POST `path` and compare the whole body. */
static bool fetch_is(proven_http_client_t *client, proven_u16 port, const char *method, const char *path, const char *body, proven_u16 status, const char *expect) {
    char url[128];
    int n = snprintf(url, sizeof url, "http://127.0.0.1:%u%s", (unsigned)port, path);
    proven_http_client_request_t req = {
        .method = proven_u8str_view_from_cstr(method),
        .url = { (const proven_byte_t *)url, (proven_size_t)n },
        .body = proven_mem_view_from_u8(proven_u8str_view_from_cstr(body)),
    };
    proven_http_client_response_t resp;
    bool ok = proven_http_client_send(client, &req, &resp) == PROVEN_OK && resp.status == status;
    proven_u8str_t text = {0};
    ok = ok && proven_http_client_read_all(&resp, proven_heap_allocator(), &text, 4096) == PROVEN_OK;
    ok = ok && (expect == NULL || proven_u8str_view_eq(proven_u8str_as_view(&text), proven_u8str_view_from_cstr(expect)));
    proven_u8str_destroy(proven_heap_allocator(), &text);
    proven_http_client_finish(&resp);
    return ok;
}

int main(void) {
    static app_t app;
    proven_allocator_t heap = proven_heap_allocator();

    /* Zero the configuration and set what you need; every zero field has a default. */
    proven_http_server_config_t config = {0};
    config.alloc = heap;
    config.handler = handle;
    config.handler_ctx = &app;
    config.max_body_bytes = 64 * 1024;        /* a larger request body is answered 413 */
    config.head_timeout_ms = 5000;            /* a client has this long to send its request head */
    proven_err_t err = proven_http_server_create(&config, &app.server);
    EXAMPLE_REQUIRE(err == PROVEN_OK, "a server");

    /* Port 0 lets the system choose; `at` says what it chose. */
    proven_net_addr_t at;
    err = proven_http_server_listen(app.server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        proven_http_server_destroy(app.server);
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "it listens");

    /* A program with a loop of its own calls poll: one round, bounded by a deadline. */
    EXAMPLE_REQUIRE(proven_http_server_poll(app.server, proven_net_deadline_in(10)) == PROVEN_ERR_TIMEOUT, "nobody has connected yet");

    /* Most programs call run. Here it runs on another thread, so this one can be the client. */
    proven_job_sys_t *threads = NULL;
    proven_job_group_t running;
    proven_job_group_init(&running);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &threads) == PROVEN_OK &&
                    proven_job_group_submit(threads, &running, serve, &app) == PROVEN_OK, "the server loop is started");

    proven_http_client_config_t client_config = {0};
    client_config.alloc = heap;
    client_config.max_idle_connections = 2;
    proven_http_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&client_config, &client) == PROVEN_OK, "a client");

    EXAMPLE_REQUIRE(fetch_is(client, at.port, "GET", "/", "", 200, "hello\n"), "GET /");
    EXAMPLE_REQUIRE(fetch_is(client, at.port, "POST", "/shout", "a body, read and answered in pieces", 200, "A BODY, READ AND ANSWERED IN PIECES"), "POST /shout");
    EXAMPLE_REQUIRE(fetch_is(client, at.port, "GET", "/whoami?verbose=1", "", 200, NULL), "GET /whoami");
    EXAMPLE_REQUIRE(fetch_is(client, at.port, "GET", "/nothing-here", "", 404, "no such page\n"), "a 404 is the handler's to send");
    EXAMPLE_REQUIRE(fetch_is(client, at.port, "GET", "/quit", "", 200, "bye\n"), "GET /quit");

    /* The handler called stop, so run returns; then the server can be taken apart. */
    proven_job_group_wait(threads, &running);
    EXAMPLE_REQUIRE(app.served == 5, "five requests reached the handler");
    EXAMPLE_REQUIRE(proven_http_server_connection_count(app.server) <= 1, "all on one connection, which is still open or just closed");

    proven_http_client_destroy(client);
    proven_http_server_destroy(app.server);   /* closes what is left */
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    return EXAMPLE_OK();
}
