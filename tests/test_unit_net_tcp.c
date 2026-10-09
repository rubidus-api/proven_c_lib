#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * The contract of the stream-socket calls, on the loopback interface, in one thread.
 *
 * One thread is enough because a TCP connection completes in the kernel: connect succeeds as
 * soon as the listener's backlog has taken it, before accept is called. So every case can be
 * laid out in order - connect, then accept - with no scheduling in the test to go wrong.
 *
 * Each section is one row of the contract in net.h: the address actually bound, end of input as
 * PROVEN_ERR_EOF, a deadline as PROVEN_ERR_TIMEOUT that leaves the connection usable, a partial
 * write that says how far it got, nothing listening as PROVEN_ERR_REFUSED, a vanished peer as
 * PROVEN_ERR_RESET, a taken port as PROVEN_ERR_BUSY.
 *
 * If this environment will not open a socket at all (a sandbox), the test says SKIP with the
 * error it got and passes: a refusal to run is not evidence about the code either way.
 */

static proven_mem_view_t mv(const char *s) {
    return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}

static proven_i64 elapsed_ms(proven_time_t since) {
    return (proven_time_monotonic_now() - since) / 1000000;
}

/* A listener on a free loopback port, a client connected to it, and the accepted server end. */
typedef struct {
    proven_net_listener_t listener;
    proven_net_addr_t at;
    proven_net_conn_t client;
    proven_net_conn_t server;
} pair_t;

static proven_err_t pair_open(pair_t *p, proven_net_family_t family) {
    *p = (pair_t){0};
    proven_err_t err = proven_net_listen(proven_net_addr_loopback(family, 0), 8, &p->listener, &p->at);
    if (err != PROVEN_OK) return err;
    err = proven_net_connect(p->at, proven_net_deadline_in(5000), &p->client);
    if (err != PROVEN_OK) { (void)proven_net_listener_close(&p->listener); return err; }
    err = proven_net_accept(&p->listener, proven_net_deadline_in(5000), &p->server, NULL);
    if (err != PROVEN_OK) { (void)proven_net_close(&p->client); (void)proven_net_listener_close(&p->listener); }
    return err;
}

static void pair_close(pair_t *p) {
    (void)proven_net_close(&p->client);
    (void)proven_net_close(&p->server);
    (void)proven_net_listener_close(&p->listener);
}

int main(void) {
    PROVEN_TEST_SUITE("net: TCP on the loopback interface",
        "Listen, connect, accept, read, write, half-close, deadlines, and the named failures.",
        "Inspect src/proven/net.c for the deadline loop and platform/proven_sys_net.c for the reason a system call was mapped to.");

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

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("listen says where it bound; both ends know both addresses",
        "Port 0 asks the OS to choose, and the chosen port comes back without a second call.",
        "");
    // ---------------------------------------------------------------
    {
        pair_t p;
        PROVEN_TEST_ASSERT(pair_open(&p, PROVEN_NET_FAMILY_IPV4) == PROVEN_OK, "connect then accept on loopback", "");
        PROVEN_TEST_ASSERT(p.at.family == PROVEN_NET_FAMILY_IPV4 && p.at.port != 0 && p.at.ip[0] == 127,
            "the bound address has the port the OS chose", "");
        proven_net_addr_t again, c_local, c_peer, s_local, s_peer;
        PROVEN_TEST_ASSERT(proven_net_listener_addr(&p.listener, &again) == PROVEN_OK && proven_net_addr_eq(&again, &p.at),
            "proven_net_listener_addr agrees with what listen reported", "");
        PROVEN_TEST_ASSERT(proven_net_conn_local_addr(&p.client, &c_local) == PROVEN_OK &&
                           proven_net_conn_peer_addr(&p.client, &c_peer) == PROVEN_OK &&
                           proven_net_conn_local_addr(&p.server, &s_local) == PROVEN_OK &&
                           proven_net_conn_peer_addr(&p.server, &s_peer) == PROVEN_OK,
            "both ends report a local and a peer address", "");
        PROVEN_TEST_ASSERT(proven_net_addr_eq(&c_peer, &p.at) && proven_net_addr_eq(&s_local, &p.at) &&
                           proven_net_addr_eq(&c_local, &s_peer),
            "the client's peer is the listener's address, and each end's local is the other's peer", "");
        PROVEN_TEST_ASSERT(proven_net_conn_set_nodelay(&p.client, true) == PROVEN_OK &&
                           proven_net_conn_set_nodelay(&p.client, false) == PROVEN_OK,
            "TCP_NODELAY can be set and cleared", "");
        pair_close(&p);
        PROVEN_TEST_ASSERT(!proven_net_conn_is_open(&p.client) && !proven_net_listener_is_open(&p.listener),
            "closing resets the value", "");
        PROVEN_TEST_ASSERT(proven_net_close(&p.client) == PROVEN_OK, "closing what is not open is not an error", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("bytes go both ways, and the end of input is PROVEN_ERR_EOF",
        "A half-close ends one direction only: the peer reads EOF and can still answer.",
        "A zero-byte success instead of EOF is the shape of a read loop that never ends.");
    // ---------------------------------------------------------------
    {
        pair_t p;
        PROVEN_TEST_ASSERT(pair_open(&p, PROVEN_NET_FAMILY_IPV4) == PROVEN_OK, "a connected pair", "");
        proven_byte_t buf[64];

        proven_result_size_t w = proven_net_write_all(&p.client, mv("ping"), proven_net_deadline_in(5000));
        PROVEN_TEST_ASSERT(w.err == PROVEN_OK && w.value == 4, "write_all sends all four bytes", "");
        proven_result_size_t r = proven_net_read(&p.server, (proven_mem_mut_t){ buf, sizeof buf }, proven_net_deadline_in(5000));
        PROVEN_TEST_ASSERT(r.err == PROVEN_OK && r.value == 4 && memcmp(buf, "ping", 4) == 0, "the server reads them", "");

        PROVEN_TEST_ASSERT(proven_net_shutdown_write(&p.client) == PROVEN_OK, "the client says it has no more to send", "");
        r = proven_net_read(&p.server, (proven_mem_mut_t){ buf, sizeof buf }, proven_net_deadline_in(5000));
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_EOF && r.value == 0, "the server reads PROVEN_ERR_EOF, not zero bytes of success", "");
        r = proven_net_read(&p.server, (proven_mem_mut_t){ buf, sizeof buf }, proven_net_deadline_in(5000));
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_EOF, "and reads it again: the end stays the end", "");

        w = proven_net_write_all(&p.server, mv("pong"), proven_net_deadline_in(5000));
        PROVEN_TEST_ASSERT(w.err == PROVEN_OK && w.value == 4, "the server can still answer after the client's half-close", "");
        r = proven_net_read(&p.client, (proven_mem_mut_t){ buf, sizeof buf }, proven_net_deadline_in(5000));
        PROVEN_TEST_ASSERT(r.err == PROVEN_OK && r.value == 4 && memcmp(buf, "pong", 4) == 0, "and the client reads the answer", "");

        r = proven_net_read(&p.client, (proven_mem_mut_t){ buf, 0 }, PROVEN_NET_DONT_WAIT);
        PROVEN_TEST_ASSERT(r.err == PROVEN_OK && r.value == 0, "a read into no space is a success of zero bytes and waits for nothing", "");
        pair_close(&p);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a deadline that passes is PROVEN_ERR_TIMEOUT, and breaks nothing",
        "A read with nothing to read waits until its deadline and no longer; the connection works afterwards.",
        "Returning early means the remaining time was rounded down; never returning means the deadline was not re-read after a wake-up.");
    // ---------------------------------------------------------------
    {
        pair_t p;
        PROVEN_TEST_ASSERT(pair_open(&p, PROVEN_NET_FAMILY_IPV4) == PROVEN_OK, "a connected pair", "");
        proven_byte_t buf[16];

        proven_time_t t0 = proven_time_monotonic_now();
        proven_result_size_t r = proven_net_read(&p.server, (proven_mem_mut_t){ buf, sizeof buf }, proven_net_deadline_in(80));
        proven_i64 took = elapsed_ms(t0);
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_TIMEOUT && r.value == 0, "nothing arrived: PROVEN_ERR_TIMEOUT", "");
        PROVEN_TEST_ASSERT(took >= 75 && took < 10000, "it waited about the 80 ms it was given - not less, and not for ever", "");

        t0 = proven_time_monotonic_now();
        r = proven_net_read(&p.server, (proven_mem_mut_t){ buf, sizeof buf }, PROVEN_NET_DONT_WAIT);
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_TIMEOUT && elapsed_ms(t0) < 1000, "PROVEN_NET_DONT_WAIT does not wait", "");

        t0 = proven_time_monotonic_now();
        proven_net_conn_t none;
        proven_err_t err = proven_net_accept(&p.listener, proven_net_deadline_in(60), &none, NULL);
        PROVEN_TEST_ASSERT(err == PROVEN_ERR_TIMEOUT && !proven_net_conn_is_open(&none) && elapsed_ms(t0) >= 55,
            "an accept with nobody connecting times out the same way, and hands back nothing to close", "");

        proven_result_size_t w = proven_net_write_all(&p.client, mv("late"), proven_net_deadline_in(5000));
        r = proven_net_read(&p.server, (proven_mem_mut_t){ buf, sizeof buf }, proven_net_deadline_in(5000));
        PROVEN_TEST_ASSERT(w.err == PROVEN_OK && r.err == PROVEN_OK && r.value == 4 && memcmp(buf, "late", 4) == 0,
            "after two timeouts the same connection still carries data", "");

        /* A past deadline with data already waiting still reads it: the deadline bounds the
         * WAIT, and there is nothing to wait for. */
        w = proven_net_write_all(&p.client, mv("x"), proven_net_deadline_in(5000));
        proven_time_sleep(50);
        r = proven_net_read(&p.server, (proven_mem_mut_t){ buf, sizeof buf }, PROVEN_NET_DONT_WAIT);
        PROVEN_TEST_ASSERT(w.err == PROVEN_OK && r.err == PROVEN_OK && r.value == 1, "data already there is read even with no time to wait", "");
        pair_close(&p);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a write that cannot finish says how far it got",
        "With nobody reading, write_all fills the buffers, times out, and reports the bytes that went.",
        "The count is what makes the failure recoverable: resume from it, or close. Losing it means resending from the start and duplicating data.");
    // ---------------------------------------------------------------
    {
        pair_t p;
        PROVEN_TEST_ASSERT(pair_open(&p, PROVEN_NET_FAMILY_IPV4) == PROVEN_OK, "a connected pair", "");
        proven_allocator_t heap = proven_heap_allocator();
        const proven_size_t total = (proven_size_t)48 * 1024 * 1024;
        proven_result_mem_mut_t mem = heap.alloc_fn(heap.ctx, total, 16);
        PROVEN_TEST_ASSERT(proven_is_ok(mem.err), "48 MiB of test data", "");
        proven_byte_t *big = mem.value.ptr;
        for (proven_size_t i = 0; i < total; ++i) big[i] = (proven_byte_t)(i * 2654435761u >> 24);

        proven_result_size_t w = proven_net_write_all(&p.client, (proven_mem_view_t){ big, total }, proven_net_deadline_in(300));
        PROVEN_TEST_ASSERT(w.err == PROVEN_ERR_TIMEOUT, "48 MiB into a connection nobody reads does not fit: PROVEN_ERR_TIMEOUT", "");
        PROVEN_TEST_ASSERT(w.value > 0 && w.value < total, "and the count says part of it went", "");

        /* Drain exactly that many bytes and check they are the prefix, in order. */
        proven_size_t got = 0;
        bool same = true;
        proven_byte_t chunk[65536];
        while (got < w.value) {
            proven_result_size_t r = proven_net_read(&p.server, (proven_mem_mut_t){ chunk, sizeof chunk }, proven_net_deadline_in(5000));
            if (r.err != PROVEN_OK) break;
            if (got + r.value > total || memcmp(chunk, big + got, r.value) != 0) same = false;
            got += r.value;
        }
        PROVEN_TEST_ASSERT(got == w.value && same, "the peer receives exactly the reported bytes, and they are the start of the data", "");
        proven_result_size_t r = proven_net_read(&p.server, (proven_mem_mut_t){ chunk, sizeof chunk }, proven_net_deadline_in(100));
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_TIMEOUT, "and not one byte more", "");

        /* A single write takes what fits now and says so. */
        proven_result_size_t one = proven_net_write(&p.client, (proven_mem_view_t){ big, total }, proven_net_deadline_in(1000));
        PROVEN_TEST_ASSERT(one.err == PROVEN_OK && one.value > 0 && one.value < total, "proven_net_write sends a part and reports its size", "");
        heap.free_fn(heap.ctx, big);
        pair_close(&p);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("nothing listening is PROVEN_ERR_REFUSED",
        "A port that was just released answers a connect with a refusal, not a timeout and not a generic error.",
        "Windows retries a refused loopback connect for about two seconds before reporting it; the deadline here allows for that.");
    // ---------------------------------------------------------------
    {
        proven_net_listener_t l;
        proven_net_addr_t at;
        PROVEN_TEST_ASSERT(proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 1, &l, &at) == PROVEN_OK, "take a port", "");
        PROVEN_TEST_ASSERT(proven_net_listener_close(&l) == PROVEN_OK, "and give it back", "");
        proven_net_conn_t c;
        proven_err_t err = proven_net_connect(at, proven_net_deadline_in(15000), &c);
        PROVEN_TEST_ASSERT(err == PROVEN_ERR_REFUSED, "connecting to it is PROVEN_ERR_REFUSED", "");
        PROVEN_TEST_ASSERT(!proven_net_conn_is_open(&c), "and leaves nothing open", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a peer that vanishes is PROVEN_ERR_RESET, and a write to it does not kill the process",
        "Closing with unread data aborts the connection; the other end reads RESET, and writing afterwards is an error value, not SIGPIPE.",
        "If this test dies with no failure message, a send was made without MSG_NOSIGNAL (or SO_NOSIGPIPE) and the signal killed it.");
    // ---------------------------------------------------------------
    {
        pair_t p;
        PROVEN_TEST_ASSERT(pair_open(&p, PROVEN_NET_FAMILY_IPV4) == PROVEN_OK, "a connected pair", "");
        proven_result_size_t w = proven_net_write_all(&p.client, mv("unread"), proven_net_deadline_in(5000));
        PROVEN_TEST_ASSERT(w.err == PROVEN_OK, "the client sends data", "");
        proven_time_sleep(50);                              /* let it reach the server's buffer */
        PROVEN_TEST_ASSERT(proven_net_close(&p.server) == PROVEN_OK, "the server closes without reading it", "");

        proven_byte_t buf[16];
        proven_result_size_t r = proven_net_read(&p.client, (proven_mem_mut_t){ buf, sizeof buf }, proven_net_deadline_in(5000));
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_RESET, "the client's read is PROVEN_ERR_RESET - not EOF: the peer did not finish, it vanished", "");

        proven_err_t last = PROVEN_OK;
        for (int i = 0; i < 20 && last == PROVEN_OK; ++i) {
            last = proven_net_write_all(&p.client, mv("into the void"), proven_net_deadline_in(1000)).err;
            proven_time_sleep(10);
        }
        PROVEN_TEST_ASSERT(last == PROVEN_ERR_RESET, "writing to the dead connection is PROVEN_ERR_RESET, and this process is still here to see it", "");
        pair_close(&p);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a taken port is PROVEN_ERR_BUSY; a closed socket is PROVEN_ERR_INVALID_STATE",
        "Two listeners cannot share one address, on any platform.",
        "On Windows this is what SO_EXCLUSIVEADDRUSE is for: with SO_REUSEADDR the second bind would succeed and take the connections.");
    // ---------------------------------------------------------------
    {
        proven_net_listener_t first, second;
        proven_net_addr_t at;
        PROVEN_TEST_ASSERT(proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 4, &first, &at) == PROVEN_OK, "the first listener", "");
        proven_err_t err = proven_net_listen(at, 4, &second, NULL);
        PROVEN_TEST_ASSERT(err == PROVEN_ERR_BUSY && !proven_net_listener_is_open(&second), "a second listener on the same address is PROVEN_ERR_BUSY", "");
        (void)proven_net_listener_close(&first);

        proven_net_conn_t closed = {0};
        proven_byte_t b[4];
        PROVEN_TEST_ASSERT(proven_net_read(&closed, (proven_mem_mut_t){ b, sizeof b }, PROVEN_NET_DONT_WAIT).err == PROVEN_ERR_INVALID_STATE &&
                           proven_net_write(&closed, mv("x"), PROVEN_NET_DONT_WAIT).err == PROVEN_ERR_INVALID_STATE &&
                           proven_net_shutdown_write(&closed) == PROVEN_ERR_INVALID_STATE,
            "reading, writing or shutting down a connection that is not open is PROVEN_ERR_INVALID_STATE", "");
        proven_net_conn_t out;
        PROVEN_TEST_ASSERT(proven_net_accept(&first, PROVEN_NET_DONT_WAIT, &out, NULL) == PROVEN_ERR_INVALID_STATE,
            "so is accepting on a closed listener", "");
        proven_net_addr_t none = {0};
        PROVEN_TEST_ASSERT(proven_net_connect(none, PROVEN_NET_DONT_WAIT, &out) == PROVEN_ERR_INVALID_ARG &&
                           proven_net_listen(none, 1, &second, NULL) == PROVEN_ERR_INVALID_ARG,
            "an address of no family is PROVEN_ERR_INVALID_ARG", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("IPv6 loopback",
        "The same exchange over ::1, where the machine has IPv6.",
        "");
    // ---------------------------------------------------------------
    {
        pair_t p;
        proven_err_t err = pair_open(&p, PROVEN_NET_FAMILY_IPV6);
        if (err != PROVEN_OK) {
            PROVEN_TEST_INFO("SKIP: no IPv6 loopback on this machine (error {}).", PROVEN_ARG((int)err));
        } else {
            proven_byte_t buf[8];
            proven_result_size_t w = proven_net_write_all(&p.client, mv("six"), proven_net_deadline_in(5000));
            proven_result_size_t r = proven_net_read(&p.server, (proven_mem_mut_t){ buf, sizeof buf }, proven_net_deadline_in(5000));
            PROVEN_TEST_ASSERT(w.err == PROVEN_OK && r.err == PROVEN_OK && r.value == 3 && memcmp(buf, "six", 3) == 0, "bytes cross an IPv6 connection", "");
            proven_net_addr_t peer;
            PROVEN_TEST_ASSERT(proven_net_conn_peer_addr(&p.client, &peer) == PROVEN_OK && peer.family == PROVEN_NET_FAMILY_IPV6 && peer.ip[15] == 1,
                "and the peer address is ::1", "");
            pair_close(&p);
        }
    }

    PROVEN_TEST_PASS("the TCP contract holds on loopback.");
    return 0;
}
