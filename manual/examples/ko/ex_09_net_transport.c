#include "example.h"

/*
 * proven_transport_t에 대고 한 번 쓴 프로토콜을 TCP 연결 위에서 돌린다.
 *
 * 아래 함수는 자기 바이트를 무엇이 실어 나르는지 모른다. 오늘은 소켓이고, TLS 아래에서는
 * 소켓 위의 암호화된 세션이 되며, 테스트에서는 메모리 버퍼 둘일 수 있다. 이 인터페이스가
 * 있는 이유가 그것이다.
 */

/* 요청 한 줄을 보내고 답 한 줄을 읽는다. 걸음마다 5초 제한. */
static proven_err_t ask(proven_transport_t t, const char *question, proven_u8str_view_t expected) {
    if (!proven_transport_is_valid(t)) return PROVEN_ERR_INVALID_ARG;

    /* 포매터가 전송에 곧바로 쓴다. */
    proven_transport_stream_t out_state;
    proven_writer_t out = proven_transport_writer(&out_state, t, 5000);
    proven_err_t err = proven_fprintln(out, "ASK {}", PROVEN_ARG(question)).err;
    if (err != PROVEN_OK) return err;

    /* 줄 reader가 거기서 읽어, 이 함수가 가진 버퍼에 담는다. */
    proven_transport_stream_t in_state;
    proven_byte_t line_buf[128];
    proven_reader_buffered_t lines;
    (void)proven_reader_buffered(&lines, proven_transport_reader(&in_state, t, 5000),
                                 (proven_mem_mut_t){ line_buf, sizeof line_buf });
    proven_result_u8str_view_t reply = proven_reader_read_line(&lines);
    if (reply.err != PROVEN_OK) return reply.err;
    return proven_u8str_view_eq(reply.val, expected) ? PROVEN_OK : PROVEN_ERR_INVALID_FORMAT;
}

int main(void) {
    proven_net_listener_t listener;
    proven_net_addr_t at;
    proven_err_t err = proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 4, &listener, &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "the listener opens");

    proven_net_conn_t client_conn, server_conn;
    EXAMPLE_REQUIRE(proven_net_connect(at, proven_net_deadline_in(5000), &client_conn) == PROVEN_OK &&
                    proven_net_accept(&listener, proven_net_deadline_in(5000), &server_conn, NULL) == PROVEN_OK,
                    "a connected pair");

    /* 전송으로서의 연결. 여기서부터는 어느 쪽도 소켓을 입에 올리지 않는다. */
    proven_transport_t client = proven_net_conn_transport(&client_conn);
    proven_transport_t server = proven_net_conn_transport(&server_conn);
    proven_net_deadline_t until = proven_net_deadline_in(5000);

    /* 서버 쪽 절반을 손으로 한다. 답을 클라이언트가 묻기 전에 써 둔다. 스레드 하나에서는
     * 괜찮다. 바이트가 연결 안에서 기다릴 뿐이다. */
    proven_result_size_t w = proven_transport_write_all(server, proven_mem_view_from_u8(PROVEN_LIT("ANSWER 42\n")), until);
    EXAMPLE_REQUIRE(w.err == PROVEN_OK && w.value == 10, "the server's reply is on its way");

    EXAMPLE_REQUIRE(ask(client, "the question", PROVEN_LIT("ANSWER 42")) == PROVEN_OK,
                    "the protocol function sends its line and reads the reply");

    /* 그리고 서버는 클라이언트가 쓴 것을 원시 호출로 읽는다. */
    proven_byte_t buf[64];
    proven_result_size_t r = proven_transport_read(server, (proven_mem_mut_t){ buf, sizeof buf }, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_OK &&
                    proven_u8str_view_eq((proven_u8str_view_t){ buf, r.value }, PROVEN_LIT("ASK the question\n")),
                    "the request line arrived as written");

    /* write 한 번은 얼마나 갔는지 보고한다. 여기서는 전부다. */
    w = proven_transport_write(server, proven_mem_view_from_u8(PROVEN_LIT("BYE\n")), until);
    EXAMPLE_REQUIRE(w.err == PROVEN_OK && w.value == 4, "one more line");

    /* 끝내기와 닫기도 같은 인터페이스를 거친다. */
    EXAMPLE_REQUIRE(proven_transport_shutdown(server) == PROVEN_OK, "the server ends its sending side");
    r = proven_transport_read(client, (proven_mem_mut_t){ buf, sizeof buf }, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_OK && r.value == 4, "the client reads the last line");
    r = proven_transport_read(client, (proven_mem_mut_t){ buf, sizeof buf }, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_ERR_EOF, "and then the end");

    EXAMPLE_REQUIRE(proven_transport_close(client) == PROVEN_OK && proven_transport_close(server) == PROVEN_OK,
                    "closing a transport closes what carries it");
    EXAMPLE_REQUIRE(!proven_net_conn_is_open(&client_conn), "the connection underneath is closed");
    (void)proven_net_listener_close(&listener);
    return EXAMPLE_OK();
}
