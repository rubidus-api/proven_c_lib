#include "proven.h"
#include "proven_test.h"
#include "test_unit_tls_pki.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * WebSocket on the event-driven server, over loopback.
 *
 * The loop runs on its own thread; the callbacks below are the "application" and run there.
 * The test's main thread is the clients: the library's blocking WebSocket client where a
 * client that behaves is wanted, and raw sockets where one that does not is. Every case is run
 * plain and then again over TLS.
 */

static proven_mem_view_t mv(const char *s) { return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }
static proven_u8str_view_t sv(const char *s) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }

static proven_allocator_t g_heap;
static proven_loop_t *g_loop;
static proven_http_event_server_t *g_server;
static proven_job_sys_t *g_threads;
static proven_tls_config_t *g_server_tls, *g_client_tls;

static atomic_int g_requests, g_done, g_accepted, g_closed, g_refused, g_writable, g_last_code, g_last_why, g_accept_error, g_messages, g_checks;

/* What the application keeps for one WebSocket connection. */
typedef struct {
    proven_ws_stream_t *ws;
    proven_byte_t *held;               /* echo: a piece the server would not take yet */
    proven_size_t held_len;
    bool held_text, held_first, held_last;
    int pushed, to_push;               /* push: messages sent so far, and to send */
    bool abort_on_message;
} peer_t;

static void on_closed(void *ctx, proven_ws_stream_t *ws, proven_u16 code, proven_err_t why) {
    (void)ctx;
    peer_t *p = proven_ws_stream_user(ws);
    if (p) { free(p->held); free(p); }
    atomic_store(&g_last_code, (int)code);
    atomic_store(&g_last_why, (int)why);
    atomic_fetch_add(&g_closed, 1);
}

/* Echo each piece as it arrives. A piece the server refuses is kept, and reading stops until
 * there is room: the client is slowed, and nothing piles up here. */
static void echo_message(void *ctx, proven_ws_stream_t *ws, bool text, proven_mem_view_t piece, bool first, bool last) {
    (void)ctx;
    peer_t *p = proven_ws_stream_user(ws);
    atomic_fetch_add(&g_messages, last ? 1 : 0);
    if (p->abort_on_message) { proven_ws_stream_abort(ws); return; }
    proven_err_t e = proven_ws_stream_send_piece(ws, text, piece, first, last);
    if (e != PROVEN_ERR_AGAIN) return;
    atomic_fetch_add(&g_refused, 1);
    p->held = malloc(piece.size ? piece.size : 1);
    if (!p->held) { proven_ws_stream_abort(ws); return; }
    if (piece.size) memcpy(p->held, piece.ptr, piece.size);
    p->held_len = piece.size; p->held_text = text; p->held_first = first; p->held_last = last;
    proven_ws_stream_pause(ws);
}

/* Push numbered messages as fast as the client takes them, then close. */
static void push_pump(peer_t *p) {
    while (p->pushed < p->to_push) {
        proven_byte_t msg[1000];
        memset(msg, 'a' + p->pushed % 26, sizeof msg);
        msg[0] = (proven_byte_t)(p->pushed >> 8); msg[1] = (proven_byte_t)p->pushed;
        proven_err_t e = proven_ws_stream_send(p->ws, false, (proven_mem_view_t){ .ptr = msg, .size = sizeof msg });
        if (e == PROVEN_ERR_AGAIN) { atomic_fetch_add(&g_refused, 1); return; }
        if (e != PROVEN_OK) return;
        p->pushed++;
    }
    proven_ws_stream_close(p->ws, 1000, sv("done"));
}

static void on_writable(void *ctx, proven_ws_stream_t *ws) {
    (void)ctx;
    peer_t *p = proven_ws_stream_user(ws);
    atomic_fetch_add(&g_writable, 1);
    if (p->to_push > 0) { push_pump(p); return; }
    if (p->held) {
        if (proven_ws_stream_send_piece(ws, p->held_text, (proven_mem_view_t){ .ptr = p->held, .size = p->held_len }, p->held_first, p->held_last) != PROVEN_OK) return;
        free(p->held); p->held = NULL;
        proven_ws_stream_resume(ws);
    }
}

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    (void)ctx;
    atomic_fetch_add(&g_requests, 1);
    char path[48] = { 0 };
    memcpy(path, head->target.ptr, head->target.size < sizeof path - 1 ? head->target.size : sizeof path - 1);
    if (strcmp(path, "/page") == 0) { (void)proven_http_stream_respond(stream, 200, NULL, 0, mv("a page")); return; }

    proven_ws_event_config_t cfg = {
        .on = { echo_message, on_writable, on_closed },
        .max_message_bytes = 200000, .max_buffered_output = 32 * 1024,
    };
    if (strcmp(path, "/chat") == 0 && proven_ws_request_offers(head, sv("chat"))) cfg.subprotocol = sv("chat");
    if (strcmp(path, "/quiet") == 0) { cfg.ping_interval_ms = 100; cfg.pong_timeout_ms = 150; }
    if (strcmp(path, "/bye-unanswered") == 0) cfg.close_timeout_ms = 300;
    peer_t *p = calloc(1, sizeof *p);
    proven_ws_stream_t *ws = NULL;
    int checks = 0;
    if (strcmp(path, "/checks") == 0) {
        /* Configurations that cannot make a WebSocket are refused with nothing sent. */
        proven_ws_event_config_t bad = cfg;
        bad.on.on_message = NULL;
        if (proven_ws_event_accept(stream, head, &bad, &ws) == PROVEN_ERR_INVALID_ARG && ws == NULL) checks |= 1;
        bad = cfg; bad.subprotocol = sv("not a token");
        if (proven_ws_event_accept(stream, head, &bad, &ws) == PROVEN_ERR_INVALID_ARG && ws == NULL) checks |= 2;
    }
    proven_err_t e = p ? proven_ws_event_accept(stream, head, &cfg, &ws) : PROVEN_ERR_NOMEM;
    if (e != PROVEN_OK) {
        free(p);
        atomic_store(&g_accept_error, (int)e);
        if (e == PROVEN_ERR_UNSUPPORTED) {
            proven_http_header_t v = { PROVEN_LIT("Sec-WebSocket-Version"), PROVEN_LIT("13") };
            (void)proven_http_stream_respond(stream, 426, &v, 1, mv(""));
        } else if (e != PROVEN_ERR_RESET) {
            (void)proven_http_stream_respond(stream, 400, NULL, 0, mv("not a websocket"));
        }
        return;
    }
    /* From here `stream` is over (on_done has been called) and `ws` is what there is. */
    atomic_fetch_add(&g_accepted, 1);
    p->ws = ws;
    proven_ws_stream_set_user(ws, p);
    if (strcmp(path, "/checks") == 0) {
        /* What the stream calls refuse, asked from where they can be asked: on the loop's thread. */
        proven_ws_stream_t *again = NULL;
        static proven_byte_t much[126];
        if (proven_ws_event_accept(stream, head, &cfg, &again) == PROVEN_ERR_INVALID_STATE && again == NULL) checks |= 4;
        if (proven_ws_stream_send_piece(ws, true, mv("x"), false, true) == PROVEN_ERR_INVALID_STATE) checks |= 8;
        if (proven_ws_stream_send_piece(ws, true, mv("a"), true, false) == PROVEN_OK &&
            proven_ws_stream_send(ws, true, mv("x")) == PROVEN_ERR_INVALID_STATE &&
            proven_ws_stream_send_piece(ws, true, mv("b"), false, true) == PROVEN_OK) checks |= 16;
        if (proven_ws_stream_ping(ws, (proven_mem_view_t){ .ptr = much, .size = sizeof much }) == PROVEN_ERR_OUT_OF_BOUNDS) checks |= 32;
        if (proven_ws_stream_user(ws) == p && proven_ws_stream_peer(ws).family == PROVEN_NET_FAMILY_IPV4) checks |= 64;
        proven_ws_stream_close(ws, 1005, sv(""));              /* a code that may not be sent */
        proven_ws_stream_close(ws, 4000, sv("second"));        /* and a second close, which is ignored */
        if (proven_ws_stream_send(ws, true, mv("x")) == PROVEN_ERR_INVALID_STATE) checks |= 128;
        atomic_store(&g_checks, checks);
    }
    else if (strncmp(path, "/push/", 6) == 0) { p->to_push = atoi(path + 6); push_pump(p); }
    else if (strncmp(path, "/bye", 4) == 0) proven_ws_stream_close(ws, 4001, sv("bye"));
    else if (strcmp(path, "/abort") == 0) p->abort_on_message = true;
}

static void on_done(void *ctx, proven_http_stream_t *stream, proven_err_t why) { (void)ctx; (void)stream; (void)why; atomic_fetch_add(&g_done, 1); }

static void run_loop(void *arg) { (void)arg; (void)proven_loop_run(g_loop); }

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

static bool closed_is(int count) {
    for (int i = 0; i < 4000; ++i) { if (atomic_load(&g_closed) == count) return true; proven_time_sleep(1); }
    return false;
}

static char g_url[160];
static proven_u8str_view_t url(bool tls, proven_u16 port, const char *path) {
    int n = snprintf(g_url, sizeof g_url, "%s://127.0.0.1:%u%s", tls ? "wss" : "ws", (unsigned)port, path);
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)g_url, .size = (proven_size_t)n };
}

/* A raw client: plain, or TLS over the same socket. */
typedef struct { proven_net_conn_t sock; proven_transport_t t; } raw_t;
static bool raw_open(raw_t *r, bool tls, proven_u16 port) {
    if (proven_net_connect(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, port), proven_net_deadline_in(5000), &r->sock) != PROVEN_OK) return false;
    r->t = proven_net_conn_transport(&r->sock);
    if (!tls) return true;
    return proven_tls_transport_client(r->t, g_client_tls, sv("127.0.0.1"), NULL, proven_net_deadline_in(5000), &r->t) == PROVEN_OK;
}
static bool raw_send_bytes(raw_t *r, const void *p, proven_size_t n) { return proven_transport_write_all(r->t, (proven_mem_view_t){ .ptr = p, .size = n }, proven_net_deadline_in(5000)).err == PROVEN_OK; }
static proven_size_t raw_read(raw_t *r, proven_byte_t *out, proven_size_t cap, proven_u32 ms, proven_err_t *end) {
    proven_size_t n = 0;
    for (;;) {
        proven_result_size_t got = proven_transport_read(r->t, (proven_mem_mut_t){ .ptr = out + n, .size = cap - n }, proven_net_deadline_in(ms));
        if (got.err != PROVEN_OK) { if (end) *end = got.err; break; }
        n += got.value;
        if (n == cap) { if (end) *end = PROVEN_OK; break; }
    }
    return n;
}
static void raw_close(raw_t *r) { (void)proven_transport_close(r->t); }

#define UPGRADE "GET %s HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: %s\r\n\r\n"
#define ACCEPT_VALUE "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="

/* Open a raw connection and do the handshake by hand; what follows the head is left in `rest`. */
static bool raw_upgrade(raw_t *r, bool tls, proven_u16 port, const char *path) {
    char req[256], got[512] = { 0 };
    int n = snprintf(req, sizeof req, UPGRADE, path, "13");
    if (!raw_open(r, tls, port) || !raw_send_bytes(r, req, (proven_size_t)n)) return false;
    proven_size_t have = 0;
    while (have < sizeof got - 1 && !strstr(got, "\r\n\r\n")) {
        proven_result_size_t g = proven_transport_read(r->t, (proven_mem_mut_t){ .ptr = (proven_byte_t *)got + have, .size = 1 }, proven_net_deadline_in(5000));
        if (g.err != PROVEN_OK) return false;
        have += g.value;
    }
    return strncmp(got, "HTTP/1.1 101", 12) == 0 && strstr(got, "Sec-WebSocket-Accept: " ACCEPT_VALUE "\r\n") != NULL;
}

static void cases(bool tls) {
    const char *how = tls ? "over TLS" : "plain";
    atomic_store(&g_requests, 0); atomic_store(&g_done, 0); atomic_store(&g_accepted, 0); atomic_store(&g_closed, 0);
    atomic_store(&g_refused, 0); atomic_store(&g_writable, 0); atomic_store(&g_messages, 0);
    PROVEN_TEST_ASSERT(proven_loop_create(g_heap, &g_loop) == PROVEN_OK, "a loop", how);
    proven_http_event_server_config_t cfg = { .on = { .on_request = on_request, .on_done = on_done }, .tls = tls ? g_server_tls : NULL };
    proven_net_addr_t at;
    PROVEN_TEST_ASSERT(proven_http_event_server_create(g_loop, &cfg, &g_server) == PROVEN_OK &&
                       proven_http_event_server_listen(g_server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "a server", how);
    proven_job_group_t running;
    proven_job_group_init(&running);
    PROVEN_TEST_ASSERT(proven_job_group_submit(g_threads, &running, run_loop, NULL) == PROVEN_OK, "the loop on its thread", how);

    proven_http_client_config_t ccfg = { .alloc = g_heap, .tls_wrap = tls ? proven_tls_http_wrap : NULL, .tls_ctx = g_client_tls };
    proven_http_client_t *client = NULL;
    PROVEN_TEST_ASSERT(proven_http_client_create(&ccfg, &client) == PROVEN_OK, "a client", how);
    proven_ws_conn_config_t wcfg = { .alloc = g_heap, .max_message_bytes = 4 * 1024 * 1024 };
    proven_ws_conn_t *ws = NULL;
    proven_ws_message_t msg;
    proven_u16 status = 0;
    int closed = 0;

    /* Messages there and back. */
    PROVEN_TEST_ASSERT(proven_ws_conn_connect(client, url(tls, at.port, "/echo"), NULL, 0, sv(""), &wcfg, &ws, &status) == PROVEN_OK && status == 101, "a WebSocket is accepted from inside on_request", how);
    /* The client has its 101 before the handler's own next line has run: wait for that. */
    for (int i = 0; i < 4000 && atomic_load(&g_accepted) != 1; ++i) proven_time_sleep(1);
    PROVEN_TEST_ASSERT(atomic_load(&g_done) == atomic_load(&g_requests) && atomic_load(&g_accepted) == 1, "on_done was called for the request it began as", how);
    PROVEN_TEST_ASSERT(proven_ws_conn_send_text(ws, sv("hello")) == PROVEN_OK && proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &msg) == PROVEN_OK &&
                       msg.text && msg.data.size == 5 && memcmp(msg.data.ptr, "hello", 5) == 0, "a text message is echoed", how);
    PROVEN_TEST_ASSERT(proven_ws_conn_send_binary(ws, (proven_mem_view_t){ 0 }) == PROVEN_OK && proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &msg) == PROVEN_OK &&
                       !msg.text && msg.data.size == 0, "and an empty binary one", how);
    {
        enum { BIG = 150000 };
        static proven_byte_t big[BIG];
        for (int i = 0; i < BIG; ++i) big[i] = (proven_byte_t)(i * 31 + i / 7);
        PROVEN_TEST_ASSERT(proven_ws_conn_send_binary(ws, (proven_mem_view_t){ .ptr = big, .size = BIG }) == PROVEN_OK &&
                           proven_ws_conn_receive(ws, proven_net_deadline_in(10000), &msg) == PROVEN_OK && !msg.text && msg.data.size == BIG && memcmp(msg.data.ptr, big, BIG) == 0,
            "150,000 bytes pass through in pieces and come back intact", how);
        PROVEN_TEST_ASSERT(proven_ws_conn_send_part(ws, true, mv("one, "), false) == PROVEN_OK && proven_ws_conn_send_part(ws, true, mv("two, "), false) == PROVEN_OK &&
                           proven_ws_conn_send_part(ws, true, mv("three"), true) == PROVEN_OK && proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &msg) == PROVEN_OK &&
                           msg.text && msg.data.size == 15 && memcmp(msg.data.ptr, "one, two, three", 15) == 0, "a message sent in three fragments is echoed as one", how);
    }
    PROVEN_TEST_ASSERT(proven_ws_conn_ping(ws, mv("are you there")) == PROVEN_OK && proven_ws_conn_send_text(ws, sv("after")) == PROVEN_OK &&
                       proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &msg) == PROVEN_OK && proven_ws_conn_pong_count(ws) == 1, "a ping is answered with a pong", how);
    PROVEN_TEST_ASSERT(proven_ws_conn_close(ws, 1000, sv("thanks")) == PROVEN_OK && closed_is(++closed) && atomic_load(&g_last_code) == 1000 && atomic_load(&g_last_why) == PROVEN_OK,
        "a close from the client is answered, and on_closed says 1000 and PROVEN_OK", how);
    proven_ws_conn_destroy(ws);

    /* A subprotocol, named only when offered. */
    PROVEN_TEST_ASSERT(proven_ws_conn_connect(client, url(tls, at.port, "/chat"), NULL, 0, sv("chat, other"), &wcfg, &ws, NULL) == PROVEN_OK &&
                       proven_u8str_view_eq(proven_ws_conn_protocol(ws), sv("chat")), "the subprotocol the server chose is named in the answer", how);
    PROVEN_TEST_ASSERT(proven_ws_conn_close(ws, 1001, sv("")) == PROVEN_OK && closed_is(++closed) && atomic_load(&g_last_code) == 1001, "and the client's close code is the one reported", how);
    proven_ws_conn_destroy(ws);

    /* The server pushes faster than the client reads: sends are refused and resumed. */
    {
        int refused_before = atomic_load(&g_refused), writable_before = atomic_load(&g_writable);
        enum { COUNT = 6000 };
        char path[32];
        snprintf(path, sizeof path, "/push/%d", (int)COUNT);
        PROVEN_TEST_ASSERT(proven_ws_conn_connect(client, url(tls, at.port, path), NULL, 0, sv(""), &wcfg, &ws, NULL) == PROVEN_OK, "a connection the server pushes 6,000 messages down", how);
        /* Read nothing until the server has been refused once. */
        for (int i = 0; i < 20000 && atomic_load(&g_refused) == refused_before; ++i) proven_time_sleep(1);
        int got = 0;
        bool in_order = true;
        proven_err_t e;
        while ((e = proven_ws_conn_receive(ws, proven_net_deadline_in(10000), &msg)) == PROVEN_OK) {
            in_order = in_order && msg.data.size == 1000 && msg.data.ptr[0] == (proven_byte_t)(got >> 8) && msg.data.ptr[1] == (proven_byte_t)got && msg.data.ptr[999] == (proven_byte_t)('a' + got % 26);
            got++;
        }
        PROVEN_TEST_ASSERT(got == COUNT && in_order, "every message arrives, whole and in order", how);
        PROVEN_TEST_ASSERT(atomic_load(&g_refused) > refused_before && atomic_load(&g_writable) > writable_before, "sends were refused with PROVEN_ERR_AGAIN and resumed through on_writable", how);
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_EOF && proven_ws_conn_close_code(ws) == 1000 && proven_u8str_view_eq(proven_ws_conn_close_reason(ws), sv("done")) &&
                           closed_is(++closed) && atomic_load(&g_last_code) == 1000 && atomic_load(&g_last_why) == PROVEN_OK,
            "then the server's close arrives with its code and reason, and both sides agree it was clean", how);
        proven_ws_conn_destroy(ws);
    }

    /* A close from the server at once, and an abort. */
    PROVEN_TEST_ASSERT(proven_ws_conn_connect(client, url(tls, at.port, "/bye"), NULL, 0, sv(""), &wcfg, &ws, NULL) == PROVEN_OK &&
                       proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &msg) == PROVEN_ERR_EOF && proven_ws_conn_close_code(ws) == 4001 &&
                       proven_u8str_view_eq(proven_ws_conn_close_reason(ws), sv("bye")), "a server that closes with 4001 is seen to", how);
    PROVEN_TEST_ASSERT(closed_is(++closed) && atomic_load(&g_last_code) == 4001 && atomic_load(&g_last_why) == PROVEN_OK, "and its on_closed says 4001, PROVEN_OK", how);
    proven_ws_conn_destroy(ws);
    PROVEN_TEST_ASSERT(proven_ws_conn_connect(client, url(tls, at.port, "/abort"), NULL, 0, sv(""), &wcfg, &ws, NULL) == PROVEN_OK && proven_ws_conn_send_text(ws, sv("x")) == PROVEN_OK &&
                       proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &msg) == PROVEN_ERR_RESET, "proven_ws_stream_abort ends the connection with no close frame", how);
    PROVEN_TEST_ASSERT(closed_is(++closed) && atomic_load(&g_last_code) == 1006 && atomic_load(&g_last_why) == PROVEN_ERR_RESET, "and on_closed says 1006, PROVEN_ERR_RESET", how);
    proven_ws_conn_destroy(ws);

    /* What the calls refuse. */
    PROVEN_TEST_ASSERT(proven_ws_conn_connect(client, url(tls, at.port, "/checks"), NULL, 0, sv(""), &wcfg, &ws, NULL) == PROVEN_OK &&
                       proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &msg) == PROVEN_OK && msg.text && msg.data.size == 2 && memcmp(msg.data.ptr, "ab", 2) == 0,
        "a message sent in two pieces by the server arrives as one", how);
    PROVEN_TEST_ASSERT(proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &msg) == PROVEN_ERR_EOF && proven_ws_conn_close_code(ws) == 1000 && closed_is(++closed) && atomic_load(&g_last_code) == 1000,
        "a close code that may not be sent goes out as 1000, and a second close changes nothing", how);
    PROVEN_TEST_ASSERT((atomic_load(&g_checks) & 3) == 3, "accept refuses a configuration with no on_message, and a subprotocol that is not a token, with PROVEN_ERR_INVALID_ARG", how);
    PROVEN_TEST_ASSERT((atomic_load(&g_checks) & 4) == 4, "accept on a stream that was already accepted is PROVEN_ERR_INVALID_STATE", how);
    PROVEN_TEST_ASSERT((atomic_load(&g_checks) & 24) == 24, "a piece that is not first outside a message, and a new message inside one, are PROVEN_ERR_INVALID_STATE", how);
    PROVEN_TEST_ASSERT((atomic_load(&g_checks) & 32) == 32, "a ping of 126 bytes is PROVEN_ERR_OUT_OF_BOUNDS", how);
    PROVEN_TEST_ASSERT((atomic_load(&g_checks) & 192) == 192, "the user pointer and the peer address are kept, and a send after a close was sent is PROVEN_ERR_INVALID_STATE", how);
    proven_ws_conn_destroy(ws);

    /* Liveness: a client that answers pings stays; one that does not is dropped. */
    PROVEN_TEST_ASSERT(proven_ws_conn_connect(client, url(tls, at.port, "/quiet"), NULL, 0, sv(""), &wcfg, &ws, NULL) == PROVEN_OK &&
                       proven_ws_conn_receive(ws, proven_net_deadline_in(700), &msg) == PROVEN_ERR_TIMEOUT, "a silent client is pinged, and answering keeps it connected for 700 ms", how);
    PROVEN_TEST_ASSERT(atomic_load(&g_closed) == closed && proven_ws_conn_send_text(ws, sv("still here")) == PROVEN_OK &&
                       proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &msg) == PROVEN_OK && msg.data.size == 10, "and it still works", how);
    PROVEN_TEST_ASSERT(proven_ws_conn_close(ws, 1000, sv("")) == PROVEN_OK && closed_is(++closed), "closed", how);
    proven_ws_conn_destroy(ws);
    raw_t raw;
    proven_byte_t bytes[256];
    proven_err_t end = PROVEN_OK;
    PROVEN_TEST_ASSERT(raw_upgrade(&raw, tls, at.port, "/quiet"), "a raw client that will not answer pings", how);
    proven_time_t t0 = proven_time_monotonic_now();
    proven_size_t n = raw_read(&raw, bytes, sizeof bytes, 5000, &end);
    proven_time_t took = proven_time_monotonic_now() - t0;
    PROVEN_TEST_ASSERT(n >= 2 && bytes[0] == 0x89 && bytes[1] == 0x00 && end != PROVEN_OK && took > 200000000 && took < 4000000000LL, "it is sent a ping, and then dropped after the pong timeout", how);
    PROVEN_TEST_ASSERT(closed_is(++closed) && atomic_load(&g_last_code) == 1006 && atomic_load(&g_last_why) == PROVEN_ERR_TIMEOUT, "with on_closed saying 1006, PROVEN_ERR_TIMEOUT", how);
    raw_close(&raw);

    PROVEN_TEST_ASSERT(raw_upgrade(&raw, tls, at.port, "/bye-unanswered"), "a raw client that will not answer a close", how);
    t0 = proven_time_monotonic_now();
    n = raw_read(&raw, bytes, sizeof bytes, 5000, &end);
    took = proven_time_monotonic_now() - t0;
    PROVEN_TEST_ASSERT(n == 7 && bytes[0] == 0x88 && bytes[1] == 0x05 && bytes[2] == 0x0f && bytes[3] == 0xa1 && memcmp(bytes + 4, "bye", 3) == 0 && end != PROVEN_OK &&
                       took > 200000000 && took < 4000000000LL, "it is sent the close, and the connection is ended after the close timeout", how);
    PROVEN_TEST_ASSERT(closed_is(++closed) && atomic_load(&g_last_code) == 4001 && atomic_load(&g_last_why) == PROVEN_ERR_TIMEOUT, "with on_closed saying 4001, PROVEN_ERR_TIMEOUT", how);
    raw_close(&raw);

    /* The close timeout is not put off by a client that keeps talking instead of answering. */
    PROVEN_TEST_ASSERT(raw_upgrade(&raw, tls, at.port, "/bye-unanswered"), "a raw client that answers a close with pings", how);
    n = 0;
    while (n < 7) {
        proven_result_size_t got = proven_transport_read(raw.t, (proven_mem_mut_t){ .ptr = bytes + n, .size = 7 - n }, proven_net_deadline_in(5000));
        if (got.err != PROVEN_OK) break;
        n += got.value;
    }
    PROVEN_TEST_ASSERT(n == 7 && bytes[0] == 0x88, "it has the server's close", how);
    t0 = proven_time_monotonic_now();
    int before_pings = atomic_load(&g_closed);
    for (int i = 0; i < 60 && atomic_load(&g_closed) == before_pings; ++i) {
        (void)raw_send_bytes(&raw, "\x89\x80\x01\x02\x03\x04", 6);
        proven_time_sleep(50);
    }
    took = proven_time_monotonic_now() - t0;
    PROVEN_TEST_ASSERT(closed_is(++closed) && atomic_load(&g_last_code) == 4001 && atomic_load(&g_last_why) == PROVEN_ERR_TIMEOUT && took < 2000000000,
        "three seconds of pings do not keep it open: it is ended after the close timeout all the same", how);
    raw_close(&raw);

    /* Clients that break the protocol. */
    PROVEN_TEST_ASSERT(raw_upgrade(&raw, tls, at.port, "/echo") && raw_send_bytes(&raw, "\x81\x02hi", 4), "a frame that is not masked", how);
    n = raw_read(&raw, bytes, sizeof bytes, 3000, &end);
    PROVEN_TEST_ASSERT(n == 4 && bytes[0] == 0x88 && bytes[1] == 0x02 && bytes[2] == 0x03 && bytes[3] == 0xea && closed_is(++closed) &&
                       atomic_load(&g_last_code) == 1002 && atomic_load(&g_last_why) == PROVEN_ERR_INVALID_FORMAT, "is answered with close 1002 and reported as PROVEN_ERR_INVALID_FORMAT", how);
    raw_close(&raw);
    PROVEN_TEST_ASSERT(raw_upgrade(&raw, tls, at.port, "/echo") && raw_send_bytes(&raw, "\x82\xff\x00\x00\x00\x00\x00\x04\x93\xe0\x01\x02\x03\x04", 14), "a frame announcing 300,000 bytes, past the limit", how);
    n = raw_read(&raw, bytes, sizeof bytes, 3000, &end);
    PROVEN_TEST_ASSERT(n == 4 && bytes[0] == 0x88 && bytes[2] == 0x03 && bytes[3] == 0xf1 && closed_is(++closed) &&
                       atomic_load(&g_last_code) == 1009 && atomic_load(&g_last_why) == PROVEN_ERR_OUT_OF_BOUNDS, "is answered with close 1009 and reported as PROVEN_ERR_OUT_OF_BOUNDS", how);
    raw_close(&raw);
    PROVEN_TEST_ASSERT(raw_upgrade(&raw, tls, at.port, "/echo") && raw_send_bytes(&raw, "\x81\x82\x00\x00\x00\x00\xc3\x28", 8), "text that is not UTF-8", how);
    n = raw_read(&raw, bytes, sizeof bytes, 3000, &end);
    PROVEN_TEST_ASSERT(n == 4 && bytes[0] == 0x88 && bytes[2] == 0x03 && bytes[3] == 0xef && closed_is(++closed) &&
                       atomic_load(&g_last_code) == 1007 && atomic_load(&g_last_why) == PROVEN_ERR_INVALID_ENCODING, "is answered with close 1007 and reported as PROVEN_ERR_INVALID_ENCODING", how);
    raw_close(&raw);
    PROVEN_TEST_ASSERT(raw_upgrade(&raw, tls, at.port, "/echo") && raw_send_bytes(&raw, "\x88\x80\x01\x02\x03\x04", 6), "a close frame with no code", how);
    n = raw_read(&raw, bytes, sizeof bytes, 3000, &end);
    PROVEN_TEST_ASSERT(n == 2 && bytes[0] == 0x88 && bytes[1] == 0x00 && closed_is(++closed) && atomic_load(&g_last_code) == 1005 && atomic_load(&g_last_why) == PROVEN_OK,
        "is answered with one, and reported as 1005, PROVEN_OK", how);
    raw_close(&raw);
    PROVEN_TEST_ASSERT(raw_upgrade(&raw, tls, at.port, "/echo"), "a client that connects", how);
    raw_close(&raw);
    PROVEN_TEST_ASSERT(closed_is(++closed) && atomic_load(&g_last_code) == 1006 && atomic_load(&g_last_why) == PROVEN_ERR_RESET, "and vanishes without a close frame: 1006, PROVEN_ERR_RESET", how);

    /* Requests that are not, or not quite, WebSocket upgrades stay ordinary requests. */
    {
        char text[512] = { 0 }, req[256];
        PROVEN_TEST_ASSERT(raw_open(&raw, tls, at.port) && raw_send_bytes(&raw, "GET /echo HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", 50), "a plain request to the WebSocket's address", how);
        n = raw_read(&raw, (proven_byte_t *)text, sizeof text - 1, 3000, &end);
        PROVEN_TEST_ASSERT(strncmp(text, "HTTP/1.1 400", 12) == 0 && strstr(text, "not a websocket") && atomic_load(&g_accept_error) == PROVEN_ERR_NOT_FOUND,
            "is PROVEN_ERR_NOT_FOUND to proven_ws_event_accept, with nothing sent: the handler answers", how);
        raw_close(&raw);
        memset(text, 0, sizeof text);
        int rn = snprintf(req, sizeof req, UPGRADE, "/echo", "12");
        PROVEN_TEST_ASSERT(raw_open(&raw, tls, at.port) && raw_send_bytes(&raw, req, (proven_size_t)rn), "an upgrade for version 12", how);
        n = raw_read(&raw, (proven_byte_t *)text, sizeof text - 1, 1500, &end);
        PROVEN_TEST_ASSERT(strncmp(text, "HTTP/1.1 426", 12) == 0 && strstr(text, "Sec-WebSocket-Version: 13") && atomic_load(&g_accept_error) == PROVEN_ERR_UNSUPPORTED,
            "is PROVEN_ERR_UNSUPPORTED, and the handler answers 426", how);
        raw_close(&raw);
        memset(text, 0, sizeof text);
        PROVEN_TEST_ASSERT(raw_open(&raw, tls, at.port) && raw_send_bytes(&raw, "GET /echo HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n\r\n", 99), "an upgrade with no key", how);
        n = raw_read(&raw, (proven_byte_t *)text, sizeof text - 1, 1500, &end);
        PROVEN_TEST_ASSERT(strncmp(text, "HTTP/1.1 400", 12) == 0 && atomic_load(&g_accept_error) == PROVEN_ERR_INVALID_FORMAT, "is PROVEN_ERR_INVALID_FORMAT, and the handler answers 400", how);
        raw_close(&raw);
        proven_http_client_response_t resp;
        int n2 = snprintf(req, sizeof req, "%s://127.0.0.1:%u/page", tls ? "https" : "http", (unsigned)at.port);
        PROVEN_TEST_ASSERT(proven_http_client_get(client, (proven_u8str_view_t){ .ptr = (const proven_byte_t *)req, .size = (proven_size_t)n2 }, &resp) == PROVEN_OK && resp.status == 200,
            "and an ordinary page is served by the same server", how);
        proven_http_client_finish(&resp);
    }
    PROVEN_TEST_ASSERT(atomic_load(&g_closed) == closed && atomic_load(&g_accepted) == closed, "every accepted connection was closed exactly once", how);
    for (int i = 0; i < 3000 && atomic_load(&g_done) != atomic_load(&g_requests); ++i) proven_time_sleep(1);
    PROVEN_TEST_ASSERT(atomic_load(&g_done) == atomic_load(&g_requests), "and on_done was called once for every on_request, upgraded or not", how);

    /* Destroying the server under an open WebSocket. */
    PROVEN_TEST_ASSERT(proven_ws_conn_connect(client, url(tls, at.port, "/echo"), NULL, 0, sv(""), &wcfg, &ws, NULL) == PROVEN_OK && connections() >= 1, "a connection left open", how);
    on_loop(do_destroy);
    PROVEN_TEST_ASSERT(g_server == NULL && closed_is(++closed) && atomic_load(&g_last_code) == 1006 && atomic_load(&g_last_why) == PROVEN_ERR_RESET,
        "destroying the server ends it with on_closed(1006, PROVEN_ERR_RESET)", how);
    PROVEN_TEST_ASSERT(proven_ws_conn_receive(ws, proven_net_deadline_in(3000), &msg) == PROVEN_ERR_RESET, "and the client sees the connection end", how);
    proven_ws_conn_destroy(ws);
    proven_http_client_destroy(client);

    proven_loop_stop(g_loop);
    proven_job_group_wait(g_threads, &running);
    proven_loop_destroy(g_loop);
    g_loop = NULL;
}

/* What an idle WebSocket connection holds, measured with a counting allocator and the loop
 * driven by hand on this thread. */
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
static int g_quiet_closed;
static void quiet_message(void *ctx, proven_ws_stream_t *ws, bool text, proven_mem_view_t piece, bool first, bool last) {
    (void)ctx; (void)first;
    if (last) (void)proven_ws_stream_send(ws, text, piece);
}
static void quiet_closed(void *ctx, proven_ws_stream_t *ws, proven_u16 code, proven_err_t why) { (void)ctx; (void)ws; (void)code; (void)why; g_quiet_closed++; }
static void quiet_request(void *ctx, proven_http_stream_t *s, const proven_http_request_t *req) {
    (void)ctx;
    proven_ws_event_config_t cfg = { .on = { .on_message = quiet_message, .on_closed = quiet_closed } };
    proven_ws_stream_t *ws = NULL;
    if (proven_ws_event_accept(s, req, &cfg, &ws) != PROVEN_OK) (void)proven_http_stream_respond(s, 400, NULL, 0, mv(""));
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
    char req[256];
    int rn = snprintf(req, sizeof req, UPGRADE, "/", "13");
    proven_size_t base = count.live;
    for (int i = 0; i < N; ++i) {
        PROVEN_TEST_ASSERT(proven_net_connect(at, proven_net_deadline_in(5000), &socks[i]) == PROVEN_OK &&
                           proven_net_write_all(&socks[i], (proven_mem_view_t){ .ptr = (const proven_byte_t *)req, .size = (proven_size_t)rn }, proven_net_deadline_in(5000)).err == PROVEN_OK &&
                           /* one message each, so that every connection has been through its whole path */
                           proven_net_write_all(&socks[i], (proven_mem_view_t){ .ptr = (const proven_byte_t *)"\x81\x82\x01\x02\x03\x04ik", .size = 8 }, proven_net_deadline_in(5000)).err == PROVEN_OK,
            "a connection that upgrades and sends one message", "");
        if (i % 16 == 15) PROVEN_TEST_ASSERT(proven_loop_poll(loop, proven_net_deadline_in(20)) == PROVEN_OK, "a round", "");
    }
    proven_net_deadline_t until = proven_net_deadline_in(400);
    while (proven_time_monotonic_now() < until) PROVEN_TEST_ASSERT(proven_loop_poll(loop, until) == PROVEN_OK, "a round", "");
    PROVEN_TEST_ASSERT(proven_http_event_server_connections(server) == N, "three hundred WebSocket connections, each echoed once, all idle", "");
    proven_size_t each = (count.live - base) / N;
    fprintf(stderr, "[PROVEN][TEST][INFO] an idle plain WebSocket connection holds %u bytes\n", (unsigned)each);
    PROVEN_TEST_ASSERT(each < 1024, "an idle WebSocket connection holds less than 1 KiB: its two structs, and no buffer", "");
    for (int i = 0; i < N; ++i) (void)proven_net_close(&socks[i]);
    until = proven_net_deadline_in(300);
    while (proven_time_monotonic_now() < until) PROVEN_TEST_ASSERT(proven_loop_poll(loop, until) == PROVEN_OK, "a round", "");
    PROVEN_TEST_ASSERT(proven_http_event_server_connections(server) == 0 && count.live == base && g_quiet_closed == N, "closed by their clients, each was reported once and gave everything back", "");
    proven_http_event_server_destroy(server);
    PROVEN_TEST_ASSERT(count.live == 0, "and so has the server, destroyed", "");
    proven_loop_destroy(loop);
}

int main(void) {
    PROVEN_TEST_SUITE("WebSocket on the event-driven server",
        "Messages both ways in pieces, sends refused and resumed, pings, closes from either side, clients that break the protocol or vanish - plain and over TLS - and what an idle connection holds.",
        "Inspect src/proven/ws_event.c: ws_on_input for what arrives, ws_on_timer for liveness, ws_end and ws_finish for the end. The connection underneath is src/proven/http_event.c in its EV_RAW state.");

    g_heap = proven_heap_allocator();
    PROVEN_TEST_ASSERT(tp_build(), "the test's certificates and keys are issued", "");
    PROVEN_TEST_ASSERT(proven_job_system_init(g_heap, 1, 4, &g_threads) == PROVEN_OK, "a thread for the loop", "");
    proven_cert_store_t *anchors = NULL;
    PROVEN_TEST_ASSERT(proven_cert_store_create(g_heap, &anchors) == PROVEN_OK &&
                       proven_cert_store_add_pem(anchors, (proven_mem_view_t){ (const proven_byte_t *)TP_CA, strlen(TP_CA) }, NULL) == PROVEN_OK, "anchors", "");
    proven_tls_options_t co = { .alloc = g_heap, .anchors = anchors };
    proven_tls_options_t so = { .alloc = g_heap, .certificate_pem = { (const proven_byte_t *)TP_SERVER_ED, strlen(TP_SERVER_ED) }, .private_key_pem = { (const proven_byte_t *)TP_SERVER_ED_KEY, strlen(TP_SERVER_ED_KEY) } };
    PROVEN_TEST_ASSERT(proven_tls_config_create(&co, &g_client_tls) == PROVEN_OK && proven_tls_config_create(&so, &g_server_tls) == PROVEN_OK, "TLS configurations", "");

    PROVEN_TEST_SECTION("what accept refuses", "Arguments that cannot make a WebSocket.", "Check proven_ws_event_accept.");
    {
        proven_ws_stream_t *ws = (proven_ws_stream_t *)1;
        proven_ws_event_config_t none = { 0 };
        proven_http_request_t head = { 0 };
        PROVEN_TEST_ASSERT(proven_ws_event_accept(NULL, &head, &none, &ws) == PROVEN_ERR_INVALID_ARG && ws == NULL, "no stream is PROVEN_ERR_INVALID_ARG", "");
        PROVEN_TEST_ASSERT(proven_ws_stream_send(NULL, true, mv("x")) == PROVEN_ERR_INVALID_ARG && proven_ws_stream_buffered(NULL) == 0 && proven_ws_stream_user(NULL) == NULL,
            "and the stream calls accept null", "");
        proven_ws_stream_close(NULL, 1000, sv("")); proven_ws_stream_abort(NULL); proven_ws_stream_pause(NULL); proven_ws_stream_resume(NULL);
    }

    PROVEN_TEST_SECTION("plain", "Every case over an unencrypted connection.", "");
    cases(false);
    PROVEN_TEST_SECTION("over TLS", "The same cases with the TLS engine between the socket and the decoder.", "");
    cases(true);
    PROVEN_TEST_SECTION("what an idle connection holds", "Three hundred WebSocket connections that have each echoed a message and now say nothing.", "Check the size of struct proven_ws_stream and of the connection struct in http_event.c.");
    memory_case();

    proven_tls_config_destroy(g_client_tls); proven_tls_config_destroy(g_server_tls); proven_cert_store_destroy(anchors);
    proven_job_system_close(g_threads); proven_job_system_destroy(g_threads);
    PROVEN_TEST_PASS("WebSocket connections on the loop carry messages, hold back, notice silence and end cleanly.");
    return 0;
}
