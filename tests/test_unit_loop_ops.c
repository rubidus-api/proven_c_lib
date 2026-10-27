#include "proven.h"
#include "proven_test.h"
#include "../src/proven/proven_internal_loop.h"
#include "../src/proven/proven_internal_net.h"
#include "../src/proven/proven_internal_quic_net.h"
#include <stdio.h>
#include <string.h>

/*
 * The loop's operations (RFC-0025): receives, sends, accepts and connects that complete.
 *
 * Every case runs twice: on a loop over the system's selector (epoll on Linux) and on one over
 * the portable poll kind. The two must answer alike. On Windows both loops are the completion
 * port (RFC-0025 step 6), so the cases run on it twice.
 *
 * The rule checked throughout: an operation is answered exactly once, from the loop, and
 * never from inside the call that started it - `g_inside` is set around every such call, and
 * every callback fails the test if it finds it set.
 */

static proven_loop_t *g_loop;
static const char *g_kind;
static bool g_inside;

#define INSIDE(call) do { g_inside = true; call; g_inside = false; } while (0)

/* One connection under test, with everything its callbacks saw. */
typedef struct {
    proven_loop_sock_t sock;
    proven_net_conn_t conn;
    int received, writable, connected, closed;
    proven_err_t recv_err, send_err, conn_err;
    proven_size_t recv_n;
    int order[8], order_n;       /* 1 received, 2 writable, 3 connected, 4 closed */
    bool rearm_zero;             /* arm a zero-byte receive again from `received` */
} conn_t;

static void log_order(conn_t *c, int what) { if (c->order_n < 8) c->order[c->order_n++] = what; }

static void on_received(void *ctx, proven_err_t err, proven_size_t n) {
    conn_t *c = ctx;
    PROVEN_TEST_ASSERT(!g_inside, "received never comes from inside the call that started it", g_kind);
    PROVEN_TEST_ASSERT(c->closed == 0, "nothing comes after closed", g_kind);
    c->received++; c->recv_err = err; c->recv_n = n; log_order(c, 1);
}
static void on_writable(void *ctx, proven_err_t err) {
    conn_t *c = ctx;
    PROVEN_TEST_ASSERT(!g_inside, "writable never comes from inside the call that asked for it", g_kind);
    PROVEN_TEST_ASSERT(c->closed == 0, "nothing comes after closed", g_kind);
    c->writable++; c->send_err = err; log_order(c, 2);
}
static void on_connected(void *ctx, proven_err_t err) {
    conn_t *c = ctx;
    PROVEN_TEST_ASSERT(!g_inside, "connected never comes from inside proven_loop_connect", g_kind);
    c->connected++; c->conn_err = err; log_order(c, 3);
}
static void on_closed(void *ctx) {
    conn_t *c = ctx;
    PROVEN_TEST_ASSERT(!g_inside, "closed never comes from inside proven_loop_close", g_kind);
    c->closed++; log_order(c, 4);
    (void)proven_net_close(&c->conn);
}
static const proven_loop_sock_fns_t g_fns = { on_received, on_writable, on_connected, on_closed };

static void rounds(int n) { for (int i = 0; i < n; ++i) (void)proven_loop_poll(g_loop, PROVEN_NET_DONT_WAIT); }

/* Poll until `*count` moves past `before`, or `ms` pass. */
static bool until_moves_in(const int *count, int before, proven_u32 ms) {
    proven_net_deadline_t end = proven_net_deadline_in(ms);
    while (*count == before && proven_time_monotonic_now() < end) (void)proven_loop_poll(g_loop, proven_net_deadline_in(20));
    return *count != before;
}
static bool until_moves(const int *count, int before) { return until_moves_in(count, before, 2000); }

static void pair(conn_t *c, proven_net_conn_t *far) {
    memset(c, 0, sizeof *c);
    PROVEN_TEST_ASSERT(proven_net_pair(&c->conn, far) == PROVEN_OK, "a connected pair", g_kind);
    PROVEN_TEST_ASSERT(proven_loop_attach(g_loop, &c->sock, &c->conn, &g_fns, c) == PROVEN_OK, "attached", g_kind);
}

static void close_and_wait(conn_t *c) {
    int before = c->closed;
    INSIDE(proven_loop_close(g_loop, &c->sock));
    PROVEN_TEST_ASSERT(until_moves(&c->closed, before) && c->closed == 1, "closed comes once", g_kind);
}

static void put(proven_net_conn_t *far, const char *text) {
    proven_result_size_t w = proven_net_write_all(far, (proven_mem_view_t){ (const proven_byte_t *)text, strlen(text) }, proven_net_deadline_in(1000));
    PROVEN_TEST_ASSERT(w.err == PROVEN_OK, "the far end writes", g_kind);
}

/* Receives: zero bytes, into a buffer, the end, cancelled. */
static void case_receive(void) {
    conn_t c;
    proven_net_conn_t far;
    pair(&c, &far);

    proven_err_t e;
    INSIDE(e = proven_loop_recv(g_loop, &c.sock, (proven_mem_mut_t){ 0 }));
    PROVEN_TEST_ASSERT(e == PROVEN_OK && c.received == 0, "a zero-byte receive starts, and is not answered inside the call", g_kind);
    PROVEN_TEST_ASSERT(proven_loop_recv(g_loop, &c.sock, (proven_mem_mut_t){ 0 }) == PROVEN_ERR_BUSY, "a second receive in flight is PROVEN_ERR_BUSY", g_kind);
    rounds(3);
    PROVEN_TEST_ASSERT(c.received == 0, "with nothing sent, it waits", g_kind);
    put(&far, "hello");
    PROVEN_TEST_ASSERT(until_moves(&c.received, 0) && c.recv_err == PROVEN_OK && c.recv_n == 0, "data arriving answers it: PROVEN_OK and nothing taken", g_kind);
    rounds(3);
    PROVEN_TEST_ASSERT(c.received == 1, "once, though the data is still unread", g_kind);
    proven_byte_t buf[16];
    proven_result_size_t r = proven_net_read(&c.conn, (proven_mem_mut_t){ buf, sizeof buf }, PROVEN_NET_DONT_WAIT);
    PROVEN_TEST_ASSERT(r.err == PROVEN_OK && r.value == 5 && memcmp(buf, "hello", 5) == 0, "and the bytes are there to read", g_kind);

    /* Into a buffer. */
    put(&far, "world!");
    proven_byte_t into[32] = { 0 };
    INSIDE(e = proven_loop_recv(g_loop, &c.sock, (proven_mem_mut_t){ into, sizeof into }));
    PROVEN_TEST_ASSERT(e == PROVEN_OK && c.received == 1, "a receive into a buffer starts, not answered inside the call though data is there", g_kind);
    PROVEN_TEST_ASSERT(until_moves(&c.received, 1) && c.recv_err == PROVEN_OK && c.recv_n == 6 && memcmp(into, "world!", 6) == 0, "it brings what arrived", g_kind);

    /* Cancelled. */
    INSIDE(e = proven_loop_recv(g_loop, &c.sock, (proven_mem_mut_t){ into, sizeof into }));
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "another", g_kind);
    INSIDE(proven_loop_recv_cancel(g_loop, &c.sock));
    PROVEN_TEST_ASSERT(c.received == 2, "a cancel is not answered inside the call either", g_kind);
    PROVEN_TEST_ASSERT(until_moves(&c.received, 2) && c.recv_err == PROVEN_ERR_CANCELLED, "a cancelled receive is answered with PROVEN_ERR_CANCELLED", g_kind);
    proven_loop_recv_cancel(g_loop, &c.sock);
    put(&far, "x");
    rounds(5);
    PROVEN_TEST_ASSERT(c.received == 3, "once; cancelling with nothing in flight is harmless, and data then answers nothing", g_kind);

    /* The end: the far side closes. */
    (void)proven_net_read(&c.conn, (proven_mem_mut_t){ buf, sizeof buf }, PROVEN_NET_DONT_WAIT);
    (void)proven_net_close(&far);
    PROVEN_TEST_ASSERT(proven_loop_recv(g_loop, &c.sock, (proven_mem_mut_t){ into, sizeof into }) == PROVEN_OK, "a receive after the far end left", g_kind);
    PROVEN_TEST_ASSERT(until_moves(&c.received, 3) && c.recv_err == PROVEN_ERR_EOF, "is PROVEN_ERR_EOF", g_kind);
    PROVEN_TEST_ASSERT(proven_loop_recv(g_loop, &c.sock, (proven_mem_mut_t){ 0 }) == PROVEN_OK, "a zero-byte receive at the end", g_kind);
    PROVEN_TEST_ASSERT(until_moves(&c.received, 4) && c.recv_err == PROVEN_OK, "is answered: the read that follows says the end", g_kind);
    close_and_wait(&c);
    PROVEN_TEST_ASSERT(proven_loop_recv(g_loop, &c.sock, (proven_mem_mut_t){ 0 }) == PROVEN_ERR_INVALID_STATE, "no receive after close", g_kind);
}

/* Sends: small, larger than the socket takes, the wait, in place, and the bytes in order. */
static proven_byte_t pattern_at(proven_size_t i) { return (proven_byte_t)((i * 131u + (i >> 9)) & 0xff); }

static void case_send(void) {
    conn_t c;
    proven_net_conn_t far;
    pair(&c, &far);

    proven_result_size_t s;
    INSIDE(s = proven_loop_send(g_loop, &c.sock, (proven_mem_view_t){ (const proven_byte_t *)"abc", 3 }));
    PROVEN_TEST_ASSERT(s.err == PROVEN_OK && s.value == 3 && proven_loop_send_queued(&c.sock) == 0, "a small send goes straight to the socket", g_kind);
    proven_byte_t buf[8];
    proven_result_size_t r = proven_net_read(&far, (proven_mem_mut_t){ buf, sizeof buf }, proven_net_deadline_in(1000));
    PROVEN_TEST_ASSERT(r.err == PROVEN_OK && r.value == 3 && memcmp(buf, "abc", 3) == 0, "and arrives", g_kind);

    proven_err_t e;
    INSIDE(e = proven_loop_send_wait(g_loop, &c.sock, 0));
    PROVEN_TEST_ASSERT(e == PROVEN_OK && c.writable == 0, "a wait that is already true is not answered inside the call", g_kind);
    PROVEN_TEST_ASSERT(until_moves(&c.writable, 0) && c.send_err == PROVEN_OK, "but from the loop", g_kind);

    /* More than the socket takes: the loop holds the rest, up to its limit. */
    enum { TOTAL = 3 * 1024 * 1024 };
    static proven_byte_t src[65536];
    proven_size_t given = 0, got = 0;
    bool in_order = true, refused = false, waiting = false;
    int writable_at_wait = 0;
    proven_size_t most_queued = 0;
    proven_net_deadline_t end = proven_net_deadline_in(20000);
    while (got < TOTAL && proven_time_monotonic_now() < end) {
        if (waiting && c.writable != writable_at_wait) {
            PROVEN_TEST_ASSERT(c.send_err == PROVEN_OK && proven_loop_send_queued(&c.sock) <= 16384, "writable came when the bytes waiting were down to what was asked", g_kind);
            waiting = false;
        }
        if (!waiting && given < TOTAL) {
            proven_size_t n = TOTAL - given < sizeof src ? TOTAL - given : sizeof src;
            for (proven_size_t i = 0; i < n; ++i) src[i] = pattern_at(given + i);
            INSIDE(s = proven_loop_send(g_loop, &c.sock, (proven_mem_view_t){ src, n }));
            given += s.value;
            if (proven_loop_send_queued(&c.sock) > most_queued) most_queued = proven_loop_send_queued(&c.sock);
            if (s.err == PROVEN_ERR_AGAIN) {
                refused = waiting = true;
                writable_at_wait = c.writable;
                PROVEN_TEST_ASSERT(proven_loop_send_wait(g_loop, &c.sock, 16384) == PROVEN_OK, "a wait for room", g_kind);
            } else {
                PROVEN_TEST_ASSERT(s.err == PROVEN_OK && s.value == n, "a send that fits is taken whole", g_kind);
            }
        }
        (void)proven_loop_poll(g_loop, PROVEN_NET_DONT_WAIT);
        /* The far end reads less per turn than is sent, so that the socket fills. */
        static proven_byte_t sink[16384];
        r = proven_net_read(&far, (proven_mem_mut_t){ sink, sizeof sink }, PROVEN_NET_DONT_WAIT);
        if (r.err == PROVEN_OK) {
            for (proven_size_t i = 0; i < r.value; ++i) in_order = in_order && sink[i] == pattern_at(got + i);
            got += r.value;
        }
    }
    PROVEN_TEST_ASSERT(got == TOTAL && in_order, "three MiB sent through the loop arrive whole and in order", g_kind);
    PROVEN_TEST_ASSERT(refused, "the far end reading slowly made a send refuse the rest", g_kind);
    PROVEN_TEST_ASSERT(most_queued <= 65536 + 65536, "the loop never held much more than its 64 KiB limit", g_kind);
    PROVEN_TEST_ASSERT(proven_loop_send_queued(&c.sock) == 0, "and holds nothing after", g_kind);

    /* In place (the drivers' way): reserved, written, committed; and then a plain send, which
     * must come after. */
    proven_mem_mut_t room;
    PROVEN_TEST_ASSERT(proven_loop_reserve_(g_loop, &c.sock, 10, &room) == PROVEN_OK && room.size >= 10, "room reserved", g_kind);
    PROVEN_TEST_ASSERT(proven_loop_reserve_(g_loop, &c.sock, PROVEN_LOOP_CHUNK_ + 1, &room) == PROVEN_ERR_INVALID_ARG, "no more than a chunk at once", g_kind);
    PROVEN_TEST_ASSERT(proven_loop_reserve_(g_loop, &c.sock, 10, &room) == PROVEN_OK, "again", g_kind);
    memcpy(room.ptr, "head:", 5);
    INSIDE(proven_loop_commit_(g_loop, &c.sock, 5));
    PROVEN_TEST_ASSERT(proven_loop_send_queued(&c.sock) == 5, "committed bytes wait for the loop", g_kind);
    s = proven_loop_send(g_loop, &c.sock, (proven_mem_view_t){ (const proven_byte_t *)"body", 4 });
    PROVEN_TEST_ASSERT(s.err == PROVEN_OK && s.value == 4 && proven_loop_send_queued(&c.sock) == 9, "a send behind them waits too", g_kind);
    rounds(2);
    r = proven_net_read(&far, (proven_mem_mut_t){ buf, sizeof buf }, proven_net_deadline_in(1000));
    proven_byte_t whole[16];
    proven_size_t have = r.err == PROVEN_OK ? r.value : 0;
    memcpy(whole, buf, have);
    if (have < 9) { r = proven_net_read(&far, (proven_mem_mut_t){ whole + have, sizeof whole - have }, proven_net_deadline_in(1000)); if (r.err == PROVEN_OK) have += r.value; }
    PROVEN_TEST_ASSERT(have == 9 && memcmp(whole, "head:body", 9) == 0, "they arrive in order", g_kind);

    /* Several pieces at once (the drivers' head and body): small ones put together, large ones
     * in one gather write; in order either way. */
    static proven_byte_t big[3][3000];
    for (int k = 0; k < 3; ++k) memset(big[k], 'a' + k, sizeof big[k]);
    const proven_mem_view_t small_parts[3] = { { (const proven_byte_t *)"one,", 4 }, { (const proven_byte_t *)"two,", 4 }, { (const proven_byte_t *)"three", 5 } };
    const proven_mem_view_t big_parts[3] = { { big[0], sizeof big[0] }, { big[1], sizeof big[1] }, { big[2], sizeof big[2] } };
    s = proven_loop_sendv_(g_loop, &c.sock, small_parts, 3);
    proven_result_size_t s2 = proven_loop_sendv_(g_loop, &c.sock, big_parts, 3);
    PROVEN_TEST_ASSERT(s.err == PROVEN_OK && s.value == 13 && s2.err == PROVEN_OK && s2.value == 9000, "pieces taken whole", g_kind);
    static proven_byte_t back[9013];
    proven_size_t back_n = 0;
    while (back_n < sizeof back) {
        rounds(1);
        r = proven_net_read(&far, (proven_mem_mut_t){ back + back_n, sizeof back - back_n }, proven_net_deadline_in(1000));
        if (r.err != PROVEN_OK) break;
        back_n += r.value;
    }
    bool pieces_in_order = back_n == sizeof back && memcmp(back, "one,two,three", 13) == 0;
    for (proven_size_t i = 0; i < 9000 && pieces_in_order; ++i) pieces_in_order = back[13 + i] == (proven_byte_t)('a' + i / 3000);
    PROVEN_TEST_ASSERT(pieces_in_order, "and arrive in order, small and large", g_kind);

    close_and_wait(&c);
    s = proven_loop_send(g_loop, &c.sock, (proven_mem_view_t){ (const proven_byte_t *)"x", 1 });
    PROVEN_TEST_ASSERT(s.err == PROVEN_ERR_INVALID_STATE && s.value == 0, "no send after close", g_kind);
    PROVEN_TEST_ASSERT(proven_loop_send_wait(g_loop, &c.sock, 0) == PROVEN_ERR_INVALID_STATE, "no wait after close", g_kind);
    (void)proven_net_close(&far);
}

/* Fill the socket and the loop's buffers; return how much was taken. */
static proven_size_t fill(conn_t *c) {
    static proven_byte_t src[65536];
    proven_size_t taken = 0;
    for (int i = 0; i < 400; ++i) {
        proven_result_size_t s = proven_loop_send(g_loop, &c->sock, (proven_mem_view_t){ src, sizeof src });
        taken += s.value;
        if (s.err != PROVEN_OK) break;
    }
    return taken;
}

/* Pieces sent together and cut by the socket at a byte inside them: the rest must be held from
 * exactly there. The cut is made on demand with a test-only cap on what one send offers: 700
 * bytes of three small pieces put together (one send), then 2,500 of three large ones (one
 * gather write). */
static void case_sendv_cut(void) {
    conn_t c;
    proven_net_conn_t far;
    pair(&c, &far);
    static proven_byte_t p6[6][1500];
    for (int k = 0; k < 6; ++k) for (int i = 0; i < 1500; ++i) p6[k][i] = (proven_byte_t)(k * 37 + i);
    const proven_mem_view_t small_parts[3] = { { p6[0], 500 }, { p6[1], 500 }, { p6[2], 500 } };
    const proven_mem_view_t big_parts[3] = { { p6[3], 1500 }, { p6[4], 1500 }, { p6[5], 1500 } };
    proven_net_write_cap_for_test_(700);
    proven_result_size_t s = proven_loop_sendv_(g_loop, &c.sock, small_parts, 3);
    const proven_size_t held_small = proven_loop_send_queued(&c.sock);
    proven_loop_close(g_loop, &c.sock);              /* nothing of it is sent: what was held is dropped */
    proven_net_write_cap_for_test_(0);
    PROVEN_TEST_ASSERT(s.err == PROVEN_OK && s.value == 1500 && held_small == 800, "small pieces put together: 700 went at once, 800 held", g_kind);
    (void)until_moves(&c.closed, 0);
    static proven_byte_t got[6000];
    proven_result_size_t r = proven_net_read(&far, (proven_mem_mut_t){ got, sizeof got }, proven_net_deadline_in(1000));
    bool same = r.err == PROVEN_OK && r.value == 700 && memcmp(got, p6[0], 500) == 0 && memcmp(got + 500, p6[1], 200) == 0;
    PROVEN_TEST_ASSERT(same, "and the 700 are the first 700 bytes of the pieces", g_kind);
    (void)proven_net_close(&far);

    /* Now kept open: the rest follows from the right byte, for both kinds of send. */
    pair(&c, &far);
    proven_net_write_cap_for_test_(700);
    s = proven_loop_sendv_(g_loop, &c.sock, small_parts, 3);
    proven_net_write_cap_for_test_(0);
    proven_result_size_t s2 = proven_loop_sendv_(g_loop, &c.sock, big_parts, 3);   /* behind what is held: held too */
    PROVEN_TEST_ASSERT(s.value == 1500 && s2.value == 4500 && proven_loop_send_queued(&c.sock) == 800 + 4500, "the rest held, and the large pieces behind it", g_kind);
    proven_size_t n = 0;
    while (n < 6000) {
        rounds(1);
        r = proven_net_read(&far, (proven_mem_mut_t){ got + n, sizeof got - n }, proven_net_deadline_in(1000));
        if (r.err != PROVEN_OK) break;
        n += r.value;
    }
    same = n == 6000;
    for (int k = 0; k < 3 && same; ++k) same = memcmp(got + k * 500, p6[k], 500) == 0 && memcmp(got + 1500 + k * 1500, p6[3 + k], 1500) == 0;
    PROVEN_TEST_ASSERT(same, "everything arrives once, in order", g_kind);
    close_and_wait(&c);
    (void)proven_net_close(&far);

    /* A gather write cut inside its second piece. */
    pair(&c, &far);
    proven_net_write_cap_for_test_(2500);
    s2 = proven_loop_sendv_(g_loop, &c.sock, big_parts, 3);
    proven_net_write_cap_for_test_(0);
    PROVEN_TEST_ASSERT(s2.value == 4500 && proven_loop_send_queued(&c.sock) == 2000, "large pieces in one gather write: 2,500 went, 2,000 held", g_kind);
    n = 0;
    while (n < 4500) {
        rounds(1);
        r = proven_net_read(&far, (proven_mem_mut_t){ got + n, sizeof got - n }, proven_net_deadline_in(1000));
        if (r.err != PROVEN_OK) break;
        n += r.value;
    }
    same = n == 4500;
    for (int k = 0; k < 3 && same; ++k) same = memcmp(got + k * 1500, p6[3 + k], 1500) == 0;
    PROVEN_TEST_ASSERT(same, "and the rest follows from the byte where it was cut", g_kind);
    close_and_wait(&c);
    (void)proven_net_close(&far);
}

/* Close with everything in flight; a send that fails in the loop. */
static void case_close_and_fail(void) {
    conn_t c;
    proven_net_conn_t far;
    pair(&c, &far);
    (void)fill(&c);
    PROVEN_TEST_ASSERT(proven_loop_send_queued(&c.sock) > 0, "bytes wait", g_kind);
    PROVEN_TEST_ASSERT(proven_loop_recv(g_loop, &c.sock, (proven_mem_mut_t){ 0 }) == PROVEN_OK && proven_loop_send_wait(g_loop, &c.sock, 0) == PROVEN_OK, "a receive and a wait in flight", g_kind);
    INSIDE(proven_loop_close(g_loop, &c.sock));
    PROVEN_TEST_ASSERT(c.received == 0 && c.writable == 0 && c.closed == 0, "close answers nothing inside the call", g_kind);
    INSIDE(proven_loop_close(g_loop, &c.sock));
    PROVEN_TEST_ASSERT(until_moves(&c.closed, 0), "closed comes", g_kind);
    rounds(3);
    PROVEN_TEST_ASSERT(c.received == 1 && c.recv_err == PROVEN_ERR_CANCELLED && c.writable == 1 && c.send_err == PROVEN_ERR_CANCELLED && c.closed == 1,
        "the receive and the wait are cancelled, each once, and closed once though close was called twice", g_kind);
    PROVEN_TEST_ASSERT(c.order_n == 3 && c.order[2] == 4, "closed comes last", g_kind);
    PROVEN_TEST_ASSERT(proven_loop_send_queued(&c.sock) == 0, "what waited was dropped", g_kind);
    (void)proven_net_close(&far);

    /* A send that fails in the loop: the far end goes away with bytes unread (a reset). */
    pair(&c, &far);
    (void)fill(&c);
    PROVEN_TEST_ASSERT(proven_loop_send_queued(&c.sock) > 0, "bytes wait again", g_kind);
    (void)proven_net_close(&far);
    PROVEN_TEST_ASSERT(until_moves(&c.writable, 0) && c.send_err != PROVEN_OK && c.send_err != PROVEN_ERR_CANCELLED,
        "the failure comes through writable, though no wait was asked for", g_kind);
    proven_err_t failed = c.send_err;
    proven_result_size_t s = proven_loop_send(g_loop, &c.sock, (proven_mem_view_t){ (const proven_byte_t *)"x", 1 });
    PROVEN_TEST_ASSERT(s.err == failed && s.value == 0 && proven_loop_send_queued(&c.sock) == 0, "and every later send returns it, taking nothing", g_kind);
    close_and_wait(&c);
    PROVEN_TEST_ASSERT(c.writable == 1, "writable came once", g_kind);
}

/* Accepts, and connects. */
typedef struct {
    proven_loop_accept_t acc;
    proven_net_listener_t listener;
    proven_net_conn_t got[8];
    int count, calls, want;
    proven_err_t last;
} acceptor_t;

static void on_accept(void *ctx, proven_err_t err, proven_net_conn_t *conn) {
    acceptor_t *a = ctx;
    PROVEN_TEST_ASSERT(!g_inside, "an accept is never answered inside the call that started it", g_kind);
    a->calls++;
    a->last = err;
    if (err != PROVEN_OK) return;
    PROVEN_TEST_ASSERT(proven_net_conn_is_open(conn) && a->count < 8, "an open connection", g_kind);
    a->got[a->count++] = *conn;
    if (a->count < a->want) {
        g_inside = true;
        proven_err_t e = proven_loop_accept(g_loop, &a->acc, &a->listener, on_accept, a);
        g_inside = false;
        PROVEN_TEST_ASSERT(e == PROVEN_OK, "armed again from inside its own function", g_kind);
    }
}

static void case_accept_connect(void) {
    acceptor_t a;
    memset(&a, 0, sizeof a);
    proven_net_addr_t at = proven_net_addr_ipv4(127, 0, 0, 1, 0), bound;
    PROVEN_TEST_ASSERT(proven_net_listen(at, 16, &a.listener, &bound) == PROVEN_OK, "a listener", g_kind);
    a.want = 3;
    proven_err_t e;
    INSIDE(e = proven_loop_accept(g_loop, &a.acc, &a.listener, on_accept, &a));
    PROVEN_TEST_ASSERT(e == PROVEN_OK && proven_loop_accept(g_loop, &a.acc, &a.listener, on_accept, &a) == PROVEN_ERR_BUSY, "an accept in flight; a second is PROVEN_ERR_BUSY", g_kind);
    proven_net_conn_t clients[3];
    for (int i = 0; i < 3; ++i) PROVEN_TEST_ASSERT(proven_net_connect(bound, proven_net_deadline_in(2000), &clients[i]) == PROVEN_OK, "a client connects", g_kind);
    proven_net_deadline_t end = proven_net_deadline_in(3000);
    while (a.count < 3 && proven_time_monotonic_now() < end) (void)proven_loop_poll(g_loop, proven_net_deadline_in(20));
    PROVEN_TEST_ASSERT(a.count == 3 && a.calls == 3, "three connections, each answered once, re-armed from inside", g_kind);

    /* Cancelled, with one in flight. */
    PROVEN_TEST_ASSERT(proven_loop_accept(g_loop, &a.acc, &a.listener, on_accept, &a) == PROVEN_OK, "armed once more", g_kind);
    INSIDE(proven_loop_accept_cancel(g_loop, &a.acc));
    PROVEN_TEST_ASSERT(a.calls == 3, "the cancel is not answered inside the call", g_kind);
    PROVEN_TEST_ASSERT(until_moves(&a.calls, 3) && a.last == PROVEN_ERR_CANCELLED, "but with PROVEN_ERR_CANCELLED, from the loop", g_kind);
    proven_loop_accept_cancel(g_loop, &a.acc);
    rounds(3);
    PROVEN_TEST_ASSERT(a.calls == 4, "once; a cancel with nothing in flight is harmless", g_kind);

    /* A connect through the loop to the same listener, accepted by the blocking call. */
    conn_t c;
    memset(&c, 0, sizeof c);
    INSIDE(e = proven_loop_connect(g_loop, &c.sock, bound, &c.conn, &g_fns, &c));
    PROVEN_TEST_ASSERT(e == PROVEN_OK && c.connected == 0, "a connect begins, not answered inside the call", g_kind);
    PROVEN_TEST_ASSERT(proven_loop_recv(g_loop, &c.sock, (proven_mem_mut_t){ 0 }) == PROVEN_ERR_INVALID_STATE, "no receive before it has connected", g_kind);
    PROVEN_TEST_ASSERT(until_moves(&c.connected, 0) && c.conn_err == PROVEN_OK, "connected", g_kind);
    proven_net_conn_t server;
    PROVEN_TEST_ASSERT(proven_net_accept(&a.listener, proven_net_deadline_in(2000), &server, NULL) == PROVEN_OK, "the other end", g_kind);
    proven_result_size_t s = proven_loop_send(g_loop, &c.sock, (proven_mem_view_t){ (const proven_byte_t *)"ping", 4 });
    proven_byte_t buf[8];
    proven_result_size_t r = proven_net_read(&server, (proven_mem_mut_t){ buf, sizeof buf }, proven_net_deadline_in(1000));
    PROVEN_TEST_ASSERT(s.err == PROVEN_OK && r.err == PROVEN_OK && r.value == 4 && memcmp(buf, "ping", 4) == 0, "and the connection carries bytes", g_kind);
    close_and_wait(&c);
    (void)proven_net_close(&server);

    /* A connect to where nothing listens. */
    proven_net_listener_t gone;
    proven_net_addr_t gone_at;
    PROVEN_TEST_ASSERT(proven_net_listen(at, 1, &gone, &gone_at) == PROVEN_OK, "a port", g_kind);
    (void)proven_net_listener_close(&gone);
    memset(&c, 0, sizeof c);
    e = proven_loop_connect(g_loop, &c.sock, gone_at, &c.conn, &g_fns, &c);
    if (e == PROVEN_OK) {
        /* Windows tries a refused connect again for about two seconds before it says so. */
        PROVEN_TEST_ASSERT(until_moves_in(&c.connected, 0, 6000) && c.conn_err == PROVEN_ERR_REFUSED, "a refused connect is answered with PROVEN_ERR_REFUSED", g_kind);
        close_and_wait(&c);
    } else {
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_REFUSED && !proven_net_conn_is_open(&c.conn), "or refused at once, with nothing to close", g_kind);
    }

    for (int i = 0; i < 3; ++i) { (void)proven_net_close(&clients[i]); (void)proven_net_close(&a.got[i]); }
    (void)proven_net_listener_close(&a.listener);
}

/* UDP (internal, for HTTP/3): receives into a buffer, re-armed from inside, closed with one in
 * flight; a send wait. */
typedef struct {
    proven_loop_udp_t lu;
    proven_net_udp_t udp;
    proven_byte_t buf[2048];
    proven_net_addr_t from;
    int received, writable, closed, rearm, order[8], order_n;
    proven_err_t err;
    proven_size_t total;
} udp_case_t;

static void u_received(void *ctx, proven_err_t err, proven_size_t n, proven_size_t seg) {
    udp_case_t *u = ctx;
    (void)seg;
    PROVEN_TEST_ASSERT(!g_inside, "a UDP receive is never answered inside the call that started it", g_kind);
    u->received++; u->err = err;
    if (u->order_n < 8) u->order[u->order_n++] = 1;
    if (err != PROVEN_OK) return;
    u->total += n;
    if (u->rearm > 0) {
        u->rearm--;
        g_inside = true;
        proven_err_t e = proven_loop_udp_recv_(g_loop, &u->lu, (proven_mem_mut_t){ u->buf, sizeof u->buf }, &u->from);
        g_inside = false;
        PROVEN_TEST_ASSERT(e == PROVEN_OK, "armed again from inside", g_kind);
    }
}
static void u_writable(void *ctx, proven_err_t err) {
    udp_case_t *u = ctx;
    PROVEN_TEST_ASSERT(!g_inside, "a UDP wait is never answered inside the call", g_kind);
    u->writable++; u->err = err;
    if (u->order_n < 8) u->order[u->order_n++] = 2;
}
static void u_closed(void *ctx) {
    udp_case_t *u = ctx;
    u->closed++;
    if (u->order_n < 8) u->order[u->order_n++] = 4;
}
static const proven_loop_udp_fns_t g_ufns = { u_received, u_writable, u_closed };

static void case_udp(void) {
    static udp_case_t u;
    memset(&u, 0, sizeof u);
    proven_net_udp_t peer;
    proven_net_addr_t at, peer_at;
    PROVEN_TEST_ASSERT(proven_net_udp_open(proven_net_addr_ipv4(127, 0, 0, 1, 0), &u.udp, &at) == PROVEN_OK &&
                       proven_net_udp_open(proven_net_addr_ipv4(127, 0, 0, 1, 0), &peer, &peer_at) == PROVEN_OK, "two UDP sockets", g_kind);
    PROVEN_TEST_ASSERT(proven_loop_udp_attach_(g_loop, &u.lu, &u.udp, &g_ufns, &u) == PROVEN_OK, "attached", g_kind);
    proven_err_t e;
    INSIDE(e = proven_loop_udp_recv_(g_loop, &u.lu, (proven_mem_mut_t){ u.buf, sizeof u.buf }, &u.from));
    PROVEN_TEST_ASSERT(e == PROVEN_OK && proven_loop_udp_recv_(g_loop, &u.lu, (proven_mem_mut_t){ u.buf, sizeof u.buf }, &u.from) == PROVEN_ERR_BUSY,
        "a receive in flight; a second is PROVEN_ERR_BUSY", g_kind);
    rounds(3);
    PROVEN_TEST_ASSERT(u.received == 0, "with nothing sent, it waits", g_kind);
    /* Five datagrams at once: the first answers the receive, the rest are taken by receives armed
     * again from inside, each still answered from the loop. */
    for (int i = 0; i < 5; ++i) PROVEN_TEST_ASSERT(proven_net_udp_send_to(&peer, at, (proven_mem_view_t){ (const proven_byte_t *)"datagram", 8 }, proven_net_deadline_in(1000)) == PROVEN_OK, "a datagram sent", g_kind);
    u.rearm = 4;
    (void)until_moves(&u.received, 0);
    for (int i = 0; i < 50 && u.received < 5; ++i) rounds(1);
    PROVEN_TEST_ASSERT(u.received == 5 && u.err == PROVEN_OK && u.total == 40 && u.from.port == peer_at.port, "five datagrams, five answers, from the sender", g_kind);
    /* A send wait: the socket can take datagrams, so writable comes - from the loop. */
    INSIDE(e = proven_loop_udp_send_wait_(g_loop, &u.lu));
    PROVEN_TEST_ASSERT(e == PROVEN_OK && u.writable == 0 && until_moves(&u.writable, 0) && u.err == PROVEN_OK, "a send wait is answered from the loop", g_kind);
    /* Closed with a receive in flight: CANCELLED, then closed last. */
    u.order_n = 0;
    PROVEN_TEST_ASSERT(proven_loop_udp_recv_(g_loop, &u.lu, (proven_mem_mut_t){ u.buf, sizeof u.buf }, &u.from) == PROVEN_OK, "another receive", g_kind);
    INSIDE(proven_loop_udp_close_(g_loop, &u.lu));
    PROVEN_TEST_ASSERT(u.closed == 0 && until_moves(&u.closed, 0), "closed comes, from the loop", g_kind);
    PROVEN_TEST_ASSERT(u.order_n == 2 && u.order[0] == 1 && u.err == PROVEN_ERR_CANCELLED && u.order[1] == 4, "the receive cancelled, then closed", g_kind);
    PROVEN_TEST_ASSERT(proven_loop_udp_recv_(g_loop, &u.lu, (proven_mem_mut_t){ u.buf, sizeof u.buf }, &u.from) == PROVEN_ERR_INVALID_STATE, "no receive after close", g_kind);
    (void)proven_net_udp_close(&u.udp);
    (void)proven_net_udp_close(&peer);
}

/* Unix-domain sockets through the loop: accepted (on Windows, AcceptEx with the family's own
 * provider), connected (on Windows, a blocking connect for its moment), and carrying bytes both
 * ways. Where the family cannot be opened the case says so and is passed over. */
static void case_unix(void) {
    proven_u8str_view_t path = PROVEN_LIT("proven_test_loop_ops.sock");
    (void)proven_fs_remove(proven_heap_allocator(), path);
    proven_net_addr_t at;
    PROVEN_TEST_ASSERT(proven_net_addr_unix(path, &at) == PROVEN_OK, "a path", g_kind);
    acceptor_t a;
    memset(&a, 0, sizeof a);
    const proven_err_t le = proven_net_listen(at, 8, &a.listener, NULL);
    if (le == PROVEN_ERR_UNSUPPORTED || le == PROVEN_ERR_PERMISSION) {
        PROVEN_TEST_INFO("SKIP: Unix-domain sockets cannot be opened here (error {}).", PROVEN_ARG((int)le));
        return;
    }
    PROVEN_TEST_ASSERT(le == PROVEN_OK, "a listener on a path", g_kind);
    a.want = 2;
    proven_err_t e;
    INSIDE(e = proven_loop_accept(g_loop, &a.acc, &a.listener, on_accept, &a));
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "an accept in flight on the path", g_kind);
    proven_net_conn_t clients[2];
    for (int i = 0; i < 2; ++i) PROVEN_TEST_ASSERT(proven_net_connect(at, proven_net_deadline_in(2000), &clients[i]) == PROVEN_OK, "a client connects by path", g_kind);
    proven_net_deadline_t end = proven_net_deadline_in(3000);
    while (a.count < 2 && proven_time_monotonic_now() < end) (void)proven_loop_poll(g_loop, proven_net_deadline_in(20));
    PROVEN_TEST_ASSERT(a.count == 2 && a.calls == 2 && a.got[0].internal.family == PROVEN_NET_FAMILY_UNIX, "two connections accepted by the loop, re-armed from inside", g_kind);

    /* What was accepted is a working socket: the loop receives on it, and it answers. */
    conn_t c;
    memset(&c, 0, sizeof c);
    c.conn = a.got[0];
    PROVEN_TEST_ASSERT(proven_loop_attach(g_loop, &c.sock, &c.conn, &g_fns, &c) == PROVEN_OK, "the accepted socket attached", g_kind);
    proven_byte_t into[32] = { 0 };
    PROVEN_TEST_ASSERT(proven_loop_recv(g_loop, &c.sock, (proven_mem_mut_t){ into, sizeof into }) == PROVEN_OK, "a receive on it", g_kind);
    put(&clients[0], "by path");
    PROVEN_TEST_ASSERT(until_moves(&c.received, 0) && c.recv_err == PROVEN_OK && c.recv_n == 7 && memcmp(into, "by path", 7) == 0, "brings what the client wrote", g_kind);
    proven_result_size_t s = proven_loop_send(g_loop, &c.sock, (proven_mem_view_t){ (const proven_byte_t *)"back", 4 });
    proven_byte_t buf[8];
    proven_result_size_t r = proven_net_read(&clients[0], (proven_mem_mut_t){ buf, sizeof buf }, proven_net_deadline_in(1000));
    PROVEN_TEST_ASSERT(s.err == PROVEN_OK && r.err == PROVEN_OK && r.value == 4 && memcmp(buf, "back", 4) == 0, "and the loop's send reaches the client", g_kind);
    (void)proven_net_close(&clients[0]);
    PROVEN_TEST_ASSERT(proven_loop_recv(g_loop, &c.sock, (proven_mem_mut_t){ into, sizeof into }) == PROVEN_OK &&
                       until_moves(&c.received, 1) && c.recv_err == PROVEN_ERR_EOF, "the client gone: PROVEN_ERR_EOF", g_kind);
    close_and_wait(&c);

    /* A connect through the loop to the path, accepted by the blocking call. */
    conn_t d;
    memset(&d, 0, sizeof d);
    INSIDE(e = proven_loop_connect(g_loop, &d.sock, at, &d.conn, &g_fns, &d));
    PROVEN_TEST_ASSERT(e == PROVEN_OK && d.connected == 0, "a connect by path begins, not answered inside the call", g_kind);
    PROVEN_TEST_ASSERT(until_moves(&d.connected, 0) && d.conn_err == PROVEN_OK, "connected", g_kind);
    proven_net_conn_t server;
    PROVEN_TEST_ASSERT(proven_net_accept(&a.listener, proven_net_deadline_in(2000), &server, NULL) == PROVEN_OK, "the other end", g_kind);
    s = proven_loop_send(g_loop, &d.sock, (proven_mem_view_t){ (const proven_byte_t *)"ping", 4 });
    r = proven_net_read(&server, (proven_mem_mut_t){ buf, sizeof buf }, proven_net_deadline_in(1000));
    PROVEN_TEST_ASSERT(s.err == PROVEN_OK && r.err == PROVEN_OK && r.value == 4 && memcmp(buf, "ping", 4) == 0, "and the connection carries bytes", g_kind);
    close_and_wait(&d);
    (void)proven_net_close(&server);

    proven_loop_accept_cancel(g_loop, &a.acc);           /* nothing in flight: forgets the listener */
    rounds(3);
    (void)proven_net_close(&clients[1]);
    (void)proven_net_close(&a.got[1]);
    (void)proven_net_listener_close(&a.listener);
    (void)proven_fs_remove(proven_heap_allocator(), path);
}

/* Batched datagrams through the loop (internal, for HTTP/3): sent in one call where the platform
 * can, received by the loop's receives - on Windows an overlapped WSARecvMsg, with URO asked for,
 * whose segment size comes back in UDP_COALESCED_INFO. Whether the system coalesced is printed,
 * never asserted; that every datagram arrives whole, in order and of its size is. A receive that
 * held several but lost its segment size would come out as one datagram of their sum. */
typedef struct {
    proven_loop_udp_t lu;
    proven_net_udp_t udp;
    proven_byte_t buf[65536];
    proven_net_addr_t from;
    proven_size_t sizes[128];
    unsigned count, batches;
    int received;
    bool bad;
    unsigned run;
} useg_t;

static proven_byte_t seg_pattern(unsigned run, unsigned dg, proven_size_t i) { return (proven_byte_t)(run * 31 + dg * 7 + i * 13 + (i >> 8)); }

static void s_received(void *ctx, proven_err_t err, proven_size_t n, proven_size_t seg) {
    useg_t *u = ctx;
    PROVEN_TEST_ASSERT(!g_inside, "a UDP receive is never answered inside the call that started it", g_kind);
    u->received++;
    if (err != PROVEN_OK) { u->bad = true; return; }
    if (seg) ++u->batches;
    const proven_size_t step = seg ? seg : n;
    for (proven_size_t off = 0; off < n; off += step) {
        const proven_size_t len = n - off < step ? n - off : step;
        if (u->count >= 128) { u->bad = true; return; }
        for (proven_size_t i = 0; i < len; ++i) if (u->buf[off + i] != seg_pattern(u->run, u->count, i)) u->bad = true;
        u->sizes[u->count++] = len;
    }
    g_inside = true;
    const proven_err_t e = proven_loop_udp_recv_(g_loop, &u->lu, (proven_mem_mut_t){ u->buf, sizeof u->buf }, &u->from);
    g_inside = false;
    if (e != PROVEN_OK) u->bad = true;
}
static void s_closed(void *ctx) { (void)ctx; }
static const proven_loop_udp_fns_t g_sfns = { s_received, NULL, s_closed };

static void case_udp_segments(void) {
    static useg_t u;
    static proven_byte_t out[PROVEN_NET_UDP_BATCH_BYTES];
    struct { proven_size_t seg; unsigned count; proven_size_t tail; } shapes[] = {
        { 1452, 1, 0 }, { 1452, 2, 0 }, { 1452, 44, 0 }, { 1452, 44, 300 }, { 1200, 53, 1 }, { 60, 64, 0 }, { 60, 63, 20 },
    };
    unsigned run = 0;
    for (int forced = 0; forced < 2; ++forced) {
        proven_net_udp_segments_off_for_test_(forced == 1);
        memset(&u, 0, sizeof u);
        proven_net_udp_t peer;
        proven_net_addr_t at;
        /* GRO (URO) asked for before anything is sent: once on, it stays on. */
        PROVEN_TEST_ASSERT(proven_net_udp_open(proven_net_addr_ipv4(127, 0, 0, 1, 0), &u.udp, &at) == PROVEN_OK &&
                           proven_net_udp_open(proven_net_addr_ipv4(127, 0, 0, 1, 0), &peer, NULL) == PROVEN_OK, "two UDP sockets", g_kind);
        const bool gro = proven_net_udp_gro_(&u.udp);
        if (forced) PROVEN_TEST_ASSERT(!gro, "with the one-at-a-time way forced, no URO either", g_kind);
        const proven_err_t ae = proven_loop_udp_attach_(g_loop, &u.lu, &u.udp, &g_sfns, &u);
        const proven_err_t re = proven_loop_udp_recv_(g_loop, &u.lu, (proven_mem_mut_t){ u.buf, sizeof u.buf }, &u.from);
        PROVEN_TEST_ASSERT(ae == PROVEN_OK && re == PROVEN_OK, "attached, a receive in flight", g_kind);
        for (unsigned k = 0; k < sizeof shapes / sizeof shapes[0]; ++k, ++run) {
            const unsigned n = shapes[k].count + (shapes[k].tail ? 1u : 0u);
            proven_size_t total = 0;
            for (unsigned d = 0; d < n; ++d) {
                const proven_size_t len = d < shapes[k].count ? shapes[k].seg : shapes[k].tail;
                for (proven_size_t i = 0; i < len; ++i) out[total + i] = seg_pattern(run, d, i);
                total += len;
            }
            u.run = run; u.count = 0;
            proven_size_t sent = 0;
            PROVEN_TEST_ASSERT(proven_net_udp_send_segments_(&peer, at, (proven_mem_view_t){ out, total }, shapes[k].seg, &sent) == PROVEN_OK && sent == total,
                               "a batch is sent whole", g_kind);
            const proven_net_deadline_t end = proven_net_deadline_in(3000);
            while (u.count < n && !u.bad && proven_time_monotonic_now() < end) (void)proven_loop_poll(g_loop, proven_net_deadline_in(20));
            bool shape = !u.bad && u.count == n;
            for (unsigned d = 0; shape && d < n; ++d) shape = u.sizes[d] == (d < shapes[k].count ? shapes[k].seg : shapes[k].tail);
            if (!shape) printf("  FAILED: %u x %zu + %zu: %u datagrams received%s\n", shapes[k].count, shapes[k].seg, shapes[k].tail, u.count, u.bad ? ", a byte or an answer wrong" : "");
            PROVEN_TEST_ASSERT(shape, "the loop's receives bring every datagram, its size and every byte, in order", g_kind);
        }
        printf("  %s, %s: receives through the loop that held several datagrams: %u (the system's choice)\n",
               g_kind, forced ? "one at a time (forced)" : gro ? "the platform's way, URO/GRO on" : "the platform's way, no URO/GRO here", u.batches);
        int closed_before = u.received;
        proven_loop_udp_close_(g_loop, &u.lu);
        (void)until_moves(&u.received, closed_before);      /* the receive in flight, CANCELLED */
        rounds(3);
        (void)proven_net_udp_close(&u.udp);
        (void)proven_net_udp_close(&peer);
    }
    proven_net_udp_segments_off_for_test_(false);
}

/* Destroyed with things in flight: nothing is called, and nothing leaks (the sanitizer runs
 * say so). */
/* The loops the cases run on: the system's default, the poll kind of selector, and io_uring. */
enum { ON_DEFAULT, ON_POLL, ON_URING };
static proven_err_t make_loop(proven_allocator_t heap, int on, proven_loop_t **out) {
    if (on == ON_POLL) return proven_loop_create_poll_(heap, out);
    if (on == ON_URING) return proven_loop_create_ex(heap, &(proven_loop_options_t){ .backend = PROVEN_LOOP_BACKEND_URING }, out);
    return proven_loop_create(heap, out);
}

static void case_destroy(proven_allocator_t heap, int on) {
    proven_loop_t *l = NULL;
    PROVEN_TEST_ASSERT(make_loop(heap, on, &l) == PROVEN_OK, "a second loop", g_kind);
    proven_loop_t *saved = g_loop;
    g_loop = l;
    conn_t c;
    proven_net_conn_t far;
    pair(&c, &far);
    (void)fill(&c);
    PROVEN_TEST_ASSERT(proven_loop_recv(g_loop, &c.sock, (proven_mem_mut_t){ 0 }) == PROVEN_OK, "a receive in flight", g_kind);
    /* And two things that have ended but not been answered yet: a socket closed, an accept
     * cancelled. Their last answers come from the destroy, so that their memory can go. */
    conn_t gone;
    proven_net_conn_t gone_far;
    pair(&gone, &gone_far);
    proven_loop_close(g_loop, &gone.sock);
    acceptor_t a;
    memset(&a, 0, sizeof a);
    PROVEN_TEST_ASSERT(proven_net_listen(proven_net_addr_ipv4(127, 0, 0, 1, 0), 4, &a.listener, NULL) == PROVEN_OK &&
                       proven_loop_accept(g_loop, &a.acc, &a.listener, on_accept, &a) == PROVEN_OK, "an accept in flight", g_kind);
    proven_loop_accept_cancel(g_loop, &a.acc);
    (void)proven_net_listener_close(&a.listener);
    proven_loop_destroy(l);
    g_loop = saved;
    PROVEN_TEST_ASSERT(c.received == 0 && c.writable == 0 && c.closed == 0, "a destroyed loop calls nothing for what is still open", g_kind);
    PROVEN_TEST_ASSERT(gone.closed == 1 && gone.received == 0 && a.calls == 1 && a.last == PROVEN_ERR_CANCELLED,
        "but gives a closed socket its closed, and a cancelled accept its PROVEN_ERR_CANCELLED", g_kind);
    (void)proven_net_close(&c.conn);
    (void)proven_net_close(&far);
    (void)proven_net_close(&gone_far);
}

static void run_all(proven_allocator_t heap, int on) {
    g_kind = on == ON_POLL ? "on the poll kind of selector" : on == ON_URING ? "on io_uring" : "on the system's default";
    PROVEN_TEST_ASSERT(make_loop(heap, on, &g_loop) == PROVEN_OK, "a loop", g_kind);

    PROVEN_TEST_SECTION("receives", "Zero bytes, into a buffer, cancelled, at the end; each answered once, never inside the call.", g_kind);
    case_receive();
    PROVEN_TEST_SECTION("sends", "Straight to the socket, the rest held up to the limit, the wait, in place, in order.", g_kind);
    case_send();
    PROVEN_TEST_SECTION("pieces cut by the socket", "Several pieces sent at once, cut inside them: the rest is held from the right byte, small pieces put together and large ones in a gather write.", g_kind);
    case_sendv_cut();
    PROVEN_TEST_SECTION("close and failure", "Close with a receive, a wait and bytes in flight; a send that fails in the loop.", g_kind);
    case_close_and_fail();
    PROVEN_TEST_SECTION("accepts and connects", "Accepts re-armed from inside, cancelled; a connect that succeeds and one that is refused.", g_kind);
    case_accept_connect();
    PROVEN_TEST_SECTION("UDP", "Receives into a buffer, re-armed from inside, closed with one in flight; a send wait (internal, for HTTP/3).", g_kind);
    case_udp();
    PROVEN_TEST_SECTION("Unix-domain sockets", "Accepted by the loop on a path, re-armed from inside; the accepted socket receives and sends through the loop; a connect by path.", g_kind);
    case_unix();
    PROVEN_TEST_SECTION("batched datagrams", "Datagrams of one size sent in one call, received through the loop with URO/GRO asked for and with the one-at-a-time way forced: every datagram whole, in order, of its size (internal, for HTTP/3).", g_kind);
    case_udp_segments();
    PROVEN_TEST_SECTION("destroy", "A loop destroyed with a receive and bytes in flight calls nothing.", g_kind);
    case_destroy(heap, on);

    proven_loop_destroy(g_loop);
    g_loop = NULL;
}

int main(void) {
    PROVEN_TEST_SUITE("the event loop's operations",
        "Receives, sends, accepts and connects that complete (RFC-0025): each answered exactly once, from the loop, never inside the call that started it; on the system's default, on the poll kind of selector, and on io_uring where the system has it.",
        "Inspect src/proven/loop.c: loop_run_done for delivery, sock_flush for sends, sock_event and acc_event for the selector's reports.");

    /* RFC-0025 section 12: the socket holds what the loop needs per connection. */
#if defined(_WIN32) || defined(_WIN64)
    PROVEN_TEST_ASSERT(sizeof(proven_loop_sock_t) == 120, "a socket of the loop is 120 bytes on Windows: its OVERLAPPED included", "");
#elif defined(__linux__)
    PROVEN_TEST_ASSERT(sizeof(proven_loop_sock_t) == 80, "a socket of the loop is 80 bytes on Linux: what an io_uring operation keeps included", "");
#else
    PROVEN_TEST_ASSERT(sizeof(proven_loop_sock_t) == 72, "a socket of the loop is 72 bytes", "");
#endif

    proven_allocator_t heap = proven_heap_allocator();
    proven_loop_sock_t s = { 0 };
    proven_net_conn_t closed_conn = { 0 };
    PROVEN_TEST_ASSERT(proven_loop_attach(NULL, &s, &closed_conn, &g_fns, NULL) == PROVEN_ERR_INVALID_ARG, "no loop is PROVEN_ERR_INVALID_ARG", "");

    run_all(heap, ON_DEFAULT);
    run_all(heap, ON_POLL);
    /* io_uring where this system lets the process have it (Linux; RFC-0029). */
    proven_loop_t *probe = NULL;
    if (proven_loop_create_ex(heap, &(proven_loop_options_t){ .backend = PROVEN_LOOP_BACKEND_URING }, &probe) == PROVEN_OK) {
        proven_loop_destroy(probe);
        run_all(heap, ON_URING);
    } else {
        PROVEN_TEST_INFO("io_uring is not available here: its run is left out");
    }

    PROVEN_TEST_PASS("every operation was answered once, from the loop, on both kinds of selector.");
    return 0;
}
