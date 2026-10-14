#include "proven.h"
#include "proven_test.h"
#include "test_unit_tls_pki.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * A WebSocket connection made on a loop (proven_ws_event_connect).
 *
 * The peer is this library's event-driven server on the same loop, which the test turns by
 * hand - so everything here happens on one thread, in an order the test decides - and, for
 * the ways a connection can fail to become a WebSocket, listeners the test answers itself.
 * A server's decoder refuses a frame that is not masked, so every message that arrives there
 * is also a check that this side masked it.
 */

static proven_mem_view_t mv(const char *s) { return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }
static proven_u8str_view_t sv(const char *s) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }

static proven_allocator_t g_heap;
static proven_loop_t *g_loop;
static proven_tls_config_t *g_server_tls, *g_client_tls;

#define BIG 400000u

/* The liveness cases wait for pings and pongs by the clock: 40 ms here, 150 there. Under
 * ThreadSanitizer, beside fifteen other tests on a busy machine, a round of the loop has been
 * seen to take that long by itself (v0.41.0, v0.43.0): there every such time is four times
 * as long. */
#if defined(__SANITIZE_THREAD__)
#define SLOW 4u
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define SLOW 4u
#endif
#endif
#ifndef SLOW
#define SLOW 1u
#endif
static proven_byte_t g_msg[BIG];

// -----------------------------------------------------------------------------
// The server: paths choose what it does with a connection
// -----------------------------------------------------------------------------

typedef struct {
    proven_ws_stream_t *ws;
    proven_byte_t *whole;              /* echo: the message so far */
    proven_size_t len, cap;
    char mode;                         /* 'e' echo, 'p' push fifty, 'c' close at once, 's' silent */
    int pushed;
    proven_loop_timer_t timer;
} srv_t;

static int g_srv_closed, g_srv_last_code, g_srv_messages;
static proven_err_t g_srv_last_why;

static void srv_push(void *ctx) {
    srv_t *s = ctx;
    while (s->pushed < 50) {
        char text[32];
        int n = snprintf(text, sizeof text, "message %d", s->pushed);
        if (proven_ws_stream_send(s->ws, true, (proven_mem_view_t){ (const proven_byte_t *)text, (proven_size_t)n }) != PROVEN_OK) return;
        s->pushed++;
    }
}

static void srv_message(void *ctx, proven_ws_stream_t *ws, bool text, proven_mem_view_t piece, bool first, bool last) {
    (void)ctx;
    srv_t *s = proven_ws_stream_user(ws);
    if (first) s->len = 0;
    if (s->len + piece.size > s->cap) {
        proven_size_t cap = s->cap ? s->cap : 4096;
        while (cap < s->len + piece.size) cap *= 2;
        s->whole = realloc(s->whole, cap);
        s->cap = cap;
    }
    if (piece.size) memcpy(s->whole + s->len, piece.ptr, piece.size);
    s->len += piece.size;
    if (!last) return;
    g_srv_messages++;
    if (s->mode == 'e') (void)proven_ws_stream_send(ws, text, (proven_mem_view_t){ s->whole, s->len });
}

static void srv_closed(void *ctx, proven_ws_stream_t *ws, proven_u16 code, proven_err_t why) {
    (void)ctx;
    srv_t *s = proven_ws_stream_user(ws);
    g_srv_closed++;
    g_srv_last_code = code;
    g_srv_last_why = why;
    if (s) { proven_loop_timer_cancel(g_loop, &s->timer); free(s->whole); free(s); }
}

static void srv_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    (void)ctx;
    char path[48] = { 0 };
    memcpy(path, head->target.ptr, head->target.size < sizeof path - 1 ? head->target.size : sizeof path - 1);
    if (strcmp(path, "/page") == 0) { (void)proven_http_stream_respond(stream, 200, NULL, 0, mv("a page, not a socket")); return; }
    proven_ws_event_config_t cfg = { .on = { srv_message, NULL, srv_closed }, .max_message_bytes = 1024 * 1024, .max_buffered_output = 4 * 1024 * 1024,
                                     .compress = strcmp(path, "/plain") != 0 };
    if (strcmp(path, "/sub") == 0) cfg.subprotocol = sv("superchat");
    if (strcmp(path, "/pings") == 0) { cfg.ping_interval_ms = 40 * SLOW; cfg.pong_timeout_ms = 150 * SLOW; }
    /* The fields a client may add are seen here: one of them is asked for back. */
    proven_u8str_view_t origin = { 0 };
    const bool has_origin = proven_http_header_find(head->headers, head->header_count, sv("Origin"), &origin);
    srv_t *s = calloc(1, sizeof *s);
    proven_ws_stream_t *ws = NULL;
    if (!s || proven_ws_event_accept(stream, head, &cfg, &ws) != PROVEN_OK) {
        free(s);
        (void)proven_http_stream_respond(stream, 400, NULL, 0, mv("no"));
        return;
    }
    s->ws = ws;
    s->mode = strcmp(path, "/push") == 0 ? 'p' : strcmp(path, "/close") == 0 ? 'c' : strcmp(path, "/silent") == 0 || strcmp(path, "/pings") == 0 ? 's' : 'e';
    proven_ws_stream_set_user(ws, s);
    if (strcmp(path, "/origin") == 0) (void)proven_ws_stream_send(ws, true, has_origin ? (proven_mem_view_t){ origin.ptr, origin.size } : mv("(none)"));
    if (s->mode == 'p') proven_loop_timer_set(g_loop, &s->timer, 1, srv_push, s);
    if (s->mode == 'c') proven_ws_stream_close(ws, 4001, sv("bye"));
}

// -----------------------------------------------------------------------------
// The client's side of one connection
// -----------------------------------------------------------------------------

typedef struct {
    proven_ws_stream_t *ws;
    int opens, closes, messages, pieces;
    proven_u16 code;
    proven_err_t why;
    char protocol[64];
    bool compressed_at_open, text;
    proven_byte_t *got;                /* the last message, whole */
    proven_size_t got_len, got_cap;
    proven_err_t sent_in_open;         /* what a send from inside on_open returned */
    const char *say_in_open;
    /* the fifty that are pushed */
    int next, out_of_order, while_paused, pause_at;
    bool paused_now;
    proven_loop_timer_t timer;
} cli_t;

static void cli_resume(void *ctx) {
    cli_t *c = ctx;
    c->paused_now = false;
    proven_ws_stream_resume(c->ws);
}

static void cli_open(void *ctx, proven_ws_stream_t *ws, proven_u8str_view_t protocol) {
    cli_t *c = ctx;
    c->opens++;
    c->compressed_at_open = proven_ws_stream_compressed(ws);
    snprintf(c->protocol, sizeof c->protocol, "%.*s", (int)protocol.size, (const char *)protocol.ptr);
    if (c->say_in_open) c->sent_in_open = proven_ws_stream_send(ws, true, mv(c->say_in_open));
}

static void cli_message(void *ctx, proven_ws_stream_t *ws, bool text, proven_mem_view_t piece, bool first, bool last) {
    cli_t *c = ctx;
    if (c->paused_now) c->while_paused++;
    c->pieces++;
    if (first) c->got_len = 0;
    if (c->got_len + piece.size > c->got_cap) {
        proven_size_t cap = c->got_cap ? c->got_cap : 4096;
        while (cap < c->got_len + piece.size) cap *= 2;
        c->got = realloc(c->got, cap);
        c->got_cap = cap;
    }
    if (piece.size) memcpy(c->got + c->got_len, piece.ptr, piece.size);
    c->got_len += piece.size;
    if (!last) return;
    c->text = text;
    c->messages++;
    if (c->pause_at) {
        char want[32];
        int n = snprintf(want, sizeof want, "message %d", c->next);
        if (c->got_len != (proven_size_t)n || memcmp(c->got, want, (size_t)n) != 0) c->out_of_order++;
        c->next++;
        if (c->next == c->pause_at) {
            c->paused_now = true;
            proven_ws_stream_pause(ws);
            proven_loop_timer_set(g_loop, &c->timer, 60 * SLOW, cli_resume, c);
        }
    }
}

static void cli_closed(void *ctx, proven_ws_stream_t *ws, proven_u16 code, proven_err_t why) {
    (void)ws;
    cli_t *c = ctx;
    c->closes++;
    c->code = code;
    c->why = why;
    proven_loop_timer_cancel(g_loop, &c->timer);
}

static void turn(proven_u32 ms) {
    proven_net_deadline_t until = proven_net_deadline_in(ms);
    while (proven_time_monotonic_now() < until) (void)proven_loop_poll(g_loop, proven_net_deadline_in(5));
}

static bool turn_until(const int *counter, int at_least, proven_u32 ms) {
    proven_net_deadline_t until = proven_net_deadline_in(ms);
    while (*counter < at_least && proven_time_monotonic_now() < until) (void)proven_loop_poll(g_loop, proven_net_deadline_in(5));
    return *counter >= at_least;
}

static char g_url[160];
static proven_u8str_view_t url(bool tls, proven_u16 port, const char *path) {
    int n = snprintf(g_url, sizeof g_url, "%s://127.0.0.1:%u%s", tls ? "wss" : "ws", (unsigned)port, path);
    return (proven_u8str_view_t){ (const proven_byte_t *)g_url, (proven_size_t)n };
}

static proven_err_t dial(cli_t *c, bool tls, proven_u16 port, const char *path, proven_ws_event_connect_options_t o) {
    const char *say = c->say_in_open;
    const int pause_at = c->pause_at;
    free(c->got);
    memset(c, 0, sizeof *c);
    c->say_in_open = say;
    c->pause_at = pause_at;
    o.url = url(tls, port, path);
    o.tls = tls ? g_client_tls : NULL;
    o.on_open = cli_open;
    o.ws.on = (proven_ws_event_callbacks_t){ cli_message, NULL, cli_closed };
    o.ws.ctx = c;
    if (o.ws.max_message_bytes == 0) o.ws.max_message_bytes = 1024 * 1024;
    return proven_ws_event_connect(g_loop, &o, &c->ws);
}

static bool echoed(cli_t *c, bool text, const proven_byte_t *p, proven_size_t n) {
    const int before = c->messages;
    if (proven_ws_stream_send(c->ws, text, (proven_mem_view_t){ p, n }) != PROVEN_OK) return false;
    return turn_until(&c->messages, before + 1, 10000) && c->text == text && c->got_len == n && (n == 0 || memcmp(c->got, p, n) == 0);
}

// -----------------------------------------------------------------------------
// Listeners the test answers itself, for upgrades that do not happen
// -----------------------------------------------------------------------------

/* Accept on `l`, read the request head, answer with `answer` (NULL: nothing), and - when
 * `then_close` - close. The loop is turned meanwhile. */
static bool scripted(proven_net_listener_t *l, proven_net_conn_t *x, const char *answer, bool then_close) {
    bool came = false;
    for (int i = 0; i < 400 && !came; ++i) { (void)proven_loop_poll(g_loop, proven_net_deadline_in(5)); came = proven_net_accept(l, PROVEN_NET_DONT_WAIT, x, NULL) == PROVEN_OK; }
    if (!came) return false;
    char req[1024];
    proven_size_t have = 0;
    memset(req, 0, sizeof req);
    for (int i = 0; i < 400 && !strstr(req, "\r\n\r\n"); ++i) {
        (void)proven_loop_poll(g_loop, proven_net_deadline_in(5));
        proven_result_size_t r = proven_net_read(x, (proven_mem_mut_t){ (proven_byte_t *)req + have, sizeof req - 1 - have }, PROVEN_NET_DONT_WAIT);
        if (r.err == PROVEN_OK) have += r.value;
    }
    if (!strstr(req, "\r\n\r\n") || strncmp(req, "GET /x HTTP/1.1\r\n", 17) != 0 || !strstr(req, "\r\nUpgrade: websocket\r\n") || !strstr(req, "\r\nSec-WebSocket-Version: 13\r\n") ||
        !strstr(req, "\r\nSec-WebSocket-Key: ") || !strstr(req, "\r\nHost: 127.0.0.1:")) return false;
    if (answer && proven_net_write_all(x, mv(answer), proven_net_deadline_in(5000)).err != PROVEN_OK) return false;
    if (then_close) (void)proven_net_close(x);
    return true;
}

/* Accept on `l`, read the request, and grant the upgrade as a server would - and then say
 * nothing at all, ever: no pong, no answer to a close. */
static bool granted_then_silent(proven_net_listener_t *l, proven_net_conn_t *x) {
    bool came = false;
    for (int i = 0; i < 400 && !came; ++i) { (void)proven_loop_poll(g_loop, proven_net_deadline_in(5)); came = proven_net_accept(l, PROVEN_NET_DONT_WAIT, x, NULL) == PROVEN_OK; }
    if (!came) return false;
    char req[1024], answer[256];
    proven_size_t have = 0;
    memset(req, 0, sizeof req);
    for (int i = 0; i < 400 && !strstr(req, "\r\n\r\n"); ++i) {
        (void)proven_loop_poll(g_loop, proven_net_deadline_in(5));
        proven_result_size_t r = proven_net_read(x, (proven_mem_mut_t){ (proven_byte_t *)req + have, sizeof req - 1 - have }, PROVEN_NET_DONT_WAIT);
        if (r.err == PROVEN_OK) have += r.value;
    }
    const char *key = strstr(req, "\r\nSec-WebSocket-Key: ");
    if (!key || !strstr(req, "\r\n\r\n")) return false;
    key += 21;
    proven_byte_t accept[PROVEN_WS_ACCEPT_SIZE];
    if (proven_ws_accept_key((proven_u8str_view_t){ (const proven_byte_t *)key, PROVEN_WS_KEY_SIZE }, accept) != PROVEN_OK) return false;
    int n = snprintf(answer, sizeof answer, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %.*s\r\n\r\n", (int)sizeof accept, (const char *)accept);
    return proven_net_write_all(x, (proven_mem_view_t){ (const proven_byte_t *)answer, (proven_size_t)n }, proven_net_deadline_in(5000)).err == PROVEN_OK;
}

static void cases(bool tls) {
    const char *how = tls ? "over TLS" : "in the clear";
    proven_http_event_server_config_t scfg = { .on = { .on_request = srv_request }, .tls = tls ? g_server_tls : NULL };
    proven_http_event_server_t *server = NULL;
    proven_net_addr_t at;
    PROVEN_TEST_ASSERT(proven_http_event_server_create(g_loop, &scfg, &server) == PROVEN_OK &&
                       proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "a server on the loop", how);
    static cli_t c;
    proven_ws_event_connect_options_t plain = { 0 }, zip = { .ws = { .compress = true } };
    g_srv_closed = 0;

    /* Open, messages both ways, and a close from this side. */
    PROVEN_TEST_ASSERT(dial(&c, tls, at.port, "/echo", plain) == PROVEN_OK && c.ws != NULL && c.opens == 0 && c.closes == 0, "a connection is begun: nothing has happened inside the call", how);
    PROVEN_TEST_ASSERT(proven_ws_stream_send(c.ws, true, mv("too early")) == PROVEN_ERR_INVALID_STATE && proven_ws_stream_ping(c.ws, mv("")) == PROVEN_ERR_INVALID_STATE &&
                       proven_ws_stream_send_piece(c.ws, true, mv("x"), true, false) == PROVEN_ERR_INVALID_STATE, "nothing may be sent before the server has accepted", how);
    PROVEN_TEST_ASSERT(turn_until(&c.opens, 1, 5000) && c.opens == 1 && c.protocol[0] == '\0' && !c.compressed_at_open && !proven_ws_stream_compressed(c.ws), "on_open, once: no subprotocol, no compression - none was asked for", how);
    proven_net_addr_t peer = proven_ws_stream_peer(c.ws);
    PROVEN_TEST_ASSERT(peer.port == at.port, "the peer is the server", how);
    PROVEN_TEST_ASSERT(echoed(&c, true, (const proven_byte_t *)"hello", 5), "a text message goes and comes back: this side's frames were masked, or the server would have refused them", "Check ws_frame in src/proven/ws_event.c: a client's frames carry a key and their payload is masked on its way.");
    static const proven_size_t sizes[] = { 0, 1, 125, 126, 127, 4095, 4096, 4097, 65535, 65536, 300000 };
    bool all = true;
    for (proven_size_t i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) all = all && echoed(&c, i % 2 == 0, g_msg, sizes[i]);
    PROVEN_TEST_ASSERT(all, "text and binary messages of eleven sizes, from empty to 300,000 bytes, come back unchanged", how);
    PROVEN_TEST_ASSERT(proven_ws_stream_send_piece(c.ws, false, (proven_mem_view_t){ g_msg, 1000 }, true, false) == PROVEN_OK &&
                       proven_ws_stream_send_piece(c.ws, false, (proven_mem_view_t){ g_msg + 1000, 5000 }, false, false) == PROVEN_OK &&
                       proven_ws_stream_send_piece(c.ws, false, (proven_mem_view_t){ g_msg + 6000, 1 }, false, true) == PROVEN_OK &&
                       turn_until(&c.messages, 13, 5000) && c.got_len == 6001 && memcmp(c.got, g_msg, 6001) == 0, "a message sent in three pieces arrives at the server as one, and comes back", how);
    PROVEN_TEST_ASSERT(proven_ws_stream_ping(c.ws, mv("are you there")) == PROVEN_OK, "a ping of this side's own", how);
    turn(30);
    proven_ws_stream_close(c.ws, 1000, sv("done"));
    PROVEN_TEST_ASSERT(turn_until(&c.closes, 1, 5000) && c.closes == 1 && c.code == 1000 && c.why == PROVEN_OK && turn_until(&g_srv_closed, 1, 5000) && g_srv_last_code == 1000,
        "a close from this side: both ends report 1000, this one once and with PROVEN_OK", how);

    /* Compression offered, and agreed or not. */
    PROVEN_TEST_ASSERT(dial(&c, tls, at.port, "/echo", zip) == PROVEN_OK && turn_until(&c.opens, 1, 5000) && c.compressed_at_open && proven_ws_stream_compressed(c.ws), "compression offered to a server that takes it: agreed", how);
    PROVEN_TEST_ASSERT(echoed(&c, true, g_msg, 100000) && echoed(&c, false, g_msg, 63) && echoed(&c, true, g_msg, 64), "a hundred thousand bytes, and messages on each side of the size compression begins at, go and come back", how);
    proven_ws_stream_set_compress(c.ws, false);
    PROVEN_TEST_ASSERT(echoed(&c, true, g_msg, 5000), "and one sent as it is, on the same connection", how);
    proven_ws_stream_set_compress(c.ws, true);
    proven_ws_stream_close(c.ws, 1000, sv(""));
    PROVEN_TEST_ASSERT(turn_until(&c.closes, 1, 5000) && c.code == 1000, "closed", how);
    PROVEN_TEST_ASSERT(dial(&c, tls, at.port, "/plain", zip) == PROVEN_OK && turn_until(&c.opens, 1, 5000) && !c.compressed_at_open && echoed(&c, true, g_msg, 20000),
        "offered to a server that does not: not agreed, and messages go as they are", how);
    proven_ws_stream_abort(c.ws);
    PROVEN_TEST_ASSERT(c.closes == 1 && c.code == 1006 && c.why == PROVEN_ERR_RESET, "aborted: the end is reported inside the call, once, as 1006 and PROVEN_ERR_RESET", how);

    /* A subprotocol, a field of the caller's, a message sent from inside on_open. */
    proven_ws_event_connect_options_t sub = { .protocols = sv("chat, superchat") };
    PROVEN_TEST_ASSERT(dial(&c, tls, at.port, "/sub", sub) == PROVEN_OK && turn_until(&c.opens, 1, 5000) && strcmp(c.protocol, "superchat") == 0, "two subprotocols offered: on_open is given the one the server chose", how);
    proven_ws_stream_abort(c.ws);
    proven_http_header_t mine[1] = { { PROVEN_LIT("Origin"), PROVEN_LIT("https://example.org") } };
    proven_ws_event_connect_options_t with = { .headers = mine, .header_count = 1 };
    PROVEN_TEST_ASSERT(dial(&c, tls, at.port, "/origin", with) == PROVEN_OK && turn_until(&c.messages, 1, 5000) && c.got_len == 19 && memcmp(c.got, "https://example.org", 19) == 0,
        "a field of the caller's reaches the server with the request", how);
    proven_ws_stream_abort(c.ws);
    c.say_in_open = "first words";
    PROVEN_TEST_ASSERT(dial(&c, tls, at.port, "/echo", plain) == PROVEN_OK && turn_until(&c.messages, 1, 5000) && c.sent_in_open == PROVEN_OK && c.got_len == 11 && memcmp(c.got, "first words", 11) == 0,
        "a message sent from inside on_open is taken, and answered", how);
    c.say_in_open = NULL;
    proven_ws_stream_abort(c.ws);

    /* The server closes; the server pushes and this side pauses in the middle. */
    PROVEN_TEST_ASSERT(dial(&c, tls, at.port, "/close", plain) == PROVEN_OK && turn_until(&c.closes, 1, 5000) && c.closes == 1 && c.code == 4001 && c.why == PROVEN_OK && c.opens == 1,
        "a server that closes at once with 4001: on_open, then on_closed with that code", how);
    c.pause_at = 7;
    PROVEN_TEST_ASSERT(dial(&c, tls, at.port, "/push", plain) == PROVEN_OK && turn_until(&c.messages, 50, 8000) && c.messages == 50 && c.out_of_order == 0 && c.while_paused == 0,
        "fifty messages pushed by the server, this side pausing at the seventh for 60 ms: all fifty in order, none while paused", "Check wsc_tr_pause and wsc_deliver in src/proven/ws_event_client.c: what arrived meanwhile is kept and delivered first.");
    c.pause_at = 0;
    proven_ws_stream_abort(c.ws);

    /* A server that pings: this side answers by itself, and the connection outlives the silence. */
    PROVEN_TEST_ASSERT(dial(&c, tls, at.port, "/pings", plain) == PROVEN_OK && turn_until(&c.opens, 1, 5000), "a connection to a server that pings every 40 ms and gives up after 150 more", how);
    turn(500 * SLOW);
    const int srv_messages_before = g_srv_messages;
    PROVEN_TEST_ASSERT(c.closes == 0 && proven_ws_stream_send(c.ws, true, mv("still here")) == PROVEN_OK && turn_until(&g_srv_messages, srv_messages_before + 1, 5000) && c.closes == 0,
        "half a second of silence later it is still open, and a message still reaches the server: its pings were answered", "Check ws_on_input in src/proven/ws_event.c: a ping is answered with a pong whichever side this is.");
    proven_ws_stream_abort(c.ws);

    /* An answer that is not 101. */
    PROVEN_TEST_ASSERT(dial(&c, tls, at.port, "/page", plain) == PROVEN_OK && turn_until(&c.closes, 1, 5000) && c.closes == 1 && c.opens == 0 && c.code == 0 && c.why == PROVEN_ERR_REFUSED,
        "a server that answers 200: no on_open, on_closed once with code 0 and PROVEN_ERR_REFUSED", how);
    turn(50);
    proven_http_event_server_destroy(server);
    turn(20);
}

int main(void) {
    PROVEN_TEST_SUITE("a WebSocket client on the loop",
        "proven_ws_event_connect against this library's event-driven server on the same loop - messages both ways at many sizes, compression agreed and not, subprotocols, pause, pings, closes from either side - plain and over TLS, and every way a connection can fail to become a WebSocket, each ending in exactly one on_closed.",
        "Inspect src/proven/ws_event_client.c - wsc_on_io and wsc_readable for the states, wsc_answer for the upgrade, the wsc_tr_ functions for what the protocol asks of the connection - and, in src/proven/ws_event.c, proven_ws_event_client_new_ and ws_frame for a client's masked frames.");

    g_heap = proven_heap_allocator();
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
    PROVEN_TEST_ASSERT(tp_build(), "the test's certificates and keys are issued", "");
    proven_cert_store_t *anchors = NULL;
    PROVEN_TEST_ASSERT(proven_cert_store_create(g_heap, &anchors) == PROVEN_OK &&
                       proven_cert_store_add_pem(anchors, (proven_mem_view_t){ (const proven_byte_t *)TP_CA, strlen(TP_CA) }, NULL) == PROVEN_OK, "anchors", "");
    proven_tls_options_t co = { .alloc = g_heap, .anchors = anchors };
    proven_tls_options_t so = { .alloc = g_heap, .certificate_pem = { (const proven_byte_t *)TP_SERVER_ED, strlen(TP_SERVER_ED) }, .private_key_pem = { (const proven_byte_t *)TP_SERVER_ED_KEY, strlen(TP_SERVER_ED_KEY) } };
    PROVEN_TEST_ASSERT(proven_tls_config_create(&co, &g_client_tls) == PROVEN_OK && proven_tls_config_create(&so, &g_server_tls) == PROVEN_OK, "TLS configurations", "");
    PROVEN_TEST_ASSERT(proven_loop_create(g_heap, &g_loop) == PROVEN_OK, "a loop, turned by the test", "");
    {
        proven_u32 x = 2463534242u;
        for (proven_size_t i = 0; i < BIG; ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; g_msg[i] = (proven_byte_t)("the quick brown fox jumps over the lazy dog "[(x >> 9) % 44]); }
    }

    PROVEN_TEST_SECTION("what connect refuses", "Arguments that cannot begin a connection: nothing is begun and nothing is told.", "Check the first lines of proven_ws_event_connect.");
    {
        static cli_t c;
        proven_ws_stream_t *ws = (proven_ws_stream_t *)&c;
        proven_ws_event_connect_options_t ok = { .url = sv("ws://127.0.0.1:9/x"), .ws = { .on = { cli_message, NULL, cli_closed }, .ctx = &c } };
        proven_ws_event_connect_options_t o = ok;
        PROVEN_TEST_ASSERT(proven_ws_event_connect(NULL, &ok, &ws) == PROVEN_ERR_INVALID_ARG && ws == NULL && proven_ws_event_connect(g_loop, NULL, &ws) == PROVEN_ERR_INVALID_ARG &&
                           proven_ws_event_connect(g_loop, &ok, NULL) == PROVEN_ERR_INVALID_ARG, "no loop, no options, nowhere to put the connection", "");
        o.ws.on.on_message = NULL;
        PROVEN_TEST_ASSERT(proven_ws_event_connect(g_loop, &o, &ws) == PROVEN_ERR_INVALID_ARG, "no on_message", "");
        o = ok; o.url = sv("http://127.0.0.1:9/x");
        PROVEN_TEST_ASSERT(proven_ws_event_connect(g_loop, &o, &ws) == PROVEN_ERR_INVALID_ARG, "a URL that is not ws or wss", "");
        o = ok; o.url = sv("/x");
        PROVEN_TEST_ASSERT(proven_ws_event_connect(g_loop, &o, &ws) == PROVEN_ERR_INVALID_ARG, "a URL that is not absolute", "");
        o = ok; o.url = sv("ws://user:secret@127.0.0.1:9/x");
        PROVEN_TEST_ASSERT(proven_ws_event_connect(g_loop, &o, &ws) == PROVEN_ERR_INVALID_ARG, "a URL that carries credentials", "");
        o = ok; o.url = sv("wss://127.0.0.1:9/x");
        PROVEN_TEST_ASSERT(proven_ws_event_connect(g_loop, &o, &ws) == PROVEN_ERR_INVALID_ARG, "wss with no TLS configuration: refused, never connected in the clear", "");
        o = ok; o.url = sv("ws://example.org/x");
        PROVEN_TEST_ASSERT(proven_ws_event_connect(g_loop, &o, &ws) == PROVEN_ERR_INVALID_ARG, "a host that is a name, with no address given", "");
        proven_http_header_t theirs[1] = { { PROVEN_LIT("sec-websocket-key"), PROVEN_LIT("x") } };
        o = ok; o.headers = theirs; o.header_count = 1;
        PROVEN_TEST_ASSERT(proven_ws_event_connect(g_loop, &o, &ws) == PROVEN_ERR_INVALID_ARG, "a header the call writes itself", "");
        theirs[0] = (proven_http_header_t){ PROVEN_LIT("X-Bad"), PROVEN_LIT("a\r\nb") };
        PROVEN_TEST_ASSERT(proven_ws_event_connect(g_loop, &o, &ws) == PROVEN_ERR_INVALID_ARG, "a header value that would end its line", "");
        static char long_offer[300];
        memset(long_offer, 'a', 256);
        o = ok; o.protocols = (proven_u8str_view_t){ (const proven_byte_t *)long_offer, 256 };
        PROVEN_TEST_ASSERT(proven_ws_event_connect(g_loop, &o, &ws) == PROVEN_ERR_INVALID_ARG, "an offer of 256 bytes", "");
        o = ok; o.ws.subprotocol = sv("chat");
        PROVEN_TEST_ASSERT(proven_ws_event_connect(g_loop, &o, &ws) == PROVEN_ERR_INVALID_ARG, "a subprotocol in the part of the options that is a server's: the offer goes in `protocols`", "");
        o = ok; o.ws.compress = true; o.ws.compress_level = 10;
        PROVEN_TEST_ASSERT(proven_ws_event_connect(g_loop, &o, &ws) == PROVEN_ERR_INVALID_ARG, "a compression level out of range", "");
        turn(20);
        PROVEN_TEST_ASSERT(c.closes == 0 && c.opens == 0, "and for none of them was anything told", "");
    }

    PROVEN_TEST_SECTION("in the clear", "Every case over an unencrypted connection.", "");
    cases(false);
    PROVEN_TEST_SECTION("over TLS", "The same cases with the TLS engine between the socket and the frames.", "Check the TLS branches of wsc_readable and wsc_flush.");
    cases(true);

    PROVEN_TEST_SECTION("connections that do not become WebSockets", "Nothing listening, a server that says nothing, that hangs up, that answers something else, that grants what was not asked: each ends in one on_closed with code 0 and the reason.", "Check wsc_gone and wsc_answer in src/proven/ws_event_client.c.");
    {
        static cli_t c;
        proven_net_listener_t l;
        proven_net_addr_t at;
        proven_net_conn_t x;
        proven_ws_event_connect_options_t plain = { 0 }, quick = { .connect_timeout_ms = 200 * SLOW };
        PROVEN_TEST_ASSERT(proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 8, &l, &at) == PROVEN_OK, "a listener the test answers", "");
        /* Nothing listens: a port that was just closed. */
        {
            proven_net_listener_t gone;
            proven_net_addr_t was;
            PROVEN_TEST_ASSERT(proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 1, &gone, &was) == PROVEN_OK, "a port", "");
            (void)proven_net_listener_close(&gone);
            proven_err_t e = dial(&c, false, was.port, "/x", plain);
            PROVEN_TEST_ASSERT(e == PROVEN_ERR_REFUSED || (e == PROVEN_OK && turn_until(&c.closes, 1, 5000) && c.closes == 1 && c.opens == 0 && c.code == 0 && c.why == PROVEN_ERR_REFUSED),
                "nothing listening: PROVEN_ERR_REFUSED, from the call or - once - from on_closed", "");
        }
        proven_time_t t0 = proven_time_monotonic_now();
        PROVEN_TEST_ASSERT(dial(&c, false, at.port, "/x", quick) == PROVEN_OK && scripted(&l, &x, NULL, false) && turn_until(&c.closes, 1, 5000) && c.closes == 1 && c.opens == 0 && c.code == 0 && c.why == PROVEN_ERR_TIMEOUT &&
                           proven_time_monotonic_now() - t0 > 150000000,
            "a server that takes the request and says nothing: PROVEN_ERR_TIMEOUT when connect_timeout_ms is over", "");
        (void)proven_net_close(&x);
        PROVEN_TEST_ASSERT(dial(&c, false, at.port, "/x", plain) == PROVEN_OK && scripted(&l, &x, NULL, true) && turn_until(&c.closes, 1, 5000) && c.closes == 1 && c.opens == 0 && c.code == 0 && c.why == PROVEN_ERR_RESET,
            "one that hangs up instead of answering: PROVEN_ERR_RESET", "");
        PROVEN_TEST_ASSERT(dial(&c, false, at.port, "/x", plain) == PROVEN_OK && scripted(&l, &x, "SSH-2.0-not-http\r\n\r\n", false) && turn_until(&c.closes, 1, 5000) && c.opens == 0 && c.code == 0 && c.why == PROVEN_ERR_INVALID_FORMAT,
            "one that answers in another protocol: PROVEN_ERR_INVALID_FORMAT", "");
        (void)proven_net_close(&x);
        PROVEN_TEST_ASSERT(dial(&c, false, at.port, "/x", plain) == PROVEN_OK && scripted(&l, &x, "HTTP/1.1 302 Found\r\nLocation: ws://127.0.0.1:1/y\r\nContent-Length: 0\r\n\r\n", false) &&
                           turn_until(&c.closes, 1, 5000) && c.opens == 0 && c.why == PROVEN_ERR_REFUSED, "a redirect is not followed: PROVEN_ERR_REFUSED", "");
        (void)proven_net_close(&x);
        PROVEN_TEST_ASSERT(dial(&c, false, at.port, "/x", plain) == PROVEN_OK &&
                           scripted(&l, &x, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n", false) &&
                           turn_until(&c.closes, 1, 5000) && c.opens == 0 && c.why == PROVEN_ERR_INVALID_FORMAT, "a 101 whose Sec-WebSocket-Accept answers another key: PROVEN_ERR_INVALID_FORMAT", "");
        (void)proven_net_close(&x);
        /* A server that grants the upgrade and is then never heard from: this side's own
         * timer finds out - a ping after the silence, and no answer to it; a close, and no
         * answer to that. */
        proven_ws_event_connect_options_t watchful = { .ws = { .ping_interval_ms = 40 * SLOW, .pong_timeout_ms = 100 * SLOW, .close_timeout_ms = 100 * SLOW } };
        t0 = proven_time_monotonic_now();
        PROVEN_TEST_ASSERT(dial(&c, false, at.port, "/x", watchful) == PROVEN_OK && granted_then_silent(&l, &x) && turn_until(&c.opens, 1, 5000) && turn_until(&c.closes, 1, 5000) &&
                           c.opens == 1 && c.closes == 1 && c.code == 1006 && c.why == PROVEN_ERR_TIMEOUT && proven_time_monotonic_now() - t0 > (proven_time_t)SLOW * 120000000,
            "a server that goes silent after granting: this side pings, is not answered, and ends the connection with 1006 and PROVEN_ERR_TIMEOUT",
            "Check wsc_on_timer in src/proven/ws_event_client.c: once the connection is open the timer is the protocol's (ws_on_timer).");
        (void)proven_net_close(&x);
        PROVEN_TEST_ASSERT(dial(&c, false, at.port, "/x", watchful) == PROVEN_OK && granted_then_silent(&l, &x) && turn_until(&c.opens, 1, 5000), "another such connection, open", "");
        proven_ws_stream_close(c.ws, 1000, sv("anyone?"));
        PROVEN_TEST_ASSERT(c.closes == 0 && turn_until(&c.closes, 1, 5000) && c.closes == 1 && c.code == 1000 && c.why == PROVEN_ERR_TIMEOUT,
            "a close that is never answered: after close_timeout_ms the end is reported with this side's code and PROVEN_ERR_TIMEOUT", "");
        (void)proven_net_close(&x);
        /* Aborted, and closed, while it is still being made. */
        PROVEN_TEST_ASSERT(dial(&c, false, at.port, "/x", plain) == PROVEN_OK, "another attempt", "");
        proven_ws_stream_abort(c.ws);
        PROVEN_TEST_ASSERT(c.closes == 1 && c.opens == 0 && c.why == PROVEN_ERR_RESET, "aborted before it was open: ended inside the call, once", "");
        PROVEN_TEST_ASSERT(dial(&c, false, at.port, "/x", plain) == PROVEN_OK, "and another", "");
        proven_ws_stream_close(c.ws, 1000, sv("never mind"));
        PROVEN_TEST_ASSERT(c.closes == 1 && c.opens == 0 && c.why == PROVEN_ERR_RESET, "closed before it was open: there is nobody to say goodbye to, and it is given up the same way", "");
        turn(50);
        while (proven_net_accept(&l, PROVEN_NET_DONT_WAIT, &x, NULL) == PROVEN_OK) (void)proven_net_close(&x);
        (void)proven_net_listener_close(&l);
        free(c.got);
        c.got = NULL;
    }

    turn(50);
    proven_loop_destroy(g_loop);
    proven_tls_config_destroy(g_client_tls);
    proven_tls_config_destroy(g_server_tls);
    proven_cert_store_destroy(anchors);
    PROVEN_TEST_PASS("a WebSocket connection made on the loop opens, carries messages both ways, pauses, answers pings, closes from either side, and reports every failed attempt once.");
    return 0;
}
