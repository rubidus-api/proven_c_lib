#include "proven.h"
#include "proven_test.h"
#include "test_unit_tls_pki.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The event-driven HTTP client, over loopback.
 *
 * One thread: the client and the event-driven server it talks to sit on the same loop, and
 * the test drives that loop by hand, a round at a time. Nothing here is concurrent, so what
 * the callbacks record is plain data. Where a server that misbehaves is wanted, the test
 * plays it itself on a listener, between rounds.
 */

static proven_mem_view_t mv(const char *s) { return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }
static proven_u8str_view_t sv(const char *s) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }

static proven_allocator_t g_heap;
static proven_loop_t *g_loop;
static proven_tls_config_t *g_server_tls, *g_client_tls, *g_stranger_tls;

static proven_byte_t pattern(proven_size_t i) { return (proven_byte_t)('a' + (i * 7 + i / 251) % 26); }

static bool spin_until(const bool *flag, proven_u32 ms) {
    proven_net_deadline_t until = proven_net_deadline_in(ms);
    while (!*flag && proven_time_monotonic_now() < until) {
        PROVEN_TEST_ASSERT(proven_loop_poll(g_loop, proven_net_deadline_in(10)) == PROVEN_OK, "a round", "");
    }
    return *flag;
}
static void spin(proven_u32 ms) { bool never = false; (void)spin_until(&never, ms); }

// ---------------------------------------------------------------------------------------
// The server side: an event-driven server on the same loop
// ---------------------------------------------------------------------------------------

typedef struct {
    proven_http_stream_t *stream;
    proven_loop_timer_t timer;
    proven_size_t sent, total, received;
    unsigned sum;
    char path[40];
} served_t;

static int g_served_requests, g_served_done;
static char g_seen_host[64];

static void served_pump(served_t *a) {
    static proven_byte_t chunk[8192];
    while (a->sent < a->total) {
        proven_size_t n = a->total - a->sent < sizeof chunk ? a->total - a->sent : sizeof chunk;
        for (proven_size_t i = 0; i < n; ++i) chunk[i] = pattern(a->sent + i);
        proven_result_size_t w = proven_http_stream_write(a->stream, (proven_mem_view_t){ .ptr = chunk, .size = n });
        if (w.err != PROVEN_OK) return;
        a->sent += w.value;
        if (w.value < n) return;
    }
    (void)proven_http_stream_end(a->stream);
}
static void served_later(void *ctx) { served_t *a = ctx; (void)proven_http_stream_respond(a->stream, 200, NULL, 0, mv("later")); }

static void srv_request(void *ctx, proven_http_stream_t *s, const proven_http_request_t *req) {
    (void)ctx;
    g_served_requests++;
    served_t *a = calloc(1, sizeof *a);
    if (!a) { proven_http_stream_abort(s); return; }
    a->stream = s;
    memcpy(a->path, req->target.ptr, req->target.size < sizeof a->path - 1 ? req->target.size : sizeof a->path - 1);
    proven_http_stream_set_user(s, a);
    proven_u8str_view_t host = { 0 };
    (void)proven_http_header_find(req->headers, req->header_count, PROVEN_LIT("Host"), &host);
    memset(g_seen_host, 0, sizeof g_seen_host);
    memcpy(g_seen_host, host.ptr, host.size < sizeof g_seen_host - 1 ? host.size : sizeof g_seen_host - 1);
    if (strcmp(a->path, "/") == 0 || strcmp(a->path, "/q?x=1&y=2") == 0) {
        proven_http_header_t h = { PROVEN_LIT("X-Test"), PROVEN_LIT("yes") };
        (void)proven_http_stream_respond(s, 200, &h, 1, mv("hello"));
    } else if (strncmp(a->path, "/big/", 5) == 0 || strncmp(a->path, "/chunked/", 9) == 0) {
        bool chunked = a->path[1] == 'c';
        a->total = (proven_size_t)atoi(a->path + (chunked ? 9 : 5));
        if (proven_http_stream_begin(s, 200, NULL, 0, chunked ? PROVEN_HTTP_EVENT_LENGTH_UNKNOWN : a->total) == PROVEN_OK) served_pump(a);
    } else if (strcmp(a->path, "/nothing") == 0) {
        (void)proven_http_stream_respond(s, 204, NULL, 0, mv(""));
    } else if (strcmp(a->path, "/slow") == 0) {
        proven_loop_timer_set(g_loop, &a->timer, 50, served_later, a);
    } else if (strcmp(a->path, "/never") == 0 || strcmp(a->path, "/echo") == 0) {
        /* /never: no answer. /echo: answered when the body has come. */
    } else {
        (void)proven_http_stream_respond(s, 404, NULL, 0, mv("no"));
    }
}
static void srv_body(void *ctx, proven_http_stream_t *s, proven_mem_view_t piece, bool last) {
    (void)ctx;
    served_t *a = proven_http_stream_user(s);
    for (proven_size_t i = 0; i < piece.size; ++i) a->sum = a->sum * 31 + piece.ptr[i];
    a->received += piece.size;
    if (last) {
        char text[64];
        int n = snprintf(text, sizeof text, "%u %u", (unsigned)a->received, a->sum);
        (void)proven_http_stream_respond(s, 200, NULL, 0, (proven_mem_view_t){ .ptr = (const proven_byte_t *)text, .size = (proven_size_t)n });
    }
}
static void srv_writable(void *ctx, proven_http_stream_t *s) { (void)ctx; served_t *a = proven_http_stream_user(s); if (a->total) served_pump(a); }
static void srv_done(void *ctx, proven_http_stream_t *s, proven_err_t why) {
    (void)ctx; (void)why;
    served_t *a = proven_http_stream_user(s);
    if (a) { proven_loop_timer_cancel(g_loop, &a->timer); free(a); }
    g_served_done++;
}

// ---------------------------------------------------------------------------------------
// The client side: what one request records
// ---------------------------------------------------------------------------------------

typedef struct {
    proven_http_event_request_t *request;
    proven_loop_timer_t timer;
    int responses, pieces, lasts, dones, writables, refusals;
    int status;
    bool saw_header, intact, done, pause_once, paused_now;
    proven_err_t why;
    proven_size_t got;
    char text[128];
    proven_size_t to_send, sent;       /* a streamed request body */
    unsigned sum;
} call_t;

static void call_resume(void *ctx) { call_t *c = ctx; c->paused_now = false; proven_http_event_request_resume(c->request); }

static void on_response(void *ctx, proven_http_event_request_t *r, const proven_http_response_t *head) {
    (void)r;
    call_t *c = ctx;
    c->responses++;
    c->status = head->status;
    proven_u8str_view_t value = { 0 };
    c->saw_header = proven_http_header_find(head->headers, head->header_count, PROVEN_LIT("X-Test"), &value) && proven_u8str_view_eq(value, PROVEN_LIT("yes"));
}
static void on_body(void *ctx, proven_http_event_request_t *r, proven_mem_view_t piece, bool last) {
    call_t *c = ctx;
    if (c->paused_now) c->intact = false;                  /* nothing may arrive while paused */
    c->pieces++;
    if (last) c->lasts++;
    for (proven_size_t i = 0; i < piece.size; i += 61) c->intact = c->intact && piece.ptr[i] == pattern(c->got + i);
    for (proven_size_t i = 0; i < piece.size && c->got + i < sizeof c->text - 1; ++i) c->text[c->got + i] = (char)piece.ptr[i];
    c->got += piece.size;
    if (c->pause_once && !last) {
        c->pause_once = false;
        c->paused_now = true;
        proven_http_event_request_pause(r);
        proven_loop_timer_set(g_loop, &c->timer, 40, call_resume, c);
    }
}
/* Write as much of the request body as the client will take; the rest waits for on_writable. */
static void call_pump(call_t *c) {
    static proven_byte_t chunk[8192];
    while (c->sent < c->to_send) {
        proven_size_t n = c->to_send - c->sent < sizeof chunk ? c->to_send - c->sent : sizeof chunk;
        for (proven_size_t i = 0; i < n; ++i) chunk[i] = pattern(c->sent + i);
        proven_result_size_t w = proven_http_event_request_write(c->request, (proven_mem_view_t){ .ptr = chunk, .size = n });
        if (w.err != PROVEN_OK) return;
        for (proven_size_t i = 0; i < w.value; ++i) c->sum = c->sum * 31 + chunk[i];
        c->sent += w.value;
        if (w.value < n) { c->refusals++; return; }
    }
    (void)proven_http_event_request_end(c->request);
}
static void on_writable(void *ctx, proven_http_event_request_t *r) { (void)r; call_t *c = ctx; c->writables++; call_pump(c); }
static void on_done(void *ctx, proven_http_event_request_t *r, proven_err_t why) {
    call_t *c = ctx;
    if (proven_http_event_request_user(r) != c) c->intact = false;
    proven_loop_timer_cancel(g_loop, &c->timer);
    c->dones++;
    c->why = why;
    c->done = true;
}

static char g_url[200];
static proven_u8str_view_t url(bool tls, proven_u16 port, const char *path) {
    int n = snprintf(g_url, sizeof g_url, "%s://127.0.0.1:%u%s", tls ? "https" : "http", (unsigned)port, path);
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)g_url, .size = (proven_size_t)n };
}

static proven_err_t start(proven_http_event_client_t *client, call_t *c, const char *method, proven_u8str_view_t u, proven_mem_view_t body, proven_u64 length) {
    *c = (call_t){ .intact = true, .pause_once = c->pause_once };
    proven_http_event_request_options_t o = {
        .method = sv(method), .url = u, .body = body, .body_length = length,
        .on = { on_response, on_body, on_writable, on_done }, .ctx = c,
    };
    proven_err_t e = proven_http_event_client_start(client, &o, &c->request);
    if (e == PROVEN_OK) proven_http_event_request_set_user(c->request, c);
    return e;
}

static void cases(bool tls) {
    const char *how = tls ? "over TLS" : "plain";
    proven_http_event_server_t *server = NULL;
    proven_http_event_server_config_t scfg = {
        .on = { srv_request, srv_body, srv_writable, srv_done }, .max_body_bytes = 32 * 1024 * 1024, .tls = tls ? g_server_tls : NULL,
    };
    proven_net_addr_t at;
    PROVEN_TEST_ASSERT(proven_http_event_server_create(g_loop, &scfg, &server) == PROVEN_OK &&
                       proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "a server on the same loop", how);
    proven_http_event_client_config_t ccfg = { .tls = tls ? g_client_tls : NULL };
    proven_http_event_client_t *client = NULL;
    PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &ccfg, &client) == PROVEN_OK && proven_http_event_client_requests(client) == 0, "a client", how);
    call_t c = { 0 };

    /* A request and its answer. */
    PROVEN_TEST_ASSERT(start(client, &c, "", url(tls, at.port, "/"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && !c.done && proven_http_event_client_requests(client) == 1,
        "a started request returns at once, with nothing called yet", how);
    PROVEN_TEST_ASSERT(spin_until(&c.done, 5000) && c.why == PROVEN_OK && c.dones == 1 && c.responses == 1 && c.status == 200 && c.saw_header &&
                       c.got == 5 && memcmp(c.text, "hello", 5) == 0 && c.lasts == 1 && proven_http_event_client_requests(client) == 0,
        "GET: the head once, the body with its last piece marked, then on_done(PROVEN_OK) once", how);
    PROVEN_TEST_ASSERT(start(client, &c, "GET", url(tls, at.port, "/q?x=1&y=2"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && spin_until(&c.done, 5000) && c.status == 200,
        "the query goes with the path", how);
    PROVEN_TEST_ASSERT(start(client, &c, "HEAD", url(tls, at.port, "/"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && spin_until(&c.done, 5000) &&
                       c.why == PROVEN_OK && c.status == 200 && c.pieces == 0, "HEAD: a head and no body call, whatever Content-Length says", how);
    PROVEN_TEST_ASSERT(start(client, &c, "GET", url(tls, at.port, "/nothing"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && spin_until(&c.done, 5000) &&
                       c.why == PROVEN_OK && c.status == 204 && c.pieces == 0, "204: no body call", how);
    PROVEN_TEST_ASSERT(start(client, &c, "GET", url(tls, at.port, "/slow"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && spin_until(&c.done, 5000) &&
                       c.why == PROVEN_OK && c.got == 5 && memcmp(c.text, "later", 5) == 0, "an answer that comes 50 ms later", how);
    PROVEN_TEST_ASSERT(start(client, &c, "GET", url(tls, at.port, "/missing"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && spin_until(&c.done, 5000) &&
                       c.why == PROVEN_OK && c.status == 404, "an error status is a response, not an error", how);

    /* Large bodies down, with a pause in the middle. */
    c.pause_once = true;
    PROVEN_TEST_ASSERT(start(client, &c, "GET", url(tls, at.port, "/big/3000000"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && spin_until(&c.done, 20000) &&
                       c.why == PROVEN_OK && c.got == 3000000 && c.intact && c.lasts == 1 && c.pieces > 10,
        "3 MB arrive in pieces, intact, and nothing arrives between pause and resume", how);
    PROVEN_TEST_ASSERT(start(client, &c, "GET", url(tls, at.port, "/chunked/500000"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && spin_until(&c.done, 20000) &&
                       c.why == PROVEN_OK && c.got == 500000 && c.intact && c.lasts == 1, "a chunked body arrives with its framing taken off", how);

    /* Bodies up: from memory, streamed with a length, streamed chunked. */
    {
        enum { MEM = 100000 };
        static proven_byte_t body[MEM];
        unsigned sum = 0;
        for (int i = 0; i < MEM; ++i) { body[i] = pattern((proven_size_t)i); sum = sum * 31 + body[i]; }
        char want[64];
        snprintf(want, sizeof want, "%u %u", (unsigned)MEM, sum);
        PROVEN_TEST_ASSERT(start(client, &c, "POST", url(tls, at.port, "/echo"), (proven_mem_view_t){ .ptr = body, .size = MEM }, 0) == PROVEN_OK && spin_until(&c.done, 20000) &&
                           c.why == PROVEN_OK && strcmp(c.text, want) == 0, "a body held in memory is sent whole", how);

        PROVEN_TEST_ASSERT(start(client, &c, "PUT", url(tls, at.port, "/echo"), (proven_mem_view_t){ 0 }, 16000000) == PROVEN_OK, "a request whose 16 MB body will be written", how);
        c.to_send = 16000000;
        call_pump(&c);
        PROVEN_TEST_ASSERT(c.refusals > 0 && c.sent < c.to_send && !c.done, "before the connection exists, writes are taken up to the limit and then refused", how);
        PROVEN_TEST_ASSERT(spin_until(&c.done, 60000) && c.why == PROVEN_OK && c.sent == 16000000 && c.writables > 0, "on_writable resumes it until all 16 MB are sent", how);
        snprintf(want, sizeof want, "%u %u", 16000000u, c.sum);
        PROVEN_TEST_ASSERT(strcmp(c.text, want) == 0, "and the server received exactly those bytes", how);

        PROVEN_TEST_ASSERT(start(client, &c, "POST", url(tls, at.port, "/echo"), (proven_mem_view_t){ 0 }, PROVEN_HTTP_EVENT_LENGTH_UNKNOWN) == PROVEN_OK, "a request body of unknown length", how);
        c.to_send = 300000;
        call_pump(&c);
        PROVEN_TEST_ASSERT(spin_until(&c.done, 20000) && c.why == PROVEN_OK, "is sent chunked", how);
        snprintf(want, sizeof want, "%u %u", 300000u, c.sum);
        PROVEN_TEST_ASSERT(strcmp(c.text, want) == 0, "and arrives whole", how);

        PROVEN_TEST_ASSERT(start(client, &c, "POST", url(tls, at.port, "/echo"), (proven_mem_view_t){ 0 }, 10) == PROVEN_OK &&
                           proven_http_event_request_write(c.request, mv("12345678901")).err == PROVEN_ERR_OUT_OF_BOUNDS &&
                           proven_http_event_request_write(c.request, mv("1234")).value == 4, "writing past the promised length is PROVEN_ERR_OUT_OF_BOUNDS", how);
        PROVEN_TEST_ASSERT(proven_http_event_request_end(c.request) == PROVEN_ERR_INVALID_FORMAT && c.done && c.why == PROVEN_ERR_INVALID_FORMAT && c.dones == 1,
            "ending short of it ends the request with PROVEN_ERR_INVALID_FORMAT", how);
        PROVEN_TEST_ASSERT(start(client, &c, "GET", url(tls, at.port, "/"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK &&
                           proven_http_event_request_write(c.request, mv("x")).err == PROVEN_ERR_INVALID_STATE && proven_http_event_request_end(c.request) == PROVEN_ERR_INVALID_STATE &&
                           spin_until(&c.done, 5000) && c.why == PROVEN_OK, "a request with no body to write refuses write and end, and is not harmed", how);
    }

    /* The address is given apart from the name. */
    {
        char named[80];
        int n = snprintf(named, sizeof named, "%s://example.test:%u/", tls ? "https" : "http", (unsigned)at.port);
        proven_net_addr_t where = proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, at.port);
        c = (call_t){ .intact = true };
        proven_http_event_request_options_t o = { .url = { (const proven_byte_t *)named, (proven_size_t)n }, .address = &where, .on = { on_response, on_body, on_writable, on_done }, .ctx = &c };
        PROVEN_TEST_ASSERT(proven_http_event_client_start(client, &o, &c.request) == PROVEN_OK, "a URL with a name, and the address to connect to", how);
        proven_http_event_request_set_user(c.request, &c);
        char host[80];
        snprintf(host, sizeof host, "example.test:%u", (unsigned)at.port);
        PROVEN_TEST_ASSERT(spin_until(&c.done, 5000) && c.why == PROVEN_OK && c.status == 200 && strcmp(g_seen_host, host) == 0,
            "connects to the address, says the name in Host - and over TLS it is the name the certificate is checked for", how);
        if (tls) {
            n = snprintf(named, sizeof named, "https://wrong.example:%u/", (unsigned)at.port);
            o.url = (proven_u8str_view_t){ (const proven_byte_t *)named, (proven_size_t)n };
            c = (call_t){ .intact = true };
            int before = g_served_requests;
            PROVEN_TEST_ASSERT(proven_http_event_client_start(client, &o, &c.request) == PROVEN_OK, "a name this server's certificate is not for", how);
            proven_http_event_request_set_user(c.request, &c);
            PROVEN_TEST_ASSERT(spin_until(&c.done, 5000) && c.why == PROVEN_ERR_NAME_MISMATCH && c.responses == 0 && g_served_requests == before,
                "ends with PROVEN_ERR_NAME_MISMATCH, and the request was never sent", how);
        }
        o.address = NULL;
        PROVEN_TEST_ASSERT(proven_http_event_client_start(client, &o, NULL) == PROVEN_ERR_INVALID_ARG, "a name with no address is PROVEN_ERR_INVALID_ARG: nothing here resolves names", how);
    }

    /* Time, giving up, and going away. */
    {
        proven_http_event_client_config_t icfg = { .tls = tls ? g_client_tls : NULL, .response_timeout_ms = 300 };
        proven_http_event_client_t *impatient = NULL;
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &icfg, &impatient) == PROVEN_OK, "a client that waits 300 ms for a response", how);
        proven_time_t t0 = proven_time_monotonic_now();
        PROVEN_TEST_ASSERT(start(impatient, &c, "GET", url(tls, at.port, "/never"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && spin_until(&c.done, 5000) &&
                           c.why == PROVEN_ERR_TIMEOUT && c.responses == 0 && proven_time_monotonic_now() - t0 > 250000000, "no answer within the response timeout is PROVEN_ERR_TIMEOUT", how);
        proven_http_event_client_destroy(impatient);
    }
    PROVEN_TEST_ASSERT(start(client, &c, "GET", url(tls, at.port, "/never"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK, "a request that will be given up", how);
    spin(40);
    proven_http_event_request_abort(c.request);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_ERR_RESET && c.dones == 1 && proven_http_event_client_requests(client) == 0, "abort calls on_done(PROVEN_ERR_RESET) inside the call", how);

    /* Many at once on the one thread. */
    {
        /* Fewer over TLS: every handshake is computed on this one thread, both ends of it, and
         * two hundred of them at once would outlast the connect timeout - which is what the
         * manual says of handshakes on a loop. */
        enum { MOST = 200 };
        const int MANY = tls ? 40 : MOST;
        static call_t many[MOST];
        int started = 0;
        for (int i = 0; i < MANY; ++i) started += start(client, &many[i], "GET", url(tls, at.port, i % 2 ? "/" : "/big/20000"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK ? 1 : 0;
        PROVEN_TEST_ASSERT(started == MANY && proven_http_event_client_requests(client) == (proven_size_t)MANY, tls ? "forty requests are in flight at once" : "two hundred requests are in flight at once", how);
        bool all = false;
        proven_net_deadline_t until = proven_net_deadline_in(60000);
        while (!all && proven_time_monotonic_now() < until) {
            PROVEN_TEST_ASSERT(proven_loop_poll(g_loop, proven_net_deadline_in(10)) == PROVEN_OK, "a round", how);
            all = proven_http_event_client_requests(client) == 0;
        }
        int good = 0;
        for (int i = 0; i < MANY; ++i) good += many[i].done && many[i].why == PROVEN_OK && many[i].dones == 1 && (i % 2 ? many[i].got == 5 && memcmp(many[i].text, "hello", 5) == 0 : many[i].got == 20000 && many[i].intact) ? 1 : 0;
        if (!(all && good == MANY)) for (int i = 0; i < MANY; ++i) if (!(many[i].done && many[i].why == PROVEN_OK)) { fprintf(stderr, "many: request %d done %d why %d got %u\n", i, (int)many[i].done, (int)many[i].why, (unsigned)many[i].got); break; }
        PROVEN_TEST_ASSERT(all && good == MANY, "and every one of them completes, once, with its own body", how);
    }

    /* Destroying the client under a request. */
    PROVEN_TEST_ASSERT(start(client, &c, "GET", url(tls, at.port, "/never"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK, "a request left in flight", how);
    spin(40);
    proven_http_event_client_destroy(client);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_ERR_RESET && c.dones == 1, "destroying the client ends it with on_done(PROVEN_ERR_RESET)", how);
    spin(60);
    proven_http_event_server_destroy(server);
    PROVEN_TEST_ASSERT(g_served_done == g_served_requests, "and the server saw every exchange end", how);
}

/* The test plays the server: accept one connection, read the request, send `answer`, close. */
static void scripted(proven_http_event_client_t *client, call_t *c, const char *answer, proven_size_t answer_len) {
    proven_net_listener_t listener;
    proven_net_addr_t at;
    PROVEN_TEST_ASSERT(proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 4, &listener, &at) == PROVEN_OK, "a listener the test answers on", "");
    PROVEN_TEST_ASSERT(start(client, c, "GET", url(false, at.port, "/"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK, "a request to it", "");
    proven_net_conn_t conn;
    bool accepted = false;
    for (int i = 0; i < 500 && !accepted; ++i) { spin(5); accepted = proven_net_accept(&listener, PROVEN_NET_DONT_WAIT, &conn, NULL) == PROVEN_OK; }
    PROVEN_TEST_ASSERT(accepted, "the client connected", "");
    char req[1024] = { 0 };
    proven_size_t have = 0;
    for (int i = 0; i < 500 && !strstr(req, "\r\n\r\n"); ++i) {
        spin(5);
        proven_result_size_t r = proven_net_read(&conn, (proven_mem_mut_t){ .ptr = (proven_byte_t *)req + have, .size = sizeof req - 1 - have }, PROVEN_NET_DONT_WAIT);
        if (r.err == PROVEN_OK) have += r.value;
    }
    PROVEN_TEST_ASSERT(strncmp(req, "GET / HTTP/1.1\r\nHost: 127.0.0.1:", 32) == 0 && strstr(req, "\r\nConnection: close\r\n"), "its request names the host and asks for the connection to be closed", "");
    PROVEN_TEST_ASSERT(proven_net_write_all(&conn, (proven_mem_view_t){ .ptr = (const proven_byte_t *)answer, .size = answer_len }, proven_net_deadline_in(5000)).err == PROVEN_OK, "the scripted answer", "");
    (void)proven_net_close(&conn);
    (void)proven_net_listener_close(&listener);
    if (!spin_until(&c->done, 5000)) fprintf(stderr, "scripted: no end for an answer beginning %.40s (responses %d, got %u)\n", answer, c->responses, (unsigned)c->got);
    PROVEN_TEST_ASSERT(c->done, "the request ends", "");
}
#define SCRIPTED(client, c, text) scripted((client), (c), (text), sizeof(text) - 1)

int main(void) {
    PROVEN_TEST_SUITE("the event-driven HTTP client",
        "Requests started and answered through callbacks on one thread: bodies down and up in pieces with backpressure, the address apart from the name, time limits, abort, many at once - plain and over TLS - and servers that answer badly.",
        "Inspect src/proven/http_event_client.c: cl_process for the response, cl_flush and cl_progress for the request, cl_arm for the time limits, cl_kill for the end.");

    g_heap = proven_heap_allocator();
    PROVEN_TEST_ASSERT(tp_build(), "the test's certificates and keys are issued", "");
    proven_cert_store_t *anchors = NULL, *none = NULL;
    PROVEN_TEST_ASSERT(proven_cert_store_create(g_heap, &anchors) == PROVEN_OK && proven_cert_store_create(g_heap, &none) == PROVEN_OK &&
                       proven_cert_store_add_pem(anchors, (proven_mem_view_t){ (const proven_byte_t *)TP_CA, strlen(TP_CA) }, NULL) == PROVEN_OK, "anchors", "");
    proven_tls_options_t co = { .alloc = g_heap, .anchors = anchors }, xo = { .alloc = g_heap, .anchors = none };
    proven_tls_options_t so = { .alloc = g_heap, .certificate_pem = { (const proven_byte_t *)TP_SERVER_ED, strlen(TP_SERVER_ED) }, .private_key_pem = { (const proven_byte_t *)TP_SERVER_ED_KEY, strlen(TP_SERVER_ED_KEY) } };
    PROVEN_TEST_ASSERT(proven_tls_config_create(&co, &g_client_tls) == PROVEN_OK && proven_tls_config_create(&so, &g_server_tls) == PROVEN_OK &&
                       proven_tls_config_create(&xo, &g_stranger_tls) == PROVEN_OK, "TLS configurations", "");
    PROVEN_TEST_ASSERT(proven_loop_create(g_heap, &g_loop) == PROVEN_OK, "a loop", "");

    PROVEN_TEST_SECTION("what start refuses", "Requests that are refused before anything is sent, with no callback made.", "Check proven_http_event_client_start.");
    {
        proven_http_event_client_t *client = (proven_http_event_client_t *)1;
        PROVEN_TEST_ASSERT(proven_http_event_client_create(NULL, NULL, &client) == PROVEN_ERR_INVALID_ARG && client == NULL, "no loop is PROVEN_ERR_INVALID_ARG", "");
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, NULL, &client) == PROVEN_OK, "a client with every default", "");
        call_t c = { 0 };
        proven_http_event_request_options_t o = { .url = sv("http://127.0.0.1:1/") };
        PROVEN_TEST_ASSERT(proven_http_event_client_start(client, &o, NULL) == PROVEN_ERR_INVALID_ARG, "no on_done", "");
        PROVEN_TEST_ASSERT(start(client, &c, "GET", sv("/relative"), (proven_mem_view_t){ 0 }, 0) == PROVEN_ERR_INVALID_ARG, "a URL that is not absolute", "");
        PROVEN_TEST_ASSERT(start(client, &c, "GET", sv("ftp://127.0.0.1/"), (proven_mem_view_t){ 0 }, 0) == PROVEN_ERR_INVALID_ARG, "a scheme that is not http or https", "");
        PROVEN_TEST_ASSERT(start(client, &c, "GET", sv("https://127.0.0.1/"), (proven_mem_view_t){ 0 }, 0) == PROVEN_ERR_INVALID_ARG, "https with no TLS configuration: refused, not sent in the clear", "");
        PROVEN_TEST_ASSERT(start(client, &c, "GET", sv("http://user:secret@127.0.0.1/"), (proven_mem_view_t){ 0 }, 0) == PROVEN_ERR_INVALID_ARG, "credentials in the URL", "");
        PROVEN_TEST_ASSERT(start(client, &c, "CONNECT", sv("http://127.0.0.1/"), (proven_mem_view_t){ 0 }, 0) == PROVEN_ERR_INVALID_ARG, "CONNECT", "");
        PROVEN_TEST_ASSERT(start(client, &c, "BAD METHOD", sv("http://127.0.0.1/"), (proven_mem_view_t){ 0 }, 0) == PROVEN_ERR_INVALID_ARG, "a method that is not a token", "");
        proven_http_header_t own = { PROVEN_LIT("Content-Length"), PROVEN_LIT("3") };
        o.on.on_done = on_done; o.headers = &own; o.header_count = 1;
        PROVEN_TEST_ASSERT(proven_http_event_client_start(client, &o, NULL) == PROVEN_ERR_INVALID_ARG, "a header the client writes itself", "");
        {
            static char longvalue[20001];
            memset(longvalue, 'x', sizeof longvalue - 1);
            proven_http_header_t big = { PROVEN_LIT("X-Long"), { (const proven_byte_t *)longvalue, sizeof longvalue - 1 } };
            o.headers = &big;
            PROVEN_TEST_ASSERT(proven_http_event_client_start(client, &o, NULL) == PROVEN_ERR_OUT_OF_BOUNDS, "a request head past max_head_bytes is PROVEN_ERR_OUT_OF_BOUNDS", "");
        }
        PROVEN_TEST_ASSERT(c.dones == 0 && proven_http_event_client_requests(client) == 0, "and none of them called anything or left anything behind", "");
        PROVEN_TEST_ASSERT(proven_http_event_request_write(NULL, mv("x")).err == PROVEN_ERR_INVALID_ARG && proven_http_event_request_end(NULL) == PROVEN_ERR_INVALID_ARG &&
                           proven_http_event_request_user(NULL) == NULL && proven_http_event_client_requests(NULL) == 0, "the request calls accept null", "");
        proven_http_event_request_abort(NULL); proven_http_event_request_pause(NULL); proven_http_event_request_resume(NULL);
        proven_http_event_client_destroy(client);
        proven_http_event_client_destroy(NULL);
    }

    PROVEN_TEST_SECTION("plain", "Every case over an unencrypted connection.", "");
    cases(false);
    PROVEN_TEST_SECTION("over TLS", "The same cases with the TLS engine between the socket and the parser.", "Check the TLS branches of cl_readable and cl_flush.");
    cases(true);

    PROVEN_TEST_SECTION("servers that answer badly, or not at all", "The test plays the server on a listener of its own.", "Check cl_process and the end-of-stream branch after its loop.");
    {
        proven_http_event_client_t *client = NULL;
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, NULL, &client) == PROVEN_OK, "a client", "");
        call_t c = { 0 };
        SCRIPTED(client, &c, "HTTP/1.1 200 OK\r\n\r\nabc");
        PROVEN_TEST_ASSERT(c.why == PROVEN_OK && c.got == 3 && memcmp(c.text, "abc", 3) == 0 && c.lasts == 1, "a body with no length ends when the server closes, and is whole", "");
        SCRIPTED(client, &c, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc");
        PROVEN_TEST_ASSERT(c.why == PROVEN_ERR_RESET && c.got == 3 && c.lasts == 0, "a body cut short of its Content-Length is PROVEN_ERR_RESET, with no last piece", "");
        SCRIPTED(client, &c, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n");
        PROVEN_TEST_ASSERT(c.why == PROVEN_ERR_RESET && c.got == 3 && c.lasts == 0, "and so is a chunked body with no final chunk", "");
        SCRIPTED(client, &c, "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </x>\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
        PROVEN_TEST_ASSERT(c.why == PROVEN_OK && c.responses == 1 && c.status == 200 && c.got == 2, "interim responses are passed over and not reported", "");
        SCRIPTED(client, &c, "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 100 Continue\r\n\r\n"
                             "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
        PROVEN_TEST_ASSERT(c.why == PROVEN_ERR_INVALID_FORMAT && c.responses == 0, "but nine of them in a row are not an answer: PROVEN_ERR_INVALID_FORMAT", "");
        SCRIPTED(client, &c, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: x\r\nConnection: Upgrade\r\n\r\n");
        PROVEN_TEST_ASSERT(c.why == PROVEN_ERR_UNSUPPORTED && c.responses == 0, "a change of protocol nobody asked for is PROVEN_ERR_UNSUPPORTED", "");
        SCRIPTED(client, &c, "this is not HTTP\r\n\r\n");
        PROVEN_TEST_ASSERT(c.why == PROVEN_ERR_INVALID_FORMAT && c.responses == 0, "something that is not HTTP is PROVEN_ERR_INVALID_FORMAT", "");
        SCRIPTED(client, &c, "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab");
        PROVEN_TEST_ASSERT(c.why == PROVEN_ERR_INVALID_FORMAT && c.pieces == 0, "two lengths that disagree are PROVEN_ERR_INVALID_FORMAT", "");
        SCRIPTED(client, &c, "HTTP/1.1 200 OK\r\n");
        PROVEN_TEST_ASSERT(c.why == PROVEN_ERR_RESET && c.responses == 0, "a head that never finishes, then a close, is PROVEN_ERR_RESET", "");
        {
            enum { LONG = 20000 };
            static char longhead[LONG + 64];
            int n = snprintf(longhead, sizeof longhead, "HTTP/1.1 200 OK\r\nX-Long: ");
            memset(longhead + n, 'x', LONG);
            scripted(client, &c, longhead, (proven_size_t)n + LONG);
            PROVEN_TEST_ASSERT(c.why == PROVEN_ERR_OUT_OF_BOUNDS && c.responses == 0, "a head past max_head_bytes is PROVEN_ERR_OUT_OF_BOUNDS", "");
        }

        {
            proven_http_event_client_config_t small = { .max_body_bytes = 5 };
            proven_http_event_client_t *limited = NULL;
            PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &small, &limited) == PROVEN_OK, "a client that accepts five bytes of body", "");
            SCRIPTED(limited, &c, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n0123456789");
            PROVEN_TEST_ASSERT(c.why == PROVEN_ERR_OUT_OF_BOUNDS && c.pieces == 0, "a body announced past max_body_bytes is PROVEN_ERR_OUT_OF_BOUNDS before any of it is delivered", "");
            SCRIPTED(limited, &c, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\n0123\r\n4\r\n4567\r\n0\r\n\r\n");
            PROVEN_TEST_ASSERT(c.why == PROVEN_ERR_OUT_OF_BOUNDS && c.lasts == 0, "and so is a chunked body that grows past it", "");
            proven_http_event_client_destroy(limited);
        }

        /* Nothing listening. The refusal may come at once or from the loop; either way exactly one report. */
        proven_net_listener_t gone;
        proven_net_addr_t at;
        PROVEN_TEST_ASSERT(proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 1, &gone, &at) == PROVEN_OK && proven_net_listener_close(&gone) == PROVEN_OK, "a port with nothing on it", "");
        proven_err_t e = start(client, &c, "GET", url(false, at.port, "/"), (proven_mem_view_t){ 0 }, 0);
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_REFUSED || (e == PROVEN_OK && spin_until(&c.done, 15000) && c.why == PROVEN_ERR_REFUSED && c.dones == 1),
            "a refused connection is PROVEN_ERR_REFUSED, from start or from on_done and not both", "");
        PROVEN_TEST_ASSERT(e != PROVEN_ERR_REFUSED || c.dones == 0, "start that fails makes no callback", "");
        proven_http_event_client_destroy(client);

        /* A server this client does not trust. */
        proven_http_event_server_t *server = NULL;
        proven_http_event_server_config_t scfg = { .on = { srv_request, srv_body, srv_writable, srv_done }, .tls = g_server_tls };
        proven_http_event_client_config_t ccfg = { .tls = g_stranger_tls };
        PROVEN_TEST_ASSERT(proven_http_event_server_create(g_loop, &scfg, &server) == PROVEN_OK &&
                           proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK &&
                           proven_http_event_client_create(g_loop, &ccfg, &client) == PROVEN_OK, "a TLS server, and a client with an empty trust store", "");
        int before = g_served_requests;
        PROVEN_TEST_ASSERT(start(client, &c, "GET", url(true, at.port, "/"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && spin_until(&c.done, 5000) &&
                           c.why == PROVEN_ERR_UNTRUSTED && c.responses == 0 && g_served_requests == before, "ends with PROVEN_ERR_UNTRUSTED, and the request was never sent", "");
        proven_http_event_client_destroy(client);
        spin(60);
        proven_http_event_server_destroy(server);
    }

    proven_loop_destroy(g_loop);
    proven_tls_config_destroy(g_client_tls); proven_tls_config_destroy(g_server_tls); proven_tls_config_destroy(g_stranger_tls);
    proven_cert_store_destroy(anchors); proven_cert_store_destroy(none);
    PROVEN_TEST_PASS("the event-driven client sends, receives, holds back, gives up and reports each end once.");
    return 0;
}
