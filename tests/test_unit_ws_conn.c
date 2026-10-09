#include "proven.h"
#include "proven_test.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/*
 * WebSocket connections, end to end on the loopback interface.
 *
 * Three kinds of peer:
 *   - this library's server accepting and this library's client connecting, for a conversation
 *     that goes right, in both handler models of the HTTP server;
 *   - a raw socket playing a client that breaks the rules, to see what the server's end does;
 *   - a scripted server that breaks them, to see what the client's end does.
 *
 * Both ends here share one codec, so this shows the driver uses it correctly and says nothing
 * about agreement with other implementations. That is checked by a private program against
 * Node's WebSocket client and Python's websockets library.
 */

static proven_u8str_view_t sv(const char *s) {
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}
static proven_mem_view_t mv(const char *s) {
    return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}

static proven_job_sys_t *g_workers;         /* for handed-off connections and scripted peers */
static proven_job_group_t g_group;
static proven_http_server_t *g_server;
static atomic_int g_peer_close_code;        /* what the server's end saw the client close with */
static atomic_int g_server_error;           /* what the server's receive ended with */
static atomic_int g_accept_error;
static atomic_int g_plain_requests;
static atomic_int g_echo_started, g_echo_done;   /* echo conversations begun and ended */

static proven_byte_t g_big[70000];

/* Echo until the peer closes; record how it ended. */
static void echo_until_closed(proven_ws_conn_t *ws) {
    atomic_fetch_add(&g_echo_started, 1);
    for (;;) {
        proven_ws_message_t m;
        proven_err_t e = proven_ws_conn_receive(ws, proven_net_deadline_in(10000), &m);
        if (e != PROVEN_OK) {
            /* The code first: the test waits for the error and then reads the code. */
            atomic_store(&g_peer_close_code, (int)proven_ws_conn_close_code(ws));
            atomic_store(&g_server_error, (int)e);
            break;
        }
        e = m.text ? proven_ws_conn_send_text(ws, (proven_u8str_view_t){ .ptr = m.data.ptr, .size = m.data.size })
                   : proven_ws_conn_send_binary(ws, m.data);
        if (e != PROVEN_OK) { atomic_store(&g_server_error, (int)e); break; }
    }
    proven_ws_conn_destroy(ws);
    atomic_fetch_add(&g_echo_done, 1);
}

/* Wait until no echo conversation is still running, so that what an earlier case's handler
 * records cannot land after the next case has reset the record. */
static bool echoes_settled(void) {
    for (int i = 0; i < 2000 && atomic_load(&g_echo_done) != atomic_load(&g_echo_started); ++i) proven_time_sleep(5);
    return atomic_load(&g_echo_done) == atomic_load(&g_echo_started);
}

static void echo_job(void *arg) {
    echo_until_closed(arg);
}

static void handler(void *ctx, proven_http_exchange_t *x) {
    (void)ctx;
    const proven_http_request_t *r = proven_http_exchange_request(x);
    proven_ws_conn_config_t cfg = { .alloc = proven_heap_allocator(), .max_message_bytes = 100000 };
    proven_ws_conn_t *ws = NULL;

    if (proven_u8str_view_eq(r->target, sv("/hello"))) {
        atomic_fetch_add(&g_plain_requests, 1);
        (void)proven_http_exchange_respond(x, 200, NULL, 0, mv("hello"));
        return;
    }
    if (proven_u8str_view_eq(r->target, sv("/missing"))) {
        (void)proven_http_exchange_respond(x, 404, NULL, 0, mv("no"));
        return;
    }
    proven_u8str_view_t protocol = proven_ws_request_offers(r, sv("echo")) ? sv("echo") : sv("");
    if (proven_u8str_view_eq(r->target, sv("/unoffered"))) protocol = sv("never-offered");
    proven_err_t e = proven_ws_conn_accept(x, protocol, &cfg, &ws);
    atomic_store(&g_accept_error, (int)e);
    if (e == PROVEN_ERR_NOT_FOUND || e == PROVEN_ERR_INVALID_ARG) {
        /* Nothing was sent: the handler still owes an answer. */
        (void)proven_http_exchange_respond(x, e == PROVEN_ERR_NOT_FOUND ? 400 : 500, NULL, 0, mv("websocket only"));
        return;
    }
    if (e != PROVEN_OK) return;

    if (proven_u8str_view_eq(r->target, sv("/echo"))) {
        echo_until_closed(ws);                  /* stays in the handler */
    } else if (proven_u8str_view_eq(r->target, sv("/handoff"))) {
        /* Gives the connection to another thread and returns: the server is free again. */
        if (proven_job_group_submit(g_workers, &g_group, echo_job, ws) != PROVEN_OK) proven_ws_conn_destroy(ws);
    } else if (proven_u8str_view_eq(r->target, sv("/push"))) {
        (void)proven_ws_conn_send_text(ws, sv("first"));
        (void)proven_ws_conn_send_binary(ws, (proven_mem_view_t){ .ptr = g_big, .size = sizeof g_big });
        (void)proven_ws_conn_send_part(ws, true, mv("in "), false);
        (void)proven_ws_conn_ping(ws, mv("between"));
        (void)proven_ws_conn_send_part(ws, true, mv("three "), false);
        (void)proven_ws_conn_send_part(ws, true, mv("parts"), true);
        proven_err_t ce = proven_ws_conn_close(ws, PROVEN_WS_CLOSE_GOING_AWAY, sv("done"));
        atomic_store(&g_server_error, (int)ce);
        atomic_store(&g_peer_close_code, (int)proven_ws_conn_close_code(ws));
        proven_ws_conn_destroy(ws);
    } else {
        proven_ws_conn_destroy(ws);
    }
}

static void serve(void *arg) {
    (void)arg;
    (void)proven_http_server_run(g_server);
}

/* Wait for a value the other thread stores. */
static bool eventually(atomic_int *v, int want) {
    for (int i = 0; i < 2000 && atomic_load(v) != want; ++i) proven_time_sleep(5);
    return atomic_load(v) == want;
}

static char g_url[128];
static proven_u8str_view_t ws_url(proven_u16 port, const char *path) {
    int n = snprintf(g_url, sizeof g_url, "ws://127.0.0.1:%u%s", (unsigned)port, path);
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)g_url, .size = (proven_size_t)n };
}

// -----------------------------------------------------------------------------
// A raw client: the handshake by hand, then whatever bytes the case wants
// -----------------------------------------------------------------------------

static const char RAW_HANDSHAKE[] = "GET /echo HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";

/* Read from `c` until `want` bytes have come or the peer closes. Returns how many came. */
static proven_size_t raw_read(proven_net_conn_t *c, proven_byte_t *buf, proven_size_t want) {
    proven_size_t n = 0;
    while (n < want) {
        proven_result_size_t r = proven_net_read(c, (proven_mem_mut_t){ .ptr = buf + n, .size = want - n }, proven_net_deadline_in(5000));
        if (r.err != PROVEN_OK) break;
        n += r.value;
    }
    return n;
}

/* Connect, send the handshake (with `extra` in the same write), and read the 101. */
static void raw_open(proven_net_conn_t *c, proven_net_addr_t at, const proven_byte_t *extra, proven_size_t extra_len) {
    static proven_byte_t out[1024];
    static char head[512];
    proven_err_t e = proven_net_connect(at, proven_net_deadline_in(5000), c);
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "a raw client connects", "");
    proven_size_t n = sizeof RAW_HANDSHAKE - 1;
    memcpy(out, RAW_HANDSHAKE, n);
    if (extra_len) memcpy(out + n, extra, extra_len);
    PROVEN_TEST_ASSERT(proven_net_write_all(c, (proven_mem_view_t){ .ptr = out, .size = n + extra_len }, proven_net_deadline_in(5000)).err == PROVEN_OK, "the handshake is sent", "");
    proven_size_t got = 0;
    while (got + 1 < sizeof head) {
        if (raw_read(c, (proven_byte_t *)head + got, 1) != 1) break;
        got++;
        if (got >= 4 && memcmp(head + got - 4, "\r\n\r\n", 4) == 0) break;
    }
    head[got] = '\0';
    PROVEN_TEST_ASSERT(strncmp(head, "HTTP/1.1 101 ", 13) == 0 && strstr(head, "\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != NULL &&
                       strstr(head, "\r\nUpgrade: websocket\r\n") != NULL && strstr(head, "\r\nConnection: Upgrade\r\n") != NULL,
        "the server answers 101 with the accept value RFC 6455 gives for this key", "Inspect proven_ws_conn_accept and proven_http_exchange_upgrade.");
    PROVEN_TEST_ASSERT(strstr(head, "Sec-WebSocket-Protocol") == NULL && strstr(head, "Sec-WebSocket-Extensions") == NULL, "and names no subprotocol or extension nobody asked for", "");
}

/* A masked client frame. */
static proven_size_t raw_frame(proven_byte_t *buf, bool fin, proven_u8 opcode, bool masked, const void *payload, proven_size_t n, proven_u64 claimed) {
    proven_ws_frame_t f = { .fin = fin, .opcode = opcode, .masked = masked, .mask = { 9, 8, 7, 6 }, .length = claimed ? claimed : n };
    proven_size_t len = 0;
    proven_err_t e = proven_ws_frame_write((proven_mem_mut_t){ .ptr = buf, .size = 256 }, &len, &f);
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "a raw frame header is written", "");
    memcpy(buf + len, payload, n);
    if (masked) proven_ws_mask((proven_mem_mut_t){ .ptr = buf + len, .size = n }, f.mask, 0);
    return len + n;
}

// -----------------------------------------------------------------------------
// A scripted server, for the client's end
// -----------------------------------------------------------------------------

typedef struct {
    proven_net_listener_t listener;
    proven_u16 port;
    int script;
    proven_byte_t got[64];      /* what the client sent after the handshake */
    proven_size_t got_len;
} peer_t;

enum { SCRIPT_MASKED, SCRIPT_BAD_ACCEPT, SCRIPT_EXTENSION, SCRIPT_EARLY, SCRIPT_NO_CLOSE, SCRIPT_BAD_UTF8 };

static void script(void *arg) {
    peer_t *p = arg;
    proven_net_conn_t c;
    static _Thread_local char head[1024];
    if (proven_net_accept(&p->listener, proven_net_deadline_in(10000), &c, NULL) != PROVEN_OK) return;
    proven_size_t n = 0;
    while (n + 1 < sizeof head) {
        if (raw_read(&c, (proven_byte_t *)head + n, 1) != 1) break;
        n++;
        if (n >= 4 && memcmp(head + n - 4, "\r\n\r\n", 4) == 0) break;
    }
    head[n] = '\0';
    const char *k = strstr(head, "Sec-WebSocket-Key: ");
    proven_byte_t accept[PROVEN_WS_ACCEPT_SIZE + 1] = {0};
    if (k && proven_ws_accept_key((proven_u8str_view_t){ .ptr = (const proven_byte_t *)k + 19, .size = 24 }, accept) == PROVEN_OK) {
        static _Thread_local char reply[512];
        if (p->script == SCRIPT_BAD_ACCEPT) accept[0] = accept[0] == 'A' ? 'B' : 'A';
        int rn = snprintf(reply, sizeof reply, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n%s\r\n",
                          (const char *)accept, p->script == SCRIPT_EXTENSION ? "Sec-WebSocket-Extensions: permessage-deflate\r\n" : "");
        proven_byte_t frame[64];
        proven_size_t fl = 0;
        if (p->script == SCRIPT_MASKED) fl = raw_frame(frame, true, PROVEN_WS_TEXT, true, "x", 1, 0);
        if (p->script == SCRIPT_EARLY) fl = raw_frame(frame, true, PROVEN_WS_TEXT, false, "early", 5, 0);
        if (p->script == SCRIPT_BAD_UTF8) fl = raw_frame(frame, true, PROVEN_WS_TEXT, false, "\xc0\xaf", 2, 0);
        /* The 101 and the first frame in one write: the client finds the frame behind the head. */
        memcpy(reply + rn, frame, fl);
        (void)proven_net_write_all(&c, (proven_mem_view_t){ .ptr = (const proven_byte_t *)reply, .size = (proven_size_t)rn + fl }, proven_net_deadline_in(5000));
        /* Then listen to what the client says, without ever answering. */
        p->got_len = raw_read(&c, p->got, p->script == SCRIPT_NO_CLOSE ? 8 : 8);
        if (p->script == SCRIPT_NO_CLOSE) proven_time_sleep(600);
    }
    (void)proven_net_close(&c);
}

static void peer_start(peer_t *p, int which) {
    memset(p, 0, sizeof *p);
    p->script = which;
    proven_net_addr_t at;
    PROVEN_TEST_ASSERT(proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 4, &p->listener, &at) == PROVEN_OK, "a scripted server listens", "");
    p->port = at.port;
    proven_job_group_init(&g_group);
    PROVEN_TEST_ASSERT(proven_job_group_submit(g_workers, &g_group, script, p) == PROVEN_OK, "its script starts", "");
}

static void peer_finish(peer_t *p) {
    proven_job_group_wait(g_workers, &g_group);
    (void)proven_net_listener_close(&p->listener);
}

// -----------------------------------------------------------------------------

static void run_cases(proven_job_sys_t *server_jobs, proven_job_sys_t *loop_thread) {
    proven_allocator_t heap = proven_heap_allocator();
    proven_http_server_config_t scfg = {0};
    scfg.alloc = heap;
    scfg.handler = handler;
    scfg.jobs = server_jobs;
    proven_err_t e = proven_http_server_create(&scfg, &g_server);
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "a server", "");
    proven_net_addr_t at;
    e = proven_http_server_listen(g_server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at);
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "it listens", "");
    proven_job_group_t running;
    proven_job_group_init(&running);
    e = proven_job_group_submit(loop_thread, &running, serve, NULL);
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "its loop runs on another thread", "");

    proven_http_client_config_t ccfg = { .alloc = heap, .max_idle_connections = 2 };
    proven_http_client_t *client = NULL;
    PROVEN_TEST_ASSERT(proven_http_client_create(&ccfg, &client) == PROVEN_OK, "an HTTP client to connect through", "");
    proven_ws_conn_config_t wcfg = { .alloc = heap };
    proven_ws_conn_t *ws = NULL;
    proven_ws_message_t m;
    proven_u16 status = 0;

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a conversation",
        "Connect, agree a subprotocol, exchange text and binary messages of every size, fragments, a ping, and close.",
        "Inspect src/proven/ws_conn.c: wc_send_frame for what is sent, wc_pump for what is received.");
    // ---------------------------------------------------------------
    {
        e = proven_ws_conn_connect(client, ws_url(at.port, "/echo"), NULL, 0, sv("chat, echo"), &wcfg, &ws, &status);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && ws != NULL && status == 101, "the client connects and the status was 101", "");
        PROVEN_TEST_ASSERT(proven_u8str_view_eq(proven_ws_conn_protocol(ws), sv("echo")), "the subprotocol the server chose is known", "");

        PROVEN_TEST_ASSERT(proven_ws_conn_send_text(ws, sv("Hello")) == PROVEN_OK, "a text message is sent", "");
        e = proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &m);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && m.text && m.data.size == 5 && memcmp(m.data.ptr, "Hello", 5) == 0, "and comes back as text", "");

        PROVEN_TEST_ASSERT(proven_ws_conn_send_binary(ws, (proven_mem_view_t){0}) == PROVEN_OK &&
                           proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &m) == PROVEN_OK && !m.text && m.data.size == 0, "an empty binary message", "");

        static const proven_size_t sizes[] = { 1, 125, 126, 127, 4081, 4082, 4083, 16384, 65535, 65536, 70000 };
        for (proven_size_t i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
            PROVEN_TEST_ASSERT(proven_ws_conn_send_binary(ws, (proven_mem_view_t){ .ptr = g_big, .size = sizes[i] }) == PROVEN_OK, "a binary message is sent", "");
            e = proven_ws_conn_receive(ws, proven_net_deadline_in(10000), &m);
            if (e != PROVEN_OK || m.data.size != sizes[i]) PROVEN_TEST_INFO("size {} -> error {}", PROVEN_ARG((proven_u64)sizes[i]), PROVEN_ARG((int)e));
            PROVEN_TEST_ASSERT(e == PROVEN_OK && !m.text && m.data.size == sizes[i] && memcmp(m.data.ptr, g_big, sizes[i]) == 0,
                "and comes back byte for byte - at each length encoding, and across the send and read buffers", "");
        }

        PROVEN_TEST_ASSERT(proven_ws_conn_send_text(ws, sv("\xce\xba\xe1\xbd\xb9\xcf\x83\xce\xbc\xce\xb5 \xf0\x9f\x98\x80")) == PROVEN_OK &&
                           proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &m) == PROVEN_OK && m.text && m.data.size == 16, "text beyond ASCII", "");

        PROVEN_TEST_ASSERT(proven_ws_conn_send_text(ws, sv("bad \xff")) == PROVEN_ERR_INVALID_ENCODING, "text that is not UTF-8 is refused before anything is sent", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_ping(ws, (proven_mem_view_t){ .ptr = g_big, .size = 126 }) == PROVEN_ERR_INVALID_ARG, "and so is a ping of 126 bytes", "");

        /* A message in pieces, with a ping between them and a character cut by a boundary. */
        PROVEN_TEST_ASSERT(proven_ws_conn_send_part(ws, true, mv("Hel\xce"), false) == PROVEN_OK, "the first fragment", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_send_text(ws, sv("x")) == PROVEN_ERR_INVALID_STATE && proven_ws_conn_send_binary(ws, mv("x")) == PROVEN_ERR_INVALID_STATE,
            "another message may not begin inside it", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_ping(ws, mv("are you there")) == PROVEN_OK, "a ping may", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_send_part(ws, true, (proven_mem_view_t){0}, false) == PROVEN_OK &&
                           proven_ws_conn_send_part(ws, false, mv("\xbalo"), true) == PROVEN_OK, "an empty fragment and the last one", "");
        e = proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &m);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && m.text && m.data.size == 7 && memcmp(m.data.ptr, "Hel\xce\xbalo", 7) == 0, "the peer received one message and echoed it whole", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_pong_count(ws) == 1, "and its pong was counted on the way", "");

        proven_time_t start = proven_time_monotonic_now();
        e = proven_ws_conn_receive(ws, proven_net_deadline_in(100), &m);
        proven_i64 took = (proven_time_monotonic_now() - start) / 1000000;
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_TIMEOUT && took >= 60 && took < 5000, "with nothing to receive: PROVEN_ERR_TIMEOUT at the deadline", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_send_text(ws, sv("still here")) == PROVEN_OK && proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &m) == PROVEN_OK,
            "after which the connection carries on", "");

        atomic_store(&g_peer_close_code, 0);
        atomic_store(&g_server_error, 0);
        e = proven_ws_conn_close(ws, PROVEN_WS_CLOSE_NORMAL, sv("bye"));
        PROVEN_TEST_ASSERT(e == PROVEN_OK, "close completes the handshake", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_close_code(ws) == 1000, "the server echoed the code", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_close(ws, PROVEN_WS_CLOSE_NORMAL, sv("")) == PROVEN_OK, "closing twice is harmless", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_send_text(ws, sv("late")) == PROVEN_ERR_INVALID_STATE && proven_ws_conn_receive(ws, proven_net_deadline_in(100), &m) == PROVEN_ERR_EOF,
            "after it, sending is INVALID_STATE and receiving is EOF", "");
        proven_ws_conn_destroy(ws);
        PROVEN_TEST_ASSERT(eventually(&g_peer_close_code, 1000) && atomic_load(&g_server_error) == (int)PROVEN_ERR_EOF, "the server's end saw a close with code 1000 as PROVEN_ERR_EOF", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("messages the server starts, and its close",
        "Fragments are reassembled, a ping between them is answered unseen, and the peer's close code and reason arrive with PROVEN_ERR_EOF.",
        "Inspect the PROVEN_WS_EVENT_CLOSE branch of wc_pump.");
    // ---------------------------------------------------------------
    {
        PROVEN_TEST_ASSERT(echoes_settled(), "the earlier conversation has ended on the server's side", "");
        atomic_store(&g_server_error, -1);
        e = proven_ws_conn_connect(client, ws_url(at.port, "/push"), NULL, 0, sv(""), &wcfg, &ws, NULL);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && proven_ws_conn_protocol(ws).size == 0, "connect, with no subprotocol", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &m) == PROVEN_OK && m.text && m.data.size == 5, "the first message", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &m) == PROVEN_OK && !m.text && m.data.size == sizeof g_big && memcmp(m.data.ptr, g_big, sizeof g_big) == 0,
            "70000 bytes, more than the read buffer holds", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &m) == PROVEN_OK && m.text && m.data.size == 14 && memcmp(m.data.ptr, "in three parts", 14) == 0,
            "three fragments with a ping between them arrive as one message", "");
        e = proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &m);
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_EOF && proven_ws_conn_close_code(ws) == 1001 && proven_u8str_view_eq(proven_ws_conn_close_reason(ws), sv("done")),
            "then the server's close: EOF, with its code and reason", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_send_text(ws, sv("late")) == PROVEN_ERR_INVALID_STATE, "nothing may be sent after it", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_close(ws, PROVEN_WS_CLOSE_NORMAL, sv("")) == PROVEN_OK, "close after the peer closed has nothing left to do", "");
        proven_ws_conn_destroy(ws);
        PROVEN_TEST_ASSERT(eventually(&g_server_error, (int)PROVEN_OK), "the server's close completed: the client answered it, having answered the ping too", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a message past the limit",
        "A receiver with a 1000-byte limit gets PROVEN_ERR_OUT_OF_BOUNDS for a larger message and tells the sender 1009.",
        "Inspect wc_fail.");
    // ---------------------------------------------------------------
    {
        proven_ws_conn_config_t small = { .alloc = heap, .max_message_bytes = 1000 };
        PROVEN_TEST_ASSERT(echoes_settled(), "the earlier conversations have ended on the server's side", "");
        atomic_store(&g_peer_close_code, 0);
        e = proven_ws_conn_connect(client, ws_url(at.port, "/echo"), NULL, 0, sv(""), &small, &ws, NULL);
        PROVEN_TEST_ASSERT(e == PROVEN_OK, "connect", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_send_binary(ws, (proven_mem_view_t){ .ptr = g_big, .size = 1000 }) == PROVEN_OK &&
                           proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &m) == PROVEN_OK && m.data.size == 1000, "a message of exactly the limit passes", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_send_binary(ws, (proven_mem_view_t){ .ptr = g_big, .size = 1001 }) == PROVEN_OK, "one byte more is sent", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &m) == PROVEN_ERR_OUT_OF_BOUNDS, "and its echo is PROVEN_ERR_OUT_OF_BOUNDS", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_receive(ws, proven_net_deadline_in(100), &m) == PROVEN_ERR_OUT_OF_BOUNDS && proven_ws_conn_send_text(ws, sv("x")) == PROVEN_ERR_OUT_OF_BOUNDS,
            "after which the connection only repeats that", "");
        proven_ws_conn_destroy(ws);
        PROVEN_TEST_ASSERT(eventually(&g_peer_close_code, 1009), "the sender was told why: close code 1009", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a connection handed off, and the server left free",
        "The handler gives the connection to another thread and returns; ordinary requests are served while the WebSocket is open.",
        "Inspect proven_http_exchange_upgrade and the detached branches of sv_run and sv_linger in src/proven/http_server.c.");
    // ---------------------------------------------------------------
    {
        proven_job_group_init(&g_group);
        e = proven_ws_conn_connect(client, ws_url(at.port, "/handoff"), NULL, 0, sv(""), &wcfg, &ws, NULL);
        PROVEN_TEST_ASSERT(e == PROVEN_OK, "connect", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_send_text(ws, sv("one")) == PROVEN_OK && proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &m) == PROVEN_OK, "the handed-off connection echoes", "");
        int before = atomic_load(&g_plain_requests);
        proven_http_client_response_t resp;
        int n = snprintf(g_url, sizeof g_url, "http://127.0.0.1:%u/hello", (unsigned)at.port);
        for (int i = 0; i < 3; ++i) {
            e = proven_http_client_get(client, (proven_u8str_view_t){ .ptr = (const proven_byte_t *)g_url, .size = (proven_size_t)n }, &resp);
            PROVEN_TEST_ASSERT(e == PROVEN_OK && resp.status == 200, "an ordinary request is served while the WebSocket is open", "With handlers on the loop thread this would wait for ever if the handler had stayed.");
            proven_http_client_finish(&resp);
        }
        PROVEN_TEST_ASSERT(atomic_load(&g_plain_requests) == before + 3, "three of them", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_send_text(ws, sv("two")) == PROVEN_OK && proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &m) == PROVEN_OK && m.data.size == 3,
            "and the WebSocket is still there afterwards", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_close(ws, PROVEN_WS_CLOSE_NORMAL, sv("")) == PROVEN_OK, "close", "");
        proven_ws_conn_destroy(ws);
        proven_job_group_wait(g_workers, &g_group);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("connecting where there is no WebSocket",
        "Another status is PROVEN_ERR_REFUSED with the status reported; wss without TLS is PROVEN_ERR_UNSUPPORTED; a handler may decline.",
        "Inspect proven_ws_conn_connect.");
    // ---------------------------------------------------------------
    {
        status = 0;
        PROVEN_TEST_ASSERT(proven_ws_conn_connect(client, ws_url(at.port, "/missing"), NULL, 0, sv(""), &wcfg, &ws, &status) == PROVEN_ERR_REFUSED && status == 404 && ws == NULL,
            "a 404 is PROVEN_ERR_REFUSED, and says it was a 404", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_connect(client, ws_url(at.port, "/unoffered"), NULL, 0, sv("chat"), &wcfg, &ws, &status) == PROVEN_ERR_REFUSED && status == 500 &&
                           atomic_load(&g_accept_error) == (int)PROVEN_ERR_INVALID_ARG,
            "a server that tries to select a subprotocol nobody offered gets PROVEN_ERR_INVALID_ARG, and may still answer", "");
        int n = snprintf(g_url, sizeof g_url, "wss://127.0.0.1:%u/echo", (unsigned)at.port);
        PROVEN_TEST_ASSERT(proven_ws_conn_connect(client, (proven_u8str_view_t){ .ptr = (const proven_byte_t *)g_url, .size = (proven_size_t)n }, NULL, 0, sv(""), &wcfg, &ws, &status) == PROVEN_ERR_UNSUPPORTED && status == 0,
            "wss without a tls_wrap is PROVEN_ERR_UNSUPPORTED, not a quiet ws", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_connect(client, sv("ws://"), NULL, 0, sv(""), &wcfg, &ws, NULL) == PROVEN_ERR_INVALID_ARG &&
                           proven_ws_conn_connect(client, sv("ws://127.0.0.1"), NULL, 0, sv(""), NULL, &ws, NULL) == PROVEN_ERR_INVALID_ARG,
            "a URL too short to be one, and no configuration, are PROVEN_ERR_INVALID_ARG", "");

        /* An ordinary GET to a WebSocket endpoint: accept says NOT_FOUND and the handler answers. */
        proven_http_client_response_t resp;
        n = snprintf(g_url, sizeof g_url, "http://127.0.0.1:%u/echo", (unsigned)at.port);
        e = proven_http_client_get(client, (proven_u8str_view_t){ .ptr = (const proven_byte_t *)g_url, .size = (proven_size_t)n }, &resp);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && resp.status == 400 && atomic_load(&g_accept_error) == (int)PROVEN_ERR_NOT_FOUND,
            "a request that is no upgrade is PROVEN_ERR_NOT_FOUND to accept, with nothing sent", "");
        proven_http_client_finish(&resp);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a client that breaks the rules",
        "A raw socket does the handshake by hand and then misbehaves; the server's end answers with the close code that says how.",
        "Inspect wc_fail and the decoder's checks in src/proven/ws.c. The expected bytes are a close frame: 88, a length of 2, and the code.");
    // ---------------------------------------------------------------
    {
        proven_net_conn_t raw;
        proven_byte_t frame[300], reply[64];
        proven_size_t n;

        /* The first frame travels with the handshake: the server must find it behind the head. */
        n = raw_frame(frame, true, PROVEN_WS_TEXT, true, "early", 5, 0);
        raw_open(&raw, at, frame, n);
        PROVEN_TEST_ASSERT(raw_read(&raw, reply, 7) == 7 && memcmp(reply, "\x81\x05" "early", 7) == 0, "a frame sent with the handshake is echoed: unmasked, as a server sends", "Inspect the `early` view of proven_http_exchange_upgrade.");
        n = raw_frame(frame, true, PROVEN_WS_PING, true, "p", 1, 0);
        PROVEN_TEST_ASSERT(proven_net_write_all(&raw, (proven_mem_view_t){ .ptr = frame, .size = n }, proven_net_deadline_in(5000)).err == PROVEN_OK &&
                           raw_read(&raw, reply, 3) == 3 && memcmp(reply, "\x8a\x01p", 3) == 0, "a ping gets a pong with the same payload", "");
        n = raw_frame(frame, true, PROVEN_WS_CLOSE, true, "\x03\xe9", 2, 0);
        PROVEN_TEST_ASSERT(proven_net_write_all(&raw, (proven_mem_view_t){ .ptr = frame, .size = n }, proven_net_deadline_in(5000)).err == PROVEN_OK &&
                           raw_read(&raw, reply, 4) == 4 && memcmp(reply, "\x88\x02\x03\xe9", 4) == 0, "a close is answered with a close carrying the same code", "");
        PROVEN_TEST_ASSERT(raw_read(&raw, reply, 1) == 0, "and then the server closes the connection", "");
        (void)proven_net_close(&raw);

        static const struct { bool fin; proven_u8 opcode; bool masked; const char *payload; proven_size_t n; proven_u64 claimed; const char *reply; const char *why; } rows[] = {
            { true, PROVEN_WS_TEXT, false, "x", 1, 0, "\x88\x02\x03\xea", "an unmasked frame: 1002" },
            { true, PROVEN_WS_CONTINUATION, true, "x", 1, 0, "\x88\x02\x03\xea", "a continuation of nothing: 1002" },
            { true, PROVEN_WS_CLOSE, true, "\x03\xed", 2, 0, "\x88\x02\x03\xea", "a close with code 1005: 1002" },
            { true, PROVEN_WS_TEXT, true, "\xc0\xaf", 2, 0, "\x88\x02\x03\xef", "text that is not UTF-8: 1007" },
            { true, PROVEN_WS_BINARY, true, "", 0, 100001, "\x88\x02\x03\xf1", "a frame announcing more than the limit: 1009, before its payload" },
        };
        for (proven_size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
            atomic_store(&g_server_error, 0);
            raw_open(&raw, at, NULL, 0);
            n = raw_frame(frame, rows[i].fin, rows[i].opcode, rows[i].masked, rows[i].payload, rows[i].n, rows[i].claimed);
            PROVEN_TEST_ASSERT(proven_net_write_all(&raw, (proven_mem_view_t){ .ptr = frame, .size = n }, proven_net_deadline_in(5000)).err == PROVEN_OK, "the offending frame is sent", "");
            proven_size_t got = raw_read(&raw, reply, 4);
            if (got != 4 || memcmp(reply, rows[i].reply, 4) != 0) PROVEN_TEST_INFO("row: {}", PROVEN_ARG(rows[i].why));
            PROVEN_TEST_ASSERT(got == 4 && memcmp(reply, rows[i].reply, 4) == 0, "the server answers with the close code for that violation", "");
            PROVEN_TEST_ASSERT(raw_read(&raw, reply, 1) == 0, "and closes", "");
            (void)proven_net_close(&raw);
        }
        /* A frame past the limit whose payload is already on its way. The server refuses at the
         * header, with most of the payload unread - and the close it sends must still arrive.
         * A socket closed with unread input answers with a reset that destroys what it had just
         * sent; loopback on Linux tends to hide that, Windows does not. */
        {
            static proven_byte_t flood[60100];
            proven_ws_frame_t f = { .fin = true, .opcode = PROVEN_WS_BINARY, .masked = true, .mask = { 1, 2, 3, 4 }, .length = 200000 };
            proven_size_t hl = 0;
            PROVEN_TEST_ASSERT(proven_ws_frame_write((proven_mem_mut_t){ .ptr = flood, .size = sizeof flood }, &hl, &f) == PROVEN_OK, "the header of a 200000-byte frame", "");
            memset(flood + hl, 0x5a, 60000);
            raw_open(&raw, at, NULL, 0);
            (void)proven_net_write_all(&raw, (proven_mem_view_t){ .ptr = flood, .size = hl + 60000 }, proven_net_deadline_in(5000));
            PROVEN_TEST_ASSERT(raw_read(&raw, reply, 4) == 4 && memcmp(reply, "\x88\x02\x03\xf1", 4) == 0,
                "a frame past the limit with 60000 bytes of it already sent: the 1009 still arrives", "A short read here means the close was destroyed by a reset: inspect the drain in wc_fail.");
            PROVEN_TEST_ASSERT(raw_read(&raw, reply, 1) == 0, "and then the connection ends", "");
            (void)proven_net_close(&raw);
        }

        /* A frame with a reserved bit: it cannot be written by the codec, so by hand. */
        raw_open(&raw, at, NULL, 0);
        PROVEN_TEST_ASSERT(proven_net_write_all(&raw, (proven_mem_view_t){ .ptr = (const proven_byte_t *)"\xc1\x80\x01\x02\x03\x04", .size = 6 }, proven_net_deadline_in(5000)).err == PROVEN_OK &&
                           raw_read(&raw, reply, 4) == 4 && memcmp(reply, "\x88\x02\x03\xea", 4) == 0, "a reserved bit (what permessage-deflate would set): 1002", "");
        (void)proven_net_close(&raw);

        /* A client that vanishes. */
        PROVEN_TEST_ASSERT(echoes_settled(), "the earlier conversations have ended on the server's side", "");
        atomic_store(&g_server_error, 0);
        atomic_store(&g_peer_close_code, 0);
        raw_open(&raw, at, NULL, 0);
        (void)proven_net_close(&raw);
        PROVEN_TEST_ASSERT(eventually(&g_server_error, (int)PROVEN_ERR_RESET) && atomic_load(&g_peer_close_code) == 1006,
            "a connection that ends without a close frame is PROVEN_ERR_RESET, code 1006", "");

        /* Handshakes the server must refuse, with the status RFC 6455 names. */
        static const struct { const char *request; const char *status; const char *also; } bad[] = {
            { "GET /echo HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 8\r\n\r\n", "HTTP/1.1 426 ", "\r\nSec-WebSocket-Version: 13\r\n" },
            { "GET /echo HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n\r\n", "HTTP/1.1 400 ", "\r\n" },
            { "GET /echo HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: c2hvcnQ=\r\nSec-WebSocket-Version: 13\r\n\r\n", "HTTP/1.1 400 ", "\r\n" },
        };
        for (proven_size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
            static char head[512];
            PROVEN_TEST_ASSERT(proven_net_connect(at, proven_net_deadline_in(5000), &raw) == PROVEN_OK, "a raw client connects", "");
            PROVEN_TEST_ASSERT(proven_net_write_all(&raw, mv(bad[i].request), proven_net_deadline_in(5000)).err == PROVEN_OK, "a bad handshake is sent", "");
            /* To the end of the head only: these are ordinary HTTP answers on a connection the
             * server keeps open, so waiting for it to close would wait out the deadline. */
            proven_size_t got = 0;
            while (got + 1 < sizeof head) {
                if (raw_read(&raw, (proven_byte_t *)head + got, 1) != 1) break;
                got++;
                if (got >= 4 && memcmp(head + got - 4, "\r\n\r\n", 4) == 0) break;
            }
            head[got] = '\0';
            PROVEN_TEST_ASSERT(strncmp(head, bad[i].status, strlen(bad[i].status)) == 0 && strstr(head, bad[i].also) != NULL, "it is refused with its status: 426 naming version 13, or 400", "");
            (void)proven_net_close(&raw);
        }
    }

    proven_http_client_destroy(client);
    proven_http_server_stop(g_server);
    proven_job_group_wait(loop_thread, &running);
    proven_http_server_destroy(g_server);
    g_server = NULL;
}

static void run_client_side(void) {
    proven_allocator_t heap = proven_heap_allocator();
    proven_http_client_config_t ccfg = { .alloc = heap };
    proven_http_client_t *client = NULL;
    PROVEN_TEST_ASSERT(proven_http_client_create(&ccfg, &client) == PROVEN_OK, "an HTTP client", "");
    proven_ws_conn_config_t wcfg = { .alloc = heap, .close_timeout_ms = 200 };
    proven_ws_conn_t *ws = NULL;
    proven_ws_message_t m;
    proven_err_t e;
    static peer_t p;

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a server that breaks the rules",
        "A scripted server answers the handshake and then misbehaves; the client's end refuses, and says why in its close.",
        "Inspect proven_ws_check_response for the handshake rows, and wc_fail.");
    // ---------------------------------------------------------------
    {
        peer_start(&p, SCRIPT_BAD_ACCEPT);
        PROVEN_TEST_ASSERT(proven_ws_conn_connect(client, ws_url(p.port, "/"), NULL, 0, sv(""), &wcfg, &ws, NULL) == PROVEN_ERR_INVALID_FORMAT && ws == NULL,
            "a 101 with the wrong accept value: PROVEN_ERR_INVALID_FORMAT", "A server that does not speak WebSocket would otherwise be taken for one.");
        peer_finish(&p);

        peer_start(&p, SCRIPT_EXTENSION);
        PROVEN_TEST_ASSERT(proven_ws_conn_connect(client, ws_url(p.port, "/"), NULL, 0, sv(""), &wcfg, &ws, NULL) == PROVEN_ERR_INVALID_FORMAT,
            "a 101 naming an extension that was not offered: PROVEN_ERR_INVALID_FORMAT", "");
        peer_finish(&p);

        peer_start(&p, SCRIPT_EARLY);
        e = proven_ws_conn_connect(client, ws_url(p.port, "/"), NULL, 0, sv(""), &wcfg, &ws, NULL);
        PROVEN_TEST_ASSERT(e == PROVEN_OK, "a correct 101 connects", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &m) == PROVEN_OK && m.data.size == 5 && memcmp(m.data.ptr, "early", 5) == 0,
            "a frame that arrived in the same read as the 101 is not lost", "Inspect the `early` view of proven_http_client_upgrade.");
        PROVEN_TEST_ASSERT(proven_ws_conn_send_text(ws, sv("hi")) == PROVEN_OK, "the client sends", "");
        proven_ws_conn_destroy(ws);
        peer_finish(&p);
        proven_byte_t key[4];
        memcpy(key, p.got + 2, 4);
        proven_ws_mask((proven_mem_mut_t){ .ptr = p.got + 6, .size = 2 }, key, 0);
        PROVEN_TEST_ASSERT(p.got_len == 8 && p.got[0] == 0x81 && p.got[1] == 0x82 && memcmp(p.got + 6, "hi", 2) == 0, "what it sent is a masked text frame that unmasks to the message", "A client frame that is not masked is a protocol violation.");

        peer_start(&p, SCRIPT_MASKED);
        e = proven_ws_conn_connect(client, ws_url(p.port, "/"), NULL, 0, sv(""), &wcfg, &ws, NULL);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &m) == PROVEN_ERR_INVALID_FORMAT, "a masked frame from a server: PROVEN_ERR_INVALID_FORMAT", "");
        proven_ws_conn_destroy(ws);
        peer_finish(&p);
        memcpy(key, p.got + 2, 4);
        proven_ws_mask((proven_mem_mut_t){ .ptr = p.got + 6, .size = 2 }, key, 0);
        PROVEN_TEST_ASSERT(p.got_len == 8 && p.got[0] == 0x88 && p.got[1] == 0x82 && memcmp(p.got + 6, "\x03\xea", 2) == 0, "and the server was sent a masked close with code 1002", "");

        peer_start(&p, SCRIPT_BAD_UTF8);
        e = proven_ws_conn_connect(client, ws_url(p.port, "/"), NULL, 0, sv(""), &wcfg, &ws, NULL);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &m) == PROVEN_ERR_INVALID_ENCODING, "text that is not UTF-8: PROVEN_ERR_INVALID_ENCODING", "");
        proven_ws_conn_destroy(ws);
        peer_finish(&p);
        memcpy(key, p.got + 2, 4);
        proven_ws_mask((proven_mem_mut_t){ .ptr = p.got + 6, .size = 2 }, key, 0);
        PROVEN_TEST_ASSERT(p.got_len == 8 && memcmp(p.got + 6, "\x03\xef", 2) == 0, "with a close of code 1007", "");

        peer_start(&p, SCRIPT_NO_CLOSE);
        e = proven_ws_conn_connect(client, ws_url(p.port, "/"), NULL, 0, sv(""), &wcfg, &ws, NULL);
        PROVEN_TEST_ASSERT(e == PROVEN_OK, "connect to a server that will never answer a close", "");
        proven_time_t start = proven_time_monotonic_now();
        e = proven_ws_conn_close(ws, PROVEN_WS_CLOSE_NORMAL, sv(""));
        proven_i64 took = (proven_time_monotonic_now() - start) / 1000000;
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_TIMEOUT && took >= 150 && took < 5000, "close gives up after close_timeout_ms: PROVEN_ERR_TIMEOUT", "");
        proven_ws_conn_destroy(ws);
        peer_finish(&p);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a connection over any transport",
        "proven_ws_conn_open over the two ends of a socket pair: no HTTP, no handshake, both ends in this thread.",
        "Inspect proven_ws_conn_open.");
    // ---------------------------------------------------------------
    {
        proven_net_conn_t a, b;
        PROVEN_TEST_ASSERT(proven_net_pair(&a, &b) == PROVEN_OK, "a socket pair", "");
        proven_ws_conn_t *client_end = NULL, *server_end = NULL;
        proven_ws_conn_config_t cfg = { .alloc = heap };
        proven_ws_conn_config_t none = {0};
        PROVEN_TEST_ASSERT(proven_ws_conn_open(proven_net_conn_transport(&a), false, (proven_mem_view_t){0}, &none, &client_end) == PROVEN_ERR_INVALID_ARG && client_end == NULL,
            "a configuration with no allocator is PROVEN_ERR_INVALID_ARG, and the transport is still the caller's", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_open(proven_net_conn_transport(&a), false, (proven_mem_view_t){0}, &cfg, &client_end) == PROVEN_OK &&
                           proven_ws_conn_open(proven_net_conn_transport(&b), true, (proven_mem_view_t){0}, &cfg, &server_end) == PROVEN_OK, "a connection on each end", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_send_text(client_end, sv("across")) == PROVEN_OK &&
                           proven_ws_conn_receive(server_end, proven_net_deadline_in(2000), &m) == PROVEN_OK && m.text && m.data.size == 6, "client to server", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_send_binary(server_end, mv("back")) == PROVEN_OK &&
                           proven_ws_conn_receive(client_end, proven_net_deadline_in(2000), &m) == PROVEN_OK && !m.text && m.data.size == 4, "server to client", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_protocol(client_end).size == 0 && proven_ws_conn_close_code(client_end) == 0 && proven_ws_conn_pong_count(client_end) == 0,
            "an open connection with no subprotocol has no close code and no pongs yet", "");
        /* One end is destroyed without a close: the other sees the connection drop. */
        proven_ws_conn_destroy(client_end);
        PROVEN_TEST_ASSERT(proven_ws_conn_receive(server_end, proven_net_deadline_in(2000), &m) == PROVEN_ERR_RESET && proven_ws_conn_close_code(server_end) == 1006,
            "destroy without close is an abnormal closure to the peer: PROVEN_ERR_RESET, 1006", "");
        PROVEN_TEST_ASSERT(proven_ws_conn_close(server_end, PROVEN_WS_CLOSE_NORMAL, sv("")) == PROVEN_ERR_RESET, "and close on a failed connection reports that failure", "");
        proven_ws_conn_destroy(server_end);
        proven_ws_conn_destroy(NULL);
    }
    proven_http_client_destroy(client);
}

int main(void) {
    PROVEN_TEST_SUITE("ws: connections",
        "WebSocket connections over loopback: this library's two ends with each other, and each with a peer that breaks the rules.",
        "Inspect src/proven/ws_conn.c.");

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
    for (proven_size_t i = 0; i < sizeof g_big; ++i) g_big[i] = (proven_byte_t)((i * 31 + i / 251) & 0xFF);

    proven_allocator_t heap = proven_heap_allocator();
    proven_job_sys_t *loop_thread = NULL, *handlers = NULL;
    PROVEN_TEST_ASSERT(proven_job_system_init(heap, 1, 4, &loop_thread) == PROVEN_OK, "a thread for the server's loop", "");
    PROVEN_TEST_ASSERT(proven_job_system_init(heap, 2, 8, &g_workers) == PROVEN_OK, "threads for handed-off connections and scripted peers", "");
    PROVEN_TEST_ASSERT(proven_job_system_init(heap, 3, 16, &handlers) == PROVEN_OK, "threads for handlers", "");

    PROVEN_TEST_INFO("model: handlers on the loop thread");
    run_cases(NULL, loop_thread);
    PROVEN_TEST_INFO("model: handlers on a job system");
    run_cases(handlers, loop_thread);
    run_client_side();

    proven_job_system_close(handlers);
    proven_job_system_destroy(handlers);
    proven_job_system_close(g_workers);
    proven_job_system_destroy(g_workers);
    proven_job_system_close(loop_thread);
    proven_job_system_destroy(loop_thread);

    PROVEN_TEST_PASS("connections keep their contract at both ends.");
    return 0;
}
