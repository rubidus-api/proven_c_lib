#include "proven.h"
#include "proven_test.h"
#include "../src/proven/proven_internal_loop.h"
#include "../src/proven/proven_internal_net.h"
#include <stdio.h>
#include <string.h>

/*
 * The loop's operations (RFC-0025): receives, sends, accepts and connects that complete.
 *
 * Every case runs twice: on a loop over the system's selector (epoll on Linux) and on one over
 * the portable poll kind, which is what the Windows build runs until its completion port.
 * The two must answer alike.
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

/* Poll until `*count` moves past `before`, or two seconds pass. */
static bool until_moves(const int *count, int before) {
    proven_net_deadline_t end = proven_net_deadline_in(2000);
    while (*count == before && proven_time_monotonic_now() < end) (void)proven_loop_poll(g_loop, proven_net_deadline_in(20));
    return *count != before;
}

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
        PROVEN_TEST_ASSERT(until_moves(&c.connected, 0) && c.conn_err == PROVEN_ERR_REFUSED, "a refused connect is answered with PROVEN_ERR_REFUSED", g_kind);
        close_and_wait(&c);
    } else {
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_REFUSED && !proven_net_conn_is_open(&c.conn), "or refused at once, with nothing to close", g_kind);
    }

    for (int i = 0; i < 3; ++i) { (void)proven_net_close(&clients[i]); (void)proven_net_close(&a.got[i]); }
    (void)proven_net_listener_close(&a.listener);
}

/* Destroyed with things in flight: nothing is called, and nothing leaks (the sanitizer runs
 * say so). */
static void case_destroy(proven_allocator_t heap, bool poll_kind) {
    proven_loop_t *l = NULL;
    PROVEN_TEST_ASSERT((poll_kind ? proven_loop_create_poll_(heap, &l) : proven_loop_create(heap, &l)) == PROVEN_OK, "a second loop", g_kind);
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

static void run_all(proven_allocator_t heap, bool poll_kind) {
    g_kind = poll_kind ? "on the poll kind of selector" : "on the system's selector";
    PROVEN_TEST_ASSERT((poll_kind ? proven_loop_create_poll_(heap, &g_loop) : proven_loop_create(heap, &g_loop)) == PROVEN_OK, "a loop", g_kind);

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
    PROVEN_TEST_SECTION("destroy", "A loop destroyed with a receive and bytes in flight calls nothing.", g_kind);
    case_destroy(heap, poll_kind);

    proven_loop_destroy(g_loop);
    g_loop = NULL;
}

int main(void) {
    PROVEN_TEST_SUITE("the event loop's operations",
        "Receives, sends, accepts and connects that complete (RFC-0025): each answered exactly once, from the loop, never inside the call that started it; on the system's selector and on the poll kind.",
        "Inspect src/proven/loop.c: loop_run_done for delivery, sock_flush for sends, sock_event and acc_event for the selector's reports.");

    /* RFC-0025 section 12: the socket holds what the loop needs per connection. */
#if defined(_WIN32) || defined(_WIN64)
    PROVEN_TEST_ASSERT(sizeof(proven_loop_sock_t) == 104, "a socket of the loop is 104 bytes on Windows", "");
#else
    PROVEN_TEST_ASSERT(sizeof(proven_loop_sock_t) == 72, "a socket of the loop is 72 bytes", "");
#endif

    proven_allocator_t heap = proven_heap_allocator();
    proven_loop_sock_t s = { 0 };
    proven_net_conn_t closed_conn = { 0 };
    PROVEN_TEST_ASSERT(proven_loop_attach(NULL, &s, &closed_conn, &g_fns, NULL) == PROVEN_ERR_INVALID_ARG, "no loop is PROVEN_ERR_INVALID_ARG", "");

    run_all(heap, false);
    run_all(heap, true);

    PROVEN_TEST_PASS("every operation was answered once, from the loop, on both kinds of selector.");
    return 0;
}
