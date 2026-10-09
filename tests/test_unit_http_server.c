#include "proven.h"
#include "proven_test.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/*
 * The HTTP server, driven from this thread through proven_http_server_poll, with raw sockets
 * for clients - so each case says exactly which bytes went in and which came out.
 *
 * Every case runs twice: with handlers on the loop's thread, and with handlers on a job
 * system's workers. The contract seen from the socket is the same in both.
 *
 * Responses are read back with this library's own response parser and body decoder. That is
 * deliberate and it is a limit: it shows the server and the codec agree, not that either agrees
 * with another implementation. The check against curl is a private program outside this suite.
 */

static proven_u8str_view_t sv(const char *s) {
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}
static proven_mem_view_t mv(const char *s) {
    return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}

// -----------------------------------------------------------------------------
// The handler under test
// -----------------------------------------------------------------------------

static atomic_int g_arg_refused;      /* driver-owned header given by the handler */
static atomic_int g_injection_refused;/* CR LF in a header value */
static atomic_int g_overrun_refused;  /* a write past the announced length */
static atomic_int g_second_refused;   /* a second response */
static atomic_int g_inside;           /* handlers inside /gate right now */
static atomic_int g_hold_entered;
static atomic_int g_hold_left;
static atomic_bool g_release;
static atomic_int g_body_error;

static bool target_is(const proven_http_request_t *r, const char *t) {
    return proven_u8str_view_eq(r->target, sv(t));
}

static void handler(void *ctx, proven_http_exchange_t *x) {
    proven_http_server_t **server = ctx;
    const proven_http_request_t *r = proven_http_exchange_request(x);

    if (target_is(r, "/hello")) {
        proven_http_header_t h = { sv("Content-Type"), sv("text/plain") };
        (void)proven_http_exchange_respond(x, 200, &h, 1, mv("hello"));
        if (proven_http_exchange_respond(x, 200, NULL, 0, mv("again")) == PROVEN_ERR_INVALID_STATE) atomic_fetch_add(&g_second_refused, 1);
    } else if (target_is(r, "/echo")) {
        static _Thread_local proven_byte_t body[70000];
        proven_size_t n = 0;
        for (;;) {
            proven_result_size_t got = proven_http_exchange_read(x, (proven_mem_mut_t){ .ptr = body + n, .size = sizeof body - n });
            if (got.err == PROVEN_ERR_EOF) break;
            if (got.err != PROVEN_OK) { atomic_store(&g_body_error, (int)got.err); return; }
            n += got.value;
        }
        (void)proven_http_exchange_respond(x, 200, NULL, 0, (proven_mem_view_t){ .ptr = body, .size = n });
    } else if (target_is(r, "/stream")) {
        if (proven_http_exchange_begin(x, 200, NULL, 0, PROVEN_HTTP_LENGTH_UNKNOWN) != PROVEN_OK) return;
        (void)proven_http_exchange_write(x, mv("one,"));
        (void)proven_http_exchange_write(x, mv(""));
        (void)proven_http_exchange_write(x, mv("two"));
        (void)proven_http_exchange_end(x);
    } else if (target_is(r, "/sized")) {
        if (proven_http_exchange_begin(x, 200, NULL, 0, 6) != PROVEN_OK) return;
        (void)proven_http_exchange_write(x, mv("abc"));
        if (proven_http_exchange_write(x, mv("defg")) == PROVEN_ERR_OUT_OF_BOUNDS) atomic_fetch_add(&g_overrun_refused, 1);
        (void)proven_http_exchange_write(x, mv("def"));
    } else if (target_is(r, "/short")) {
        if (proven_http_exchange_begin(x, 200, NULL, 0, 10) != PROVEN_OK) return;
        (void)proven_http_exchange_write(x, mv("abc"));
    } else if (target_is(r, "/nothing")) {
        return;
    } else if (target_is(r, "/empty")) {
        (void)proven_http_exchange_respond(x, 204, NULL, 0, (proven_mem_view_t){0});
    } else if (target_is(r, "/refuse")) {
        (void)proven_http_exchange_respond(x, 403, NULL, 0, mv("no"));
    } else if (target_is(r, "/badheader")) {
        proven_http_header_t own = { sv("content-length"), sv("3") };
        proven_http_header_t inj = { sv("X-A"), sv("b\r\nSet-Cookie: x=1") };
        if (proven_http_exchange_respond(x, 200, &own, 1, mv("abc")) == PROVEN_ERR_INVALID_ARG) atomic_fetch_add(&g_arg_refused, 1);
        if (proven_http_exchange_respond(x, 200, &inj, 1, mv("abc")) != PROVEN_OK) atomic_fetch_add(&g_injection_refused, 1);
        (void)proven_http_exchange_respond(x, 200, NULL, 0, mv("ok"));
    } else if (target_is(r, "/peer")) {
        proven_net_addr_t peer = proven_http_exchange_peer(x);
        proven_byte_t text[PROVEN_NET_ADDR_TEXT_MAX];
        proven_size_t n = 0;
        if (proven_net_addr_format(&peer, (proven_mem_mut_t){ .ptr = text, .size = sizeof text }, &n) != PROVEN_OK) return;
        (void)proven_http_exchange_respond(x, 200, NULL, 0, (proven_mem_view_t){ .ptr = text, .size = n });
    } else if (target_is(r, "/gate")) {
        /* Answers "both" only if another handler is inside at the same time. */
        atomic_fetch_add(&g_inside, 1);
        bool both = false;
        for (int i = 0; i < 400 && !both; ++i) {
            both = atomic_load(&g_inside) >= 2;
            if (!both) proven_time_sleep(5);
        }
        (void)proven_http_exchange_respond(x, 200, NULL, 0, both ? mv("both") : mv("alone"));
        proven_time_sleep(20);
        atomic_fetch_sub(&g_inside, 1);
    } else if (target_is(r, "/hold")) {
        atomic_fetch_add(&g_hold_entered, 1);
        for (int i = 0; i < 1000 && !atomic_load(&g_release); ++i) proven_time_sleep(5);
        (void)proven_http_exchange_respond(x, 200, NULL, 0, mv("held"));
        atomic_fetch_add(&g_hold_left, 1);
    } else if (target_is(r, "/nap")) {
        atomic_fetch_add(&g_hold_entered, 1);
        proven_time_sleep(150);
        (void)proven_http_exchange_respond(x, 200, NULL, 0, mv("rested"));
        atomic_fetch_add(&g_hold_left, 1);
    } else if (target_is(r, "/stop")) {
        (void)proven_http_exchange_respond(x, 200, NULL, 0, mv("stopping"));
        proven_http_server_stop(*server);
    } else {
        (void)proven_http_exchange_respond(x, 404, NULL, 0, mv("not found"));
    }
}

// -----------------------------------------------------------------------------
// A raw client that reads replies with the library's codec
// -----------------------------------------------------------------------------

typedef struct {
    proven_net_conn_t conn;
    proven_byte_t buf[90000];
    proven_size_t len;
    bool eof;
} client_t;

typedef struct {
    proven_u16 status;
    proven_size_t interim;              /* 1xx responses that came first */
    proven_byte_t body[70000];
    proven_size_t body_len;
    proven_size_t declared_length;      /* Content-Length as sent, or SIZE_MAX */
    bool chunked;
    bool close;                         /* Connection: close */
    bool keep_alive_header;
    bool has_date;
    bool has_type;
    bool truncated;                     /* the connection ended inside the body */
} reply_t;

static proven_http_server_t *g_server;

static void client_open(client_t *c, proven_net_addr_t at) {
    c->len = 0;
    c->eof = false;
    proven_err_t e = proven_net_connect(at, proven_net_deadline_in(5000), &c->conn);
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "a client connects to the server's listener", "");
}

static void client_send(client_t *c, const char *bytes) {
    proven_result_size_t w = proven_net_write_all(&c->conn, mv(bytes), proven_net_deadline_in(5000));
    PROVEN_TEST_ASSERT(w.err == PROVEN_OK, "the client's request bytes are written", "");
}

/* One turn of the server's loop, then whatever has arrived for the client. */
static void pump(client_t *c) {
    proven_err_t e = proven_http_server_poll(g_server, proven_net_deadline_in(2));
    PROVEN_TEST_ASSERT(e == PROVEN_OK || e == PROVEN_ERR_TIMEOUT, "a round of the server loop succeeds or finds nothing", "");
    if (!c || c->eof || c->len == sizeof c->buf) return;
    proven_result_size_t r = proven_net_read(&c->conn, (proven_mem_mut_t){ .ptr = c->buf + c->len, .size = sizeof c->buf - c->len }, proven_net_deadline_in(2));
    if (r.err == PROVEN_OK) c->len += r.value;
    else if (r.err != PROVEN_ERR_TIMEOUT) c->eof = true;
}

/* Read one response to a request made with `method`. Returns false if none came in 10 s. */
static bool client_reply(client_t *c, proven_http_method_t method, reply_t *out) {
    proven_time_t start = proven_time_monotonic_now();
    memset(out, 0, sizeof *out);
    out->declared_length = SIZE_MAX;
    proven_http_header_t headers[32];
    proven_http_response_t resp;
    proven_size_t head = 0;
    for (;;) {
        proven_err_t e = proven_http_parse_response((proven_mem_view_t){ .ptr = c->buf, .size = c->len }, headers, 32, 0, &resp, &head);
        if (e == PROVEN_OK && resp.status < 200) {
            out->interim++;
            memmove(c->buf, c->buf + head, c->len - head);
            c->len -= head;
            continue;
        }
        if (e == PROVEN_OK) break;
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_NEED_MORE, "what the server sent parses as a response head", "Inspect sv_build_head in src/proven/http_server.c.");
        if (c->eof) return false;
        if ((proven_time_monotonic_now() - start) / 1000000 > 10000) return false;
        pump(c);
    }
    out->status = resp.status;
    proven_u8str_view_t v;
    if (proven_http_header_find(headers, resp.header_count, sv("Content-Length"), &v)) {
        out->declared_length = 0;
        for (proven_size_t i = 0; i < v.size; ++i) out->declared_length = out->declared_length * 10 + (proven_size_t)(v.ptr[i] - '0');
    }
    out->chunked = proven_http_header_has_token(headers, resp.header_count, sv("Transfer-Encoding"), sv("chunked"));
    out->close = proven_http_header_has_token(headers, resp.header_count, sv("Connection"), sv("close"));
    out->keep_alive_header = proven_http_header_has_token(headers, resp.header_count, sv("Connection"), sv("keep-alive"));
    out->has_date = proven_http_header_count(headers, resp.header_count, sv("Date")) == 1;
    out->has_type = proven_http_header_count(headers, resp.header_count, sv("Content-Type")) == 1;

    proven_http_framing_t framing;
    proven_err_t e = proven_http_response_framing(&resp, method, &framing);
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "the response's framing is unambiguous", "");
    proven_http_body_t body;
    e = proven_http_body_init(&body, framing, sizeof out->body);
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "the response body fits the test's buffer", "");
    proven_size_t pos = head;
    bool done = framing.kind == PROVEN_HTTP_BODY_NONE || (framing.kind == PROVEN_HTTP_BODY_LENGTH && framing.length == 0);
    while (!done) {
        if (pos == c->len) {
            if (c->eof) {
                if (proven_http_body_end(&body) != PROVEN_OK) out->truncated = true;
                break;
            }
            if ((proven_time_monotonic_now() - start) / 1000000 > 10000) return false;
            pump(c);
            continue;
        }
        proven_size_t used = 0;
        proven_mem_view_t payload;
        e = proven_http_body_feed(&body, (proven_mem_view_t){ .ptr = c->buf + pos, .size = c->len - pos }, &used, &payload, &done);
        PROVEN_TEST_ASSERT(e == PROVEN_OK, "the response body's framing is well formed", "Inspect proven_http_exchange_write in src/proven/http_server.c.");
        memcpy(out->body + out->body_len, payload.ptr, payload.size);
        out->body_len += payload.size;
        pos += used;
    }
    memmove(c->buf, c->buf + pos, c->len - pos);
    c->len -= pos;
    return true;
}

static bool body_is(const reply_t *r, const char *text) {
    return r->body_len == strlen(text) && memcmp(r->body, text, r->body_len) == 0;
}

/* True when the server closes the connection within `ms`, having sent nothing more. */
static bool client_sees_close(client_t *c, proven_u32 ms) {
    proven_time_t start = proven_time_monotonic_now();
    while (!c->eof && (proven_time_monotonic_now() - start) / 1000000 < (proven_i64)ms) pump(c);
    return c->eof && c->len == 0;
}

static void settle(void) {
    /* Let the loop notice what the last case left behind - closed clients, finished workers. */
    for (int i = 0; i < 10; ++i) pump(NULL);
}

static bool wait_count(proven_size_t want) {
    for (int i = 0; i < 2000; ++i) {
        if (proven_http_server_connection_count(g_server) == want) return true;
        pump(NULL);
    }
    return false;
}

static reply_t rp;      /* large; static so no case puts it on the stack */
static client_t ca, cb, cc;

static void run_cases(proven_job_sys_t *jobs) {
    const bool threaded = jobs != NULL;
    proven_http_server_config_t cfg = {0};
    cfg.alloc = proven_heap_allocator();
    cfg.handler = handler;
    cfg.handler_ctx = &g_server;
    cfg.jobs = jobs;
    cfg.max_body_bytes = 65536;
    cfg.max_head_bytes = 2048;
    cfg.max_headers = 16;
    proven_err_t e = proven_http_server_create(&cfg, &g_server);
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "a server is created", "");
    proven_net_addr_t at;
    e = proven_http_server_listen(g_server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at);
    PROVEN_TEST_ASSERT(e == PROVEN_OK && at.port != 0, "it listens on a free loopback port and says which", "");
    PROVEN_TEST_ASSERT(proven_http_server_poll(g_server, proven_net_deadline_in(20)) == PROVEN_ERR_TIMEOUT,
        "a round with nothing to do is PROVEN_ERR_TIMEOUT", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a request, a response, and the connection stays open",
        "The response carries Date and Content-Length written by the server; a second request on the same connection is served.",
        "Inspect sv_service and sv_build_head in src/proven/http_server.c.");
    // ---------------------------------------------------------------
    {
        client_open(&ca, at);
        client_send(&ca, "GET /hello HTTP/1.1\r\nHost: t\r\n\r\n");
        PROVEN_TEST_ASSERT(client_reply(&ca, PROVEN_HTTP_GET, &rp), "a response arrives", "");
        PROVEN_TEST_ASSERT(rp.status == 200 && body_is(&rp, "hello"), "it is the handler's 200 with its body", "");
        PROVEN_TEST_ASSERT(rp.declared_length == 5 && !rp.chunked, "a body given whole is sent with Content-Length", "");
        PROVEN_TEST_ASSERT(rp.has_date && rp.has_type, "the server adds Date; the handler's own header is there", "");
        PROVEN_TEST_ASSERT(!rp.close && !rp.keep_alive_header, "HTTP/1.1 stays open without saying so", "");
        PROVEN_TEST_ASSERT(atomic_load(&g_second_refused) >= 1, "a second response to one request is PROVEN_ERR_INVALID_STATE", "");
        PROVEN_TEST_ASSERT(wait_count(1), "the connection is counted", "");

        client_send(&ca, "GET /missing HTTP/1.1\r\nHost: t\r\n\r\n");
        PROVEN_TEST_ASSERT(client_reply(&ca, PROVEN_HTTP_GET, &rp) && rp.status == 404, "the same connection carries a second request", "");

        client_send(&ca, "GET /peer HTTP/1.1\r\nHost: t\r\n\r\n");
        PROVEN_TEST_ASSERT(client_reply(&ca, PROVEN_HTTP_GET, &rp) && rp.status == 200, "the handler can ask who is calling", "");
        proven_net_addr_t mine;
        proven_byte_t text[PROVEN_NET_ADDR_TEXT_MAX];
        proven_size_t n = 0;
        PROVEN_TEST_ASSERT(proven_net_conn_local_addr(&ca.conn, &mine) == PROVEN_OK &&
                           proven_net_addr_format(&mine, (proven_mem_mut_t){ .ptr = text, .size = sizeof text }, &n) == PROVEN_OK, "the client knows its own address", "");
        PROVEN_TEST_ASSERT(rp.body_len == n && memcmp(rp.body, text, n) == 0, "the peer address the handler sees is the client's", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("requests sent back to back are answered in order",
        "Three requests in one write: the bytes after the first request are the next one, not lost and not part of a body.",
        "Inspect the leftover handling at the end of sv_service and in sv_collect_done.");
    // ---------------------------------------------------------------
    {
        client_send(&ca, "GET /hello HTTP/1.1\r\nHost: t\r\n\r\n"
                         "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\n\r\nABCD"
                         "GET /empty HTTP/1.1\r\nHost: t\r\n\r\n");
        PROVEN_TEST_ASSERT(client_reply(&ca, PROVEN_HTTP_GET, &rp) && rp.status == 200 && body_is(&rp, "hello"), "first: the GET", "");
        PROVEN_TEST_ASSERT(client_reply(&ca, PROVEN_HTTP_POST, &rp) && rp.status == 200 && body_is(&rp, "ABCD"), "second: the POST with exactly its four bytes", "");
        PROVEN_TEST_ASSERT(client_reply(&ca, PROVEN_HTTP_GET, &rp) && rp.status == 204, "third: the 204", "");
        PROVEN_TEST_ASSERT(rp.declared_length == SIZE_MAX && !rp.chunked && rp.body_len == 0, "a 204 has neither a length nor a body", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("request bodies: by length, chunked, and left unread",
        "The handler reads the same bytes however they were framed; a body the handler ignores does not turn into the next request.",
        "Inspect sv_read_body and the drain in sv_run.");
    // ---------------------------------------------------------------
    {
        client_send(&ca, "POST /echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n"
                         "3\r\nabc\r\n5;ext=1\r\ndefgh\r\n0\r\nX-Trailer: 1\r\n\r\n");
        PROVEN_TEST_ASSERT(client_reply(&ca, PROVEN_HTTP_POST, &rp) && rp.status == 200 && body_is(&rp, "abcdefgh"), "a chunked body arrives as its payload", "");

        /* A body larger than the window after the head, so the handler's reads refill it. */
        static char big[20200];
        int k = snprintf(big, sizeof big, "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 20000\r\n\r\n");
        for (int i = 0; i < 20000; ++i) big[k + i] = (char)('a' + i % 26);
        big[k + 20000] = '\0';
        client_send(&ca, big);
        PROVEN_TEST_ASSERT(client_reply(&ca, PROVEN_HTTP_POST, &rp) && rp.status == 200 && rp.body_len == 20000, "a 20000-byte body is read through a 4 KiB window", "");
        PROVEN_TEST_ASSERT(memcmp(rp.body, big + k, 20000) == 0, "and is the bytes that were sent", "");

        client_send(&ca, "POST /refuse HTTP/1.1\r\nHost: t\r\nContent-Length: 5\r\n\r\nGET /"
                         "GET /hello HTTP/1.1\r\nHost: t\r\n\r\n");
        PROVEN_TEST_ASSERT(client_reply(&ca, PROVEN_HTTP_POST, &rp) && rp.status == 403, "the handler answers without reading the body", "");
        PROVEN_TEST_ASSERT(client_reply(&ca, PROVEN_HTTP_GET, &rp) && rp.status == 200 && body_is(&rp, "hello"),
            "the unread body was skipped: the next request is the one after it", "A 400 here means body bytes were parsed as a request.");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("responses written in pieces",
        "Unknown length is chunked for HTTP/1.1 and runs to the close for HTTP/1.0; a known length is checked.",
        "Inspect proven_http_exchange_begin, _write and _end.");
    // ---------------------------------------------------------------
    {
        client_send(&ca, "GET /stream HTTP/1.1\r\nHost: t\r\n\r\n");
        PROVEN_TEST_ASSERT(client_reply(&ca, PROVEN_HTTP_GET, &rp) && rp.status == 200 && rp.chunked && body_is(&rp, "one,two"), "unknown length: chunked, complete", "");

        client_send(&ca, "GET /sized HTTP/1.1\r\nHost: t\r\n\r\n");
        PROVEN_TEST_ASSERT(client_reply(&ca, PROVEN_HTTP_GET, &rp) && rp.declared_length == 6 && body_is(&rp, "abcdef"), "known length: the bytes, with Content-Length", "");
        PROVEN_TEST_ASSERT(atomic_load(&g_overrun_refused) >= 1, "a write past the announced length is PROVEN_ERR_OUT_OF_BOUNDS and sends nothing", "");

        client_send(&ca, "GET /badheader HTTP/1.1\r\nHost: t\r\n\r\n");
        PROVEN_TEST_ASSERT(client_reply(&ca, PROVEN_HTTP_GET, &rp) && rp.status == 200 && body_is(&rp, "ok"), "after a refused response the handler can send another", "");
        PROVEN_TEST_ASSERT(atomic_load(&g_arg_refused) >= 1, "a handler's own Content-Length is PROVEN_ERR_INVALID_ARG", "");
        PROVEN_TEST_ASSERT(atomic_load(&g_injection_refused) >= 1, "a header value with a line break is refused", "A response-splitting hole otherwise.");

        client_send(&ca, "HEAD /hello HTTP/1.1\r\nHost: t\r\n\r\n");
        PROVEN_TEST_ASSERT(client_reply(&ca, PROVEN_HTTP_HEAD, &rp) && rp.status == 200 && rp.declared_length == 5 && rp.body_len == 0,
            "HEAD: the headers of the GET, Content-Length included, and no body", "");
        client_send(&ca, "HEAD /stream HTTP/1.1\r\nHost: t\r\n\r\nGET /hello HTTP/1.1\r\nHost: t\r\n\r\n");
        PROVEN_TEST_ASSERT(client_reply(&ca, PROVEN_HTTP_HEAD, &rp) && rp.status == 200 && rp.chunked, "HEAD of a streamed response says chunked", "");
        PROVEN_TEST_ASSERT(client_reply(&ca, PROVEN_HTTP_GET, &rp) && body_is(&rp, "hello"), "and sends no chunk bytes: the next response follows at once", "");

        /* Connection: close with a streamed body: the last chunk must still be sent. */
        client_open(&cb, at);
        client_send(&cb, "GET /stream HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        PROVEN_TEST_ASSERT(client_reply(&cb, PROVEN_HTTP_GET, &rp) && rp.close && rp.chunked && body_is(&rp, "one,two") && !rp.truncated,
            "Connection: close: the chunked body is still terminated", "Inspect the last-chunk condition in proven_http_exchange_end.");
        PROVEN_TEST_ASSERT(client_sees_close(&cb, 5000), "and then the server closes", "");
        (void)proven_net_close(&cb.conn);

        client_open(&cb, at);
        client_send(&cb, "GET /stream HTTP/1.0\r\n\r\n");
        PROVEN_TEST_ASSERT(client_reply(&cb, PROVEN_HTTP_GET, &rp) && !rp.chunked && rp.close && body_is(&rp, "one,two"),
            "HTTP/1.0: no chunking, the body ends where the connection does", "");
        (void)proven_net_close(&cb.conn);

        client_open(&cb, at);
        client_send(&cb, "GET /hello HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
        PROVEN_TEST_ASSERT(client_reply(&cb, PROVEN_HTTP_GET, &rp) && rp.keep_alive_header, "HTTP/1.0 that asks to stay open is told it may", "");
        client_send(&cb, "GET /hello HTTP/1.0\r\n\r\n");
        PROVEN_TEST_ASSERT(client_reply(&cb, PROVEN_HTTP_GET, &rp) && rp.close && body_is(&rp, "hello"), "and without asking is closed after the response", "");
        PROVEN_TEST_ASSERT(client_sees_close(&cb, 5000), "the server closes it", "");
        (void)proven_net_close(&cb.conn);

        client_open(&cb, at);
        client_send(&cb, "GET /short HTTP/1.1\r\nHost: t\r\n\r\n");
        PROVEN_TEST_ASSERT(client_reply(&cb, PROVEN_HTTP_GET, &rp) && rp.declared_length == 10 && rp.body_len == 3 && rp.truncated,
            "a response that wrote less than it announced is cut off by a close, not padded", "");
        (void)proven_net_close(&cb.conn);

        client_open(&cb, at);
        client_send(&cb, "GET /nothing HTTP/1.1\r\nHost: t\r\n\r\n");
        PROVEN_TEST_ASSERT(client_reply(&cb, PROVEN_HTTP_GET, &rp) && rp.status == 500, "a handler that sends nothing gets a 500 sent for it", "");
        client_send(&cb, "GET /hello HTTP/1.1\r\nHost: t\r\n\r\n");
        PROVEN_TEST_ASSERT(client_reply(&cb, PROVEN_HTTP_GET, &rp) && rp.status == 200, "and the connection is still good", "");
        (void)proven_net_close(&cb.conn);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("Expect: 100-continue",
        "The go-ahead is sent when the handler first reads the body, and not at all when it refuses by the headers.",
        "Inspect the expect_continue handling in sv_read_body and sv_run.");
    // ---------------------------------------------------------------
    {
        client_open(&cb, at);
        if (threaded) {
            client_send(&cb, "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 3\r\nExpect: 100-continue\r\n\r\n");
            /* Wait for the interim response before sending a byte of body. */
            proven_time_t start = proven_time_monotonic_now();
            while (cb.len < 25 && (proven_time_monotonic_now() - start) / 1000000 < 10000) pump(&cb);
            PROVEN_TEST_ASSERT(cb.len == 25 && memcmp(cb.buf, "HTTP/1.1 100 Continue\r\n\r\n", 25) == 0,
                "the server sends 100 Continue before any body byte has been sent", "");
            cb.len = 0;
            client_send(&cb, "xyz");
        } else {
            client_send(&cb, "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 3\r\nExpect: 100-continue\r\n\r\nxyz");
        }
        PROVEN_TEST_ASSERT(client_reply(&cb, PROVEN_HTTP_POST, &rp) && rp.status == 200 && body_is(&rp, "xyz"), "the body is then read and answered", "");
        PROVEN_TEST_ASSERT(rp.interim == (threaded ? 0u : 1u), "exactly one 100 Continue came first", "");

        client_send(&cb, "POST /refuse HTTP/1.1\r\nHost: t\r\nContent-Length: 3\r\nExpect: 100-continue\r\n\r\n");
        proven_time_t start = proven_time_monotonic_now();
        PROVEN_TEST_ASSERT(client_reply(&cb, PROVEN_HTTP_POST, &rp) && rp.status == 403 && rp.interim == 0,
            "a handler that refuses without reading never invites the body", "");
        PROVEN_TEST_ASSERT(rp.close, "and since the body's fate is unknown, the connection is closed", "");
        PROVEN_TEST_ASSERT(client_sees_close(&cb, 5000), "the server closes", "");
        PROVEN_TEST_ASSERT((proven_time_monotonic_now() - start) / 1000000 < 3000,
            "without waiting for a body that was never invited", "The drain in sv_run must not wait when 100 Continue was never sent.");
        (void)proven_net_close(&cb.conn);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("requests the server refuses before any handler runs",
        "Malformed, ambiguous, oversized and unsupported requests get their status and the connection is closed.",
        "Inspect the refusal ladder in sv_service. A status other than the expected one is a mapping error; a 200 is a request-smuggling hole.");
    // ---------------------------------------------------------------
    {
        static const struct { const char *bytes; proven_u16 status; const char *why; } refused[] = {
            { "GET /hello HTTP/1.1\r\n\r\n", 400, "HTTP/1.1 without Host" },
            { "GET /hello HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400, "two Host fields" },
            { "GET /hello HTTP/1.1\nHost: t\n\n", 400, "bare LF line endings" },
            { "GET /hello HTTP/1.1\r\nHost : t\r\n\r\n", 400, "a space before the colon" },
            { "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n", 400, "both a length and chunked" },
            { "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\n", 400, "two different lengths" },
            { "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: -1\r\n\r\n", 400, "a negative length" },
            { "GET /hello HTTP/2.0\r\nHost: t\r\n\r\n", 505, "a version this server does not speak" },
            { "POST /echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", 501, "a transfer coding it does not implement" },
            { "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 65537\r\n\r\n", 413, "a body announced larger than max_body_bytes" },
            { "GET /hello HTTP/1.1\r\nHost: t\r\nA: 1\r\nB: 1\r\nC: 1\r\nD: 1\r\nE: 1\r\nF: 1\r\nG: 1\r\nH: 1\r\nI: 1\r\nJ: 1\r\nK: 1\r\nL: 1\r\nM: 1\r\nN: 1\r\nO: 1\r\nP: 1\r\n\r\n", 431, "more header fields than max_headers" },
        };
        for (proven_size_t i = 0; i < sizeof refused / sizeof refused[0]; ++i) {
            client_open(&cb, at);
            client_send(&cb, refused[i].bytes);
            bool got = client_reply(&cb, PROVEN_HTTP_GET, &rp);
            if (!got || rp.status != refused[i].status) PROVEN_TEST_INFO("case: {} -> status {}", PROVEN_ARG(refused[i].why), PROVEN_ARG((int)rp.status));
            PROVEN_TEST_ASSERT(got && rp.status == refused[i].status, "the request is refused with its status", "");
            PROVEN_TEST_ASSERT(rp.close && client_sees_close(&cb, 5000), "and the connection is closed", "");
            (void)proven_net_close(&cb.conn);
        }

        /* A head that never ends: refused at the limit, not buffered for ever. */
        client_open(&cb, at);
        static char longhead[3000];
        int k = snprintf(longhead, sizeof longhead, "GET /hello HTTP/1.1\r\nHost: t\r\nX-Pad: ");
        memset(longhead + k, 'a', 2400);
        longhead[k + 2400] = '\0';
        client_send(&cb, longhead);
        PROVEN_TEST_ASSERT(client_reply(&cb, PROVEN_HTTP_GET, &rp) && rp.status == 431, "a head past max_head_bytes is 431 before its end arrives", "");
        (void)proven_net_close(&cb.conn);

        /* A chunked body that grows past the limit is only discovered while reading. */
        client_open(&cb, at);
        static char huge[70000];
        k = snprintf(huge, sizeof huge, "POST /echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n10400\r\n");
        memset(huge + k, 'z', 66560);
        memcpy(huge + k + 66560, "\r\n0\r\n\r\n", 8);
        atomic_store(&g_body_error, 0);
        client_send(&cb, huge);
        PROVEN_TEST_ASSERT(client_reply(&cb, PROVEN_HTTP_POST, &rp) && rp.status == 413, "a chunked body past max_body_bytes is 413", "");
        PROVEN_TEST_ASSERT(atomic_load(&g_body_error) == (int)PROVEN_ERR_OUT_OF_BOUNDS, "the handler's read reported PROVEN_ERR_OUT_OF_BOUNDS", "");
        (void)proven_net_close(&cb.conn);

        client_open(&cb, at);
        client_send(&cb, "POST /echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\n");
        PROVEN_TEST_ASSERT(client_reply(&cb, PROVEN_HTTP_POST, &rp) && rp.status == 400, "malformed chunk framing is 400", "");
        (void)proven_net_close(&cb.conn);

        client_send(&ca, "GET /hello HTTP/1.1\r\nHost: t\r\n\r\n");
        PROVEN_TEST_ASSERT(client_reply(&ca, PROVEN_HTTP_GET, &rp) && rp.status == 200, "through all of that the first connection is still served", "");
    }

    if (threaded) {
        // ---------------------------------------------------------------
        PROVEN_TEST_SECTION("with a job system, handlers run at the same time",
            "Two requests on two connections are both inside the handler at once, which one thread cannot do.",
            "Inspect the dispatch in sv_service and the hand-back in sv_job.");
        // ---------------------------------------------------------------
        client_open(&cb, at);
        client_send(&ca, "GET /gate HTTP/1.1\r\nHost: t\r\n\r\n");
        client_send(&cb, "GET /gate HTTP/1.1\r\nHost: t\r\n\r\n");
        PROVEN_TEST_ASSERT(client_reply(&ca, PROVEN_HTTP_GET, &rp) && body_is(&rp, "both"), "the first saw the second inside", "");
        PROVEN_TEST_ASSERT(client_reply(&cb, PROVEN_HTTP_GET, &rp) && body_is(&rp, "both"), "the second saw the first", "");
        /* And both connections came back to the loop and are served again. */
        client_send(&cb, "GET /hello HTTP/1.1\r\nHost: t\r\n\r\n");
        PROVEN_TEST_ASSERT(client_reply(&cb, PROVEN_HTTP_GET, &rp) && body_is(&rp, "hello"), "a connection handed back by a worker carries the next request", "");
        (void)proven_net_close(&cb.conn);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("stop, and destroy with a handler still running",
        "A handler may stop the server; run then returns. Destroy waits for handlers on workers.",
        "Inspect proven_http_server_stop, _run and _destroy.");
    // ---------------------------------------------------------------
    {
        if (threaded) {
            client_open(&cb, at);
            atomic_store(&g_hold_entered, 0);
            atomic_store(&g_hold_left, 0);
            client_send(&cb, "GET /nap HTTP/1.1\r\nHost: t\r\n\r\n");
            for (int i = 0; i < 5000 && atomic_load(&g_hold_entered) == 0; ++i) pump(NULL);
            PROVEN_TEST_ASSERT(atomic_load(&g_hold_entered) == 1, "a handler is running on a worker", "");
        } else {
            client_send(&ca, "GET /stop HTTP/1.1\r\nHost: t\r\n\r\n");
            e = proven_http_server_run(g_server);
            PROVEN_TEST_ASSERT(e == PROVEN_OK, "run returns PROVEN_OK once a handler has called stop", "");
            ca.eof = false;
        }
        (void)proven_net_close(&ca.conn);
        proven_http_server_destroy(g_server);
        if (threaded) {
            PROVEN_TEST_ASSERT(atomic_load(&g_hold_left) == 1, "destroy returned only after the handler did", "A use-after-free under ASan here means destroy freed a connection a worker owned.");
            (void)proven_net_close(&cb.conn);
        }
        g_server = NULL;
    }
}

/* Timeouts get their own server, with limits short enough to wait for. */
static void run_timeouts(proven_job_sys_t *jobs) {
    proven_http_server_config_t cfg = {0};
    cfg.alloc = proven_heap_allocator();
    cfg.handler = handler;
    cfg.handler_ctx = &g_server;
    cfg.jobs = jobs;
    cfg.head_timeout_ms = 150;
    cfg.idle_timeout_ms = 400;
    cfg.body_timeout_ms = 150;
    cfg.max_connections = 2;
    proven_err_t e = proven_http_server_create(&cfg, &g_server);
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "a server with short timeouts is created", "");
    proven_net_addr_t at;
    e = proven_http_server_listen(g_server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at);
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "it listens", "");
    proven_net_addr_t extra;
    for (int i = 0; i < 3; ++i) {
        PROVEN_TEST_ASSERT(proven_http_server_listen(g_server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &extra) == PROVEN_OK, "up to four listeners", "");
    }
    PROVEN_TEST_ASSERT(proven_http_server_listen(g_server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), NULL) == PROVEN_ERR_OUT_OF_BOUNDS,
        "a fifth listener is PROVEN_ERR_OUT_OF_BOUNDS", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("every wait has a limit",
        "Half a head is a 408 after head_timeout_ms; a silent connection is closed without a word; half a body is a 408 after body_timeout_ms.",
        "Inspect the deadline sweep at the end of proven_http_server_poll.");
    // ---------------------------------------------------------------
    {
        client_open(&ca, extra);
        client_send(&ca, "GET /hello HT");
        proven_time_t start = proven_time_monotonic_now();
        PROVEN_TEST_ASSERT(client_reply(&ca, PROVEN_HTTP_GET, &rp) && rp.status == 408 && rp.close, "half a request head: 408", "");
        proven_i64 took = (proven_time_monotonic_now() - start) / 1000000;
        PROVEN_TEST_ASSERT(took >= 100 && took < 5000, "after about head_timeout_ms, not at once and not never", "");
        PROVEN_TEST_ASSERT(client_sees_close(&ca, 5000), "then closed", "");
        (void)proven_net_close(&ca.conn);

        client_open(&ca, at);
        start = proven_time_monotonic_now();
        PROVEN_TEST_ASSERT(client_sees_close(&ca, 5000), "a connection that never says anything is closed, with nothing sent", "");
        took = (proven_time_monotonic_now() - start) / 1000000;
        PROVEN_TEST_ASSERT(took >= 100, "after the head timeout", "");
        (void)proven_net_close(&ca.conn);

        client_open(&ca, at);
        client_send(&ca, "GET /hello HTTP/1.1\r\nHost: t\r\n\r\n");
        PROVEN_TEST_ASSERT(client_reply(&ca, PROVEN_HTTP_GET, &rp) && rp.status == 200, "a request is served", "");
        start = proven_time_monotonic_now();
        PROVEN_TEST_ASSERT(client_sees_close(&ca, 5000), "and the idle connection is then closed in silence", "");
        took = (proven_time_monotonic_now() - start) / 1000000;
        PROVEN_TEST_ASSERT(took >= 300, "after idle_timeout_ms, which is the longer one", "");
        (void)proven_net_close(&ca.conn);

        client_open(&ca, at);
        atomic_store(&g_body_error, 0);
        client_send(&ca, "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 10\r\n\r\nabc");
        PROVEN_TEST_ASSERT(client_reply(&ca, PROVEN_HTTP_POST, &rp) && rp.status == 408, "half a body: the handler's read times out and the server answers 408", "");
        PROVEN_TEST_ASSERT(atomic_load(&g_body_error) == (int)PROVEN_ERR_TIMEOUT, "the handler saw PROVEN_ERR_TIMEOUT", "");
        (void)proven_net_close(&ca.conn);

        client_open(&ca, at);
        atomic_store(&g_body_error, 0);
        client_send(&ca, "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 10\r\n\r\nabc");
        (void)proven_net_shutdown_write(&ca.conn);
        for (int i = 0; i < 2000 && atomic_load(&g_body_error) == 0; ++i) pump(&ca);
        PROVEN_TEST_ASSERT(atomic_load(&g_body_error) == (int)PROVEN_ERR_RESET, "a client that leaves mid-body is PROVEN_ERR_RESET to the handler, not a short body", "");
        (void)proven_net_close(&ca.conn);
        settle();
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("max_connections",
        "A third client waits in the backlog until one of two leaves; it is not refused and not served early.",
        "Inspect the listening condition in proven_http_server_poll.");
    // ---------------------------------------------------------------
    {
        /* A server of its own, with the default timeouts: with the short ones above, two idle
         * connections could time out while the third is being watched, and make room for it. */
        proven_http_server_destroy(g_server);
        cfg.head_timeout_ms = 0;
        cfg.idle_timeout_ms = 0;
        cfg.body_timeout_ms = 0;
        e = proven_http_server_create(&cfg, &g_server);
        PROVEN_TEST_ASSERT(e == PROVEN_OK, "a server limited to two connections", "");
        e = proven_http_server_listen(g_server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at);
        PROVEN_TEST_ASSERT(e == PROVEN_OK, "it listens", "");
        client_open(&ca, at);
        client_open(&cb, at);
        client_send(&ca, "GET /hello HTTP/1.1\r\nHost: t\r\n\r\n");
        client_send(&cb, "GET /hello HTTP/1.1\r\nHost: t\r\n\r\n");
        PROVEN_TEST_ASSERT(client_reply(&ca, PROVEN_HTTP_GET, &rp) && client_reply(&cb, PROVEN_HTTP_GET, &rp), "two connections are served", "");
        client_open(&cc, at);
        client_send(&cc, "GET /hello HTTP/1.1\r\nHost: t\r\n\r\n");
        for (int i = 0; i < 20; ++i) pump(&cc);
        PROVEN_TEST_ASSERT(proven_http_server_connection_count(g_server) == 2 && cc.len == 0, "the third is not yet accepted", "");
        (void)proven_net_close(&ca.conn);
        PROVEN_TEST_ASSERT(client_reply(&cc, PROVEN_HTTP_GET, &rp) && rp.status == 200, "and is served once one of the two has left", "");
        (void)proven_net_close(&cb.conn);
        (void)proven_net_close(&cc.conn);
    }
    proven_http_server_destroy(g_server);
    g_server = NULL;
}

/* A job system that can take no more: the request is answered 503, not left waiting. */
static void run_overload(void) {
    proven_job_sys_t *small = NULL;
    proven_err_t e = proven_job_system_init(proven_heap_allocator(), 1, 2, &small);
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "a job system with one worker and a queue of two", "");
    proven_http_server_config_t cfg = {0};
    cfg.alloc = proven_heap_allocator();
    cfg.handler = handler;
    cfg.handler_ctx = &g_server;
    cfg.jobs = small;
    e = proven_http_server_create(&cfg, &g_server);
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "a server on it", "");
    proven_net_addr_t at;
    e = proven_http_server_listen(g_server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at);
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "it listens", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a full job queue",
        "Eight requests whose handlers do not return, one worker, a queue of two: the ones that do not fit are answered 503 and closed, the rest are served once the worker is free.",
        "Inspect the proven_job_submit_ex branch in sv_service.");
    // ---------------------------------------------------------------
    enum { N = 8 };
    static client_t many[N];
    atomic_store(&g_release, false);
    atomic_store(&g_hold_entered, 0);
    for (int i = 0; i < N; ++i) {
        client_open(&many[i], at);
        client_send(&many[i], "GET /hold HTTP/1.1\r\nHost: t\r\n\r\n");
    }
    /* Let the loop accept and dispatch all eight; the first handler is then holding the worker. */
    for (int i = 0; i < 3000 && atomic_load(&g_hold_entered) == 0; ++i) pump(NULL);
    for (int i = 0; i < 30; ++i) pump(NULL);
    int refused = 0, served = 0;
    for (int i = 0; i < N; ++i) {
        for (int k = 0; k < 20; ++k) pump(&many[i]);
        if (many[i].len > 0) {
            PROVEN_TEST_ASSERT(client_reply(&many[i], PROVEN_HTTP_GET, &rp) && rp.status == 503 && rp.close, "a request that did not fit is answered 503 with a close", "");
            refused++;
        }
    }
    PROVEN_TEST_ASSERT(refused >= 1 && refused < N, "some of the eight were refused, and not all", "");
    atomic_store(&g_release, true);
    for (int i = 0; i < N; ++i) {
        if (many[i].eof) continue;
        if (client_reply(&many[i], PROVEN_HTTP_GET, &rp) && rp.status == 200 && body_is(&rp, "held")) served++;
    }
    PROVEN_TEST_ASSERT(served + refused == N, "every request was answered, one way or the other", "");
    for (int i = 0; i < N; ++i) (void)proven_net_close(&many[i].conn);
    proven_http_server_destroy(g_server);
    g_server = NULL;
    proven_job_system_close(small);
    proven_job_system_destroy(small);
}

int main(void) {
    PROVEN_TEST_SUITE("http: the server",
        "Requests in, responses out, over loopback sockets, with handlers on the loop thread and on workers.",
        "Inspect src/proven/http_server.c.");

    {
        proven_net_listener_t probe;
        proven_err_t err = proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 1, &probe, NULL);
        if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
            PROVEN_TEST_INFO("SKIP: this environment refuses to open a listening socket (error {}).", PROVEN_ARG((int)err));
            PROVEN_TEST_PASS("skipped: no sockets here.");
            return 0;
        }
        PROVEN_TEST_ASSERT(err == PROVEN_OK, "a loopback listener on a free port opens", "");
        (void)proven_net_listener_close(&probe);
    }

    {
        proven_http_server_config_t bad = {0};
        proven_http_server_t *none = (proven_http_server_t *)&bad;
        PROVEN_TEST_ASSERT(proven_http_server_create(&bad, &none) == PROVEN_ERR_INVALID_ARG && none == NULL,
            "a configuration without an allocator or a handler is PROVEN_ERR_INVALID_ARG", "");
        proven_http_server_destroy(NULL);
        proven_http_server_stop(NULL);
    }

    PROVEN_TEST_INFO("model: handlers on the loop thread");
    run_cases(NULL);
    run_timeouts(NULL);

    proven_job_sys_t *jobs = NULL;
    proven_err_t e = proven_job_system_init(proven_heap_allocator(), 4, 64, &jobs);
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "a job system with four workers starts", "");
    PROVEN_TEST_INFO("model: handlers on a job system");
    run_cases(jobs);
    run_timeouts(jobs);
    run_overload();
    proven_job_system_close(jobs);
    proven_job_system_destroy(jobs);

    PROVEN_TEST_PASS("the server keeps its contract in both models.");
    return 0;
}
