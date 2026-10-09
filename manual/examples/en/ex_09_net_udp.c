#include "example.h"

/*
 * UDP: messages, not a stream. Each send is one datagram and each receive is one datagram,
 * with the sender's address attached - and no promise that it arrives.
 */

int main(void) {
    proven_net_udp_t server, client;
    proven_net_addr_t server_at, client_at;
    proven_err_t err = proven_net_udp_open(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &server, &server_at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK && proven_net_udp_is_open(&server), "the server's socket opens");
    EXAMPLE_REQUIRE(proven_net_udp_open(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &client, NULL) == PROVEN_OK,
                    "and the client's");
    EXAMPLE_REQUIRE(proven_net_udp_addr(&client, &client_at) == PROVEN_OK && client_at.port != 0,
                    "a socket can be asked where it is bound");

    proven_net_deadline_t until = proven_net_deadline_in(2000);

    /* Two sends are two datagrams. Success means each left this machine. */
    EXAMPLE_REQUIRE(proven_net_udp_send_to(&client, server_at, proven_mem_view_from_u8(PROVEN_LIT("first")), until) == PROVEN_OK &&
                    proven_net_udp_send_to(&client, server_at, proven_mem_view_from_u8(PROVEN_LIT("second message")), until) == PROVEN_OK,
                    "two datagrams leave");

    /* Each receive is one whole datagram and says who sent it. */
    proven_byte_t buf[32];
    proven_net_addr_t from;
    proven_result_size_t r = proven_net_udp_recv_from(&server, (proven_mem_mut_t){ buf, sizeof buf }, &from, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_OK && r.value == 5, "the first datagram, alone: five bytes");
    EXAMPLE_REQUIRE(proven_net_addr_eq(&from, &client_at), "from the client's address");

    /* A buffer that is too small cuts the datagram and says so. The rest of it is gone - the
     * next receive is the next datagram - so size the buffer for the largest message. */
    proven_byte_t small[6];
    r = proven_net_udp_recv_from(&server, (proven_mem_mut_t){ small, sizeof small }, &from, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_ERR_OUT_OF_BOUNDS && r.value == sizeof small,
                    "fourteen bytes into six: PROVEN_ERR_OUT_OF_BOUNDS, six kept");

    /* The reply goes to the address the datagram came from. */
    EXAMPLE_REQUIRE(proven_net_udp_send_to(&server, from, proven_mem_view_from_u8(PROVEN_LIT("ack")), until) == PROVEN_OK,
                    "the server answers the sender");
    r = proven_net_udp_recv_from(&client, (proven_mem_mut_t){ buf, sizeof buf }, NULL, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_OK && r.value == 3, "and the client receives it");

    /* Nothing else is coming. A lost datagram looks exactly like this, which is why every
     * receive has a deadline and every UDP protocol has a retry. */
    r = proven_net_udp_recv_from(&client, (proven_mem_mut_t){ buf, sizeof buf }, NULL, proven_net_deadline_in(50));
    EXAMPLE_REQUIRE(r.err == PROVEN_ERR_TIMEOUT, "silence is a timeout");

    EXAMPLE_REQUIRE(proven_net_udp_close(&client) == PROVEN_OK && proven_net_udp_close(&server) == PROVEN_OK,
                    "both sockets are closed by their owner");
    return EXAMPLE_OK();
}
