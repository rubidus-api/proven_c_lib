#include "example.h"

/*
 * A TCP exchange, start to finish, with a deadline on everything that can wait.
 *
 * Both ends live in this one program, which works because the kernel completes a connection as
 * soon as the listener's backlog takes it - before accept is called. A real client and server
 * are these same calls in two processes.
 */

int main(void) {
    /* Port 0: the OS picks a free one, and `at` says which. Loopback: this machine only. */
    proven_net_listener_t listener;
    proven_net_addr_t at;
    proven_err_t err = proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 16, &listener, &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK && proven_net_listener_is_open(&listener), "the listener opens");
    EXAMPLE_REQUIRE(at.port != 0, "and reports the port it was given");

    /* One deadline for the whole exchange: five seconds from now, however many calls it takes. */
    proven_net_deadline_t until = proven_net_deadline_in(5000);

    proven_net_conn_t client, server;
    proven_net_addr_t peer;
    EXAMPLE_REQUIRE(proven_net_connect(at, until, &client) == PROVEN_OK, "the client connects");
    EXAMPLE_REQUIRE(proven_net_accept(&listener, until, &server, &peer) == PROVEN_OK, "the server accepts");
    EXAMPLE_REQUIRE(proven_net_conn_is_open(&client) && proven_net_conn_is_open(&server), "two open ends");

    /* Each end knows both addresses. The accepted peer is the client's own local address. */
    proven_net_addr_t client_local, client_peer, listening;
    EXAMPLE_REQUIRE(proven_net_conn_local_addr(&client, &client_local) == PROVEN_OK &&
                    proven_net_conn_peer_addr(&client, &client_peer) == PROVEN_OK, "the client's two addresses");
    EXAMPLE_REQUIRE(proven_net_addr_eq(&client_local, &peer), "the server saw the client's address");
    EXAMPLE_REQUIRE(proven_net_listener_addr(&listener, &listening) == PROVEN_OK &&
                    proven_net_addr_eq(&client_peer, &listening), "the client's peer is where the server listens");

    /* A request written in two small pieces: without this they may wait to be sent together. */
    EXAMPLE_REQUIRE(proven_net_conn_set_nodelay(&client, true) == PROVEN_OK, "small writes go at once");

    /* write_all sends everything or says how far it got. */
    proven_mem_view_t request = proven_mem_view_from_u8(PROVEN_LIT("what time is it?"));
    proven_result_size_t sent = proven_net_write_all(&client, request, until);
    EXAMPLE_REQUIRE(sent.err == PROVEN_OK && sent.value == request.size, "the whole request went");

    /* "I have nothing more to send." The server will read the request, then end of input. */
    EXAMPLE_REQUIRE(proven_net_shutdown_write(&client) == PROVEN_OK, "the client half-closes");

    /* The server reads until the end. A read returns what has arrived - a stream has no message
     * boundaries - so a loop is the only correct way to read "all of it". */
    proven_byte_t buf[64];
    proven_size_t got = 0;
    for (;;) {
        proven_result_size_t r = proven_net_read(&server, (proven_mem_mut_t){ buf + got, sizeof buf - got }, until);
        if (r.err == PROVEN_ERR_EOF) break;                  /* the client finished */
        EXAMPLE_REQUIRE(r.err == PROVEN_OK, "a read that is not the end succeeds");
        if (r.err != PROVEN_OK) break;
        got += r.value;
    }
    EXAMPLE_REQUIRE(got == request.size, "the server received the whole request");

    /* A single write may send only part; the count says how much. Here it all fits. */
    proven_mem_view_t reply = proven_mem_view_from_u8(PROVEN_LIT("time to close"));
    proven_result_size_t w = proven_net_write(&server, reply, until);
    EXAMPLE_REQUIRE(w.err == PROVEN_OK && w.value == reply.size, "a short reply goes in one write");

    proven_result_size_t r = proven_net_read(&client, (proven_mem_mut_t){ buf, sizeof buf }, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_OK && r.value == reply.size, "the client reads the reply after its half-close");

    /* Nothing more is coming, and the client will not wait for ever to find that out. A timeout
     * is not damage: the connection is exactly as usable as before. */
    r = proven_net_read(&client, (proven_mem_mut_t){ buf, sizeof buf }, proven_net_deadline_in(50));
    EXAMPLE_REQUIRE(r.err == PROVEN_ERR_TIMEOUT, "silence is a timeout, not a hang");

    /* Every socket is closed by its owner. */
    EXAMPLE_REQUIRE(proven_net_close(&server) == PROVEN_OK, "the server closes its end");
    r = proven_net_read(&client, (proven_mem_mut_t){ buf, sizeof buf }, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_ERR_EOF, "which the client reads as the end");
    EXAMPLE_REQUIRE(proven_net_close(&client) == PROVEN_OK, "the client closes");
    EXAMPLE_REQUIRE(proven_net_listener_close(&listener) == PROVEN_OK, "the listener closes");

    /* With the listener gone, the same address refuses - a different answer from a timeout. */
    proven_net_conn_t nobody;
    EXAMPLE_REQUIRE(proven_net_connect(at, proven_net_deadline_in(15000), &nobody) == PROVEN_ERR_REFUSED,
                    "nothing listening is PROVEN_ERR_REFUSED");

    return EXAMPLE_OK();
}
