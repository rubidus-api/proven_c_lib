#include "example.h"

/*
 * A protocol written once, against proven_transport_t, and run over a TCP connection.
 *
 * The function below does not know what carries its bytes. Today it is a socket; under TLS it
 * will be an encrypted session over a socket; in a test it can be two memory buffers. That is
 * what the interface is for.
 */

/* Send one request line and read one reply line, each with a five-second limit per step. */
static proven_err_t ask(proven_transport_t t, const char *question, proven_u8str_view_t expected) {
    if (!proven_transport_is_valid(t)) return PROVEN_ERR_INVALID_ARG;

    /* The formatter writes straight to the transport. */
    proven_transport_stream_t out_state;
    proven_writer_t out = proven_transport_writer(&out_state, t, 5000);
    proven_err_t err = proven_fprintln(out, "ASK {}", PROVEN_ARG(question)).err;
    if (err != PROVEN_OK) return err;

    /* The line reader reads from it, into a buffer this function owns. */
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

    /* A connection as a transport. From here on, neither side names a socket. */
    proven_transport_t client = proven_net_conn_transport(&client_conn);
    proven_transport_t server = proven_net_conn_transport(&server_conn);
    proven_net_deadline_t until = proven_net_deadline_in(5000);

    /* The server's half, done by hand: the reply is written before the client asks, which is
     * fine in one thread because the bytes simply wait in the connection. */
    proven_result_size_t w = proven_transport_write_all(server, proven_mem_view_from_u8(PROVEN_LIT("ANSWER 42\n")), until);
    EXAMPLE_REQUIRE(w.err == PROVEN_OK && w.value == 10, "the server's reply is on its way");

    EXAMPLE_REQUIRE(ask(client, "the question", PROVEN_LIT("ANSWER 42")) == PROVEN_OK,
                    "the protocol function sends its line and reads the reply");

    /* And the server reads what the client wrote, with the raw calls. */
    proven_byte_t buf[64];
    proven_result_size_t r = proven_transport_read(server, (proven_mem_mut_t){ buf, sizeof buf }, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_OK &&
                    proven_u8str_view_eq((proven_u8str_view_t){ buf, r.value }, PROVEN_LIT("ASK the question\n")),
                    "the request line arrived as written");

    /* A single write reports how much went; here, all of it. */
    w = proven_transport_write(server, proven_mem_view_from_u8(PROVEN_LIT("BYE\n")), until);
    EXAMPLE_REQUIRE(w.err == PROVEN_OK && w.value == 4, "one more line");

    /* Ending and closing go through the same interface. */
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
