#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * Unix-domain stream sockets: the same calls as TCP, addressed by a filesystem path.
 *
 * The socket file is made in the directory the test runs in. What differs from TCP is asserted
 * here: the path exists after listen and is left behind by close, a missing path is
 * PROVEN_ERR_NOT_FOUND, a path that is taken is PROVEN_ERR_BUSY, and TCP_NODELAY does not apply.
 *
 * Windows has had AF_UNIX since Windows 10 1803. Where the family is not there at all the test
 * reports SKIP.
 */

static proven_mem_view_t mv(const char *s) {
    return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}

int main(void) {
    PROVEN_TEST_SUITE("net: Unix-domain stream sockets",
        "Listen, connect and exchange over a path; the failures that are particular to paths.",
        "Inspect to_native and from_native in platform/proven_sys_net.c for the path handling.");

    proven_u8str_view_t path = PROVEN_LIT("proven_test_unix.sock");
    proven_allocator_t heap = proven_heap_allocator();
    (void)proven_fs_remove(heap, path);
    proven_net_addr_t at;
    PROVEN_TEST_ASSERT(proven_net_addr_unix(path, &at) == PROVEN_OK, "the path makes an address", "");

    proven_net_listener_t listener;
    proven_net_addr_t bound;
    proven_err_t err = proven_net_listen(at, 4, &listener, &bound);
    if (err == PROVEN_ERR_UNSUPPORTED || err == PROVEN_ERR_PERMISSION) {
        PROVEN_TEST_INFO("SKIP: Unix-domain sockets cannot be opened here (error {}).", PROVEN_ARG((int)err));
        PROVEN_TEST_PASS("skipped: no Unix-domain sockets here.");
        return 0;
    }
    PROVEN_TEST_ASSERT(err == PROVEN_OK, "a listener opens on the path", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the path is the address",
        "listen creates it and reports it; a second listener on it is PROVEN_ERR_BUSY.",
        "");
    // ---------------------------------------------------------------
    {
        PROVEN_TEST_ASSERT(bound.family == PROVEN_NET_FAMILY_UNIX && proven_net_addr_eq(&bound, &at),
            "the bound address is the path that was asked for", "");
        proven_net_listener_t second;
        PROVEN_TEST_ASSERT(proven_net_listen(at, 4, &second, NULL) == PROVEN_ERR_BUSY, "a second listener on the same path is PROVEN_ERR_BUSY", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the same exchange as TCP",
        "Connect, accept, both directions, half-close and EOF, a deadline.",
        "");
    // ---------------------------------------------------------------
    {
        proven_net_conn_t client, server;
        proven_net_addr_t peer;
        PROVEN_TEST_ASSERT(proven_net_connect(at, proven_net_deadline_in(5000), &client) == PROVEN_OK, "connect by path", "");
        PROVEN_TEST_ASSERT(proven_net_accept(&listener, proven_net_deadline_in(5000), &server, &peer) == PROVEN_OK, "accept", "");
        PROVEN_TEST_ASSERT(peer.family == PROVEN_NET_FAMILY_UNIX, "the peer is a Unix-domain address (usually one with no path)", "");

        proven_byte_t buf[32];
        proven_result_size_t w = proven_net_write_all(&client, mv("over a path"), proven_net_deadline_in(5000));
        proven_result_size_t r = proven_net_read(&server, (proven_mem_mut_t){ buf, sizeof buf }, proven_net_deadline_in(5000));
        PROVEN_TEST_ASSERT(w.err == PROVEN_OK && r.err == PROVEN_OK && r.value == 11 && memcmp(buf, "over a path", 11) == 0, "client to server", "");
        w = proven_net_write_all(&server, mv("and back"), proven_net_deadline_in(5000));
        r = proven_net_read(&client, (proven_mem_mut_t){ buf, sizeof buf }, proven_net_deadline_in(5000));
        PROVEN_TEST_ASSERT(w.err == PROVEN_OK && r.err == PROVEN_OK && r.value == 8 && memcmp(buf, "and back", 8) == 0, "server to client", "");

        r = proven_net_read(&client, (proven_mem_mut_t){ buf, sizeof buf }, proven_net_deadline_in(60));
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_TIMEOUT, "a read with nothing to read times out", "");
        PROVEN_TEST_ASSERT(proven_net_shutdown_write(&client) == PROVEN_OK, "half-close", "");
        r = proven_net_read(&server, (proven_mem_mut_t){ buf, sizeof buf }, proven_net_deadline_in(5000));
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_EOF, "the server reads PROVEN_ERR_EOF", "");

        PROVEN_TEST_ASSERT(proven_net_conn_set_nodelay(&client, true) == PROVEN_ERR_UNSUPPORTED,
            "TCP_NODELAY on a Unix-domain connection is PROVEN_ERR_UNSUPPORTED", "");
        (void)proven_net_close(&client);
        (void)proven_net_close(&server);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("what closing leaves, and what a missing path answers",
        "The socket file outlives its listener; with the file gone, connect is PROVEN_ERR_NOT_FOUND.",
        "A leftover file from a crashed server is why a server removes the path before listening.");
    // ---------------------------------------------------------------
    {
        PROVEN_TEST_ASSERT(proven_net_listener_close(&listener) == PROVEN_OK, "the listener closes", "");
        /* The file's presence is observed through the sockets themselves - refused is the
         * answer of a path that exists with nobody behind it, not found of one that does not -
         * because a socket file is not something every platform's stat will open. */
        proven_net_conn_t c;
        proven_err_t e = proven_net_connect(at, proven_net_deadline_in(5000), &c);
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_REFUSED, "the socket file is still there: connecting to it is PROVEN_ERR_REFUSED, nobody listens", "");
        proven_net_listener_t again;
        PROVEN_TEST_ASSERT(proven_net_listen(at, 4, &again, NULL) == PROVEN_ERR_BUSY, "and a new listener cannot take the leftover path: PROVEN_ERR_BUSY", "");
        PROVEN_TEST_ASSERT(proven_fs_remove(heap, path) == PROVEN_OK, "the file is removed like any other", "");
        e = proven_net_connect(at, proven_net_deadline_in(5000), &c);
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_NOT_FOUND, "and then connecting is PROVEN_ERR_NOT_FOUND", "");
    }

    PROVEN_TEST_PASS("Unix-domain streams behave like TCP, addressed by a path.");
    return 0;
}
