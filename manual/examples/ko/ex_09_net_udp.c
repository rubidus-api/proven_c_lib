#include "example.h"

/*
 * UDP: 스트림이 아니라 메시지다. 보내기 한 번이 데이터그램 하나이고 받기 한 번이 데이터그램
 * 하나이며, 보낸 쪽 주소가 붙어 온다 - 그리고 도착한다는 약속은 없다.
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

    /* 보내기 둘은 데이터그램 둘이다. 성공은 각각이 이 기계를 떠났다는 뜻이다. */
    EXAMPLE_REQUIRE(proven_net_udp_send_to(&client, server_at, proven_mem_view_from_u8(PROVEN_LIT("first")), until) == PROVEN_OK &&
                    proven_net_udp_send_to(&client, server_at, proven_mem_view_from_u8(PROVEN_LIT("second message")), until) == PROVEN_OK,
                    "two datagrams leave");

    /* 받기 한 번은 온전한 데이터그램 하나이고, 누가 보냈는지 말해 준다. */
    proven_byte_t buf[32];
    proven_net_addr_t from;
    proven_result_size_t r = proven_net_udp_recv_from(&server, (proven_mem_mut_t){ buf, sizeof buf }, &from, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_OK && r.value == 5, "the first datagram, alone: five bytes");
    EXAMPLE_REQUIRE(proven_net_addr_eq(&from, &client_at), "from the client's address");

    /* 너무 작은 버퍼는 데이터그램을 자르고, 잘랐다고 말한다. 나머지는 사라진다 - 다음
     * 받기는 다음 데이터그램이다 - 그러니 버퍼는 가장 큰 메시지에 맞춰라. */
    proven_byte_t small[6];
    r = proven_net_udp_recv_from(&server, (proven_mem_mut_t){ small, sizeof small }, &from, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_ERR_OUT_OF_BOUNDS && r.value == sizeof small,
                    "fourteen bytes into six: PROVEN_ERR_OUT_OF_BOUNDS, six kept");

    /* 답장은 데이터그램이 온 주소로 간다. */
    EXAMPLE_REQUIRE(proven_net_udp_send_to(&server, from, proven_mem_view_from_u8(PROVEN_LIT("ack")), until) == PROVEN_OK,
                    "the server answers the sender");
    r = proven_net_udp_recv_from(&client, (proven_mem_mut_t){ buf, sizeof buf }, NULL, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_OK && r.value == 3, "and the client receives it");

    /* 더 오는 것이 없다. 잃어버린 데이터그램도 정확히 이렇게 보인다. 모든 받기에 기한이
     * 있고 모든 UDP 프로토콜에 재시도가 있는 이유다. */
    r = proven_net_udp_recv_from(&client, (proven_mem_mut_t){ buf, sizeof buf }, NULL, proven_net_deadline_in(50));
    EXAMPLE_REQUIRE(r.err == PROVEN_ERR_TIMEOUT, "silence is a timeout");

    EXAMPLE_REQUIRE(proven_net_udp_close(&client) == PROVEN_OK && proven_net_udp_close(&server) == PROVEN_OK,
                    "both sockets are closed by their owner");
    return EXAMPLE_OK();
}
