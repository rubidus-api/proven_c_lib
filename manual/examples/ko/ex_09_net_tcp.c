#include "example.h"

/*
 * 처음부터 끝까지의 TCP 교환. 기다릴 수 있는 모든 호출에 기한이 붙는다.
 *
 * 양쪽 끝이 이 프로그램 하나에 산다. 커널은 리스너의 백로그가 연결을 받는 순간 - accept가
 * 불리기 전에 - 연결을 완성하므로 이것이 가능하다. 실제 클라이언트와 서버는 같은 호출들을
 * 두 프로세스에 나눠 놓은 것이다.
 */

int main(void) {
    /* 포트 0: OS가 빈 포트를 고르고, `at`이 어느 것인지 알려 준다. 루프백: 이 기계만. */
    proven_net_listener_t listener;
    proven_net_addr_t at;
    proven_err_t err = proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 16, &listener, &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK && proven_net_listener_is_open(&listener), "the listener opens");
    EXAMPLE_REQUIRE(at.port != 0, "and reports the port it was given");

    /* 교환 전체에 기한 하나: 호출이 몇 번이 되든 지금부터 5초. */
    proven_net_deadline_t until = proven_net_deadline_in(5000);

    proven_net_conn_t client, server;
    proven_net_addr_t peer;
    EXAMPLE_REQUIRE(proven_net_connect(at, until, &client) == PROVEN_OK, "the client connects");
    EXAMPLE_REQUIRE(proven_net_accept(&listener, until, &server, &peer) == PROVEN_OK, "the server accepts");
    EXAMPLE_REQUIRE(proven_net_conn_is_open(&client) && proven_net_conn_is_open(&server), "two open ends");

    /* 양쪽 끝이 두 주소를 다 안다. accept가 준 상대 주소는 클라이언트 자신의 로컬 주소다. */
    proven_net_addr_t client_local, client_peer, listening;
    EXAMPLE_REQUIRE(proven_net_conn_local_addr(&client, &client_local) == PROVEN_OK &&
                    proven_net_conn_peer_addr(&client, &client_peer) == PROVEN_OK, "the client's two addresses");
    EXAMPLE_REQUIRE(proven_net_addr_eq(&client_local, &peer), "the server saw the client's address");
    EXAMPLE_REQUIRE(proven_net_listener_addr(&listener, &listening) == PROVEN_OK &&
                    proven_net_addr_eq(&client_peer, &listening), "the client's peer is where the server listens");

    /* 작은 조각 둘로 쓰는 요청: 이것이 없으면 둘이 함께 나가려고 기다릴 수 있다. */
    EXAMPLE_REQUIRE(proven_net_conn_set_nodelay(&client, true) == PROVEN_OK, "small writes go at once");

    /* write_all은 전부 보내거나, 어디까지 갔는지 말한다. */
    proven_mem_view_t request = proven_mem_view_from_u8(PROVEN_LIT("what time is it?"));
    proven_result_size_t sent = proven_net_write_all(&client, request, until);
    EXAMPLE_REQUIRE(sent.err == PROVEN_OK && sent.value == request.size, "the whole request went");

    /* "더 보낼 것이 없다." 서버는 요청을 읽고, 그다음 입력의 끝을 읽는다. */
    EXAMPLE_REQUIRE(proven_net_shutdown_write(&client) == PROVEN_OK, "the client half-closes");

    /* 서버는 끝까지 읽는다. read는 도착한 만큼을 돌려준다 - 스트림에는 메시지 경계가
     * 없다 - 그러니 "전부"를 읽는 올바른 방법은 루프뿐이다. */
    proven_byte_t buf[64];
    proven_size_t got = 0;
    for (;;) {
        proven_result_size_t r = proven_net_read(&server, (proven_mem_mut_t){ buf + got, sizeof buf - got }, until);
        if (r.err == PROVEN_ERR_EOF) break;                  /* 클라이언트가 끝냈다 */
        EXAMPLE_REQUIRE(r.err == PROVEN_OK, "a read that is not the end succeeds");
        if (r.err != PROVEN_OK) break;
        got += r.value;
    }
    EXAMPLE_REQUIRE(got == request.size, "the server received the whole request");

    /* write 한 번은 일부만 보낼 수 있고, 개수가 얼마인지 말한다. 여기서는 전부 들어간다. */
    proven_mem_view_t reply = proven_mem_view_from_u8(PROVEN_LIT("time to close"));
    proven_result_size_t w = proven_net_write(&server, reply, until);
    EXAMPLE_REQUIRE(w.err == PROVEN_OK && w.value == reply.size, "a short reply goes in one write");

    proven_result_size_t r = proven_net_read(&client, (proven_mem_mut_t){ buf, sizeof buf }, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_OK && r.value == reply.size, "the client reads the reply after its half-close");

    /* 더 오는 것이 없고, 클라이언트는 그것을 알아내려고 영원히 기다리지 않는다. 타임아웃은
     * 손상이 아니다. 연결은 전과 똑같이 쓸 수 있다. */
    r = proven_net_read(&client, (proven_mem_mut_t){ buf, sizeof buf }, proven_net_deadline_in(50));
    EXAMPLE_REQUIRE(r.err == PROVEN_ERR_TIMEOUT, "silence is a timeout, not a hang");

    /* 모든 소켓은 그 주인이 닫는다. */
    EXAMPLE_REQUIRE(proven_net_close(&server) == PROVEN_OK, "the server closes its end");
    r = proven_net_read(&client, (proven_mem_mut_t){ buf, sizeof buf }, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_ERR_EOF, "which the client reads as the end");
    EXAMPLE_REQUIRE(proven_net_close(&client) == PROVEN_OK, "the client closes");
    EXAMPLE_REQUIRE(proven_net_listener_close(&listener) == PROVEN_OK, "the listener closes");

    /* 리스너가 사라지면 같은 주소가 거절한다 - 타임아웃과는 다른 답이다. */
    proven_net_conn_t nobody;
    EXAMPLE_REQUIRE(proven_net_connect(at, proven_net_deadline_in(15000), &nobody) == PROVEN_ERR_REFUSED,
                    "nothing listening is PROVEN_ERR_REFUSED");

    return EXAMPLE_OK();
}
