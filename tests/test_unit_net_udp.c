#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * UDP on the loopback interface. A datagram is a message, not a stream: it arrives whole or
 * cut, a zero-length one is a real message, and "sent" promises only that it left.
 *
 * Loopback does not lose datagrams under this load, so the cases are deterministic; nothing
 * here asserts delivery across a real network, which UDP does not promise.
 */

static proven_mem_view_t mv(const char *s) {
    return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}

int main(void) {
    PROVEN_TEST_SUITE("net: UDP on the loopback interface",
        "Datagrams keep their boundaries; a cut one says so; a wait ends at its deadline.",
        "Inspect proven_net_udp_recv_from in src/proven/net.c and proven_sys_net_recv_from in platform/proven_sys_net.c.");

    proven_net_udp_t a, b;
    proven_net_addr_t a_at, b_at;
    proven_err_t err = proven_net_udp_open(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &a, &a_at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        PROVEN_TEST_INFO("SKIP: this environment refuses to open a datagram socket (error {}).", PROVEN_ARG((int)err));
        PROVEN_TEST_PASS("skipped: no sockets here.");
        return 0;
    }
    PROVEN_TEST_ASSERT(err == PROVEN_OK && a_at.port != 0, "a UDP socket opens on a port the OS chose", "");
    PROVEN_TEST_ASSERT(proven_net_udp_open(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &b, &b_at) == PROVEN_OK, "and a second one", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("datagrams keep their boundaries and name their sender",
        "Three sends are three receives, in sizes that match, each from the sender's address.",
        "");
    // ---------------------------------------------------------------
    {
        proven_net_addr_t again;
        PROVEN_TEST_ASSERT(proven_net_udp_addr(&a, &again) == PROVEN_OK && proven_net_addr_eq(&again, &a_at),
            "proven_net_udp_addr agrees with what open reported", "");

        PROVEN_TEST_ASSERT(proven_net_udp_send_to(&a, b_at, mv("one"), proven_net_deadline_in(2000)) == PROVEN_OK &&
                           proven_net_udp_send_to(&a, b_at, mv(""), proven_net_deadline_in(2000)) == PROVEN_OK &&
                           proven_net_udp_send_to(&a, b_at, mv("three!"), proven_net_deadline_in(2000)) == PROVEN_OK,
            "three datagrams leave, the middle one empty", "");

        proven_byte_t buf[64];
        proven_net_addr_t from;
        proven_result_size_t r = proven_net_udp_recv_from(&b, (proven_mem_mut_t){ buf, sizeof buf }, &from, proven_net_deadline_in(2000));
        PROVEN_TEST_ASSERT(r.err == PROVEN_OK && r.value == 3 && memcmp(buf, "one", 3) == 0 && proven_net_addr_eq(&from, &a_at),
            "the first arrives whole, from the sender's address", "");
        r = proven_net_udp_recv_from(&b, (proven_mem_mut_t){ buf, sizeof buf }, &from, proven_net_deadline_in(2000));
        PROVEN_TEST_ASSERT(r.err == PROVEN_OK && r.value == 0, "the empty datagram is a success of zero bytes - a message, not an end", "");
        r = proven_net_udp_recv_from(&b, (proven_mem_mut_t){ buf, sizeof buf }, NULL, proven_net_deadline_in(2000));
        PROVEN_TEST_ASSERT(r.err == PROVEN_OK && r.value == 6 && memcmp(buf, "three!", 6) == 0, "the third arrives as its own message, and the sender may go unasked", "");

        PROVEN_TEST_ASSERT(proven_net_udp_send_to(&b, from, mv("reply"), proven_net_deadline_in(2000)) == PROVEN_OK, "a reply to the address that was reported", "");
        r = proven_net_udp_recv_from(&a, (proven_mem_mut_t){ buf, sizeof buf }, &from, proven_net_deadline_in(2000));
        PROVEN_TEST_ASSERT(r.err == PROVEN_OK && r.value == 5 && proven_net_addr_eq(&from, &b_at), "reaches the first socket", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a datagram larger than the buffer is cut, and says so",
        "PROVEN_ERR_OUT_OF_BOUNDS with the bytes that were kept; the rest of that datagram is gone, and the next one is intact.",
        "On POSIX only recvmsg reports the cut (MSG_TRUNC); recvfrom would return a short success.");
    // ---------------------------------------------------------------
    {
        PROVEN_TEST_ASSERT(proven_net_udp_send_to(&a, b_at, mv("0123456789"), proven_net_deadline_in(2000)) == PROVEN_OK &&
                           proven_net_udp_send_to(&a, b_at, mv("next"), proven_net_deadline_in(2000)) == PROVEN_OK,
            "a ten-byte datagram, then a four-byte one", "");
        proven_byte_t small[4];
        proven_result_size_t r = proven_net_udp_recv_from(&b, (proven_mem_mut_t){ small, sizeof small }, NULL, proven_net_deadline_in(2000));
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_OUT_OF_BOUNDS && r.value == 4 && memcmp(small, "0123", 4) == 0,
            "four bytes of room: PROVEN_ERR_OUT_OF_BOUNDS, and the first four bytes", "");
        proven_byte_t buf[16];
        r = proven_net_udp_recv_from(&b, (proven_mem_mut_t){ buf, sizeof buf }, NULL, proven_net_deadline_in(2000));
        PROVEN_TEST_ASSERT(r.err == PROVEN_OK && r.value == 4 && memcmp(buf, "next", 4) == 0,
            "the next receive is the next datagram, not the tail of the cut one", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("waiting ends at the deadline; a silent port breaks nothing",
        "Nothing to receive is PROVEN_ERR_TIMEOUT. A datagram to a port nobody holds does not poison later receives.",
        "On Windows a send to a closed port makes a later receive fail with a reset unless SIO_UDP_CONNRESET is turned off.");
    // ---------------------------------------------------------------
    {
        proven_byte_t buf[16];
        proven_time_t t0 = proven_time_monotonic_now();
        proven_result_size_t r = proven_net_udp_recv_from(&b, (proven_mem_mut_t){ buf, sizeof buf }, NULL, proven_net_deadline_in(60));
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_TIMEOUT && (proven_time_monotonic_now() - t0) >= 55 * 1000000LL,
            "an empty socket times out after its 60 ms", "");
        r = proven_net_udp_recv_from(&b, (proven_mem_mut_t){ buf, sizeof buf }, NULL, PROVEN_NET_DONT_WAIT);
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_TIMEOUT, "PROVEN_NET_DONT_WAIT does not wait", "");

        proven_net_udp_t gone;
        proven_net_addr_t gone_at;
        PROVEN_TEST_ASSERT(proven_net_udp_open(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &gone, &gone_at) == PROVEN_OK &&
                           proven_net_udp_close(&gone) == PROVEN_OK, "a port that was open and is not any more", "");
        (void)proven_net_udp_send_to(&a, gone_at, mv("anyone?"), proven_net_deadline_in(2000));
        proven_time_sleep(50);
        PROVEN_TEST_ASSERT(proven_net_udp_send_to(&b, a_at, mv("still here"), proven_net_deadline_in(2000)) == PROVEN_OK, "a real datagram afterwards", "");
        r = proven_net_udp_recv_from(&a, (proven_mem_mut_t){ buf, sizeof buf }, NULL, proven_net_deadline_in(2000));
        /* Linux reports the earlier ICMP error once on the next call; that one report is
         * tolerated, and the datagram must then be there. */
        if (r.err == PROVEN_ERR_REFUSED) {
            r = proven_net_udp_recv_from(&a, (proven_mem_mut_t){ buf, sizeof buf }, NULL, proven_net_deadline_in(2000));
        }
        PROVEN_TEST_ASSERT(r.err == PROVEN_OK && r.value == 10, "is still received by the socket that sent into the void", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("closing, and what a closed socket answers",
        "Close resets the value; using it afterwards is PROVEN_ERR_INVALID_STATE.",
        "");
    // ---------------------------------------------------------------
    {
        PROVEN_TEST_ASSERT(proven_net_udp_close(&a) == PROVEN_OK && proven_net_udp_close(&b) == PROVEN_OK && !proven_net_udp_is_open(&a),
            "both sockets close", "");
        PROVEN_TEST_ASSERT(proven_net_udp_close(&a) == PROVEN_OK, "closing again is not an error", "");
        proven_byte_t buf[4];
        PROVEN_TEST_ASSERT(proven_net_udp_send_to(&a, b_at, mv("x"), PROVEN_NET_DONT_WAIT) == PROVEN_ERR_INVALID_STATE &&
                           proven_net_udp_recv_from(&a, (proven_mem_mut_t){ buf, sizeof buf }, NULL, PROVEN_NET_DONT_WAIT).err == PROVEN_ERR_INVALID_STATE,
            "sending or receiving on a closed socket is PROVEN_ERR_INVALID_STATE", "");
        proven_net_addr_t un;
        proven_net_udp_t u;
        PROVEN_TEST_ASSERT(proven_net_addr_unix(PROVEN_LIT("/tmp/x"), &un) == PROVEN_OK &&
                           proven_net_udp_open(un, &u, NULL) == PROVEN_ERR_UNSUPPORTED,
            "a UDP socket on a Unix-domain path is PROVEN_ERR_UNSUPPORTED", "");
    }

    PROVEN_TEST_PASS("UDP datagrams behave as messages.");
    return 0;
}
