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

static int g_served_requests, g_served_done, g_served_close;    /* ... and those that came with `Connection: close` */
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
    if (proven_http_header_has_token(req->headers, req->header_count, PROVEN_LIT("Connection"), PROVEN_LIT("close"))) g_served_close++;
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
    int order;                         /* which on_done of the test's this was */
    int chain;                         /* requests still to be started, each from the on_done before it */
} call_t;

static int g_order;
static proven_http_event_client_t *g_chain_client;
static proven_u16 g_chain_port;
static bool g_chain_tls;
static proven_err_t start(proven_http_event_client_t *client, call_t *c, const char *method, proven_u8str_view_t u, proven_mem_view_t body, proven_u64 length);
static proven_u8str_view_t url(bool tls, proven_u16 port, const char *path);

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
    c->order = ++g_order;
    if (c->chain > 0 && why == PROVEN_OK) {
        /* The next one, from inside this callback: the connection is already free for it. */
        const int left = c->chain - 1, dones = c->dones;
        if (start(g_chain_client, c, "GET", url(g_chain_tls, g_chain_port, "/"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK) { c->chain = left; c->dones = dones; return; }
    }
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
    PROVEN_TEST_ASSERT(strncmp(req, "GET / HTTP/1.1\r\nHost: 127.0.0.1:", 32) == 0 && !strstr(req, "\r\nConnection:"), "its request names the host, and does not ask for the connection to be closed", "");
    PROVEN_TEST_ASSERT(proven_net_write_all(&conn, (proven_mem_view_t){ .ptr = (const proven_byte_t *)answer, .size = answer_len }, proven_net_deadline_in(5000)).err == PROVEN_OK, "the scripted answer", "");
    (void)proven_net_close(&conn);
    (void)proven_net_listener_close(&listener);
    if (!spin_until(&c->done, 5000)) fprintf(stderr, "scripted: no end for an answer beginning %.40s (responses %d, got %u)\n", answer, c->responses, (unsigned)c->got);
    PROVEN_TEST_ASSERT(c->done, "the request ends", "");
}
#define SCRIPTED(client, c, text) scripted((client), (c), (text), sizeof(text) - 1)

// ---------------------------------------------------------------------------------------
// Connections: kept, counted, and waited for
// ---------------------------------------------------------------------------------------

static proven_http_event_server_t *a_server(bool tls, proven_net_addr_t *at) {
    proven_http_event_server_t *server = NULL;
    proven_http_event_server_config_t scfg = { .on = { srv_request, srv_body, srv_writable, srv_done }, .tls = tls ? g_server_tls : NULL };
    PROVEN_TEST_ASSERT(proven_http_event_server_create(g_loop, &scfg, &server) == PROVEN_OK &&
                       proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), at) == PROVEN_OK, "a server on the same loop", tls ? "over TLS" : "plain");
    return server;
}

static proven_http_event_client_t *a_client(proven_http_event_client_config_t cfg, bool tls) {
    proven_http_event_client_t *client = NULL;
    cfg.tls = tls ? g_client_tls : NULL;
    PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &cfg, &client) == PROVEN_OK, "a client", tls ? "over TLS" : "plain");
    return client;
}

static bool get_ok(proven_http_event_client_t *client, call_t *c, bool tls, proven_u16 port, const char *path, proven_size_t size) {
    return start(client, c, "GET", url(tls, port, path), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && spin_until(&c->done, 10000) &&
           c->why == PROVEN_OK && c->dones == 1 && c->got == size;
}

static void connections(bool tls) {
    const char *how = tls ? "over TLS" : "plain";
    proven_net_addr_t at;
    proven_http_event_server_t *server = a_server(tls, &at);
    proven_http_event_client_t *client = a_client((proven_http_event_client_config_t){ 0 }, tls);
    call_t c = { 0 };
    static call_t many[40];

    /* One after another: one connection carries them all, whatever the shape of the response. */
    const int served = g_served_requests, closes = g_served_close;
    bool all = true;
    for (int i = 0; i < 5; i++) all = all && get_ok(client, &c, tls, at.port, "/", 5);
    all = all && get_ok(client, &c, tls, at.port, "/big/100000", 100000) && c.intact && get_ok(client, &c, tls, at.port, "/chunked/50000", 50000) && c.intact;
    all = all && get_ok(client, &c, tls, at.port, "/nothing", 0) && get_ok(client, &c, tls, at.port, "/missing", 2);
    all = all && start(client, &c, "HEAD", url(tls, at.port, "/"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && spin_until(&c.done, 5000) && c.why == PROVEN_OK && c.pieces == 0;
    all = all && get_ok(client, &c, tls, at.port, "/", 5) && memcmp(c.text, "hello", 5) == 0;
    PROVEN_TEST_ASSERT(all && g_served_requests == served + 11, "eleven requests one after another are each answered whole", how);
    PROVEN_TEST_ASSERT(proven_http_event_server_connections(server) == 1 && g_served_close == closes, "on one connection, and none of them asked for it to be closed", how);

    /* Each started from the on_done of the one before: the connection is free by then. */
    g_chain_client = client; g_chain_port = at.port; g_chain_tls = tls;
    PROVEN_TEST_ASSERT(start(client, &c, "GET", url(tls, at.port, "/"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK, "the first of a chain", how);
    c.chain = 20;
    PROVEN_TEST_ASSERT(spin_until(&c.done, 10000) && c.why == PROVEN_OK && c.dones == 21 && c.chain == 0 && proven_http_event_server_connections(server) == 1,
        "twenty more, each started from the on_done before it, go on the same one connection", how);

    /* More at once than an origin may have connections: six go, the rest wait their turn. */
    g_order = 0;
    for (int i = 0; i < 20; i++) PROVEN_TEST_ASSERT(start(client, &many[i], "GET", url(tls, at.port, "/slow"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK, "a request is started", how);
    PROVEN_TEST_ASSERT(proven_http_event_client_requests(client) == 20, "twenty requests are counted, those waiting among them", how);
    spin(tls ? 45 : 30);
    PROVEN_TEST_ASSERT(proven_http_event_server_connections(server) == 6, "six connections are made, not twenty", how);
    for (int i = 0; i < 20; i++) (void)spin_until(&many[i].done, 10000);
    int good = 0, in_turn = 0;
    for (int i = 0; i < 20; i++) good += many[i].done && many[i].why == PROVEN_OK && many[i].dones == 1 && many[i].got == 5 && memcmp(many[i].text, "later", 5) == 0;
    /* The server answers each 50 ms after it came, and a waiting request is given the next
     * connection that comes free, in the order of starting: so no request ends before one that
     * was started six or more places ahead of it - six being all that can be under way at once. */
    for (int i = 0; i < 20; i++) {
        bool ok = true;
        for (int j = 0; j + 6 <= i; j++) ok = ok && many[j].order < many[i].order;
        in_turn += ok;
    }
    PROVEN_TEST_ASSERT(good == 20 && in_turn == 20 && proven_http_event_server_connections(server) == 6, "all twenty are answered, in the order of starting, on those six", how);
    proven_http_event_client_destroy(client);
    spin(60);
    PROVEN_TEST_ASSERT(proven_http_event_server_connections(server) == 0, "destroying the client closes the idle connections", how);

    if (!tls) {
        /* The queue is bounded, and a request in it can be given up, can run out of time, and
         * ends with its client. */
        client = a_client((proven_http_event_client_config_t){ .max_connections_per_origin = 1, .max_waiting_requests = 2, .connect_timeout_ms = 250 }, false);
        const int before = g_served_requests;
        for (int i = 0; i < 3; i++) PROVEN_TEST_ASSERT(start(client, &many[i], "GET", url(false, at.port, "/slow"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK, "one that goes and two that wait", "");
        many[3].dones = 0;
        PROVEN_TEST_ASSERT(start(client, &many[3], "GET", url(false, at.port, "/slow"), (proven_mem_view_t){ 0 }, 0) == PROVEN_ERR_BUSY && many[3].dones == 0 && proven_http_event_client_requests(client) == 3,
            "a third to wait is PROVEN_ERR_BUSY at max_waiting_requests of two, with no callback", "");
        proven_http_event_request_abort(many[1].request);
        PROVEN_TEST_ASSERT(many[1].done && many[1].why == PROVEN_ERR_RESET && many[1].dones == 1, "aborting a waiting request ends it inside the call", "");
        PROVEN_TEST_ASSERT(spin_until(&many[0].done, 5000) && spin_until(&many[2].done, 5000) && many[0].why == PROVEN_OK && many[2].why == PROVEN_OK && g_served_requests == before + 2,
            "the other two are answered, and the one given up was never sent", "");
        PROVEN_TEST_ASSERT(proven_http_event_server_connections(server) == 1, "on the origin's one connection", "");
        /* Waiting is on the connecting clock, from the start of the request. */
        proven_time_t t0 = proven_time_monotonic_now();
        PROVEN_TEST_ASSERT(start(client, &many[0], "GET", url(false, at.port, "/never"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK &&
                           start(client, &many[1], "GET", url(false, at.port, "/"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK, "one the server sits on, and one behind it", "");
        PROVEN_TEST_ASSERT(spin_until(&many[1].done, 5000) && many[1].why == PROVEN_ERR_TIMEOUT && many[1].dones == 1 && !many[0].done && proven_time_monotonic_now() - t0 > 200000000,
            "the one that waits ends with PROVEN_ERR_TIMEOUT after connect_timeout_ms, and the one in front goes on", "");
        PROVEN_TEST_ASSERT(start(client, &many[1], "GET", url(false, at.port, "/"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK, "another behind it", "");
        proven_http_event_client_destroy(client);
        PROVEN_TEST_ASSERT(many[0].done && many[1].done && many[0].why == PROVEN_ERR_RESET && many[1].why == PROVEN_ERR_RESET && many[0].dones == 1 && many[1].dones == 1,
            "destroying the client ends both, the one that was waiting too, once each", "");
        spin(60);
        /* (The server keeps the connection of the request it never answered: a client may
         * close its sending side and still wait. It is counted out of what follows.) */
        const proven_size_t held = proven_http_event_server_connections(server);
        PROVEN_TEST_ASSERT(held == 1, "the server still holds the one connection whose request it sits on", "");

        /* Idle connections are bounded in number and in time. */
        client = a_client((proven_http_event_client_config_t){ .max_kept_connections = 2 }, false);
        for (int i = 0; i < 6; i++) PROVEN_TEST_ASSERT(start(client, &many[i], "GET", url(false, at.port, "/slow"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK, "six at once", "");
        for (int i = 0; i < 6; i++) (void)spin_until(&many[i].done, 5000);
        spin(60);
        PROVEN_TEST_ASSERT(proven_http_event_server_connections(server) == held + 2, "of six connections, max_kept_connections of two are kept", "");
        proven_http_event_client_destroy(client);
        client = a_client((proven_http_event_client_config_t){ .idle_timeout_ms = 120 }, false);
        PROVEN_TEST_ASSERT(get_ok(client, &c, false, at.port, "/", 5) && proven_http_event_server_connections(server) == held + 1, "a request, and its connection kept", "");
        spin(60);
        PROVEN_TEST_ASSERT(get_ok(client, &c, false, at.port, "/", 5), "used again within the time", "");
        PROVEN_TEST_ASSERT(proven_http_event_server_connections(server) == held + 1, "on the same connection", "");
        spin(400);
        PROVEN_TEST_ASSERT(proven_http_event_server_connections(server) == held, "and closed when it has been idle for idle_timeout_ms", "");
        PROVEN_TEST_ASSERT(get_ok(client, &c, false, at.port, "/", 5), "the next request makes a new one", "");
        proven_http_event_client_destroy(client);
        spin(60);

        /* With reuse off: a connection each, closed when it is done - and still six at a time. */
        client = a_client((proven_http_event_client_config_t){ .no_connection_reuse = true }, false);
        const int closes_before = g_served_close;
        PROVEN_TEST_ASSERT(get_ok(client, &c, false, at.port, "/", 5) && get_ok(client, &c, false, at.port, "/", 5), "two requests with no_connection_reuse", "");
        spin(60);
        PROVEN_TEST_ASSERT(g_served_close == closes_before + 2 && proven_http_event_server_connections(server) == held, "each said Connection: close, and nothing is kept", "");
        for (int i = 0; i < 14; i++) PROVEN_TEST_ASSERT(start(client, &many[i], "GET", url(false, at.port, "/slow"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK, "fourteen at once", "");
        spin(30);
        PROVEN_TEST_ASSERT(proven_http_event_server_connections(server) == held + 6, "the limit on connections to an origin holds all the same", "");
        for (int i = 0; i < 14; i++) (void)spin_until(&many[i].done, 10000);
        good = 0;
        for (int i = 0; i < 14; i++) good += many[i].why == PROVEN_OK && many[i].dones == 1;
        PROVEN_TEST_ASSERT(good == 14, "and all fourteen are answered", "");
        proven_http_event_client_destroy(client);
        spin(60);

        /* Two origins: a connection is used only for where it leads. */
        proven_net_addr_t at2;
        proven_http_event_server_t *second = a_server(false, &at2);
        client = a_client((proven_http_event_client_config_t){ 0 }, false);
        all = true;
        for (int i = 0; i < 6; i++) all = all && get_ok(client, &c, false, i % 2 ? at2.port : at.port, "/", 5);
        PROVEN_TEST_ASSERT(all && proven_http_event_server_connections(server) == held + 1 && proven_http_event_server_connections(second) == 1, "requests to two servers in turn: one connection to each", "");
        proven_http_event_client_destroy(client);
        spin(60);

        /* The bound on idle connections is the client's, not an origin's: when a third is to be
         * kept and two may be, the one that has been idle longest goes, wherever it leads. */
        proven_net_addr_t at3;
        proven_http_event_server_t *third = a_server(false, &at3);
        client = a_client((proven_http_event_client_config_t){ .max_kept_connections = 2 }, false);
        PROVEN_TEST_ASSERT(get_ok(client, &c, false, at.port, "/", 5), "a request to the first server", "");
        spin(15);
        PROVEN_TEST_ASSERT(get_ok(client, &c, false, at2.port, "/", 5), "then one to the second", "");
        spin(15);
        PROVEN_TEST_ASSERT(proven_http_event_server_connections(server) == held + 1 && proven_http_event_server_connections(second) == 1, "two idle connections, one to each", "");
        PROVEN_TEST_ASSERT(get_ok(client, &c, false, at3.port, "/", 5), "then one to a third", "");
        spin(60);
        PROVEN_TEST_ASSERT(proven_http_event_server_connections(server) == held && proven_http_event_server_connections(second) == 1 && proven_http_event_server_connections(third) == 1,
            "max_kept_connections of two: the first server's, idle longest, is closed to keep the third's - and the second's stays", "");
        PROVEN_TEST_ASSERT(get_ok(client, &c, false, at2.port, "/", 5) && get_ok(client, &c, false, at.port, "/", 5), "the second server's is used again, and the first is connected to anew", "");
        spin(60);
        PROVEN_TEST_ASSERT(proven_http_event_server_connections(server) == held + 1 && proven_http_event_server_connections(second) == 1 && proven_http_event_server_connections(third) == 0,
            "and now the third's was the one idle longest", "");
        proven_http_event_client_destroy(client);
        spin(60);
        proven_http_event_server_destroy(third);
        proven_http_event_server_destroy(second);
    }
    proven_http_event_server_destroy(server);

    /* A server that closes idle connections sooner than the client does, and a request that
     * crosses the close: the loop is not turned while the server's time runs out, so the
     * client learns of nothing before it has given the kept connection to the next request.
     * (On one machine the request's own write brings the reset back before anything is read,
     * so this is the path of a failed write or read. The other path over TLS - the server's
     * goodbye read first, which is bytes and not an answer - needs a network between.) */
    proven_http_event_server_config_t quick = { .on = { srv_request, srv_body, srv_writable, srv_done }, .tls = tls ? g_server_tls : NULL, .idle_timeout_ms = 60 };
    PROVEN_TEST_ASSERT(proven_http_event_server_create(g_loop, &quick, &server) == PROVEN_OK &&
                       proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "a server that keeps an idle connection 60 ms", how);
    client = a_client((proven_http_event_client_config_t){ 0 }, tls);
    for (int round = 0; round < 3; round++) {
        PROVEN_TEST_ASSERT(get_ok(client, &c, tls, at.port, "/", 5), "a request, and its connection kept", how);
        for (proven_time_t until = proven_time_monotonic_now() + 150000000; proven_time_monotonic_now() < until; ) { }
        /* One turn: the server's timer runs, after the sockets of that turn, and its close goes
         * out. The client's connection is told in the next turn - by which time it is in use. */
        PROVEN_TEST_ASSERT(proven_loop_poll(g_loop, PROVEN_NET_DONT_WAIT) == PROVEN_OK, "one turn of the loop", how);
        PROVEN_TEST_ASSERT(start(client, &c, "POST", url(tls, at.port, "/echo"), (proven_mem_view_t){ (const proven_byte_t *)"abc", 3 }, 0) == PROVEN_OK && spin_until(&c.done, 5000) &&
                           c.why == PROVEN_OK && c.dones == 1 && c.responses == 1, "the next request crosses the server's close, and is answered all the same: sent again on a new connection, with one on_done", how);
    }
    proven_http_event_client_destroy(client);
    spin(60);
    proven_http_event_server_destroy(server);
}

/* The test as a server that keeps its connection: one exchange on it. False if no request came. */
static bool peer_exchange(proven_net_conn_t *conn, const char *answer, char *req, proven_size_t cap) {
    proven_size_t have = 0;
    memset(req, 0, cap);
    for (int i = 0; i < 400 && !strstr(req, "\r\n\r\n"); ++i) {
        spin(5);
        proven_result_size_t r = proven_net_read(conn, (proven_mem_mut_t){ .ptr = (proven_byte_t *)req + have, .size = cap - 1 - have }, PROVEN_NET_DONT_WAIT);
        if (r.err == PROVEN_OK) have += r.value;
        else if (r.err == PROVEN_ERR_EOF) return false;
    }
    if (!strstr(req, "\r\n\r\n")) return false;
    return !answer || proven_net_write_all(conn, (proven_mem_view_t){ .ptr = (const proven_byte_t *)answer, .size = strlen(answer) }, proven_net_deadline_in(5000)).err == PROVEN_OK;
}

static bool peer_accept(proven_net_listener_t *l, proven_net_conn_t *conn, int rounds) {
    for (int i = 0; i < rounds; ++i) { spin(5); if (proven_net_accept(l, PROVEN_NET_DONT_WAIT, conn, NULL) == PROVEN_OK) return true; }
    return false;
}

static bool peer_sees_close(proven_net_conn_t *conn) {
    proven_byte_t b[64];
    for (int i = 0; i < 100; ++i) {
        spin(5);
        proven_result_size_t r = proven_net_read(conn, (proven_mem_mut_t){ .ptr = b, .size = sizeof b }, PROVEN_NET_DONT_WAIT);
        if (r.err == PROVEN_ERR_EOF) return true;
        if (r.err == PROVEN_OK && r.value > 0) return false;
    }
    return false;
}

#define OK2 "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"

static void kept_or_not(void) {
    proven_net_listener_t l;
    proven_net_addr_t at;
    proven_net_conn_t x, y;
    char req[1024];
    call_t c = { 0 };
    proven_http_event_client_t *client = a_client((proven_http_event_client_config_t){ 0 }, false);
    PROVEN_TEST_ASSERT(proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 8, &l, &at) == PROVEN_OK, "a listener the test answers on", "");

    /* What a response must be for its connection to be kept: each of these is not. */
    static const struct { const char *answer, *why; } not_kept[] = {
        { "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 2\r\n\r\nok", "the server said Connection: close" },
        { "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok", "the server answered in HTTP/1.0" },
        { "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokHTTP/1.1 200 OK\r\n", "more came than the response" },
    };
    for (int i = 0; i < 3; i++) {
        PROVEN_TEST_ASSERT(start(client, &c, "GET", url(false, at.port, "/"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && peer_accept(&l, &x, 400) && peer_exchange(&x, not_kept[i].answer, req, sizeof req) &&
                           spin_until(&c.done, 5000) && c.why == PROVEN_OK && c.got == 2, "a request is answered", not_kept[i].why);
        PROVEN_TEST_ASSERT(peer_sees_close(&x), "and the client closes the connection", not_kept[i].why);
        (void)proven_net_close(&x);
    }

    /* Kept, and found dead when it is used: the server reads the second request and closes. */
    PROVEN_TEST_ASSERT(start(client, &c, "GET", url(false, at.port, "/"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && peer_accept(&l, &x, 400) && peer_exchange(&x, OK2, req, sizeof req) &&
                       spin_until(&c.done, 5000) && c.why == PROVEN_OK, "a request is answered, and its connection kept", "");
    PROVEN_TEST_ASSERT(start(client, &c, "POST", url(false, at.port, "/again"), (proven_mem_view_t){ (const proven_byte_t *)"body", 4 }, 0) == PROVEN_OK && peer_exchange(&x, NULL, req, sizeof req) &&
                       strncmp(req, "POST /again ", 12) == 0 && !peer_accept(&l, &y, 6), "the next request comes on the same connection, and on no other", "");
    (void)proven_net_close(&x);
    PROVEN_TEST_ASSERT(peer_accept(&l, &y, 400) && peer_exchange(&y, OK2, req, sizeof req) && strncmp(req, "POST /again ", 12) == 0 && strstr(req, "\r\n\r\nbody"),
        "closed with nothing said: the request comes again, whole, on a new connection", "");
    PROVEN_TEST_ASSERT(spin_until(&c.done, 5000) && c.why == PROVEN_OK && c.dones == 1 && c.responses == 1 && c.got == 2, "and is answered, with one on_done and nothing told of the first try", "");

    /* Once, not twice: the new connection is its own, and its loss is the request's. */
    PROVEN_TEST_ASSERT(start(client, &c, "GET", url(false, at.port, "/third"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && peer_exchange(&y, NULL, req, sizeof req), "a third request, on the kept connection", "");
    (void)proven_net_close(&y);
    PROVEN_TEST_ASSERT(peer_accept(&l, &x, 400) && peer_exchange(&x, NULL, req, sizeof req) && strncmp(req, "GET /third ", 11) == 0, "sent again on a new one", "");
    (void)proven_net_close(&x);
    PROVEN_TEST_ASSERT(spin_until(&c.done, 5000) && c.why == PROVEN_ERR_RESET && c.dones == 1 && !peer_accept(&l, &y, 6), "which is closed too: PROVEN_ERR_RESET, and no third sending", "");

    /* Not when something of an answer had come. */
    PROVEN_TEST_ASSERT(start(client, &c, "GET", url(false, at.port, "/"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && peer_accept(&l, &x, 400) && peer_exchange(&x, OK2, req, sizeof req) &&
                       spin_until(&c.done, 5000) && c.why == PROVEN_OK, "a connection kept", "");
    PROVEN_TEST_ASSERT(start(client, &c, "GET", url(false, at.port, "/part"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && peer_exchange(&x, "HTTP/1.1 200 OK\r\nContent-Le", req, sizeof req), "a request on it, and the beginning of an answer", "");
    spin(30);
    (void)proven_net_close(&x);
    PROVEN_TEST_ASSERT(spin_until(&c.done, 5000) && c.why == PROVEN_ERR_RESET && c.dones == 1 && !peer_accept(&l, &y, 6), "cut after a part of the answer: PROVEN_ERR_RESET, not sent again", "");

    /* Nor a request whose body is written in pieces: the client does not have it to send again. */
    PROVEN_TEST_ASSERT(start(client, &c, "GET", url(false, at.port, "/"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && peer_accept(&l, &x, 400) && peer_exchange(&x, OK2, req, sizeof req) &&
                       spin_until(&c.done, 5000) && c.why == PROVEN_OK, "a connection kept", "");
    c.to_send = 0;
    PROVEN_TEST_ASSERT(start(client, &c, "POST", url(false, at.port, "/pieces"), (proven_mem_view_t){ 0 }, 3) == PROVEN_OK, "a request whose body is to be written", "");
    PROVEN_TEST_ASSERT(proven_http_event_request_write(c.request, (proven_mem_view_t){ (const proven_byte_t *)"abc", 3 }).value == 3 && proven_http_event_request_end(c.request) == PROVEN_OK &&
                       peer_exchange(&x, NULL, req, sizeof req), "written, ended, and read by the server", "");
    (void)proven_net_close(&x);
    PROVEN_TEST_ASSERT(spin_until(&c.done, 5000) && c.why == PROVEN_ERR_AGAIN && c.dones == 1 && !peer_accept(&l, &y, 6), "the connection was dead: not sent again, and PROVEN_ERR_AGAIN - its caller may start it again", "");

    /* But once something of an answer has come, such a request may have been acted on. */
    PROVEN_TEST_ASSERT(start(client, &c, "GET", url(false, at.port, "/"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && peer_accept(&l, &x, 400) && peer_exchange(&x, OK2, req, sizeof req) &&
                       spin_until(&c.done, 5000) && c.why == PROVEN_OK, "a connection kept", "");
    PROVEN_TEST_ASSERT(start(client, &c, "POST", url(false, at.port, "/pieces"), (proven_mem_view_t){ 0 }, 3) == PROVEN_OK &&
                       proven_http_event_request_write(c.request, (proven_mem_view_t){ (const proven_byte_t *)"abc", 3 }).value == 3 && proven_http_event_request_end(c.request) == PROVEN_OK &&
                       peer_exchange(&x, "HTTP/1.1 200 OK\r\nContent-Le", req, sizeof req), "a request written in pieces, and the beginning of an answer", "");
    spin(30);
    (void)proven_net_close(&x);
    PROVEN_TEST_ASSERT(spin_until(&c.done, 5000) && c.why == PROVEN_ERR_RESET && c.dones == 1, "cut after a part of the answer: PROVEN_ERR_RESET", "");

    /* A kept connection that the server closes while nobody uses it is simply gone. */
    PROVEN_TEST_ASSERT(start(client, &c, "GET", url(false, at.port, "/"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && peer_accept(&l, &x, 400) && peer_exchange(&x, OK2, req, sizeof req) &&
                       spin_until(&c.done, 5000) && c.why == PROVEN_OK, "a connection kept", "");
    (void)proven_net_close(&x);
    spin(40);
    PROVEN_TEST_ASSERT(start(client, &c, "GET", url(false, at.port, "/fresh"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && peer_accept(&l, &y, 400) && peer_exchange(&y, OK2, req, sizeof req) &&
                       spin_until(&c.done, 5000) && c.why == PROVEN_OK && c.dones == 1, "closed while idle: the next request makes a new connection", "");
    /* Given up by its caller on a kept connection: that is an end, not a dead connection. */
    PROVEN_TEST_ASSERT(start(client, &c, "GET", url(false, at.port, "/given-up"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK, "a request on the kept connection", "");
    proven_http_event_request_abort(c.request);
    PROVEN_TEST_ASSERT(c.done && c.why == PROVEN_ERR_RESET && c.dones == 1 && proven_http_event_client_requests(client) == 0 && !peer_accept(&l, &x, 6), "aborted at once: PROVEN_ERR_RESET inside the call, and nothing is sent again", "");
    (void)proven_net_close(&y);
    proven_http_event_client_destroy(client);

    /* A client destroyed with a request waiting does not make that request a connection on the way out. */
    client = a_client((proven_http_event_client_config_t){ .max_connections_per_origin = 1 }, false);
    call_t behind = { 0 };
    PROVEN_TEST_ASSERT(start(client, &c, "GET", url(false, at.port, "/"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && peer_accept(&l, &x, 400) && peer_exchange(&x, NULL, req, sizeof req) &&
                       start(client, &behind, "GET", url(false, at.port, "/behind"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK, "a request the server does not answer, and one waiting behind it", "");
    proven_http_event_client_destroy(client);
    PROVEN_TEST_ASSERT(c.done && behind.done && c.why == PROVEN_ERR_RESET && behind.why == PROVEN_ERR_RESET && c.dones == 1 && behind.dones == 1 && !peer_accept(&l, &y, 6),
        "destroyed: both end once, and no connection is made for the one that waited", "");
    (void)proven_net_close(&x);

    /* With reuse off, a server that does not close is not waited for. */
    client = a_client((proven_http_event_client_config_t){ .no_connection_reuse = true }, false);
    PROVEN_TEST_ASSERT(start(client, &c, "GET", url(false, at.port, "/"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && peer_accept(&l, &x, 400) && peer_exchange(&x, OK2, req, sizeof req) &&
                       strstr(req, "\r\nConnection: close\r\n") && spin_until(&c.done, 5000) && c.why == PROVEN_OK, "no_connection_reuse: the request says Connection: close", "");
    PROVEN_TEST_ASSERT(peer_sees_close(&x), "and the client closes when the response has ended, though the server did not", "");
    (void)proven_net_close(&x);
    proven_http_event_client_destroy(client);
    (void)proven_net_listener_close(&l);
}

/* The test as a TLS server on a socket of its own: the engine, driven by hand between rounds. */
typedef struct { proven_net_conn_t conn; proven_tls_conn_t *tls; char req[2048]; proven_size_t have; bool eof; } tpeer_t;

/* What has come is fed in and what is to go is written; `turn_loop` false leaves the client
 * as it is, so that it learns of nothing yet. */
static bool tpeer_turn(tpeer_t *p, bool turn_loop) {
    static proven_byte_t in[16384];
    if (turn_loop) spin(5);
    proven_result_size_t got = proven_net_read(&p->conn, (proven_mem_mut_t){ in, sizeof in }, PROVEN_NET_DONT_WAIT);
    if (got.err == PROVEN_ERR_EOF) p->eof = true;
    for (proven_size_t pos = 0; got.err == PROVEN_OK && pos < got.value; ) {
        proven_size_t used = 0;
        if (proven_tls_feed(p->tls, (proven_mem_view_t){ in + pos, got.value - pos }, &used) != PROVEN_OK || used == 0) return false;
        pos += used;
    }
    for (;;) {
        proven_result_size_t plain = proven_tls_read(p->tls, (proven_mem_mut_t){ (proven_byte_t *)p->req + p->have, sizeof p->req - 1 - p->have });
        if (plain.err != PROVEN_OK || plain.value == 0) break;
        p->have += plain.value;
    }
    proven_mem_view_t out = proven_tls_pending_output(p->tls);
    if (out.size > 0) {
        if (proven_net_write_all(&p->conn, out, proven_net_deadline_in(5000)).err != PROVEN_OK) return false;
        proven_tls_output_sent(p->tls, out.size);
    }
    return true;
}

static bool tpeer_accept(proven_net_listener_t *l, tpeer_t *p) {
    memset(p, 0, sizeof *p);
    if (!peer_accept(l, &p->conn, 400) || proven_tls_server_create(g_server_tls, &p->tls) != PROVEN_OK) return false;
    for (int i = 0; i < 400 && !(proven_tls_is_established(p->tls) && proven_tls_pending_output(p->tls).size == 0); ++i) if (!tpeer_turn(p, true)) return false;
    return proven_tls_is_established(p->tls);
}

/* A request, whole: its head, and `body` after it when one is expected. (It may have come
 * with the end of the handshake; what was read is forgotten when it is answered.) */
static bool tpeer_request(tpeer_t *p, const char *body) {
    for (int i = 0; i < 400; ++i) {
        const char *end = strstr(p->req, "\r\n\r\n");
        if (end && (!body || strcmp(end + 4, body) == 0)) return true;
        if (!tpeer_turn(p, true)) return false;
    }
    return false;
}

static bool tpeer_say(tpeer_t *p, const char *text) {
    proven_result_size_t w = proven_tls_write(p->tls, mv(text));
    memset(p->req, 0, sizeof p->req);
    p->have = 0;
    return w.err == PROVEN_OK && w.value == strlen(text) && tpeer_turn(p, true);
}

static bool tpeer_sees_close(tpeer_t *p) {
    for (int i = 0; i < 100 && !p->eof; ++i) (void)tpeer_turn(p, true);
    return p->eof;
}

static void tpeer_end(tpeer_t *p) { proven_tls_conn_destroy(p->tls); (void)proven_net_close(&p->conn); }

/* Over TLS a server that closes says so first: its goodbye (close_notify) is bytes on the
 * connection, and is not an answer. Here it says goodbye and leaves the socket open, so the
 * goodbye is what the client reads - not a reset, which on one machine would overtake it. */
static void kept_over_tls(void) {
    proven_net_listener_t l;
    proven_net_addr_t at;
    static tpeer_t a, b;
    call_t c = { 0 };
    proven_http_event_client_t *client = a_client((proven_http_event_client_config_t){ 0 }, true);
    PROVEN_TEST_ASSERT(proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 8, &l, &at) == PROVEN_OK, "a listener the test answers on, as a TLS server", "");

    /* The goodbye arrives after the request has gone on the kept connection. */
    PROVEN_TEST_ASSERT(start(client, &c, "GET", url(true, at.port, "/"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && tpeer_accept(&l, &a) && tpeer_request(&a, NULL) && tpeer_say(&a, OK2) &&
                       spin_until(&c.done, 5000) && c.why == PROVEN_OK && c.got == 2, "a request is answered, and its connection kept", "");
    PROVEN_TEST_ASSERT(start(client, &c, "POST", url(true, at.port, "/again"), (proven_mem_view_t){ (const proven_byte_t *)"body", 4 }, 0) == PROVEN_OK && tpeer_request(&a, "body") &&
                       strncmp(a.req, "POST /again ", 12) == 0, "the next request comes on the same connection", "");
    PROVEN_TEST_ASSERT(proven_tls_close(a.tls) == PROVEN_OK && tpeer_turn(&a, true), "the server says goodbye, and does not close the socket", "");
    PROVEN_TEST_ASSERT(tpeer_accept(&l, &b) && tpeer_request(&b, "body") && strncmp(b.req, "POST /again ", 12) == 0 && tpeer_say(&b, OK2),
        "a goodbye is not an answer: the request comes again, whole, on a new connection", "");
    PROVEN_TEST_ASSERT(spin_until(&c.done, 5000) && c.why == PROVEN_OK && c.dones == 1 && c.responses == 1 && c.got == 2, "and is answered, with one on_done", "");
    PROVEN_TEST_ASSERT(tpeer_sees_close(&a), "the connection that said goodbye is closed by the client", "");
    tpeer_end(&a);

    /* The goodbye is already there, unread, when the kept connection is given to a request:
     * it is written before the client's loop turns, and the request is started at once. */
    PROVEN_TEST_ASSERT(proven_tls_close(b.tls) == PROVEN_OK && tpeer_turn(&b, false), "the server says goodbye to the idle connection, and the client has not looked yet", "");
    PROVEN_TEST_ASSERT(start(client, &c, "POST", url(true, at.port, "/crossed"), (proven_mem_view_t){ (const proven_byte_t *)"body", 4 }, 0) == PROVEN_OK, "a request is given that connection", "");
    PROVEN_TEST_ASSERT(tpeer_accept(&l, &a) && tpeer_request(&a, "body") && strncmp(a.req, "POST /crossed ", 14) == 0 && tpeer_say(&a, OK2) &&
                       spin_until(&c.done, 5000) && c.why == PROVEN_OK && c.dones == 1 && c.responses == 1, "it crosses the goodbye, comes again on a new connection, and is answered with one on_done", "");
    tpeer_end(&b);

    /* Said to a connection nobody is using: the connection is simply gone. */
    PROVEN_TEST_ASSERT(proven_tls_close(a.tls) == PROVEN_OK && tpeer_turn(&a, true) && tpeer_sees_close(&a), "a goodbye to an idle connection: the client closes it", "");
    tpeer_end(&a);
    PROVEN_TEST_ASSERT(start(client, &c, "GET", url(true, at.port, "/fresh"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && tpeer_accept(&l, &a) && tpeer_request(&a, NULL) && tpeer_say(&a, OK2) &&
                       spin_until(&c.done, 5000) && c.why == PROVEN_OK && c.dones == 1, "and the next request makes a new one", "");

    /* A part of an answer, then the goodbye: something came back, so nothing is sent again. */
    PROVEN_TEST_ASSERT(start(client, &c, "GET", url(true, at.port, "/part"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && tpeer_request(&a, NULL) && tpeer_say(&a, "HTTP/1.1 200 OK\r\nContent-Le"), "a request on the kept connection, and the beginning of an answer", "");
    spin(30);
    PROVEN_TEST_ASSERT(proven_tls_close(a.tls) == PROVEN_OK && tpeer_turn(&a, true) && spin_until(&c.done, 5000) && c.why == PROVEN_ERR_RESET && c.dones == 1 && !peer_accept(&l, &b.conn, 6),
        "then the goodbye: PROVEN_ERR_RESET, and not sent again", "");
    tpeer_end(&a);
    proven_http_event_client_destroy(client);
    (void)proven_net_listener_close(&l);
}

/* retry_unsent over HTTP/1.1: a connection lost before any of the request was given to it.
 * Over TLS that is a server that takes the connection and goes away while TLS is being agreed. */
static void unsent(void) {
    proven_net_listener_t l;
    proven_net_addr_t at;
    proven_net_conn_t x;
    static tpeer_t a;
    call_t c = { 0 };
    PROVEN_TEST_ASSERT(proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 8, &l, &at) == PROVEN_OK, "a listener the test answers on", "");

    proven_http_event_client_t *client = a_client((proven_http_event_client_config_t){ 0 }, true);
    PROVEN_TEST_ASSERT(start(client, &c, "GET", url(true, at.port, "/"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && peer_accept(&l, &x, 400), "a request, and a server that takes the connection", "");
    spin(20);
    (void)proven_net_close(&x);
    PROVEN_TEST_ASSERT(spin_until(&c.done, 5000) && c.why == PROVEN_ERR_RESET && c.dones == 1 && !peer_accept(&l, &x, 6), "and closes it before TLS is agreed: PROVEN_ERR_RESET without the setting, and no second connection", "");
    proven_http_event_client_destroy(client);

    client = a_client((proven_http_event_client_config_t){ .retry_unsent = 1, .connect_timeout_ms = 300 }, true);
    PROVEN_TEST_ASSERT(start(client, &c, "POST", url(true, at.port, "/again"), (proven_mem_view_t){ (const proven_byte_t *)"body", 4 }, 0) == PROVEN_OK && peer_accept(&l, &x, 400), "with retry_unsent of one: a request, and the connection taken", "");
    spin(20);
    (void)proven_net_close(&x);
    PROVEN_TEST_ASSERT(tpeer_accept(&l, &a) && tpeer_request(&a, "body") && strncmp(a.req, "POST /again ", 12) == 0 && tpeer_say(&a, OK2), "closed before TLS is agreed: the request comes on a new connection", "");
    PROVEN_TEST_ASSERT(spin_until(&c.done, 5000) && c.why == PROVEN_OK && c.dones == 1 && c.responses == 1 && c.got == 2, "and is answered, with one on_done", "");
    PROVEN_TEST_ASSERT(proven_tls_close(a.tls) == PROVEN_OK && tpeer_turn(&a, true) && tpeer_sees_close(&a), "(the kept connection is said goodbye to)", "");
    tpeer_end(&a);

    PROVEN_TEST_ASSERT(start(client, &c, "GET", url(true, at.port, "/twice"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && peer_accept(&l, &x, 400), "a request, and the connection taken", "");
    spin(20);
    (void)proven_net_close(&x);
    PROVEN_TEST_ASSERT(peer_accept(&l, &x, 400), "closed: a second connection is made", "");
    spin(20);
    (void)proven_net_close(&x);
    PROVEN_TEST_ASSERT(spin_until(&c.done, 5000) && c.why == PROVEN_ERR_RESET && c.dones == 1 && !peer_accept(&l, &x, 6), "closed too: PROVEN_ERR_RESET, and no third", "");

    /* Each try has the time to be connected: the first is closed 200 ms in, the second says nothing. */
    const proven_time_t t0 = proven_time_monotonic_now();
    PROVEN_TEST_ASSERT(start(client, &c, "GET", url(true, at.port, "/slow"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && peer_accept(&l, &x, 400), "a request, and the connection taken", "");
    spin(200);
    (void)proven_net_close(&x);
    PROVEN_TEST_ASSERT(peer_accept(&l, &x, 400) && spin_until(&c.done, 5000) && c.why == PROVEN_ERR_TIMEOUT && c.dones == 1 && proven_time_monotonic_now() - t0 > 450000000,
        "closed after 200 ms, and the second connection never answers: PROVEN_ERR_TIMEOUT after connect_timeout_ms of that try", "");
    (void)proven_net_close(&x);
    PROVEN_TEST_ASSERT(!peer_accept(&l, &x, 6), "and a time limit is not tried again", "");

    /* Not a request whose body is written in pieces. */
    PROVEN_TEST_ASSERT(start(client, &c, "POST", url(true, at.port, "/pieces"), (proven_mem_view_t){ 0 }, 3) == PROVEN_OK && peer_accept(&l, &x, 400), "a request whose body is to be written", "");
    spin(20);
    (void)proven_net_close(&x);
    PROVEN_TEST_ASSERT(spin_until(&c.done, 5000) && c.why == PROVEN_ERR_RESET && c.dones == 1 && !peer_accept(&l, &x, 6), "is not begun again: PROVEN_ERR_RESET", "");

    /* Nor one that was sent: the server may have acted on it. */
    PROVEN_TEST_ASSERT(start(client, &c, "GET", url(true, at.port, "/sent"), (proven_mem_view_t){ 0 }, 0) == PROVEN_OK && tpeer_accept(&l, &a) && tpeer_request(&a, NULL), "a request the server has read", "");
    tpeer_end(&a);
    PROVEN_TEST_ASSERT(spin_until(&c.done, 5000) && c.why == PROVEN_ERR_RESET && c.dones == 1 && !peer_accept(&l, &x, 6), "and then the connection is cut: PROVEN_ERR_RESET, and not sent again", "");
    proven_http_event_client_destroy(client);
    (void)proven_net_listener_close(&l);
}

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

    PROVEN_TEST_SECTION("connections kept, counted and waited for", "Requests to one origin share the connections it may have: used again when an exchange ended cleanly, six at a time, the rest in a queue.", "Check cl_kill, cl_reusable, cl_conn_keep, cl_conn_give and cl_conn_close, and the choice in proven_http_event_client_start_h1_: an idle connection, a new one under the limit, or the queue.");
    connections(false);
    connections(true);
    PROVEN_TEST_SECTION("a connection that is kept, and one that is not", "The test plays the server and keeps its connection, or closes it at each moment that matters.", "Check cl_reusable for what may be kept, cl_send_again for the one sending more a request gets when a kept connection was dead, and cl_idle_io for a connection closed while idle.");
    kept_or_not();
    PROVEN_TEST_SECTION("a kept connection, and a TLS server's goodbye", "Over TLS a server that closes a connection says so first. The test is that server, and says it at each moment that matters.", "Check cl_readable: only what the server said counts as an answer (cl_alive), not the bytes of its goodbye; and cl_idle_io for a goodbye to a connection nobody uses.");
    kept_over_tls();
    PROVEN_TEST_SECTION("retry_unsent", "A connection lost before any of the request was given to it, with the setting and without.", "Check the second branch at the top of cl_kill, and `sent` in cl_flush.");
    unsent();

    proven_loop_destroy(g_loop);
    proven_tls_config_destroy(g_client_tls); proven_tls_config_destroy(g_server_tls); proven_tls_config_destroy(g_stranger_tls);
    proven_cert_store_destroy(anchors); proven_cert_store_destroy(none);
    PROVEN_TEST_PASS("the event-driven client sends, receives, holds back, gives up and reports each end once.");
    return 0;
}
