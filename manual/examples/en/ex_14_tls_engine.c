#include "example.h"
#include <string.h>

/*
 * The TLS engine with no network in sight: a client and a server connection in one program,
 * with this file carrying the bytes from one to the other. That is all a network does for them.
 *
 * It is the layer under the transport wrapper. Use it directly when the transport is yours:
 * an event loop, a serial line, a test.
 */

/* There is no certificate in this file. The server's identity is made when the program runs:
 * a self-signed certificate for example.test and its key. A certificate nobody vouches for is
 * believed only by a peer that already holds it, so the client is given it as its anchor. */

/* The engine reads no clock. A program gives it one; this one always says 2027-06-01, so the
 * example behaves the same on any day. */
static proven_i64 fixed_now(void *ctx) { (void)ctx; return 1811808000; }

/* Hand everything one side wants sent to the other. The receiver may take less than it was
 * given when it has application data waiting to be read; what it did not take stays pending. */
static proven_err_t carry(proven_tls_conn_t *from, proven_tls_conn_t *to) {
    proven_mem_view_t out = proven_tls_pending_output(from);
    proven_size_t at = 0;
    while (at < out.size) {
        proven_size_t used = 0;
        proven_err_t e = proven_tls_feed(to, (proven_mem_view_t){ out.ptr + at, out.size - at }, &used);
        if (e != PROVEN_OK) return e;
        if (used == 0) break;                 /* it wants its application data read first */
        at += used;
    }
    proven_tls_output_sent(from, at);
    return PROVEN_OK;
}

/* True when the bytes of `text` appear, in order, somewhere in `data`. */
static bool appears(proven_mem_view_t data, const char *text) {
    proven_size_t n = strlen(text);
    for (proven_size_t i = 0; i + n <= data.size; ++i) {
        if (memcmp(data.ptr + i, text, n) == 0) return true;
    }
    return false;
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    // ---- an identity ---------------------------------------------------------------
    /* proven_tls_self_signed makes an Ed25519 key and a certificate that names what you ask,
     * valid between two times. Here: from the day before this example's fixed clock to a year
     * after. A real server would be given a certificate from an authority instead. */
    proven_byte_t cert_pem[1024], key_pem[256];
    proven_size_t cert_len = 0, key_len = 0;
    proven_u8str_view_t names[1] = { PROVEN_LIT("example.test") };
    EXAMPLE_REQUIRE(proven_tls_self_signed(names, 1, 1811808000 - 86400, 1811808000 + 365 * 86400, NULL, NULL,
                                           (proven_mem_mut_t){ cert_pem, sizeof cert_pem }, &cert_len,
                                           (proven_mem_mut_t){ key_pem, sizeof key_pem }, &key_len) == PROVEN_OK, "a self-signed certificate and its key");

    // ---- two configurations ------------------------------------------------------
    /* A config says whom to believe and who you are. It is built once and shared by every
     * connection. The client believes the certificate just made; the server is that
     * certificate with its key. */
    proven_cert_store_t *anchors = NULL;
    EXAMPLE_REQUIRE(proven_cert_store_create(heap, &anchors) == PROVEN_OK &&
                    proven_cert_store_add_pem(anchors, (proven_mem_view_t){ cert_pem, cert_len }, NULL) == PROVEN_OK,
                    "the anchors");

    proven_u8str_view_t protocols[1] = { PROVEN_LIT("example/1") };
    proven_tls_options_t client_options = { .alloc = heap, .anchors = anchors, .now = fixed_now, .alpn = protocols, .alpn_count = 1 };
    proven_tls_options_t server_options = {
        .alloc = heap, .now = fixed_now, .alpn = protocols, .alpn_count = 1,
        .certificate_pem = { cert_pem, cert_len },
        .private_key_pem = { key_pem, key_len },
    };
    proven_tls_config_t *client_config = NULL, *server_config = NULL;
    EXAMPLE_REQUIRE(proven_tls_config_create(&client_options, &client_config) == PROVEN_OK, "a client configuration");
    EXAMPLE_REQUIRE(proven_tls_config_create(&server_options, &server_config) == PROVEN_OK, "a server configuration: its key matches its certificate");

    // ---- the handshake ------------------------------------------------------------
    /* A client connection has its first message ready the moment it exists. A server
     * connection waits. The session is where a ticket for next time will be put. */
    proven_tls_session_t session = { 0 };
    proven_tls_conn_t *client = NULL, *server = NULL;
    EXAMPLE_REQUIRE(proven_tls_client_create(client_config, PROVEN_LIT("example.test"), &session, &client) == PROVEN_OK, "a client");
    EXAMPLE_REQUIRE(proven_tls_server_create(server_config, &server) == PROVEN_OK, "a server");
    EXAMPLE_REQUIRE(proven_tls_pending_output(client).size > 0 && proven_tls_pending_output(server).size == 0, "the client speaks first");

    /* Carry bytes back and forth until both are done. Two round trips do it. */
    for (int round = 0; round < 10 && !(proven_tls_is_established(client) && proven_tls_is_established(server)); ++round) {
        EXAMPLE_REQUIRE(carry(client, server) == PROVEN_OK, "client to server");
        EXAMPLE_REQUIRE(carry(server, client) == PROVEN_OK, "server to client");
    }
    EXAMPLE_REQUIRE(proven_tls_is_established(client) && proven_tls_is_established(server), "both sides are established");
    EXAMPLE_REQUIRE(proven_tls_version(client) == PROVEN_TLS_VERSION_1_3 && proven_tls_version(server) == PROVEN_TLS_VERSION_1_3, "both spoke TLS 1.3, the newest version both sides have");
    EXAMPLE_REQUIRE(proven_tls_cipher_suite(client) == proven_tls_cipher_suite(server) && proven_tls_cipher_suite(client) != 0, "they agreed on a cipher suite");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_tls_alpn(client), PROVEN_LIT("example/1")), "and on the application protocol");
    EXAMPLE_REQUIRE(!proven_tls_resumed(client), "this was a full handshake");

    // ---- application data ----------------------------------------------------------
    /* Writing encrypts into the pending output; reading gives what feeding decrypted. */
    proven_result_size_t wrote = proven_tls_write(client, proven_mem_view_from_u8(PROVEN_LIT("hello, server")));
    EXAMPLE_REQUIRE(wrote.err == PROVEN_OK && wrote.value == 13, "thirteen bytes taken");
    proven_mem_view_t wire = proven_tls_pending_output(client);
    EXAMPLE_REQUIRE(wire.size > 13 && !appears(wire, "hello, server"), "what goes on the wire is a record, and not the text");
    EXAMPLE_REQUIRE(carry(client, server) == PROVEN_OK, "client to server");
    proven_byte_t buf[64];
    proven_result_size_t got = proven_tls_read(server, (proven_mem_mut_t){ buf, sizeof buf });
    EXAMPLE_REQUIRE(got.err == PROVEN_OK && got.value == 13 && memcmp(buf, "hello, server", 13) == 0, "the server reads what the client wrote");
    EXAMPLE_REQUIRE(proven_tls_read(server, (proven_mem_mut_t){ buf, sizeof buf }).err == PROVEN_ERR_NEED_MORE, "and then there is nothing more yet");

    // ---- closing --------------------------------------------------------------------
    /* A close is a message too: it tells the peer that the end is the end and not a cut wire. */
    EXAMPLE_REQUIRE(proven_tls_close(client) == PROVEN_OK && carry(client, server) == PROVEN_OK, "the close travels");
    EXAMPLE_REQUIRE(proven_tls_read(server, (proven_mem_mut_t){ buf, sizeof buf }).err == PROVEN_ERR_EOF, "and the server's read reports a proper end");
    proven_tls_conn_destroy(client);
    proven_tls_conn_destroy(server);

    // ---- resumption -----------------------------------------------------------------
    /* The first connection left a ticket in the session. A second connection that is given the
     * same session offers it, and the handshake skips the certificates and one signature. */
    EXAMPLE_REQUIRE(proven_tls_client_create(client_config, PROVEN_LIT("example.test"), &session, &client) == PROVEN_OK &&
                    proven_tls_server_create(server_config, &server) == PROVEN_OK, "a second pair");
    for (int round = 0; round < 10 && !(proven_tls_is_established(client) && proven_tls_is_established(server)); ++round) {
        EXAMPLE_REQUIRE(carry(client, server) == PROVEN_OK && carry(server, client) == PROVEN_OK, "client to server");
    }
    EXAMPLE_REQUIRE(proven_tls_resumed(client) && proven_tls_resumed(server), "both sides resumed");
    proven_tls_conn_destroy(client);
    proven_tls_conn_destroy(server);

    // ---- a server that is not the one asked for -------------------------------------
    /* The same server, reached under a name its certificate does not carry. The client refuses,
     * and says precisely why. */
    proven_tls_conn_t *wrong = NULL;
    EXAMPLE_REQUIRE(proven_tls_client_create(client_config, PROVEN_LIT("elsewhere.test"), NULL, &wrong) == PROVEN_OK &&
                    proven_tls_server_create(server_config, &server) == PROVEN_OK, "a second pair");
    EXAMPLE_REQUIRE(carry(wrong, server) == PROVEN_OK, "client to server");
    EXAMPLE_REQUIRE(carry(server, wrong) == PROVEN_ERR_NAME_MISMATCH, "the client's feed returns PROVEN_ERR_NAME_MISMATCH");
    EXAMPLE_REQUIRE(proven_tls_peer_fault(wrong) == PROVEN_CERT_FAULT_NAME_MISMATCH && !proven_tls_is_established(wrong), "with the precise fault, and no connection");
    /* A failure leaves an alert in the pending output. Send it: it is how the peer learns. */
    EXAMPLE_REQUIRE(proven_tls_pending_output(wrong).size > 0 && carry(wrong, server) == PROVEN_ERR_PROTOCOL, "the server is told");
    EXAMPLE_REQUIRE(proven_tls_alert_received(server) == 42, "bad_certificate, by its number");
    proven_tls_conn_destroy(wrong);
    proven_tls_conn_destroy(server);

    proven_mem_wipe((proven_mem_mut_t){ session.opaque, sizeof session.opaque });
    proven_mem_wipe((proven_mem_mut_t){ key_pem, sizeof key_pem });
    proven_tls_config_destroy(client_config);
    proven_tls_config_destroy(server_config);
    proven_cert_store_destroy(anchors);
    return EXAMPLE_OK();
}
