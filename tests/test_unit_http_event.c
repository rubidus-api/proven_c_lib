#include "proven.h"
#include "proven_test.h"
#include "test_unit_tls_pki.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The event-driven HTTP server, over loopback.
 *
 * The loop runs on its own thread; the callbacks below are the "application" and run there.
 * The test's main thread is the clients: the library's blocking HTTP client where a client
 * that behaves is wanted, and raw sockets where one that does not is.
 *
 * What the callbacks record is read from the main thread, so it is atomic or guarded by the
 * exchange being over. Every case is run plain and then again over TLS.
 */

static proven_mem_view_t mv(const char *s) { return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }
static proven_u8str_view_t sv(const char *s) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }

static proven_allocator_t g_heap;
static proven_loop_t *g_loop;
static proven_http_event_server_t *g_server;
static proven_job_sys_t *g_threads, *g_workers;
static proven_tls_config_t *g_server_tls, *g_client_tls;

static atomic_int g_requests, g_done_ok, g_done_err, g_writable_calls, g_refused, g_last_error;
static atomic_ulong g_max_buffered;

/* Per-stream state the handlers attach. */
typedef struct {
    proven_http_stream_t *stream;
    proven_loop_timer_t timer;
    proven_size_t sent, total;         /* /big: bytes written so far, and to write */
    proven_size_t received;            /* /upload: bytes seen */
    unsigned sum;
    bool paused_once;
    char path[32];
} app_t;

static proven_byte_t big_byte(proven_size_t i) { return (proven_byte_t)('a' + (i * 7 + i / 251) % 26); }

/* Write as much of the big body as the server will take; the rest waits for on_writable. */
static void big_pump(app_t *a) {
    static _Thread_local proven_byte_t chunk[8192];
    while (a->sent < a->total) {
        proven_size_t n = a->total - a->sent < sizeof chunk ? a->total - a->sent : sizeof chunk;
        for (proven_size_t i = 0; i < n; ++i) chunk[i] = big_byte(a->sent + i);
        proven_result_size_t w = proven_http_stream_write(a->stream, (proven_mem_view_t){ .ptr = chunk, .size = n });
        if (w.err != PROVEN_OK) return;
        a->sent += w.value;
        unsigned long held = (unsigned long)proven_http_stream_buffered(a->stream);
        if (held > atomic_load(&g_max_buffered)) atomic_store(&g_max_buffered, held);
        if (w.value < n) { atomic_fetch_add(&g_refused, 1); return; }   /* refused: wait to be told there is room */
    }
    (void)proven_http_stream_end(a->stream);
}

static void later_fire(void *ctx) {
    app_t *a = ctx;
    (void)proven_http_stream_respond(a->stream, 200, NULL, 0, mv("later"));
}

static void resume_fire(void *ctx) { proven_http_stream_resume(((app_t *)ctx)->stream); }

/* Work done on another thread, and its answer posted back to the loop. */
static void work_reply(void *ctx) {
    app_t *a = ctx;
    (void)proven_http_stream_respond(a->stream, 200, NULL, 0, mv("worked"));
}
static void work_job(void *ctx) {
    proven_time_sleep(30);                        /* something that takes time, off the loop */
    while (proven_loop_post(g_loop, work_reply, ctx) != PROVEN_OK) { }
}

static void on_request(void *ctx, proven_http_stream_t *s, const proven_http_request_t *req) {
    (void)ctx;
    atomic_fetch_add(&g_requests, 1);
    app_t *a = calloc(1, sizeof *a);
    a->stream = s;
    proven_size_t n = req->target.size < sizeof a->path - 1 ? req->target.size : sizeof a->path - 1;
    memcpy(a->path, req->target.ptr, n);
    proven_http_stream_set_user(s, a);
    if (strcmp(a->path, "/") == 0) {
        proven_http_header_t type = { PROVEN_LIT("Content-Type"), PROVEN_LIT("text/plain") };
        (void)proven_http_stream_respond(s, 200, &type, 1, mv("hello"));
    } else if (strncmp(a->path, "/big/", 5) == 0) {
        a->total = (proven_size_t)atoi(a->path + 5);
        /* /big/N sends N bytes with a Content-Length; /big/N?c sends them chunked. */
        bool chunked = strchr(a->path, '?') != NULL;
        if (proven_http_stream_begin(s, 200, NULL, 0, chunked ? PROVEN_HTTP_EVENT_LENGTH_UNKNOWN : a->total) == PROVEN_OK) big_pump(a);
    } else if (strcmp(a->path, "/later") == 0) {
        proven_loop_timer_set(g_loop, &a->timer, 40, later_fire, a);
    } else if (strcmp(a->path, "/work") == 0) {
        if (!proven_job_submit(g_workers, work_job, a)) (void)proven_http_stream_respond(s, 503, NULL, 0, mv(""));
    } else if (strcmp(a->path, "/never") == 0) {
        /* No answer: the test closes the connection, or destroys the server, under it. */
    } else if (strcmp(a->path, "/abort") == 0) {
        proven_http_stream_abort(s);
    } else if (strcmp(a->path, "/nothing") == 0) {
        (void)proven_http_stream_respond(s, 204, NULL, 0, mv("ignored"));
    } else if (strcmp(a->path, "/early") == 0) {
        /* Answers before the body has arrived. */
        (void)proven_http_stream_respond(s, 403, NULL, 0, mv("no"));
    } else if (strcmp(a->path, "/bad-header") == 0) {
        proven_http_header_t own = { PROVEN_LIT("Content-Length"), PROVEN_LIT("3") };
        proven_err_t e = proven_http_stream_respond(s, 200, &own, 1, mv("abc"));
        atomic_store(&g_last_error, (int)e);
        (void)proven_http_stream_respond(s, 200, NULL, 0, mv("ok"));
    } else if (strcmp(a->path, "/short") == 0) {
        /* Promises ten bytes and ends after three. */
        (void)proven_http_stream_begin(s, 200, NULL, 0, 10);
        (void)proven_http_stream_write(s, mv("abc"));
        atomic_store(&g_last_error, (int)proven_http_stream_end(s));
    } else if (strcmp(a->path, "/upload") != 0 && strcmp(a->path, "/upload-slow") != 0) {
        (void)proven_http_stream_respond(s, 404, NULL, 0, mv("not here"));
    }
    /* /upload and /upload-slow answer when the body is complete: see on_body. */
}

static void on_body(void *ctx, proven_http_stream_t *s, proven_mem_view_t piece, bool last) {
    (void)ctx;
    app_t *a = proven_http_stream_user(s);
    if (!a) return;
    for (proven_size_t i = 0; i < piece.size; ++i) a->sum += piece.ptr[i];
    a->received += piece.size;
    if (strcmp(a->path, "/upload-slow") == 0 && !a->paused_once && !last) {
        /* Take a breath: stop the body for 60 ms, then let it flow again. */
        a->paused_once = true;
        proven_http_stream_pause(s);
        proven_loop_timer_set(g_loop, &a->timer, 60, resume_fire, a);
    }
    if (last && strncmp(a->path, "/upload", 7) == 0) {
        char text[64];
        int n = snprintf(text, sizeof text, "%u %u", (unsigned)a->received, a->sum);
        (void)proven_http_stream_respond(s, 200, NULL, 0, (proven_mem_view_t){ .ptr = (const proven_byte_t *)text, .size = (proven_size_t)n });
    }
}

static void on_writable(void *ctx, proven_http_stream_t *s) {
    (void)ctx;
    atomic_fetch_add(&g_writable_calls, 1);
    app_t *a = proven_http_stream_user(s);
    if (a && a->total > 0) big_pump(a);
}

static void on_done(void *ctx, proven_http_stream_t *s, proven_err_t why) {
    (void)ctx;
    app_t *a = proven_http_stream_user(s);
    if (a) { proven_loop_timer_cancel(g_loop, &a->timer); free(a); }
    if (why == PROVEN_OK) atomic_fetch_add(&g_done_ok, 1);
    else { atomic_fetch_add(&g_done_err, 1); atomic_store(&g_last_error, (int)why); }
}

static void run_loop(void *arg) { (void)arg; (void)proven_loop_run(g_loop); }

/* Things that must happen on the loop's thread, asked for from the test's. */
static atomic_int g_ran;
static atomic_ulong g_conn_count;
static void do_count(void *ctx) { (void)ctx; atomic_store(&g_conn_count, (unsigned long)proven_http_event_server_connections(g_server)); atomic_fetch_add(&g_ran, 1); }
static void do_destroy(void *ctx) { (void)ctx; proven_http_event_server_destroy(g_server); g_server = NULL; atomic_fetch_add(&g_ran, 1); }
static void on_loop(proven_loop_fn fn) {
    int before = atomic_load(&g_ran);
    PROVEN_TEST_ASSERT(proven_loop_post(g_loop, fn, NULL) == PROVEN_OK, "posted to the loop", "");
    for (int i = 0; i < 4000 && atomic_load(&g_ran) == before; ++i) proven_time_sleep(1);
}
static unsigned long connections(void) { on_loop(do_count); return atomic_load(&g_conn_count); }

static bool settled(int requests) {
    for (int i = 0; i < 4000; ++i) {
        if (atomic_load(&g_done_ok) + atomic_load(&g_done_err) == requests) return true;
        proven_time_sleep(1);
    }
    return false;
}

static char g_url[160];
static proven_u8str_view_t url(bool tls, proven_u16 port, const char *path) {
    int n = snprintf(g_url, sizeof g_url, "%s://127.0.0.1:%u%s", tls ? "https" : "http", (unsigned)port, path);
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)g_url, .size = (proven_size_t)n };
}

static bool body_is(proven_http_client_response_t *resp, const char *want) {
    proven_u8str_t body = { 0 };
    bool ok = proven_http_client_read_all(resp, g_heap, &body, 1024 * 1024) == PROVEN_OK;
    proven_u8str_view_t v = proven_u8str_as_view(&body);
    ok = ok && v.size == strlen(want) && (v.size == 0 || memcmp(v.ptr, want, v.size) == 0);
    proven_u8str_destroy(g_heap, &body);
    return ok;
}

/* A raw client: plain, or TLS over the same socket. */
typedef struct { proven_net_conn_t sock; proven_transport_t t; bool tls; } raw_t;
static bool raw_open(raw_t *r, bool tls, proven_u16 port) {
    r->tls = tls;
    if (proven_net_connect(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, port), proven_net_deadline_in(5000), &r->sock) != PROVEN_OK) return false;
    r->t = proven_net_conn_transport(&r->sock);
    if (!tls) return true;
    return proven_tls_transport_client(r->t, g_client_tls, sv("127.0.0.1"), NULL, proven_net_deadline_in(5000), &r->t) == PROVEN_OK;
}
static bool raw_send(raw_t *r, const char *text) { return proven_transport_write_all(r->t, mv(text), proven_net_deadline_in(5000)).err == PROVEN_OK; }
/* Read until the peer closes or `ms` passes with nothing; returns what came. */
static proven_size_t raw_read(raw_t *r, char *out, proven_size_t cap, proven_u32 ms, proven_err_t *end) {
    proven_size_t n = 0;
    for (;;) {
        proven_result_size_t got = proven_transport_read(r->t, (proven_mem_mut_t){ .ptr = (proven_byte_t *)out + n, .size = cap - 1 - n }, proven_net_deadline_in(ms));
        if (got.err != PROVEN_OK) { if (end) *end = got.err; break; }
        n += got.value;
        if (n == cap - 1) { if (end) *end = PROVEN_OK; break; }
    }
    out[n] = 0;
    return n;
}
static void raw_close(raw_t *r) { (void)proven_transport_close(r->t); }
static int count_of(const char *hay, const char *needle) { int c = 0; for (const char *p = hay; (p = strstr(p, needle)) != NULL; ++p) c++; return c; }

static void cases(bool tls) {
    const char *how = tls ? "over TLS" : "plain";
    atomic_store(&g_requests, 0); atomic_store(&g_done_ok, 0); atomic_store(&g_done_err, 0);
    atomic_store(&g_writable_calls, 0); atomic_store(&g_max_buffered, 0); atomic_store(&g_last_error, 0);
    PROVEN_TEST_ASSERT(proven_loop_create(g_heap, &g_loop) == PROVEN_OK, "a loop", how);
    proven_http_event_server_config_t cfg = {
        .on = { on_request, on_body, on_writable, on_done },
        .max_buffered_output = 32 * 1024, .max_body_bytes = 2 * 1024 * 1024, .max_head_bytes = 4096,
        .head_timeout_ms = 500, .idle_timeout_ms = 60000, .tls = tls ? g_server_tls : NULL,
    };
    proven_net_addr_t at;
    PROVEN_TEST_ASSERT(proven_http_event_server_create(g_loop, &cfg, &g_server) == PROVEN_OK &&
                       proven_http_event_server_listen(g_server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "a server on it", how);
    proven_job_group_t running;
    proven_job_group_init(&running);
    PROVEN_TEST_ASSERT(proven_job_group_submit(g_threads, &running, run_loop, NULL) == PROVEN_OK, "the loop runs on its own thread", how);

    proven_http_client_config_t ccfg = { .alloc = g_heap, .max_idle_connections = 2, .tls_wrap = tls ? proven_tls_http_wrap : NULL, .tls_ctx = g_client_tls };
    proven_http_client_t *client = NULL;
    PROVEN_TEST_ASSERT(proven_http_client_create(&ccfg, &client) == PROVEN_OK, "a client", how);
    proven_http_client_response_t resp;
    int expected = 0;

    for (int i = 0; i < 3; ++i) {
        proven_err_t e = proven_http_client_get(client, url(tls, at.port, "/"), &resp);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && resp.status == 200 && body_is(&resp, "hello"), "a request answered inside on_request", how);
        proven_http_client_finish(&resp);
        expected++;
    }
    PROVEN_TEST_ASSERT(settled(expected) && connections() == 1, "three requests on one kept connection, each ended with on_done", how);

    /* An upload: the body arrives in pieces and is answered when the last has come. */
    static proven_byte_t upload[300000];
    unsigned sum = 0;
    for (proven_size_t i = 0; i < sizeof upload; ++i) { upload[i] = (proven_byte_t)(i * 5 + 3); sum += upload[i]; }
    char want[64];
    snprintf(want, sizeof want, "%u %u", (unsigned)sizeof upload, sum);
    proven_http_client_request_t req = { .method = sv("POST"), .url = url(tls, at.port, "/upload"), .body = { .ptr = upload, .size = sizeof upload } };
    PROVEN_TEST_ASSERT(proven_http_client_send(client, &req, &resp) == PROVEN_OK && resp.status == 200 && body_is(&resp, want), "a 300,000-byte body delivered in pieces, all of it", how);
    proven_http_client_finish(&resp);
    req.url = url(tls, at.port, "/upload-slow");
    proven_time_t t0 = proven_time_monotonic_now();
    PROVEN_TEST_ASSERT(proven_http_client_send(client, &req, &resp) == PROVEN_OK && resp.status == 200 && body_is(&resp, want) && proven_time_monotonic_now() - t0 > 50000000,
        "a handler that pauses the body and resumes it later still receives every byte", how);
    proven_http_client_finish(&resp);
    expected += 2;

    /* A download far larger than the output limit and than any socket buffer, to a client that
     * does not read at first: the server must hold back rather than pile it up. Then a smaller
     * one sent chunked, for the framing. */
    for (int chunked = 0; chunked < 2; ++chunked) {
        const proven_size_t total = chunked ? 3000000 : 16000000;
        atomic_store(&g_max_buffered, 0);
        int writable_before = atomic_load(&g_writable_calls), refused_before = atomic_load(&g_refused);
        PROVEN_TEST_ASSERT(proven_http_client_get(client, url(tls, at.port, chunked ? "/big/3000000?c" : "/big/16000000"), &resp) == PROVEN_OK && resp.status == 200, "a large response begins", how);
        static proven_byte_t piece[65536];
        proven_size_t got = 0;
        bool intact = true;
        proven_err_t last_read = PROVEN_OK;
        /* Read nothing until the server has been refused once: how soon that is depends on the
         * machine, and a client that starts early may keep up and never let it happen. */
        if (!chunked) { for (int i = 0; i < 20000 && atomic_load(&g_refused) == refused_before; ++i) proven_time_sleep(1); }
        else proven_time_sleep(150);
        for (;;) {
            proven_result_size_t r = proven_http_client_read(&resp, (proven_mem_mut_t){ .ptr = piece, .size = sizeof piece });
            if (r.err != PROVEN_OK) { last_read = r.err; break; }
            for (proven_size_t i = 0; i < r.value; i += 61) intact = intact && piece[i] == big_byte(got + i);
            got += r.value;
        }
        proven_http_client_finish(&resp);
        expected++;
        if (got != total || !intact) fprintf(stderr, "download: got %u of %u, intact %d, last client error %d, done ok %d err %d (last %d)\n", (unsigned)got, (unsigned)total, (int)intact, (int)last_read, atomic_load(&g_done_ok), atomic_load(&g_done_err), atomic_load(&g_last_error));
        PROVEN_TEST_ASSERT(got == total && intact, chunked ? "3 MB arrive intact, chunked" : "16 MB arrive intact, with a Content-Length", how);
        if (!chunked) {
            PROVEN_TEST_ASSERT(atomic_load(&g_refused) > refused_before && atomic_load(&g_writable_calls) > writable_before, "writes were refused while the client was not reading, and resumed through on_writable", how);
        }
        PROVEN_TEST_ASSERT(atomic_load(&g_max_buffered) <= 32 * 1024 + 20 * 1024, "the server never held more than the output limit and one piece", how);
    }

    /* Answers that come later: from a timer, and from another thread through the loop. */
    PROVEN_TEST_ASSERT(proven_http_client_get(client, url(tls, at.port, "/later"), &resp) == PROVEN_OK && body_is(&resp, "later"), "a response sent from a timer, after on_request returned", how);
    proven_http_client_finish(&resp);
    PROVEN_TEST_ASSERT(proven_http_client_get(client, url(tls, at.port, "/work"), &resp) == PROVEN_OK && body_is(&resp, "worked"), "a response computed on another thread and posted back", how);
    proven_http_client_finish(&resp);
    PROVEN_TEST_ASSERT(proven_http_client_get(client, url(tls, at.port, "/nothing"), &resp) == PROVEN_OK && resp.status == 204 && body_is(&resp, ""), "a 204 carries no body whatever was given", how);
    proven_http_client_finish(&resp);
    PROVEN_TEST_ASSERT(proven_http_client_get(client, url(tls, at.port, "/missing"), &resp) == PROVEN_OK && resp.status == 404, "a 404", how);
    proven_http_client_finish(&resp);
    PROVEN_TEST_ASSERT(proven_http_client_get(client, url(tls, at.port, "/bad-header"), &resp) == PROVEN_OK && body_is(&resp, "ok") && atomic_load(&g_last_error) == PROVEN_ERR_INVALID_ARG,
        "a header the server writes itself is PROVEN_ERR_INVALID_ARG, and the stream can still be answered", how);
    proven_http_client_finish(&resp);
    expected += 5;
    PROVEN_TEST_ASSERT(settled(expected) && atomic_load(&g_done_err) == 0, "every exchange so far ended with on_done(PROVEN_OK)", how);

    /* Three requests in one write, the middle one answered late: the answers keep their order. */
    raw_t raw;
    static char text[70000];
    proven_err_t end = PROVEN_OK;
    PROVEN_TEST_ASSERT(raw_open(&raw, tls, at.port) && raw_send(&raw, "GET / HTTP/1.1\r\nHost: x\r\n\r\nGET /later HTTP/1.1\r\nHost: x\r\n\r\nGET /missing HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"), "three pipelined requests", how);
    raw_read(&raw, text, sizeof text, 3000, &end);
    const char *first = strstr(text, "hello"), *second = strstr(text, "later"), *third = strstr(text, "not here");
    PROVEN_TEST_ASSERT(first && second && third && first < second && second < third && count_of(text, "HTTP/1.1 ") == 3 && end == PROVEN_ERR_EOF,
        "three answers in the order asked, then the connection closes as the last request said", how);
    raw_close(&raw);
    expected += 3;

    /* HEAD, HTTP/1.0, and a request whose answer comes before its body. */
    PROVEN_TEST_ASSERT(raw_open(&raw, tls, at.port) && raw_send(&raw, "HEAD / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"), "HEAD", how);
    raw_read(&raw, text, sizeof text, 3000, &end);
    PROVEN_TEST_ASSERT(strstr(text, "Content-Length: 5\r\n") && !strstr(text, "hello"), "a response to HEAD has the headers of the body and none of its bytes", how);
    raw_close(&raw);
    PROVEN_TEST_ASSERT(raw_open(&raw, tls, at.port) && raw_send(&raw, "GET /big/100?c HTTP/1.0\r\n\r\n"), "an HTTP/1.0 request", how);
    proven_size_t n10 = raw_read(&raw, text, sizeof text, 3000, &end);
    const char *body10 = strstr(text, "\r\n\r\n");
    PROVEN_TEST_ASSERT(body10 && (proven_size_t)(text + n10 - (body10 + 4)) == 100 && !strstr(text, "chunked") && strstr(text, "Connection: close") && end == PROVEN_ERR_EOF,
        "to HTTP/1.0 a body of unknown length is sent plain and ended by closing", how);
    raw_close(&raw);
    PROVEN_TEST_ASSERT(raw_open(&raw, tls, at.port) && raw_send(&raw, "POST /early HTTP/1.1\r\nHost: x\r\nContent-Length: 100000\r\n\r\nonly a little"), "a request answered before its body arrives", how);
    raw_read(&raw, text, sizeof text, 3000, &end);
    PROVEN_TEST_ASSERT(strstr(text, "HTTP/1.1 403") && strstr(text, "Connection: close") && end == PROVEN_ERR_EOF, "the answer says the connection will close, and it does", how);
    raw_close(&raw);
    PROVEN_TEST_ASSERT(raw_open(&raw, tls, at.port) && raw_send(&raw, "POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\nExpect: 100-continue\r\n\r\n"), "Expect: 100-continue", how);
    proven_time_sleep(150);
    PROVEN_TEST_ASSERT(raw_send(&raw, "abc"), "the body follows", how);
    raw_read(&raw, text, sizeof text, 600, &end);
    PROVEN_TEST_ASSERT(strstr(text, "HTTP/1.1 100 Continue\r\n\r\n") && strstr(text, "HTTP/1.1 200") && strstr(text, "3 294"), "the server says 100 Continue, then answers the upload", how);
    raw_close(&raw);
    expected += 4;
    PROVEN_TEST_ASSERT(settled(expected), "all of those ended", how);

    /* Requests the server refuses itself: no on_request, an answer, a closed connection. */
    static const struct { const char *send; const char *status; const char *what; } refused[] = {
        { "NOT A REQUEST LINE\r\n\r\n", "HTTP/1.1 400", "a malformed request line is 400" },
        { "GET / HTTP/1.1\r\n\r\n", "HTTP/1.1 400", "HTTP/1.1 without Host is 400" },
        { "GET / HTTP/2.0\r\nHost: x\r\n\r\n", "HTTP/1.1 505", "another HTTP version is 505" },
        { "POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 9999999\r\n\r\n", "HTTP/1.1 413", "a body announced above the limit is 413" },
        { "POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\n\r\n", "HTTP/1.1 501", "a transfer coding it does not know is 501" },
    };
    int before = atomic_load(&g_requests);
    for (proven_size_t i = 0; i < sizeof refused / sizeof refused[0]; ++i) {
        PROVEN_TEST_ASSERT(raw_open(&raw, tls, at.port) && raw_send(&raw, refused[i].send), "a request to refuse", how);
        raw_read(&raw, text, sizeof text, 3000, &end);
        PROVEN_TEST_ASSERT(strstr(text, refused[i].status) && end == PROVEN_ERR_EOF, refused[i].what, how);
        raw_close(&raw);
    }
    static char long_head[6000];
    memset(long_head, 'a', sizeof long_head - 1);
    memcpy(long_head, "GET /", 5);
    PROVEN_TEST_ASSERT(raw_open(&raw, tls, at.port) && raw_send(&raw, long_head), "a head that never ends", how);
    raw_read(&raw, text, sizeof text, 3000, &end);
    PROVEN_TEST_ASSERT(strstr(text, "HTTP/1.1 431") && atomic_load(&g_requests) == before, "a head larger than the limit is 431; none of these reached on_request", how);
    raw_close(&raw);

    /* A chunked upload with broken framing fails after on_request: the handler is told. */
    int err_before = atomic_load(&g_done_err);
    PROVEN_TEST_ASSERT(raw_open(&raw, tls, at.port) && raw_send(&raw, "POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\nZZ\r\n"), "a chunked body with a bad size line", how);
    raw_read(&raw, text, sizeof text, 3000, &end);
    expected++;
    PROVEN_TEST_ASSERT(strstr(text, "HTTP/1.1 400") && settled(expected) && atomic_load(&g_done_err) == err_before + 1 && atomic_load(&g_last_error) == PROVEN_ERR_INVALID_FORMAT,
        "it is answered 400, and on_done says PROVEN_ERR_INVALID_FORMAT", how);
    raw_close(&raw);

    /* A handler that ends short of what it promised: the connection is cut, not completed. */
    PROVEN_TEST_ASSERT(raw_open(&raw, tls, at.port) && raw_send(&raw, "GET /short HTTP/1.1\r\nHost: x\r\n\r\n"), "a handler that writes less than its Content-Length", how);
    raw_read(&raw, text, sizeof text, 3000, &end);
    expected++;
    PROVEN_TEST_ASSERT(strstr(text, "Content-Length: 10") && settled(expected) && atomic_load(&g_last_error) == PROVEN_ERR_INVALID_FORMAT && end != PROVEN_OK,
        "proven_http_stream_end reports PROVEN_ERR_INVALID_FORMAT and the client sees the connection end early", how);
    raw_close(&raw);

    /* The client goes away while the handler has not answered; and a handler that aborts. */
    err_before = atomic_load(&g_done_err);
    PROVEN_TEST_ASSERT(raw_open(&raw, tls, at.port) && raw_send(&raw, "GET /big/50000000 HTTP/1.1\r\nHost: x\r\n\r\n"), "a 50 MB response is begun", how);
    proven_time_sleep(80);
    raw_close(&raw);
    expected++;
    PROVEN_TEST_ASSERT(settled(expected) && atomic_load(&g_done_err) == err_before + 1, "a client that leaves in the middle of a response ends the exchange with an error", how);
    PROVEN_TEST_ASSERT(raw_open(&raw, tls, at.port) && raw_send(&raw, "GET /abort HTTP/1.1\r\nHost: x\r\n\r\n"), "a request the handler aborts", how);
    proven_size_t na = raw_read(&raw, text, sizeof text, 3000, &end);
    expected++;
    PROVEN_TEST_ASSERT(na == 0 && end != PROVEN_OK && settled(expected) && atomic_load(&g_last_error) == PROVEN_ERR_RESET, "proven_http_stream_abort closes the connection with nothing sent, and on_done says PROVEN_ERR_RESET", how);
    raw_close(&raw);

    /* Time limits. */
    PROVEN_TEST_ASSERT(raw_open(&raw, tls, at.port) && raw_send(&raw, "GET / HTTP/1.1\r\nHost"), "half a request head, and then silence", how);
    t0 = proven_time_monotonic_now();
    raw_read(&raw, text, sizeof text, 3000, &end);
    proven_time_t took = proven_time_monotonic_now() - t0;
    PROVEN_TEST_ASSERT(strstr(text, "HTTP/1.1 408") && took > 250000000 && took < 3000000000, "after the head timeout the server answers 408 and closes", how);
    raw_close(&raw);

    PROVEN_TEST_ASSERT(connections() <= 2, "nothing is left open but the client's kept connection", how);

    /* Many connections that say nothing: what one costs. */
    {
        enum { IDLE = 400 };
        static proven_net_conn_t idle[IDLE];
        static raw_t idle_tls[40];
        int opened = 0;
        int n = tls ? 40 : IDLE;
        for (int i = 0; i < n; ++i) {
            if (tls) { if (raw_open(&idle_tls[i], true, at.port) && raw_send(&idle_tls[i], "GET / HTTP/1.1\r\nHost: x\r\n\r\n")) opened++; }
            else if (proven_net_connect(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, at.port), proven_net_deadline_in(5000), &idle[i]) == PROVEN_OK &&
                     proven_net_write_all(&idle[i], mv("GET / HTTP/1.1\r\nHost: x\r\n\r\n"), proven_net_deadline_in(5000)).err == PROVEN_OK) opened++;
        }
        expected += opened;
        if (!(opened == n && settled(expected) && connections() >= (unsigned long)n)) fprintf(stderr, "idle: opened %d of %d, done %d of %d (err %d, last %d), connections %lu\n", opened, n, atomic_load(&g_done_ok) + atomic_load(&g_done_err), expected, atomic_load(&g_done_err), atomic_load(&g_last_error), connections());
        PROVEN_TEST_ASSERT(opened == n && settled(expected) && connections() >= (unsigned long)n, "many connections, each having made one request, now idle", how);
        PROVEN_TEST_ASSERT(proven_http_client_get(client, url(tls, at.port, "/"), &resp) == PROVEN_OK && body_is(&resp, "hello"), "and a request among them is served as before", how);
        proven_http_client_finish(&resp);
        expected++;
        for (int i = 0; i < n; ++i) { if (tls) raw_close(&idle_tls[i]); else (void)proven_net_close(&idle[i]); }
        for (int i = 0; i < 3000 && connections() > 2; ++i) proven_time_sleep(2);
        PROVEN_TEST_ASSERT(connections() <= 2, "they close when their clients do", how);
    }

    /* Destroying the server under an exchange that has not been answered. */
    err_before = atomic_load(&g_done_err);
    PROVEN_TEST_ASSERT(raw_open(&raw, tls, at.port) && raw_send(&raw, "GET /never HTTP/1.1\r\nHost: x\r\n\r\n"), "a request the handler never answers", how);
    expected++;
    for (int i = 0; i < 2000 && atomic_load(&g_requests) < expected - 0 && atomic_load(&g_requests) < before + 1000; ++i) { if (atomic_load(&g_requests) >= 1 && atomic_load(&g_done_ok) + atomic_load(&g_done_err) == expected - 1) break; proven_time_sleep(1); }
    proven_time_sleep(60);
    proven_http_client_destroy(client);
    on_loop(do_destroy);
    PROVEN_TEST_ASSERT(g_server == NULL && settled(expected) && atomic_load(&g_done_err) == err_before + 1 && atomic_load(&g_last_error) == PROVEN_ERR_RESET,
        "destroying the server ends it with on_done(PROVEN_ERR_RESET)", how);
    raw_read(&raw, text, sizeof text, 1000, &end);
    PROVEN_TEST_ASSERT(end != PROVEN_OK, "and the client's connection is closed", how);
    raw_close(&raw);
    PROVEN_TEST_ASSERT(atomic_load(&g_requests) == atomic_load(&g_done_ok) + atomic_load(&g_done_err), "on_done was called exactly once for every on_request", how);

    proven_loop_stop(g_loop);
    proven_job_group_wait(g_threads, &running);
    proven_loop_destroy(g_loop);
    g_loop = NULL;
}

/* What an idle connection holds, measured with a counting allocator and the loop driven by
 * hand on this thread. */
typedef struct { proven_allocator_t heap; proven_size_t live; } counting_t;
typedef struct { proven_size_t size; proven_size_t pad; } count_head_t;
static proven_result_mem_mut_t count_alloc(void *ctx, proven_size_t size, proven_size_t align) {
    counting_t *c = ctx; (void)align;
    proven_result_mem_mut_t m = c->heap.alloc_fn(c->heap.ctx, size + sizeof(count_head_t), 16);
    if (m.err != PROVEN_OK) return m;
    ((count_head_t *)(void *)m.value.ptr)->size = size;
    c->live += size;
    m.value.ptr += sizeof(count_head_t); m.value.size = size;
    return m;
}
static void count_free(void *ctx, void *ptr) {
    counting_t *c = ctx;
    if (!ptr) return;
    count_head_t *h = (count_head_t *)(void *)((proven_byte_t *)ptr - sizeof(count_head_t));
    c->live -= h->size;
    c->heap.free_fn(c->heap.ctx, h);
}
static proven_result_mem_mut_t count_realloc(void *ctx, void *old, proven_size_t old_size, proven_size_t new_size, proven_size_t align) {
    proven_result_mem_mut_t m = count_alloc(ctx, new_size, align);
    if (m.err != PROVEN_OK) return m;
    memcpy(m.value.ptr, old, old_size < new_size ? old_size : new_size);
    count_free(ctx, old);
    return m;
}
static void quiet_request(void *ctx, proven_http_stream_t *s, const proven_http_request_t *req) {
    (void)ctx; (void)req;
    (void)proven_http_stream_respond(s, 200, NULL, 0, mv("ok"));
}

static void memory_case(void) {
    counting_t count = { .heap = g_heap, .live = 0 };
    proven_allocator_t alloc = { .ctx = &count, .alloc_fn = count_alloc, .realloc_fn = count_realloc, .free_fn = count_free };
    proven_loop_t *loop = NULL;
    proven_http_event_server_t *server = NULL;
    proven_http_event_server_config_t cfg = { .alloc = alloc, .on = { .on_request = quiet_request } };
    proven_net_addr_t at;
    PROVEN_TEST_ASSERT(proven_loop_create(g_heap, &loop) == PROVEN_OK && proven_http_event_server_create(loop, &cfg, &server) == PROVEN_OK &&
                       proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "a server with a counting allocator", "");
    enum { N = 300 };
    static proven_net_conn_t socks[N];
    proven_size_t base = count.live;
    for (int i = 0; i < N; ++i) {
        PROVEN_TEST_ASSERT(proven_net_connect(at, proven_net_deadline_in(5000), &socks[i]) == PROVEN_OK &&
                           proven_net_write_all(&socks[i], mv("GET / HTTP/1.1\r\nHost: x\r\n\r\n"), proven_net_deadline_in(5000)).err == PROVEN_OK, "a connection that makes one request", "");
        if (i % 16 == 15) PROVEN_TEST_ASSERT(proven_loop_poll(loop, proven_net_deadline_in(20)) == PROVEN_OK, "a round", "");
    }
    proven_net_deadline_t until = proven_net_deadline_in(400);
    while (proven_time_monotonic_now() < until) PROVEN_TEST_ASSERT(proven_loop_poll(loop, until) == PROVEN_OK, "a round", "");
    PROVEN_TEST_ASSERT(proven_http_event_server_connections(server) == N, "three hundred connections, each answered once, all idle", "");
    proven_size_t each = (count.live - base) / N;
    fprintf(stderr, "[PROVEN][TEST][INFO] an idle plain connection holds %u bytes\n", (unsigned)each);
    PROVEN_TEST_ASSERT(each < 512, "an idle connection holds less than 512 bytes: its struct, and no buffer", "");
    for (int i = 0; i < N; ++i) (void)proven_net_close(&socks[i]);
    until = proven_net_deadline_in(300);
    while (proven_time_monotonic_now() < until) PROVEN_TEST_ASSERT(proven_loop_poll(loop, until) == PROVEN_OK, "a round", "");
    PROVEN_TEST_ASSERT(proven_http_event_server_connections(server) == 0 && count.live == base, "closed by their clients, they have given everything back", "");
    proven_http_event_server_destroy(server);
    PROVEN_TEST_ASSERT(count.live == 0, "and so has the server, destroyed", "");
    proven_loop_destroy(loop);
}

/* A connection that has been answered and then says nothing is closed after the idle timeout. */
static void idle_case(void) {
    proven_loop_t *loop = NULL;
    proven_http_event_server_t *server = NULL;
    proven_http_event_server_config_t cfg = { .on = { .on_request = quiet_request }, .idle_timeout_ms = 200 };
    proven_net_addr_t at;
    proven_net_conn_t sock;
    PROVEN_TEST_ASSERT(proven_loop_create(g_heap, &loop) == PROVEN_OK && proven_http_event_server_create(loop, &cfg, &server) == PROVEN_OK &&
                       proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "a server with a 200 ms idle timeout", "");
    PROVEN_TEST_ASSERT(proven_net_connect(at, proven_net_deadline_in(5000), &sock) == PROVEN_OK &&
                       proven_net_write_all(&sock, mv("GET / HTTP/1.1\r\nHost: x\r\n\r\n"), proven_net_deadline_in(5000)).err == PROVEN_OK, "a connection that makes one request", "");
    bool seen = false;
    proven_time_t answered = 0, closed = 0;
    proven_net_deadline_t until = proven_net_deadline_in(5000);
    while (proven_time_monotonic_now() < until && closed == 0) {
        PROVEN_TEST_ASSERT(proven_loop_poll(loop, proven_net_deadline_in(10)) == PROVEN_OK, "a round", "");
        proven_size_t open = proven_http_event_server_connections(server);
        if (!seen && open == 1) { seen = true; answered = proven_time_monotonic_now(); }
        if (seen && open == 0) closed = proven_time_monotonic_now();
    }
    char text[256];
    proven_result_size_t got = proven_net_read(&sock, (proven_mem_mut_t){ .ptr = (proven_byte_t *)text, .size = sizeof text - 1 }, proven_net_deadline_in(2000));
    PROVEN_TEST_ASSERT(got.err == PROVEN_OK && got.value > 12 && memcmp(text, "HTTP/1.1 200", 12) == 0, "it was answered", "");
    PROVEN_TEST_ASSERT(seen && closed != 0 && closed - answered >= 100000000, "and closed by the server after the idle timeout, not before", "");
    got = proven_net_read(&sock, (proven_mem_mut_t){ .ptr = (proven_byte_t *)text, .size = sizeof text }, proven_net_deadline_in(2000));
    PROVEN_TEST_ASSERT(got.err != PROVEN_OK || got.value == 0, "the client sees the end of the connection", "");
    (void)proven_net_close(&sock);
    proven_http_event_server_destroy(server);
    proven_loop_destroy(loop);
}

int main(void) {
    PROVEN_TEST_SUITE("the event-driven HTTP server",
        "Requests answered at once and later, bodies in pieces with backpressure both ways, pipelining, refusals, time limits and disappearing clients - plain and over TLS - and what an idle connection holds.",
        "Inspect src/proven/http_event.c: ev_process for input, ev_flush/ev_progress for output and what follows it, ev_finish for the end of an exchange.");

    g_heap = proven_heap_allocator();
    PROVEN_TEST_ASSERT(tp_build(), "the test's certificates and keys are issued", "");
    PROVEN_TEST_ASSERT(proven_job_system_init(g_heap, 1, 4, &g_threads) == PROVEN_OK && proven_job_system_init(g_heap, 2, 64, &g_workers) == PROVEN_OK, "threads", "");
    proven_cert_store_t *anchors = NULL;
    PROVEN_TEST_ASSERT(proven_cert_store_create(g_heap, &anchors) == PROVEN_OK &&
                       proven_cert_store_add_pem(anchors, (proven_mem_view_t){ (const proven_byte_t *)TP_CA, strlen(TP_CA) }, NULL) == PROVEN_OK, "anchors", "");
    proven_tls_options_t co = { .alloc = g_heap, .anchors = anchors };
    proven_tls_options_t so = { .alloc = g_heap, .certificate_pem = { (const proven_byte_t *)TP_SERVER_ED, strlen(TP_SERVER_ED) }, .private_key_pem = { (const proven_byte_t *)TP_SERVER_ED_KEY, strlen(TP_SERVER_ED_KEY) } };
    PROVEN_TEST_ASSERT(proven_tls_config_create(&co, &g_client_tls) == PROVEN_OK && proven_tls_config_create(&so, &g_server_tls) == PROVEN_OK, "TLS configurations", "");

    PROVEN_TEST_SECTION("making a server", "What a configuration must have.", "Check proven_http_event_server_create.");
    {
        proven_loop_t *loop = NULL;
        proven_http_event_server_t *s = (proven_http_event_server_t *)1;
        proven_http_event_server_config_t none = { 0 }, with_client_tls = { .on = { .on_request = quiet_request }, .tls = g_client_tls };
        PROVEN_TEST_ASSERT(proven_loop_create(g_heap, &loop) == PROVEN_OK, "a loop", "");
        PROVEN_TEST_ASSERT(proven_http_event_server_create(loop, &none, &s) == PROVEN_ERR_INVALID_ARG && s == NULL, "no on_request is PROVEN_ERR_INVALID_ARG", "");
        PROVEN_TEST_ASSERT(proven_http_event_server_create(NULL, &with_client_tls, &s) == PROVEN_ERR_INVALID_ARG, "and so is no loop", "");
        PROVEN_TEST_ASSERT(proven_http_event_server_create(loop, &with_client_tls, &s) == PROVEN_ERR_INVALID_ARG && s == NULL, "a TLS configuration with no certificate cannot serve, and is refused here", "");
        proven_http_event_server_destroy(NULL);
        proven_loop_destroy(loop);
    }

    PROVEN_TEST_SECTION("plain HTTP", "Every case over an unencrypted connection.", "");
    cases(false);
    PROVEN_TEST_SECTION("over TLS", "The same cases with the TLS engine between the socket and the parser.", "Check the TLS branches of ev_readable and ev_flush.");
    cases(true);
    PROVEN_TEST_SECTION("over TLS 1.2", "The same cases once more, with clients that speak nothing newer than TLS 1.2.", "Check the TLS branches of ev_readable and ev_flush; the 1.2 handshake itself is test_unit_tls's business.");
    {
        proven_tls_config_t *any = g_client_tls;
        proven_tls_options_t co12 = co;
        co12.max_version = PROVEN_TLS_VERSION_1_2;
        PROVEN_TEST_ASSERT(proven_tls_config_create(&co12, &g_client_tls) == PROVEN_OK, "a client configuration limited to TLS 1.2", "");
        cases(true);
        proven_tls_config_destroy(g_client_tls);
        g_client_tls = any;
    }
    PROVEN_TEST_SECTION("what an idle connection holds", "Three hundred connections that have each made a request and now say nothing.", "Check that ev_process releases the stash and ev_flush the output buffer.");
    memory_case();
    PROVEN_TEST_SECTION("the idle timeout", "A connection that has been answered and then says nothing.", "Check ev_arm: the idle timer is set when an exchange ends with nothing buffered.");
    idle_case();

    proven_tls_config_destroy(g_client_tls); proven_tls_config_destroy(g_server_tls); proven_cert_store_destroy(anchors);
    proven_job_system_close(g_workers); proven_job_system_destroy(g_workers);
    proven_job_system_close(g_threads); proven_job_system_destroy(g_threads);
    PROVEN_TEST_PASS("the event-driven server answers, holds back, refuses and cleans up as it should.");
    return 0;
}
