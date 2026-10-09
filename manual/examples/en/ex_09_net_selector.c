#include "example.h"

/*
 * A selector: register each socket once, and be told only about the ones that are ready.
 *
 * The loop is the one from the poll example - wait, then serve what is ready without waiting -
 * but nothing is rebuilt between waits, and a thousand idle connections cost a wait nothing.
 */

typedef struct {
    proven_net_conn_t conn;
    int number;
    bool open;
} client_t;

int main(void) {
    proven_net_listener_t listener;
    proven_net_addr_t at;
    proven_err_t err = proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 16, &listener, &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "the listener opens");

    proven_net_selector_t *selector = NULL;
    EXAMPLE_REQUIRE(proven_net_selector_create(proven_heap_allocator(), &selector) == PROVEN_OK, "a selector");
    /* epoll on Linux, kqueue on the BSDs and macOS; elsewhere the same interface over poll. */
    proven_net_selector_kind_t kind = proven_net_selector_kind(selector);
    printf("selector kind: %s\n", kind == PROVEN_NET_SELECTOR_EPOLL ? "epoll" : kind == PROVEN_NET_SELECTOR_KQUEUE ? "kqueue" : "poll");

    /* The tag is what comes back with an event: a pointer to your own record of the socket.
     * The listener's tag here is the listener itself. */
    EXAMPLE_REQUIRE(proven_net_selector_add(selector, proven_net_listener_handle(&listener), PROVEN_NET_READABLE, &listener) == PROVEN_OK,
                    "the listener is registered, once");

    /* Three clients connect; the second and third say something. */
    proven_net_conn_t far[3];
    for (int i = 0; i < 3; ++i) EXAMPLE_REQUIRE(proven_net_connect(at, proven_net_deadline_in(5000), &far[i]) == PROVEN_OK, "a client connects");
    EXAMPLE_REQUIRE(proven_net_write_all(&far[1], proven_mem_view_from_u8(PROVEN_LIT("from one")), proven_net_deadline_in(5000)).err == PROVEN_OK &&
                    proven_net_write_all(&far[2], proven_mem_view_from_u8(PROVEN_LIT("from two")), proven_net_deadline_in(5000)).err == PROVEN_OK, "two of them send");

    client_t clients[3] = {0};
    int accepted = 0, messages = 0;
    for (int round = 0; round < 20 && messages < 2; ++round) {
        proven_net_ready_t ready[8];
        proven_size_t count = 0;
        err = proven_net_selector_wait(selector, ready, 8, proven_net_deadline_in(1000), &count);
        if (err == PROVEN_ERR_TIMEOUT) continue;        /* the idle tick of a loop */
        EXAMPLE_REQUIRE(err == PROVEN_OK, "something is ready");
        for (proven_size_t i = 0; i < count; ++i) {
            if (ready[i].tag == &listener) {
                /* Ready means "will not wait": take every pending connection, without waiting. */
                while (accepted < 3 && proven_net_accept(&listener, PROVEN_NET_DONT_WAIT, &clients[accepted].conn, NULL) == PROVEN_OK) {
                    client_t *c = &clients[accepted];
                    c->number = accepted++;
                    c->open = true;
                    EXAMPLE_REQUIRE(proven_net_selector_add(selector, proven_net_conn_handle(&c->conn), PROVEN_NET_READABLE, c) == PROVEN_OK,
                                    "each accepted connection is registered, with its record as the tag");
                }
            } else {
                client_t *c = ready[i].tag;
                proven_byte_t buf[32];
                proven_result_size_t got = proven_net_read(&c->conn, (proven_mem_mut_t){ buf, sizeof buf }, PROVEN_NET_DONT_WAIT);
                if (got.err == PROVEN_OK) messages++;
            }
        }
    }
    EXAMPLE_REQUIRE(accepted == 3 && messages == 2, "three connections accepted, two messages read");
    EXAMPLE_REQUIRE(proven_net_selector_count(selector) == 4, "four sockets registered: the listener and three connections");

    /* The first connection never spoke, so it was never reported: nothing to do for it. Now ask
     * a different question of it - may I write? - and it is reported at once. */
    proven_net_ready_t one[4];
    proven_size_t count = 0;
    EXAMPLE_REQUIRE(proven_net_selector_wait(selector, one, 4, proven_net_deadline_in(30), &count) == PROVEN_ERR_TIMEOUT, "all quiet: nothing is reported");
    EXAMPLE_REQUIRE(proven_net_selector_modify(selector, proven_net_conn_handle(&clients[0].conn), PROVEN_NET_WRITABLE, &clients[0]) == PROVEN_OK, "ask about writing instead");
    EXAMPLE_REQUIRE(proven_net_selector_wait(selector, one, 4, proven_net_deadline_in(1000), &count) == PROVEN_OK && count == 1 &&
                    one[0].tag == &clients[0] && (one[0].got & PROVEN_NET_WRITABLE), "an idle connection is writable");
    /* Ask for writability only while you have something to write, or every wait returns at once. */
    EXAMPLE_REQUIRE(proven_net_selector_modify(selector, proven_net_conn_handle(&clients[0].conn), PROVEN_NET_READABLE, &clients[0]) == PROVEN_OK, "back to reading");

    /* Remove a socket BEFORE closing it. */
    for (int i = 0; i < 3; ++i) {
        EXAMPLE_REQUIRE(proven_net_selector_remove(selector, proven_net_conn_handle(&clients[i].conn)) == PROVEN_OK, "removed");
        (void)proven_net_close(&clients[i].conn);
        (void)proven_net_close(&far[i]);
    }
    EXAMPLE_REQUIRE(proven_net_selector_remove(selector, proven_net_listener_handle(&listener)) == PROVEN_OK &&
                    proven_net_selector_count(selector) == 0, "and the listener: the selector is empty");
    (void)proven_net_listener_close(&listener);

    /* The portable kind can be asked for by name - to compare the two, or to run on Linux the
     * code path Windows uses. */
    proven_net_selector_t *portable = NULL;
    EXAMPLE_REQUIRE(proven_net_selector_create_poll(proven_heap_allocator(), &portable) == PROVEN_OK &&
                    proven_net_selector_kind(portable) == PROVEN_NET_SELECTOR_POLL, "a selector of the poll kind");
    proven_net_selector_destroy(portable);
    proven_net_selector_destroy(selector);
    return EXAMPLE_OK();
}
