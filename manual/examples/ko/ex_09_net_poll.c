#include "example.h"

/*
 * 스레드 하나, 소켓 여럿: 그중 하나라도 손이 필요해질 때까지 기다렸다가, 필요한 것들을
 * 처리한다. 모든 단일 스레드 서버 안에 있는 루프다.
 *
 * "준비됨"은 그 종류의 다음 호출이 기다리지 않는다는 뜻이다. 그래서 준비됐다고 보고된
 * 소켓은 PROVEN_NET_DONT_WAIT로 다루고, 루프는 poll 말고는 어디서도 블록하지 않는다.
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

    /* 클라이언트 둘이 연결하고 각자 메시지 하나를 보낸다. */
    proven_net_conn_t clients[2];
    for (int i = 0; i < 2; ++i) {
        EXAMPLE_REQUIRE(proven_net_connect(at, proven_net_deadline_in(5000), &clients[i]) == PROVEN_OK, "a client connects");
    }
    EXAMPLE_REQUIRE(proven_net_write_all(&clients[0], proven_mem_view_from_u8(PROVEN_LIT("from the first")), proven_net_deadline_in(5000)).err == PROVEN_OK &&
                    proven_net_write_all(&clients[1], proven_mem_view_from_u8(PROVEN_LIT("from the second")), proven_net_deadline_in(5000)).err == PROVEN_OK,
                    "each sends a message");

    /* 서버: 리스너 하나와 연결 최대 둘을 목록 하나에. */
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

        /* 읽을 수 있는 리스너에는 기다리는 연결이 있다. */
        if (items[0].got & PROVEN_NET_READABLE) {
            EXAMPLE_REQUIRE(served_count < 2 &&
                            proven_net_accept(&listener, PROVEN_NET_DONT_WAIT, &served[served_count], NULL) == PROVEN_OK,
                            "the accept that poll predicted does not wait");
            served_count++;
        }
        /* 읽을 수 있는 연결에는 데이터가, 아니면 끝이 있다. `items[1 + i]`는 `served[i]`에서 만들었다. */
        for (proven_size_t i = 1; i < count; ++i) {
            if (!items[i].got) continue;
            proven_byte_t buf[64];
            proven_result_size_t r = proven_net_read(&served[i - 1], (proven_mem_mut_t){ buf, sizeof buf }, PROVEN_NET_DONT_WAIT);
            EXAMPLE_REQUIRE(r.err == PROVEN_OK, "the read that poll predicted does not wait");
            bytes_seen += r.value;
        }
    }
    EXAMPLE_REQUIRE(served_count == 2 && bytes_seen == 29, "both clients were accepted and both messages read, in one thread");

    /* proven_net_poll은 PROVEN_NET_POLL_INLINE_MAX개까지 받는다. 그보다 많은 서버는 자기
     * 작업 메모리를 넘긴다. OS는 배열을 원하고, 이 라이브러리는 그것을 할당하지 않는다. */
    proven_net_poll_item_t one = { .handle = proven_net_conn_handle(&served[0]), .want = PROVEN_NET_WRITABLE };
    proven_byte_t scratch[256];
    proven_size_t ready = 0;
    EXAMPLE_REQUIRE(proven_net_poll_scratch_size(1) <= sizeof scratch, "the scratch for one item is small");
    EXAMPLE_REQUIRE(proven_net_poll_with((proven_mem_mut_t){ scratch, sizeof scratch }, &one, 1, PROVEN_NET_DONT_WAIT, &ready) == PROVEN_OK &&
                    (one.got & PROVEN_NET_WRITABLE), "an idle connection is writable at once");

    /* UDP 소켓도 다른 것들과 같은 목록에서 기다린다. */
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
