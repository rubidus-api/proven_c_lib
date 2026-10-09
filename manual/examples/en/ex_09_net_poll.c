#include "example.h"

/*
 * One thread, many sockets: wait until any of them needs attention, then serve the ones
 * that do. This is the loop inside every single-threaded server.
 *
 * "Ready" means the next call of that kind will not wait - so each socket reported ready is
 * handled with PROVEN_NET_DONT_WAIT, and the loop never blocks anywhere but in the poll.
 */

int main(void) {
    proven_net_listener_t listener;
    proven_net_addr_t at;
    proven_err_t err = proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 16, &listener, &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "the listener opens");

    /* Two clients connect and each sends one message. */
    proven_net_conn_t clients[2];
    for (int i = 0; i < 2; ++i) {
        EXAMPLE_REQUIRE(proven_net_connect(at, proven_net_deadline_in(5000), &clients[i]) == PROVEN_OK, "a client connects");
    }
    EXAMPLE_REQUIRE(proven_net_write_all(&clients[0], proven_mem_view_from_u8(PROVEN_LIT("from the first")), proven_net_deadline_in(5000)).err == PROVEN_OK &&
                    proven_net_write_all(&clients[1], proven_mem_view_from_u8(PROVEN_LIT("from the second")), proven_net_deadline_in(5000)).err == PROVEN_OK,
                    "each sends a message");

    /* The server: a listener and up to two connections, in one list. */
    proven_net_conn_t served[2] = {0};
    proven_size_t served_count = 0;
    proven_size_t bytes_seen = 0;

    proven_net_deadline_t until = proven_net_deadline_in(5000);
    while (bytes_seen < 14 + 15) {
        proven_net_poll_item_t items[3];
        proven_size_t count = 0;
        items[count++] = (proven_net_poll_item_t){ .handle = proven_net_listener_handle(&listener), .want = PROVEN_NET_READABLE };
        for (proven_size_t i = 0; i < served_count; ++i) {
            items[count++] = (proven_net_poll_item_t){ .handle = proven_net_conn_handle(&served[i]), .want = PROVEN_NET_READABLE };
        }

        proven_size_t ready = 0;
        err = proven_net_poll(items, count, until, &ready);
        EXAMPLE_REQUIRE(err == PROVEN_OK && ready > 0, "something becomes ready before the deadline");
        if (err != PROVEN_OK) break;

        /* A readable listener has a connection waiting. */
        if (items[0].got & PROVEN_NET_READABLE) {
            EXAMPLE_REQUIRE(served_count < 2 &&
                            proven_net_accept(&listener, PROVEN_NET_DONT_WAIT, &served[served_count], NULL) == PROVEN_OK,
                            "the accept that poll predicted does not wait");
            served_count++;
        }
        /* A readable connection has data, or its end. `items[1 + i]` was built from `served[i]`. */
        for (proven_size_t i = 1; i < count; ++i) {
            if (!items[i].got) continue;
            proven_byte_t buf[64];
            proven_result_size_t r = proven_net_read(&served[i - 1], (proven_mem_mut_t){ buf, sizeof buf }, PROVEN_NET_DONT_WAIT);
            EXAMPLE_REQUIRE(r.err == PROVEN_OK, "the read that poll predicted does not wait");
            bytes_seen += r.value;
        }
    }
    EXAMPLE_REQUIRE(served_count == 2 && bytes_seen == 29, "both clients were accepted and both messages read, in one thread");

    /* proven_net_poll takes up to PROVEN_NET_POLL_INLINE_MAX sockets. A server with more passes
     * its own working memory: the OS wants an array, and this library does not allocate one. */
    proven_net_poll_item_t one = { .handle = proven_net_conn_handle(&served[0]), .want = PROVEN_NET_WRITABLE };
    proven_byte_t scratch[256];
    proven_size_t ready = 0;
    EXAMPLE_REQUIRE(proven_net_poll_scratch_size(1) <= sizeof scratch, "the scratch for one item is small");
    EXAMPLE_REQUIRE(proven_net_poll_with((proven_mem_mut_t){ scratch, sizeof scratch }, &one, 1, PROVEN_NET_DONT_WAIT, &ready) == PROVEN_OK &&
                    (one.got & PROVEN_NET_WRITABLE), "an idle connection is writable at once");

    /* A UDP socket waits in the same list as everything else. */
    proven_net_udp_t udp;
    EXAMPLE_REQUIRE(proven_net_udp_open(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &udp, NULL) == PROVEN_OK, "a UDP socket");
    proven_net_poll_item_t quiet = { .handle = proven_net_udp_handle(&udp), .want = PROVEN_NET_READABLE };
    EXAMPLE_REQUIRE(proven_net_poll(&quiet, 1, proven_net_deadline_in(30), &ready) == PROVEN_ERR_TIMEOUT && ready == 0,
                    "nothing arrives on it: PROVEN_ERR_TIMEOUT, the idle tick of an event loop");
    (void)proven_net_udp_close(&udp);

    for (int i = 0; i < 2; ++i) {
        (void)proven_net_close(&clients[i]);
        (void)proven_net_close(&served[i]);
    }
    (void)proven_net_listener_close(&listener);
    return EXAMPLE_OK();
}
