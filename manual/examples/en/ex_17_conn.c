#include "example.h"
#include <stdio.h>
#include <string.h>

/*
 * An HTTP/2 connection, both ends of it, with no socket: what one end queues is handed to the
 * other as if it had crossed a wire.
 *
 * proven_h2_conn_t does no I/O. You feed it the bytes the peer sent and it reports one event
 * at a time; you ask it to send and it queues frames for you to collect. A real program puts
 * a socket (and TLS) where carry() is.
 */

typedef struct {
    proven_h2_conn_t *conn;
    const char *name;
    /* What this end has learned, for the checks at the bottom. */
    int requests, responses, trailers, resets, writable, pongs, goaways, closed;
    proven_size_t body_bytes;
    proven_u32 last_stream, reset_code, close_code;
    bool hold;                             /* do not say that DATA was consumed */
} ex_end_t;

/* Take every event the bytes in `in` hold. One call reports one event: call again with the
 * rest until there is no event and nothing left. */
static bool take_events(ex_end_t *e, proven_mem_view_t in) {
    proven_size_t pos = 0;
    for (;;) {
        proven_size_t used = 0;
        proven_h2_event_t ev;
        if (proven_h2_conn_feed(e->conn, (proven_mem_view_t){ in.ptr + pos, in.size - pos }, &used, &ev) != PROVEN_OK) return false;
        pos += used;
        switch (ev.kind) {
            case PROVEN_H2_EVENT_NONE:
                if (pos == in.size) return true;           /* everything was used: read more */
                break;
            case PROVEN_H2_EVENT_HEADERS:
                e->last_stream = ev.stream_id;
                if (ev.status == 0) {                      /* a server: a request, already checked and taken apart */
                    e->requests++;
                    printf("%s: stream %u asks %.*s %.*s\n", e->name, (unsigned)ev.stream_id, (int)ev.request.method.size, (const char *)ev.request.method.ptr,
                           (int)ev.request.path.size, (const char *)ev.request.path.ptr);
                } else {                                   /* a client: a response head */
                    e->responses++;
                    printf("%s: stream %u answers %u%s\n", e->name, (unsigned)ev.stream_id, (unsigned)ev.status, ev.user ? " (the stream we marked)" : "");
                }
                break;
            case PROVEN_H2_EVENT_DATA:
                /* ev.data points into `in`: nothing was copied. Saying it was consumed is
                 * what lets the peer send more. */
                e->body_bytes += ev.data.size;
                if (!e->hold) proven_h2_conn_consumed(e->conn, ev.stream_id, ev.data.size);
                break;
            case PROVEN_H2_EVENT_TRAILERS: e->trailers++; break;
            case PROVEN_H2_EVENT_STREAM_RESET: e->resets++; e->reset_code = ev.code; break;
            case PROVEN_H2_EVENT_STREAM_WRITABLE: e->writable++; break;
            case PROVEN_H2_EVENT_PONG: e->pongs++; break;
            case PROVEN_H2_EVENT_GOAWAY: e->goaways++; break;
            case PROVEN_H2_EVENT_CLOSED: e->closed++; e->close_code = ev.code; return true;
        }
    }
}

/* The wire: what `from` has queued goes to `to`. Then `from` is asked once more, with
 * nothing, for what sending made due - a stream that may go on. */
static bool carry(ex_end_t *from, ex_end_t *to) {
    static proven_byte_t wire[200000];
    proven_mem_view_t out = proven_h2_conn_pending_output(from->conn);
    if (out.size > sizeof wire) return false;
    if (out.size > 0) memcpy(wire, out.ptr, out.size);
    proven_size_t n = out.size;
    proven_h2_conn_output_sent(from->conn, n);             /* a socket would say how many it took */
    if (!to->closed && !take_events(to, (proven_mem_view_t){ wire, n })) return false;
    return from->closed || take_events(from, (proven_mem_view_t){ wire, 0 });
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    ex_end_t client = { .name = "client" }, server = { .name = "server" };

    /* Zero means the default for every limit. `server` is the one thing that must be said. */
    proven_h2_config_t ccfg = { .alloc = heap };
    proven_h2_config_t scfg = { .alloc = heap, .server = true, .max_concurrent_streams = 10 };
    EXAMPLE_REQUIRE(proven_h2_conn_create(&ccfg, &client.conn) == PROVEN_OK && proven_h2_conn_create(&scfg, &server.conn) == PROVEN_OK, "two ends");

    /* The opening is already queued: the client's preface and both ends' settings. */
    EXAMPLE_REQUIRE(carry(&client, &server) && carry(&server, &client) && carry(&client, &server), "settings cross and are acknowledged");
    /* A client that waits for this before its first request never asks for more streams than the server takes. */
    EXAMPLE_REQUIRE(proven_h2_conn_has_peer_settings(client.conn) && proven_h2_conn_has_peer_settings(server.conn), "each end has the other's settings");

    // ---- a request with a body, and its answer --------------------------------------------
    proven_http_header_t request[] = {
        { PROVEN_LIT(":method"), PROVEN_LIT("POST") }, { PROVEN_LIT(":scheme"), PROVEN_LIT("https") },
        { PROVEN_LIT(":authority"), PROVEN_LIT("www.example.com") }, { PROVEN_LIT(":path"), PROVEN_LIT("/upload") },
        { PROVEN_LIT("content-type"), PROVEN_LIT("text/plain") },
    };
    proven_u32 stream = 0;
    int mark = 0;
    EXAMPLE_REQUIRE(proven_h2_conn_submit_request(client.conn, request, 5, false, &stream) == PROVEN_OK && stream == 1, "a client's first stream is 1");
    proven_h2_conn_stream_set_user(client.conn, stream, &mark);       /* events for this stream will carry it */
    EXAMPLE_REQUIRE(proven_h2_conn_stream_user(client.conn, stream) == &mark, "and it can be asked for");
    proven_result_size_t sent = proven_h2_conn_send_data(client.conn, stream, proven_mem_view_from_u8(PROVEN_LIT("hello, ")), false);
    EXAMPLE_REQUIRE(sent.err == PROVEN_OK && sent.value == 7, "seven bytes of body");
    sent = proven_h2_conn_send_data(client.conn, stream, proven_mem_view_from_u8(PROVEN_LIT("world")), true);     /* true: the end of the body */
    EXAMPLE_REQUIRE(sent.err == PROVEN_OK && sent.value == 5 && carry(&client, &server), "and five more, the last");
    EXAMPLE_REQUIRE(server.requests == 1 && server.body_bytes == 12 && proven_h2_conn_stream_count(server.conn) == 1, "the server has the request and its twelve bytes");

    proven_http_header_t response[] = { { PROVEN_LIT(":status"), PROVEN_LIT("200") }, { PROVEN_LIT("content-type"), PROVEN_LIT("text/plain") } };
    proven_http_header_t trailers[] = { { PROVEN_LIT("x-checksum"), PROVEN_LIT("9f86d081") } };
    EXAMPLE_REQUIRE(proven_h2_conn_submit_response(server.conn, stream, response, 2, false) == PROVEN_OK, "a response head");
    sent = proven_h2_conn_send_data(server.conn, stream, proven_mem_view_from_u8(PROVEN_LIT("thanks")), false);
    EXAMPLE_REQUIRE(sent.err == PROVEN_OK && sent.value == 6 && proven_h2_conn_send_trailers(server.conn, stream, trailers, 1) == PROVEN_OK, "a body, and trailers to end it");
    EXAMPLE_REQUIRE(carry(&server, &client), "back to the client");
    EXAMPLE_REQUIRE(client.responses == 1 && client.body_bytes == 6 && client.trailers == 1, "the client has all three parts");
    EXAMPLE_REQUIRE(proven_h2_conn_stream_count(client.conn) == 0 && proven_h2_conn_stream_count(server.conn) == 0, "and the stream is over at both ends");

    // ---- flow control: a receiver that falls behind stops its sender ------------------------
    static proven_byte_t big[100000];
    server.hold = true;                                    /* the server's handler is busy: it consumes nothing */
    server.body_bytes = 0;
    /* A field no hop may remember is marked for the one list that follows: here the last, as if it were a key. */
    static const bool keep_out[5] = { false, false, false, false, true };
    proven_h2_conn_next_sensitive(client.conn, keep_out);
    EXAMPLE_REQUIRE(proven_h2_conn_submit_request(client.conn, request, 5, false, &stream) == PROVEN_OK && stream == 3 && carry(&client, &server), "a second request");
    sent = proven_h2_conn_send_data(client.conn, stream, (proven_mem_view_t){ big, sizeof big }, true);
    const proven_size_t taken = sent.value;
    printf("client: offered %u bytes, %u were taken\n", (unsigned)sizeof big, (unsigned)taken);
    /* The connection's window is 65,535 bytes by default, and the twelve bytes of the first
     * request have not been given back yet: windows are reopened in batches, not byte by byte. */
    EXAMPLE_REQUIRE(sent.err == PROVEN_OK && taken == 65535 - 12, "what the windows allow and no more: taking less than was offered is not an error");
    EXAMPLE_REQUIRE(proven_h2_conn_send_room(client.conn, stream) == 0 && carry(&client, &server) && carry(&server, &client) && client.writable == 0, "nothing more may go until the server says so");
    proven_h2_conn_consumed(server.conn, stream, taken);   /* the handler caught up */
    server.hold = false;
    EXAMPLE_REQUIRE(carry(&server, &client) && client.writable == 1, "and now the client is told: PROVEN_H2_EVENT_STREAM_WRITABLE");
    sent = proven_h2_conn_send_data(client.conn, stream, (proven_mem_view_t){ big + taken, sizeof big - taken }, true);
    EXAMPLE_REQUIRE(sent.err == PROVEN_OK && sent.value == sizeof big - taken && carry(&client, &server) && server.body_bytes == sizeof big, "the rest follows");

    /* The server gives this one up instead of answering. */
    EXAMPLE_REQUIRE(proven_h2_conn_reset(server.conn, stream, PROVEN_H2_CANCEL) == PROVEN_OK && carry(&server, &client), "a reset");
    EXAMPLE_REQUIRE(client.resets == 1 && client.reset_code == PROVEN_H2_CANCEL && proven_h2_conn_stream_count(client.conn) == 0, "which the client hears of, with the code");

    // ---- is anybody there? -----------------------------------------------------------------
    static const proven_byte_t opaque[8] = { 'p', 'r', 'o', 'v', 'e', 'n', '!', '!' };
    EXAMPLE_REQUIRE(proven_h2_conn_ping(client.conn, opaque) == PROVEN_OK && carry(&client, &server) && carry(&server, &client) && client.pongs == 1, "a ping is answered by the connection itself");

    // ---- settings changed after the opening ------------------------------------------------
    /* A server under load asks for fewer streams at once and smaller windows: one SETTINGS
     * frame, in force once the client acknowledges it. The numbers given at creation stay the
     * ceilings; streams already open are not touched. */
    EXAMPLE_REQUIRE(proven_h2_conn_set_max_streams(server.conn, 1) == PROVEN_OK && proven_h2_conn_set_initial_window(server.conn, 16384) == PROVEN_OK, "the server lowers both");
    EXAMPLE_REQUIRE(carry(&server, &client) && carry(&client, &server), "the change crosses, and is acknowledged");
    EXAMPLE_REQUIRE(proven_h2_conn_submit_request(client.conn, request, 5, true, &stream) == PROVEN_OK && carry(&client, &server), "one request");
    proven_u32 second = 0;
    EXAMPLE_REQUIRE(proven_h2_conn_submit_request(client.conn, request, 5, true, &second) == PROVEN_ERR_BUSY, "and not a second at the same time: the client holds itself to the new number");
    EXAMPLE_REQUIRE(proven_h2_conn_set_max_streams(server.conn, 11) == PROVEN_ERR_INVALID_ARG, "above the number given at creation: refused");
    EXAMPLE_REQUIRE(proven_h2_conn_reset(server.conn, stream, PROVEN_H2_CANCEL) == PROVEN_OK && carry(&server, &client), "the server lets that one go");

    // ---- a peer that breaks a rule ---------------------------------------------------------
    /* A WINDOW_UPDATE that adds nothing is a protocol error. The connection says so to its
     * peer (GOAWAY), reports that it is over, and takes nothing more. */
    ex_end_t strict = { .name = "strict" };
    EXAMPLE_REQUIRE(proven_h2_conn_create(&scfg, &strict.conn) == PROVEN_OK, "another server");
    proven_byte_t bad[128];
    proven_size_t len = PROVEN_H2_PREFACE_SIZE;
    memcpy(bad, PROVEN_H2_PREFACE, PROVEN_H2_PREFACE_SIZE);
    EXAMPLE_REQUIRE(proven_h2_write_settings((proven_mem_mut_t){ bad, sizeof bad }, &len, NULL, 0) == PROVEN_OK, "a client that opens properly");
    static const proven_byte_t zero_increment[13] = { 0, 0, 4, PROVEN_H2_WINDOW_UPDATE, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    memcpy(bad + len, zero_increment, sizeof zero_increment);
    len += sizeof zero_increment;
    EXAMPLE_REQUIRE(take_events(&strict, (proven_mem_view_t){ bad, len }), "the feed itself succeeds: a peer's fault is an event");
    printf("strict: closed with code %u\n", (unsigned)strict.close_code);
    EXAMPLE_REQUIRE(strict.closed == 1 && strict.close_code == PROVEN_H2_PROTOCOL_ERROR && proven_h2_conn_is_closed(strict.conn), "PROTOCOL_ERROR, and the connection is over");
    EXAMPLE_REQUIRE(proven_h2_conn_pending_output(strict.conn).size > 0, "with a GOAWAY still to be sent before the socket is closed");
    proven_h2_conn_destroy(strict.conn);

    /* And bytes that are not HTTP/2 at all are an error of the call, at the first of them. */
    EXAMPLE_REQUIRE(proven_h2_conn_create(&scfg, &strict.conn) == PROVEN_OK, "one more");
    proven_size_t used = 0;
    proven_h2_event_t ev;
    EXAMPLE_REQUIRE(proven_h2_conn_feed(strict.conn, proven_mem_view_from_u8(PROVEN_LIT("GET / HTTP/1.1\r\n\r\n")), &used, &ev) == PROVEN_ERR_INVALID_FORMAT && used == 0,
                    "HTTP/1.1 is PROVEN_ERR_INVALID_FORMAT with nothing taken: the bytes are yours to give to something else");
    proven_h2_conn_destroy(strict.conn);

    // ---- goodbye --------------------------------------------------------------------------
    proven_h2_conn_shutdown(server.conn, PROVEN_H2_NO_ERROR);          /* no new streams; open ones would finish first */
    EXAMPLE_REQUIRE(carry(&server, &client) && carry(&client, &server), "the goodbye crosses");
    EXAMPLE_REQUIRE(client.goaways == 1 && client.closed == 1 && server.closed == 1 && client.close_code == PROVEN_H2_NO_ERROR, "with no stream open, both ends report the end at once");
    EXAMPLE_REQUIRE(proven_h2_conn_submit_request(client.conn, request, 5, true, &stream) == PROVEN_ERR_INVALID_STATE, "and nothing more is begun");

    proven_h2_conn_destroy(client.conn);
    proven_h2_conn_destroy(server.conn);
    return EXAMPLE_OK();
}
