#include "proven.h"
#include "proven_test.h"
#include "test_unit_tls_pki.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * permessage-deflate in the two WebSocket drivers, over loopback.
 *
 * Two servers answer the same paths: the blocking one (ws_conn over http_server, a job per
 * connection) and the event-driven one (ws_event, on a loop with a thread of its own). The
 * test's main thread is the clients: the library's WebSocket client where one that behaves is
 * wanted, and raw sockets - a handshake written by hand, frames built byte by byte - where one
 * that does not is, and to see what the servers put on the wire.
 *
 *   /echo   compression accepted, with context takeover (the default); every message comes back
 *   /fresh  compression accepted without takeover (`compress_no_takeover`); the same
 *   /plain  compression not accepted; the same
 *   /small  compression accepted, messages limited to 100,000 bytes
 *   /count  (event server) pauses after every piece and answers with what it saw
 *   /push   (event server) sends twenty thousand messages through a small output limit
 *
 * The bit, the negotiation and the transform themselves are test_unit_ws_deflate's business;
 * that Node's implementations agree with these drivers is checked by a private program.
 */

static proven_u8str_view_t sv(const char *s) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }

#define MIB ((proven_size_t)1024 * 1024)

static proven_allocator_t g_heap;
static proven_loop_t *g_loop;
static proven_http_server_t *g_blocking;
static proven_byte_t g_msg[MIB + 16];
static atomic_int g_event_closed, g_refused, g_writable, g_bad_config;

/* A message of `n` bytes: text with a two-byte character every so often (so that pieces cut
 * characters), or binary with stretches that do not compress. */
static void fill(proven_size_t n, bool text, unsigned seed) {
    proven_u32 x = 2463534242u + seed;
    for (proven_size_t i = 0; i < n; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        if (!text) g_msg[i] = (i / 3000) % 2 ? (proven_byte_t)(x >> 7) : (proven_byte_t)(i & 0xff);
        else if (i % 97 == 0 && i + 1 < n) { g_msg[i] = 0xc3; g_msg[++i] = 0xa9; }
        else g_msg[i] = (proven_byte_t)("a line of words said again \n"[(i + (x >> 29)) % 28]);
    }
}

// ---------------------------------------------------------------------------------------
// The blocking server
// ---------------------------------------------------------------------------------------

static void blocking_handler(void *ctx, proven_http_exchange_t *x) {
    (void)ctx;
    const proven_http_request_t *req = proven_http_exchange_request(x);
    char path[32] = { 0 };
    memcpy(path, req->target.ptr, req->target.size < sizeof path - 1 ? req->target.size : sizeof path - 1);
    proven_ws_conn_config_t cfg = { .alloc = g_heap, .max_message_bytes = 2 * MIB, .compress = strcmp(path, "/plain") != 0, .compress_window_bits = 12,
                                    .compress_no_takeover = strcmp(path, "/fresh") == 0 };
    if (strcmp(path, "/small") == 0) cfg.max_message_bytes = 100000;
    proven_ws_conn_t *c = NULL;
    if (proven_ws_conn_accept(x, sv(""), &cfg, &c) != PROVEN_OK) return;
    for (;;) {
        proven_ws_message_t m;
        if (proven_ws_conn_receive(c, proven_net_deadline_in(20000), &m) != PROVEN_OK) break;
        proven_err_t e;
        /* A message that begins with "3:" goes back in three parts. */
        if (m.data.size >= 2 && m.data.ptr[0] == '3' && m.data.ptr[1] == ':') {
            proven_size_t a = m.data.size / 3, b = 2 * m.data.size / 3;
            e = proven_ws_conn_send_part(c, m.text, (proven_mem_view_t){ m.data.ptr, a }, false);
            if (e == PROVEN_OK) e = proven_ws_conn_send_part(c, m.text, (proven_mem_view_t){ m.data.ptr + a, b - a }, false);
            if (e == PROVEN_OK) e = proven_ws_conn_send_part(c, m.text, (proven_mem_view_t){ m.data.ptr + b, m.data.size - b }, true);
        } else {
            /* A message that begins with "u:" goes back as it is, and only that one. */
            const bool as_it_is = m.data.size >= 2 && m.data.ptr[0] == 'u' && m.data.ptr[1] == ':';
            if (as_it_is) proven_ws_conn_set_compress(c, false);
            if (m.text) e = proven_ws_conn_send_text(c, (proven_u8str_view_t){ m.data.ptr, m.data.size });
            else e = proven_ws_conn_send_binary(c, m.data);
            if (as_it_is) proven_ws_conn_set_compress(c, true);
        }
        if (e != PROVEN_OK) break;
    }
    (void)proven_ws_conn_close(c, 1000, sv(""));
    proven_ws_conn_destroy(c);
}
static void blocking_serve(void *arg) { (void)arg; (void)proven_http_server_run(g_blocking); }

// ---------------------------------------------------------------------------------------
// The event-driven server
// ---------------------------------------------------------------------------------------

typedef struct {
    proven_ws_stream_t *ws;
    proven_loop_timer_t timer;
    char mode;                         /* 'e' echo, 'c' count, 'p' push */
    proven_byte_t *held;               /* echo: a piece the server would not take yet */
    proven_size_t held_len;
    bool held_text, held_first, held_last;
    /* count */
    unsigned pieces, firsts, lasts, sum;
    proven_size_t total, largest;
    bool paused_now, while_paused;
    /* push */
    int pushed;
} peer_t;

static void ev_closed(void *ctx, proven_ws_stream_t *ws, proven_u16 code, proven_err_t why) {
    (void)ctx; (void)code; (void)why;
    peer_t *p = proven_ws_stream_user(ws);
    if (p) { proven_loop_timer_cancel(g_loop, &p->timer); free(p->held); free(p); }
    atomic_fetch_add(&g_event_closed, 1);
}

static void count_resume(void *ctx) {
    peer_t *p = ctx;
    p->paused_now = false;
    proven_ws_stream_resume(p->ws);
}

static void push_pump(peer_t *p) {
    while (p->pushed < 20000) {
        proven_byte_t msg[1000];
        memset(msg, 'a' + p->pushed % 26, sizeof msg);
        msg[0] = (proven_byte_t)(p->pushed >> 8); msg[1] = (proven_byte_t)p->pushed;
        /* Bytes that do not compress fill most of each, so that the client's not reading is felt. */
        proven_u32 x = 2463534242u + (proven_u32)p->pushed;
        for (int i = 2; i < 800; ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; msg[i] = (proven_byte_t)(x >> 9); }
        proven_err_t e = proven_ws_stream_send(p->ws, false, (proven_mem_view_t){ .ptr = msg, .size = sizeof msg });
        if (e == PROVEN_ERR_AGAIN) { atomic_fetch_add(&g_refused, 1); return; }
        if (e != PROVEN_OK) return;
        p->pushed++;
    }
    proven_ws_stream_close(p->ws, 1000, sv("done"));
}

static void ev_message(void *ctx, proven_ws_stream_t *ws, bool text, proven_mem_view_t piece, bool first, bool last) {
    (void)ctx;
    peer_t *p = proven_ws_stream_user(ws);
    if (p->mode == 'c') {
        if (p->paused_now) p->while_paused = true;
        p->pieces++;
        if (first) p->firsts++;
        if (last) p->lasts++;
        if (piece.size > p->largest) p->largest = piece.size;
        for (proven_size_t i = 0; i < piece.size; ++i) p->sum = p->sum * 31 + piece.ptr[i];
        p->total += piece.size;
        if (last) {
            char answer[128];
            int n = snprintf(answer, sizeof answer, "%u %u %u %u %u %u %d", p->pieces, p->firsts, p->lasts, (unsigned)p->largest, (unsigned)p->total, p->sum, (int)p->while_paused);
            (void)proven_ws_stream_send(ws, true, (proven_mem_view_t){ .ptr = (const proven_byte_t *)answer, .size = (proven_size_t)n });
            p->pieces = p->firsts = p->lasts = p->sum = 0; p->total = p->largest = 0;
        } else if (p->pieces % 3 == 0) {
            /* Every third piece: stop and go on at once, from inside the delivery itself -
             * which must neither lose a piece nor deliver one twice. */
            proven_ws_stream_pause(ws);
            proven_ws_stream_resume(ws);
        } else {
            /* Otherwise stop, and go on from a timer. The rest of the message waits. */
            p->paused_now = true;
            proven_ws_stream_pause(ws);
            proven_loop_timer_set(g_loop, &p->timer, 1, count_resume, p);
        }
        return;
    }
    /* Echo each piece as it arrives; one the server refuses is kept until there is room. A
     * message that begins with "u:" goes back as it is, and only that one: the switch is read
     * when a message begins, so it can be put back at once. */
    const bool as_it_is = first && piece.size >= 2 && piece.ptr[0] == 'u' && piece.ptr[1] == ':';
    if (as_it_is) proven_ws_stream_set_compress(ws, false);
    proven_err_t e = proven_ws_stream_send_piece(ws, text, piece, first, last);
    if (as_it_is && e == PROVEN_OK) proven_ws_stream_set_compress(ws, true);
    if (e != PROVEN_ERR_AGAIN) return;
    atomic_fetch_add(&g_refused, 1);
    p->held = malloc(piece.size ? piece.size : 1);
    if (!p->held) { proven_ws_stream_abort(ws); return; }
    if (piece.size) memcpy(p->held, piece.ptr, piece.size);
    p->held_len = piece.size; p->held_text = text; p->held_first = first; p->held_last = last;
    proven_ws_stream_pause(ws);
}

static void ev_writable(void *ctx, proven_ws_stream_t *ws) {
    (void)ctx;
    peer_t *p = proven_ws_stream_user(ws);
    atomic_fetch_add(&g_writable, 1);
    if (p->mode == 'p') { push_pump(p); return; }
    if (p->held) {
        if (proven_ws_stream_send_piece(ws, p->held_text, (proven_mem_view_t){ .ptr = p->held, .size = p->held_len }, p->held_first, p->held_last) != PROVEN_OK) return;
        free(p->held); p->held = NULL;
        proven_ws_stream_set_compress(ws, true);
        proven_ws_stream_resume(ws);
    }
}

static void ev_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    (void)ctx;
    char path[32] = { 0 };
    memcpy(path, head->target.ptr, head->target.size < sizeof path - 1 ? head->target.size : sizeof path - 1);
    proven_ws_event_config_t cfg = { .on = { ev_message, ev_writable, ev_closed }, .max_message_bytes = 2 * MIB, .max_buffered_output = 16 * 1024,
                                     .compress = strcmp(path, "/plain") != 0, .compress_window_bits = 12, .compress_no_takeover = strcmp(path, "/fresh") == 0 };
    if (strcmp(path, "/small") == 0) cfg.max_message_bytes = 100000;
    proven_ws_stream_t *ws = NULL;
    if (strcmp(path, "/config") == 0) {
        /* Settings out of range are refused with nothing sent; the request is answered below. */
        int bad = 0;
        proven_ws_event_config_t c2 = cfg;
        c2.compress_level = 10;
        if (proven_ws_event_accept(stream, head, &c2, &ws) == PROVEN_ERR_INVALID_ARG && ws == NULL) bad |= 1;
        c2 = cfg; c2.compress_level = -2;
        if (proven_ws_event_accept(stream, head, &c2, &ws) == PROVEN_ERR_INVALID_ARG && ws == NULL) bad |= 2;
        c2 = cfg; c2.compress_window_bits = 8;
        if (proven_ws_event_accept(stream, head, &c2, &ws) == PROVEN_ERR_INVALID_ARG && ws == NULL) bad |= 4;
        c2 = cfg; c2.compress_window_bits = 16;
        if (proven_ws_event_accept(stream, head, &c2, &ws) == PROVEN_ERR_INVALID_ARG && ws == NULL) bad |= 8;
        atomic_store(&g_bad_config, bad);
    }
    peer_t *p = calloc(1, sizeof *p);
    if (!p || proven_ws_event_accept(stream, head, &cfg, &ws) != PROVEN_OK) {
        free(p);
        (void)proven_http_stream_respond(stream, 400, NULL, 0, (proven_mem_view_t){ 0 });
        return;
    }
    p->ws = ws;
    p->mode = strcmp(path, "/count") == 0 ? 'c' : strcmp(path, "/push") == 0 ? 'p' : 'e';
    proven_ws_stream_set_user(ws, p);
    if (p->mode == 'p') push_pump(p);
}

static void run_loop(void *arg) { (void)arg; (void)proven_loop_run(g_loop); }

// ---------------------------------------------------------------------------------------
// The clients
// ---------------------------------------------------------------------------------------

static proven_http_client_t *g_http;

/* A second event-driven server, made on the loop's thread while the loop runs. */
static atomic_bool g_made;
static proven_http_event_server_config_t g_made_on;
static proven_http_event_server_t **g_made_out, *g_tls_to_end;
static proven_net_addr_t *g_made_at;
static void make_event_server(void *ctx) {
    (void)ctx;
    if (proven_http_event_server_create(g_loop, &g_made_on, g_made_out) != PROVEN_OK ||
        proven_http_event_server_listen(*g_made_out, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), g_made_at) != PROVEN_OK) *g_made_out = NULL;
    atomic_store(&g_made, true);
}
static proven_http_server_t *g_blocking_tls;
static proven_cert_store_t *anchors;                /* kept until the servers that use them are gone */
static proven_tls_config_t *server_tls, *client_tls;
static void blocking_tls_serve(void *arg) { (void)arg; (void)proven_http_server_run(g_blocking_tls); }

static proven_http_client_t *g_https;       /* the same, with TLS: for wss */
static bool g_wss;

static bool g_no_takeover;                  /* the client dials with compress_no_takeover */

static proven_ws_conn_t *dial(proven_u16 port, const char *path, bool compress) {
    char url[96];
    int n = snprintf(url, sizeof url, "%s://127.0.0.1:%u%s", g_wss ? "wss" : "ws", (unsigned)port, path);
    proven_ws_conn_config_t cfg = { .alloc = g_heap, .max_message_bytes = 2 * MIB, .compress = compress, .compress_no_takeover = g_no_takeover };
    proven_ws_conn_t *c = NULL;
    proven_u16 status = 0;
    proven_err_t e = proven_ws_conn_connect(g_wss ? g_https : g_http, (proven_u8str_view_t){ (const proven_byte_t *)url, (proven_size_t)n }, NULL, 0, sv(""), &cfg, &c, &status);
    PROVEN_TEST_ASSERT(e == PROVEN_OK && c != NULL, "the client connects", path);
    return c;
}

/* Send g_msg[0..n) - whole, or in three parts - and want it back unchanged. */
static bool echo_ok(proven_ws_conn_t *c, proven_size_t n, bool text, bool parts) {
    proven_err_t e;
    if (parts) {
        proven_size_t a = n / 3, b = 2 * n / 3;
        e = proven_ws_conn_send_part(c, text, (proven_mem_view_t){ g_msg, a }, false);
        if (e == PROVEN_OK) e = proven_ws_conn_send_part(c, text, (proven_mem_view_t){ g_msg + a, b - a }, false);
        if (e == PROVEN_OK) e = proven_ws_conn_send_part(c, text, (proven_mem_view_t){ g_msg + b, n - b }, true);
    } else if (text) e = proven_ws_conn_send_text(c, (proven_u8str_view_t){ g_msg, n });
    else e = proven_ws_conn_send_binary(c, (proven_mem_view_t){ g_msg, n });
    if (e != PROVEN_OK) return false;
    proven_ws_message_t m;
    e = proven_ws_conn_receive(c, proven_net_deadline_in(30000), &m);
    return e == PROVEN_OK && m.text == text && m.data.size == n && (n == 0 || memcmp(m.data.ptr, g_msg, n) == 0);
}

static void hang_up(proven_ws_conn_t *c) {
    PROVEN_TEST_ASSERT(proven_ws_conn_close(c, 1000, sv("")) == PROVEN_OK, "the close handshake completes", "");
    proven_ws_conn_destroy(c);
}

/* A raw client: the handshake by hand, with or without the offer. `head` receives the answer. */
#define KEY "dGhlIHNhbXBsZSBub25jZQ=="
static bool raw_upgrade(proven_net_conn_t *c, proven_u16 port, const char *path, const char *extensions, char *head, proven_size_t cap) {
    char req[512];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " KEY "\r\nSec-WebSocket-Version: 13\r\n%s%s%s\r\n",
                     path, extensions ? "Sec-WebSocket-Extensions: " : "", extensions ? extensions : "", extensions ? "\r\n" : "");
    if (proven_net_connect(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, port), proven_net_deadline_in(5000), c) != PROVEN_OK) return false;
    if (proven_net_write_all(c, (proven_mem_view_t){ (const proven_byte_t *)req, (proven_size_t)n }, proven_net_deadline_in(5000)).err != PROVEN_OK) return false;
    proven_size_t have = 0;
    head[0] = '\0';
    while (have < cap - 1 && !strstr(head, "\r\n\r\n")) {
        proven_result_size_t g = proven_net_read(c, (proven_mem_mut_t){ .ptr = (proven_byte_t *)head + have, .size = 1 }, proven_net_deadline_in(5000));
        if (g.err != PROVEN_OK) return false;
        have += g.value;
        head[have] = '\0';
    }
    return strncmp(head, "HTTP/1.1 101", 12) == 0;
}

/* One frame from this raw client: masked, as a client's must be, with a key of four zeros -
 * which leaves the payload as it is. */
static bool raw_frame(proven_net_conn_t *c, proven_byte_t b0, const proven_byte_t *payload, proven_size_t n) {
    proven_byte_t h[8];
    proven_size_t hl = 0;
    h[hl++] = b0;
    if (n < 126) h[hl++] = (proven_byte_t)(0x80 | n);
    else { h[hl++] = 0x80 | 126; h[hl++] = (proven_byte_t)(n >> 8); h[hl++] = (proven_byte_t)n; }
    memset(h + hl, 0, 4);
    hl += 4;
    return proven_net_write_all(c, (proven_mem_view_t){ h, hl }, proven_net_deadline_in(5000)).err == PROVEN_OK &&
           (n == 0 || proven_net_write_all(c, (proven_mem_view_t){ payload, n }, proven_net_deadline_in(5000)).err == PROVEN_OK);
}

static bool raw_exact(proven_net_conn_t *c, proven_byte_t *out, proven_size_t n) {
    proven_size_t have = 0;
    while (have < n) {
        proven_result_size_t g = proven_net_read(c, (proven_mem_mut_t){ .ptr = out + have, .size = n - have }, proven_net_deadline_in(5000));
        if (g.err != PROVEN_OK) return false;
        have += g.value;
    }
    return true;
}

/* The next frame the server sends: its first byte, and its payload. */
static bool raw_next(proven_net_conn_t *c, proven_byte_t *b0, proven_byte_t *payload, proven_size_t cap, proven_size_t *n) {
    proven_byte_t h[2], ext[2];
    if (!raw_exact(c, h, 2)) return false;
    *b0 = h[0];
    *n = h[1] & 0x7f;
    if ((h[1] & 0x80) != 0) return false;              /* a server never masks */
    if (*n == 126) { if (!raw_exact(c, ext, 2)) return false; *n = ((proven_size_t)ext[0] << 8) | ext[1]; }
    else if (*n == 127) return false;
    return *n <= cap && raw_exact(c, payload, *n);
}

/* The close code the server ends with after `b0`+payload was sent to `path`. 0 when none came. */
static unsigned raw_verdict(proven_u16 port, const char *path, const char *extensions, proven_byte_t b0, const proven_byte_t *payload, proven_size_t n) {
    proven_net_conn_t c;
    char head[600];
    proven_byte_t first = 0, got[256];
    proven_size_t len = 0;
    unsigned code = 0;
    if (raw_upgrade(&c, port, path, extensions, head, sizeof head) && raw_frame(&c, b0, payload, n) &&
        raw_next(&c, &first, got, sizeof got, &len) && first == 0x88 && len >= 2) code = ((unsigned)got[0] << 8) | got[1];
    (void)proven_net_close(&c);
    return code;
}

#define OFFER "permessage-deflate; client_max_window_bits"

int main(void) {
    PROVEN_TEST_SUITE("ws: compressed messages in the drivers",
        "permessage-deflate over loopback: negotiated or not, messages of every size both ways through both servers, the limit on what a message may inflate to, pause in the middle of one, and what a peer that breaks the rules is told.",
        "Inspect proven_ws_conn_send_part, wc_message_inflate and the handshakes in src/proven/ws_conn.c; ws_rx_run, ws_rx_hold, proven_ws_stream_send_piece and proven_ws_stream_resume in src/proven/ws_event.c.");

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

    proven_job_sys_t *threads = NULL, *workers = NULL;
    PROVEN_TEST_ASSERT(proven_job_system_init(g_heap, 2, 4, &threads) == PROVEN_OK && proven_job_system_init(g_heap, 4, 16, &workers) == PROVEN_OK, "threads for the two servers", "");
    proven_http_server_config_t bcfg = { .alloc = g_heap, .handler = blocking_handler, .jobs = workers };
    proven_net_addr_t bat, eat;
    PROVEN_TEST_ASSERT(proven_http_server_create(&bcfg, &g_blocking) == PROVEN_OK &&
                       proven_http_server_listen(g_blocking, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &bat) == PROVEN_OK, "the blocking server listens", "");
    PROVEN_TEST_ASSERT(proven_loop_create(g_heap, &g_loop) == PROVEN_OK, "a loop", "");
    proven_http_event_server_config_t ecfg = { .on = { .on_request = ev_request } };
    proven_http_event_server_t *eserver = NULL;
    PROVEN_TEST_ASSERT(proven_http_event_server_create(g_loop, &ecfg, &eserver) == PROVEN_OK &&
                       proven_http_event_server_listen(eserver, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &eat) == PROVEN_OK, "the event-driven server listens", "");
    proven_job_group_t running;
    proven_job_group_init(&running);
    PROVEN_TEST_ASSERT(proven_job_group_submit(threads, &running, blocking_serve, NULL) == PROVEN_OK &&
                       proven_job_group_submit(threads, &running, run_loop, NULL) == PROVEN_OK, "both are started", "");
    proven_http_client_config_t hcfg = { .alloc = g_heap };
    PROVEN_TEST_ASSERT(proven_http_client_create(&hcfg, &g_http) == PROVEN_OK, "an HTTP client for the handshakes", "");

    PROVEN_TEST_SECTION("settings", "What is refused before anything is sent.", "Check wc_config_ok and proven_ws_event_accept.");
    {
        proven_net_conn_t a, b;
        proven_net_listener_t l;
        proven_net_addr_t at;
        PROVEN_TEST_ASSERT(proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 1, &l, &at) == PROVEN_OK &&
                           proven_net_connect(at, proven_net_deadline_in(5000), &a) == PROVEN_OK &&
                           proven_net_accept(&l, proven_net_deadline_in(5000), &b, NULL) == PROVEN_OK, "a pair of sockets", "");
        static const struct { int level, bits; } bad[] = { { 10, 0 }, { -2, 0 }, { 0, 8 }, { 0, 16 }, { 0, -1 } };
        for (proven_size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
            proven_ws_conn_config_t cfg = { .alloc = g_heap, .compress = true, .compress_level = bad[i].level, .compress_window_bits = bad[i].bits };
            proven_ws_conn_t *c = (proven_ws_conn_t *)1;
            PROVEN_TEST_ASSERT(proven_ws_conn_open(proven_net_conn_transport(&a), false, (proven_mem_view_t){ 0 }, &cfg, &c) == PROVEN_ERR_INVALID_ARG && c == NULL,
                "a compression level or window outside its range is PROVEN_ERR_INVALID_ARG", "");
        }
        /* proven_ws_conn_open negotiates nothing: both ends take over unless told not to. Two
         * ends that agree read each other; a sender that takes over and a receiver told
         * compress_no_takeover (as every receiver before v0.80.0 was) do not, from the second
         * message on. */
        for (int k = 0; k < 2; ++k) {
            proven_ws_conn_config_t sc = { .alloc = g_heap, .compress = true }, rc = sc;
            rc.compress_no_takeover = k == 1;
            proven_ws_conn_t *tx = NULL, *rx = NULL;
            PROVEN_TEST_ASSERT(proven_ws_conn_open(proven_net_conn_transport(&a), false, (proven_mem_view_t){ 0 }, &sc, &tx) == PROVEN_OK &&
                               proven_ws_conn_open(proven_net_conn_transport(&b), true, (proven_mem_view_t){ 0 }, &rc, &rx) == PROVEN_OK, "two ends opened over a pair of sockets", "");
            static const char line[] = "the same line of text, said once and said again";
            proven_err_t got = PROVEN_OK;
            int read = 0;
            for (int i = 0; i < 3 && got == PROVEN_OK; ++i) {
                PROVEN_TEST_ASSERT(proven_ws_conn_send_text(tx, (proven_u8str_view_t){ (const proven_byte_t *)line, sizeof line - 1 }) == PROVEN_OK, "a message is sent", "");
                proven_ws_message_t m;
                got = proven_ws_conn_receive(rx, proven_net_deadline_in(5000), &m);
                if (got == PROVEN_OK && m.data.size == sizeof line - 1 && memcmp(m.data.ptr, line, m.data.size) == 0) read++;
            }
            if (k == 0) PROVEN_TEST_ASSERT(read == 3, "both ends with takeover: every message is read", "");
            else PROVEN_TEST_ASSERT(read == 1 && got == PROVEN_ERR_INVALID_ENCODING && proven_ws_conn_close_code(rx) == 0,
                "a sender that takes over, a receiver told not to: the first message is read and the second is not", "The CHANGELOG names this: ends before v0.80.0 need compress_no_takeover on the other end.");
            /* The connections own the transports: they are made again for the second round. */
            proven_ws_conn_destroy(tx); proven_ws_conn_destroy(rx);
            if (k == 0) {
                PROVEN_TEST_ASSERT(proven_net_connect(at, proven_net_deadline_in(5000), &a) == PROVEN_OK &&
                                   proven_net_accept(&l, proven_net_deadline_in(5000), &b, NULL) == PROVEN_OK, "a second pair of sockets", "");
            }
        }
        (void)proven_net_listener_close(&l);
        proven_ws_conn_t *c = dial(eat.port, "/config", true);
        PROVEN_TEST_ASSERT(atomic_load(&g_bad_config) == 15, "and from proven_ws_event_accept too, four ways, with the request still answerable", "");
        hang_up(c);
        PROVEN_TEST_ASSERT(!proven_ws_conn_compressed(NULL) && !proven_ws_stream_compressed(NULL), "the two questions accept null", "");
    }

    for (int which = 0; which < 2; ++which) {
        proven_u16 port = which ? eat.port : bat.port;
        const char *server = which ? "the event-driven server" : "the blocking server";
        PROVEN_TEST_SECTION(which ? "the event-driven server" : "the blocking server", "Negotiated or not, and messages both ways.", "");

        static const struct { const char *path; bool client; bool on; } pairs[] = {
            { "/echo", true, true }, { "/echo", false, false }, { "/plain", true, false }, { "/plain", false, false },
        };
        for (proven_size_t i = 0; i < 4; ++i) {
            proven_ws_conn_t *c = dial(port, pairs[i].path, pairs[i].client);
            PROVEN_TEST_ASSERT(proven_ws_conn_compressed(c) == pairs[i].on, "compression is on exactly when the client offered it and the server took it", server);
            fill(5000, true, 1);
            PROVEN_TEST_ASSERT(echo_ok(c, 5000, true, false), "and a message goes round either way", server);
            hang_up(c);
        }

        /* Takeover on one side only, either way: the side that keeps nothing says so, and
         * messages of every kind still go round, many times over. */
        for (int k = 0; k < 2; ++k) {
            g_no_takeover = k == 1;
            proven_ws_conn_t *c = dial(port, k ? "/echo" : "/fresh", true);
            PROVEN_TEST_ASSERT(proven_ws_conn_compressed(c), "compression is agreed with takeover on one side only", server);
            bool ok = true;
            for (int m = 0; m < 30 && ok; ++m) {
                fill(m % 3 == 0 ? 7 : m % 3 == 1 ? 200 : 9000, m % 2 == 0, (unsigned)(m % 4));
                ok = echo_ok(c, m % 3 == 0 ? 7 : m % 3 == 1 ? 200 : 9000, m % 2 == 0, m % 5 == 0);
            }
            PROVEN_TEST_ASSERT(ok, k ? "a client with compress_no_takeover and a server with takeover: thirty messages, repeated, round" :
                                       "a client with takeover and a server with compress_no_takeover: thirty messages, repeated, round", server);
            hang_up(c);
            g_no_takeover = false;
        }

        proven_ws_conn_t *c = dial(port, "/echo", true);
        static const proven_size_t sizes[] = { 0, 1, 2, 7, 8, 9, 63, 64, 65, 1000, 16384, 70000, MIB };
        for (proven_size_t i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
            /* The event-driven server here answers a message while it is still arriving, and
             * this client sends a whole message before it reads: with a megabyte, each end
             * waits for the other to read once the system's buffers are full - small ones,
             * on some systems. That is the echo's design, not the transform's; the megabyte
             * goes to this server through /count below. */
            if (which && sizes[i] > 70000) continue;
            for (int form = 0; form < 4; ++form) {
                fill(sizes[i], form % 2 == 0, (unsigned)(i * 4 + (proven_size_t)form));
                PROVEN_TEST_ASSERT(echo_ok(c, sizes[i], form % 2 == 0, form >= 2), "text and binary, whole and in three parts, come back unchanged at every size", server);
            }
        }
        if (!which) {
            /* The blocking server sends "3:..." back in three parts: a compressed message in fragments. */
            fill(60000, true, 77);
            g_msg[0] = '3'; g_msg[1] = ':';
            PROVEN_TEST_ASSERT(echo_ok(c, 60000, true, false), "a compressed message that the server sends in three parts arrives as one", server);
        }
        hang_up(c);

        /* What a message may inflate to. */
        c = dial(port, "/small", true);
        memset(g_msg, 'a', 200000);
        proven_ws_message_t m;
        PROVEN_TEST_ASSERT(proven_ws_conn_send_binary(c, (proven_mem_view_t){ g_msg, 100000 }) == PROVEN_OK &&
                           proven_ws_conn_receive(c, proven_net_deadline_in(10000), &m) == PROVEN_OK && m.data.size == 100000, "a message of exactly the limit, a few hundred bytes on the wire, is taken", server);
        PROVEN_TEST_ASSERT(proven_ws_conn_send_binary(c, (proven_mem_view_t){ g_msg, 200000 }) == PROVEN_OK, "one that inflates to twice the limit is sent", server);
        PROVEN_TEST_ASSERT(proven_ws_conn_receive(c, proven_net_deadline_in(10000), &m) == PROVEN_ERR_EOF && proven_ws_conn_close_code(c) == 1009,
            "and the server ends the connection with 1009: the limit is on what comes out", "Check wc_message_room and ws_rx_run: max_message_bytes counts decoded bytes.");
        proven_ws_conn_destroy(c);

        /* Raw frames: what the server puts on the wire, and what it says to a peer that breaks the rules. */
        {
            proven_net_conn_t r;
            char head[600];
            proven_byte_t b0 = 0, payload[20000];
            proven_size_t n = 0;
            PROVEN_TEST_ASSERT(raw_upgrade(&r, port, "/echo", OFFER, head, sizeof head), "a handshake by hand, offering the extension", server);
            PROVEN_TEST_ASSERT(strstr(head, "Sec-WebSocket-Extensions: permessage-deflate\r\n") != NULL,
                "is answered with no parameter: each side may keep its window", server);
            /* RFC 7692, 7.2.3.1: "Hello", compressed by somebody else. */
            static const proven_byte_t hello[] = { 0xf2, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00 };
            PROVEN_TEST_ASSERT(raw_frame(&r, 0xc1, hello, sizeof hello) && raw_next(&r, &b0, payload, sizeof payload, &n) && b0 == 0x81 && n == 5 && memcmp(payload, "Hello", 5) == 0,
                "the RFC's compressed Hello is read, and comes back uncompressed: five bytes are too few to gain", server);
            memset(g_msg, 'x', 10000);
            PROVEN_TEST_ASSERT(raw_frame(&r, 0x82, g_msg, 10000), "ten thousand bytes sent uncompressed, which the extension allows", server);
            proven_size_t total = 0;
            bool rsv1_once = true, fin = false;
            for (int frames = 0; !fin && frames < 64; ++frames) {
                PROVEN_TEST_ASSERT(raw_next(&r, &b0, payload, sizeof payload, &n), "a frame of the answer", server);
                if (frames == 0) rsv1_once = (b0 & 0x4f) == 0x42;            /* binary, RSV1 */
                else if ((b0 & 0x4f) != 0x00) rsv1_once = false;             /* continuations, without the bit */
                fin = (b0 & 0x80) != 0;
                total += n;
            }
            PROVEN_TEST_ASSERT(fin && rsv1_once && total < 200, "come back compressed: the bit on the first frame only, and under two hundred bytes in all", server);
            /* One message the server was told to send as it is, and the one after it. */
            g_msg[0] = 'u'; g_msg[1] = ':';
            total = 0; fin = false;
            bool no_bit = true;
            PROVEN_TEST_ASSERT(raw_frame(&r, 0x82, g_msg, 10000), "the same bytes, marked for the server to answer as they are", server);
            for (int frames = 0; !fin && frames < 64; ++frames) {
                PROVEN_TEST_ASSERT(raw_next(&r, &b0, payload, sizeof payload, &n), "a frame of the answer", server);
                if ((b0 & 0x40) != 0) no_bit = false;
                fin = (b0 & 0x80) != 0;
                total += n;
            }
            PROVEN_TEST_ASSERT(fin && no_bit && total == 10000, "come back uncompressed, on a connection that agreed to compression: proven_ws_conn_set_compress and proven_ws_stream_set_compress", server);
            g_msg[0] = 'x'; g_msg[1] = 'x';
            total = 0; fin = false;
            PROVEN_TEST_ASSERT(raw_frame(&r, 0x82, g_msg, 10000) && raw_next(&r, &b0, payload, sizeof payload, &n) && (b0 & 0x4f) == 0x42, "and the message after it is compressed again", server);
            (void)proven_net_close(&r);

            PROVEN_TEST_ASSERT(raw_upgrade(&r, port, "/echo", NULL, head, sizeof head) && strstr(head, "Sec-WebSocket-Extensions") == NULL, "with no offer, no extension is named", server);
            PROVEN_TEST_ASSERT(raw_frame(&r, 0x82, g_msg, 10000) && raw_next(&r, &b0, payload, sizeof payload, &n) && b0 == 0x82 && n == 10000, "and the same message comes back as it is", server);
            (void)proven_net_close(&r);
            PROVEN_TEST_ASSERT(raw_upgrade(&r, port, "/echo", "permessage-deflate; server_max_window_bits=8", head, sizeof head) && strstr(head, "Sec-WebSocket-Extensions") == NULL,
                "an offer that cannot be met is passed over, and the connection goes on without", server);
            (void)proven_net_close(&r);
            PROVEN_TEST_ASSERT(raw_upgrade(&r, port, "/echo", "permessage-deflate; server_max_window_bits=10", head, sizeof head) && strstr(head, "; server_max_window_bits=10\r\n") != NULL,
                "a window that was asked for is echoed", server);
            (void)proven_net_close(&r);

            /* Without takeover, the answer as it always was. */
            PROVEN_TEST_ASSERT(raw_upgrade(&r, port, "/fresh", OFFER, head, sizeof head) &&
                               strstr(head, "Sec-WebSocket-Extensions: permessage-deflate; server_no_context_takeover; client_no_context_takeover\r\n") != NULL,
                "a server with compress_no_takeover answers with both no-takeover parameters", server);
            (void)proven_net_close(&r);
            PROVEN_TEST_ASSERT(raw_upgrade(&r, port, "/echo", "permessage-deflate; client_no_context_takeover", head, sizeof head) &&
                               strstr(head, "Sec-WebSocket-Extensions: permessage-deflate; client_no_context_takeover\r\n") != NULL,
                "and a server with takeover forbids it only where the offer asked", server);
            (void)proven_net_close(&r);

            /* Takeover from the client: RFC 7692 7.2.3.2, a second Hello that is a match into the
             * first; and 7.2.3.4, a first that ends in a final block. */
            {
                static const proven_byte_t again[] = { 0xf2, 0x00, 0x11, 0x00, 0x00 };
                static const proven_byte_t final_block[] = { 0xf3, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00, 0x00 };
                const proven_byte_t *firsts[2] = { hello, final_block };
                const proven_size_t first_sizes[2] = { sizeof hello, sizeof final_block };
                for (int k = 0; k < 2; ++k) {
                    PROVEN_TEST_ASSERT(raw_upgrade(&r, port, "/echo", OFFER, head, sizeof head), "a handshake by hand, with takeover agreed", server);
                    bool ok = raw_frame(&r, 0xc1, firsts[k], first_sizes[k]) && raw_next(&r, &b0, payload, sizeof payload, &n) && n == 5 && memcmp(payload, "Hello", 5) == 0;
                    for (int m = 0; m < 2 && ok; ++m) ok = raw_frame(&r, 0xc1, again, sizeof again) && raw_next(&r, &b0, payload, sizeof payload, &n) && n == 5 && memcmp(payload, "Hello", 5) == 0;
                    PROVEN_TEST_ASSERT(ok, k ? "a final block, then messages that reach back across it: the server's decompressor keeps its window"
                                             : "the RFC's shared window: the second Hello refers into the first, and the server reads it", server);
                    (void)proven_net_close(&r);
                }
                PROVEN_TEST_ASSERT(raw_verdict(port, "/fresh", OFFER, 0xc1, again, sizeof again) == 1007,
                    "where takeover was forbidden, the same bytes refer to nothing: 1007", server);
            }

            /* Takeover from the server: the same 3,000 bytes twice. The second answer refers back
             * into the first. */
            {
                fill(3000, true, 4242);
                proven_size_t sizes[2] = { 0, 0 };
                PROVEN_TEST_ASSERT(raw_upgrade(&r, port, "/echo", OFFER, head, sizeof head), "a handshake by hand, with takeover agreed", server);
                for (int k = 0; k < 2; ++k) {
                    fin = false;
                    PROVEN_TEST_ASSERT(raw_frame(&r, 0x81, g_msg, 3000), "three thousand bytes of text, uncompressed", server);
                    for (int frames = 0; !fin && frames < 64; ++frames) {
                        PROVEN_TEST_ASSERT(raw_next(&r, &b0, payload, sizeof payload, &n), "a frame of the answer", server);
                        fin = (b0 & 0x80) != 0;
                        sizes[k] += n;
                    }
                }
                fprintf(stderr, "[PROVEN][TEST][INFO] %s: 3000 bytes sent back as %u, then the same as %u\n", server, (unsigned)sizes[0], (unsigned)sizes[1]);
                PROVEN_TEST_ASSERT(sizes[1] * 10 < sizes[0], "the second answer is under a tenth of the first: the server kept its window", server);
                (void)proven_net_close(&r);
            }

            static const proven_byte_t not_deflate[] = { 0x06, 0x00 };                               /* a block type that does not exist */
            static const proven_byte_t not_text[] = { 0x00, 0x02, 0x00, 0xfd, 0xff, 0xc3, 0x28, 0x00 };  /* a stored block holding c3 28 */
            static const proven_byte_t hello_tail[] = { 0xc9, 0xc9, 0x07, 0x00 };
            PROVEN_TEST_ASSERT(raw_verdict(port, "/echo", OFFER, 0xc2, not_deflate, sizeof not_deflate) == 1007, "bytes that do not inflate end the connection with 1007", server);
            PROVEN_TEST_ASSERT(raw_verdict(port, "/echo", OFFER, 0xc1, not_text, sizeof not_text) == 1007, "and so does text that inflates to something that is not UTF-8", server);
            PROVEN_TEST_ASSERT(raw_verdict(port, "/echo", OFFER, 0xc0, hello_tail, sizeof hello_tail) == 1002, "the bit on a continuation is a protocol error: 1002", server);
            PROVEN_TEST_ASSERT(raw_verdict(port, "/echo", OFFER, 0xc9, hello_tail, 0) == 1002, "and on a ping", server);
            PROVEN_TEST_ASSERT(raw_verdict(port, "/plain", OFFER, 0xc1, hello, sizeof hello) == 1002, "and anywhere at all on a connection that did not agree the extension", server);
            PROVEN_TEST_ASSERT(raw_verdict(port, "/echo", NULL, 0xc1, hello, sizeof hello) == 1002, "whichever side declined", server);
        }
    }

    PROVEN_TEST_SECTION("pieces and pauses", "The event-driven server hands a compressed message on in pieces, and stops between them when told.", "Check ws_rx_run, ws_rx_hold and proven_ws_stream_resume in src/proven/ws_event.c.");
    {
        proven_ws_conn_t *c = dial(eat.port, "/count", true);
        static const proven_size_t sizes[] = { 300000, 16384, 100, 0, 1000000 };
        for (proven_size_t i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
            proven_size_t n = sizes[i];
            fill(n, true, (unsigned)(900 + i));
            /* The last one is a megabyte of one letter: a kilobyte on the wire, one frame, and
             * sixty-four pieces out of it - so the pauses fall in the middle of a frame. */
            if (i == sizeof sizes / sizeof sizes[0] - 1) memset(g_msg, 'a', n);
            unsigned sum = 0;
            for (proven_size_t k = 0; k < n; ++k) sum = sum * 31 + g_msg[k];
            PROVEN_TEST_ASSERT(proven_ws_conn_send_text(c, (proven_u8str_view_t){ g_msg, n }) == PROVEN_OK, "a text message, compressed on its way", "");
            proven_ws_message_t m;
            PROVEN_TEST_ASSERT(proven_ws_conn_receive(c, proven_net_deadline_in(60000), &m) == PROVEN_OK && m.data.size < 120, "the server's account of it", "");
            char answer[128] = { 0 };
            memcpy(answer, m.data.ptr, m.data.size);
            unsigned pieces = 0, firsts = 0, lasts = 0, largest = 0, total = 0, got_sum = 0;
            int while_paused = 1;
            PROVEN_TEST_ASSERT(sscanf(answer, "%u %u %u %u %u %u %d", &pieces, &firsts, &lasts, &largest, &total, &got_sum, &while_paused) == 7, "is seven numbers", answer);
            PROVEN_TEST_ASSERT(total == n && got_sum == sum, "every byte arrived, in order", answer);
            PROVEN_TEST_ASSERT(firsts == 1 && lasts == 1, "with one first piece and one last", answer);
            PROVEN_TEST_ASSERT(largest <= 16384 && (n < 64 || pieces >= n / 16384), "in pieces of at most 16 KiB", answer);
            PROVEN_TEST_ASSERT(while_paused == 0, "and nothing was delivered between a pause and its resume", answer);
        }
        hang_up(c);

        atomic_store(&g_refused, 0);
        atomic_store(&g_writable, 0);
        c = dial(eat.port, "/push", true);
        /* Read nothing until the server has run into its limit: the system's own buffers
         * take a few megabytes first. */
        for (int waited = 0; atomic_load(&g_refused) == 0 && waited < 1000; ++waited) proven_time_sleep(10);
        int got = 0;
        bool in_order = true;
        for (;;) {
            proven_ws_message_t m;
            proven_err_t e = proven_ws_conn_receive(c, proven_net_deadline_in(20000), &m);
            if (e != PROVEN_OK) break;
            if (m.data.size != 1000 || m.data.ptr[0] != (proven_byte_t)(got >> 8) || m.data.ptr[1] != (proven_byte_t)got || m.data.ptr[999] != (proven_byte_t)('a' + got % 26)) in_order = false;
            got++;
        }
        PROVEN_TEST_ASSERT(got == 20000 && in_order && proven_ws_conn_close_code(c) == 1000, "twenty thousand compressed messages pushed by the server arrive whole and in order, then its close", "");
        PROVEN_TEST_ASSERT(atomic_load(&g_refused) > 0 && atomic_load(&g_writable) > 0, "though sends were refused on the way and resumed through on_writable", "Check ws_may_send: a compressed send is admitted on the queue as any other.");
        proven_ws_conn_destroy(c);
    }

    PROVEN_TEST_SECTION("over TLS", "The same extension where the bytes pass through TLS records in both servers: compressed messages of several sizes, both kinds, whole and in parts.", "Check that nothing in ws_conn.c or ws_event.c assumes a frame arrives in one read: TLS hands bytes on a record at a time.");
    {
        PROVEN_TEST_ASSERT(tp_build(), "the test's certificates and keys are issued", "");
        PROVEN_TEST_ASSERT(proven_cert_store_create(g_heap, &anchors) == PROVEN_OK &&
                           proven_cert_store_add_pem(anchors, (proven_mem_view_t){ (const proven_byte_t *)TP_CA, strlen(TP_CA) }, NULL) == PROVEN_OK, "anchors", "");
        proven_tls_options_t co = { .alloc = g_heap, .anchors = anchors };
        proven_tls_options_t so = { .alloc = g_heap, .certificate_pem = { (const proven_byte_t *)TP_SERVER_ED, strlen(TP_SERVER_ED) }, .private_key_pem = { (const proven_byte_t *)TP_SERVER_ED_KEY, strlen(TP_SERVER_ED_KEY) } };
        PROVEN_TEST_ASSERT(proven_tls_config_create(&co, &client_tls) == PROVEN_OK && proven_tls_config_create(&so, &server_tls) == PROVEN_OK, "TLS configurations", "");
        /* A second pair of servers, with TLS: the event-driven one on the loop that is already running. */
        proven_http_server_t *blocking_tls = NULL;
        proven_http_event_server_t *event_tls = NULL;
        proven_http_server_config_t bt = bcfg; bt.tls = server_tls;
        proven_http_event_server_config_t et = ecfg; et.tls = server_tls;
        proven_net_addr_t btat, etat;
        PROVEN_TEST_ASSERT(proven_http_server_create(&bt, &blocking_tls) == PROVEN_OK &&
                           proven_http_server_listen(blocking_tls, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &btat) == PROVEN_OK, "a blocking server with TLS listens", "");
        g_made = false; g_made_on = et; g_made_out = &event_tls; g_made_at = &etat;
        PROVEN_TEST_ASSERT(proven_loop_post(g_loop, make_event_server, NULL) == PROVEN_OK, "an event-driven server with TLS is asked for on the loop's thread", "");
        for (int i = 0; i < 500 && !atomic_load(&g_made); ++i) proven_time_sleep(10);
        PROVEN_TEST_ASSERT(atomic_load(&g_made) && event_tls != NULL, "and listens", "");
        proven_job_sys_t *more = NULL;
        proven_job_group_t tls_running;
        proven_job_group_init(&tls_running);
        g_blocking_tls = blocking_tls;
        PROVEN_TEST_ASSERT(proven_job_system_init(g_heap, 1, 2, &more) == PROVEN_OK && proven_job_group_submit(more, &tls_running, blocking_tls_serve, NULL) == PROVEN_OK, "it is started", "");
        proven_http_client_config_t hc = { .alloc = g_heap, .tls_wrap = proven_tls_http_wrap, .tls_ctx = client_tls };
        PROVEN_TEST_ASSERT(proven_http_client_create(&hc, &g_https) == PROVEN_OK, "an HTTPS client for the handshakes", "");
        g_wss = true;
        for (int which = 0; which < 2; ++which) {
            const char *server = which ? "the event-driven server, over TLS" : "the blocking server, over TLS";
            proven_ws_conn_t *c = dial(which ? etat.port : btat.port, "/echo", true);
            PROVEN_TEST_ASSERT(c && proven_ws_conn_compressed(c), "compression is agreed over wss", server);
            static const proven_size_t sizes[] = { 0, 1, 64, 1000, 16384, 16385, 70000 };
            for (proven_size_t i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
                for (int form = 0; form < 4; ++form) {
                    fill(sizes[i], form % 2 == 0, (unsigned)(i * 4 + (proven_size_t)form + 100));
                    PROVEN_TEST_ASSERT(echo_ok(c, sizes[i], form % 2 == 0, form >= 2), "text and binary, whole and in three parts, come back unchanged through TLS at every size", server);
                }
            }
            hang_up(c);
        }
        g_wss = false;
        proven_http_client_destroy(g_https);
        proven_http_server_stop(blocking_tls);
        proven_job_group_wait(more, &tls_running);
        proven_job_system_close(more); proven_job_system_destroy(more);
        proven_http_server_destroy(blocking_tls);
        g_tls_to_end = event_tls;
    }

    proven_http_client_destroy(g_http);
    proven_http_server_stop(g_blocking);
    proven_loop_stop(g_loop);
    proven_job_group_wait(threads, &running);
    proven_http_event_server_destroy(g_tls_to_end);
    proven_tls_config_destroy(server_tls); proven_tls_config_destroy(client_tls); proven_cert_store_destroy(anchors);
    proven_http_event_server_destroy(eserver);
    proven_loop_destroy(g_loop);
    proven_http_server_destroy(g_blocking);
    proven_job_system_close(threads); proven_job_system_destroy(threads);
    proven_job_system_close(workers); proven_job_system_destroy(workers);
    PROVEN_TEST_PASS("compressed messages cross both drivers in both directions, bounded, in pieces, and refused when they are not what they say.");
    return 0;
}
