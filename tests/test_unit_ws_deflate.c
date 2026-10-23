#include "proven.h"
#include "proven_test.h"
#include "../src/proven/proven_internal_ws.h"
#include <stdio.h>
#include <string.h>

/*
 * permessage-deflate (RFC 7692) below the drivers: the frame bit, the decoder's rules for it,
 * the negotiation, and the transform - with no socket.
 *
 * The byte sequences marked "RFC" are the examples of RFC 7692 section 7.2.3, copied from the
 * published text: they are what somebody else's compressor produced, and this side must read
 * them. What the drivers do with all this over a connection is test_unit_ws_conn's and
 * test_unit_ws_event's business; that Node's WebSocket implementations agree is checked by a
 * private program outside this suite.
 */

static proven_u8str_view_t sv(const char *s) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }

static proven_allocator_t g_heap;
static proven_byte_t g_plain[300000], g_wire[400000], g_back[300000 + 64];   /* room to spare: the end of a message is seen with room left */

/* Inflate one compressed message given as `count` pieces, each fed `step` bytes at a time into
 * `room` bytes of output. Returns the error, and the decoded size in *size. */
static proven_err_t inflate_pieces(const proven_mem_view_t *pieces, int count, bool text, proven_size_t step, proven_size_t room,
                                   proven_u64 max_bytes, proven_size_t *size) {
    proven_ws_inflate_t_ m = { 0 };
    *size = 0;
    proven_err_t e = proven_ws_inflate_begin_(&m, g_heap, text, NULL);
    bool done = false;
    for (int i = 0; i < count && e == PROVEN_OK; ++i) {
        proven_size_t at = 0;
        bool last = i == count - 1;
        for (;;) {
            proven_size_t give = pieces[i].size - at < step ? pieces[i].size - at : step;
            proven_size_t used = 0, made = 0;
            bool final_call = last && at + give == pieces[i].size;
            proven_size_t space = sizeof g_back - *size < room ? sizeof g_back - *size : room;
            e = proven_ws_inflate_step_(&m, (proven_mem_view_t){ .ptr = pieces[i].ptr + at, .size = give }, &used, final_call,
                                        (proven_mem_mut_t){ .ptr = g_back + *size, .size = space }, &made, max_bytes, &done);
            if (e != PROVEN_OK) break;
            at += used;
            *size += made;
            if (done) break;
            if (at == pieces[i].size && !last && used == 0 && made == 0) break;
            if (at == pieces[i].size && !last && made < space) break;
            if (used == 0 && made == 0 && space == 0) { e = PROVEN_ERR_OUT_OF_BOUNDS; break; }
        }
    }
    proven_ws_inflate_end_(&m);
    if (e == PROVEN_OK && !done) e = PROVEN_ERR_NEED_MORE;
    return e;
}

static proven_err_t inflate_one(const proven_byte_t *bytes, proven_size_t n, bool text, proven_size_t step, proven_size_t room, proven_size_t *size) {
    proven_mem_view_t piece = { .ptr = bytes, .size = n };
    return inflate_pieces(&piece, 1, text, step, room, 0, size);
}

/* Compress `n` bytes of g_plain as one message given in `parts` parts (each flushed, as a
 * message sent in parts is), into g_wire, through `room` bytes of output at a time. */
static proven_size_t deflate_parts(proven_size_t n, int parts, proven_size_t room, int level, int window_bits) {
    proven_deflate_t *z = NULL;
    PROVEN_TEST_ASSERT(proven_ws_deflate_begin_(g_heap, level, window_bits, parts == 1 ? n : 0, &z) == PROVEN_OK, "a compressor for one message", "");
    proven_byte_t tail[4];
    proven_u8 tail_len = 0;
    proven_size_t wire = 0, at = 0;
    for (int p = 0; p < parts; ++p) {
        proven_size_t end = p == parts - 1 ? n : n * (proven_size_t)(p + 1) / (proven_size_t)parts;
        bool done = false;
        while (!done) {
            proven_size_t used = 0, made = 0;
            proven_byte_t buf[4096];
            proven_size_t space = room < sizeof buf ? room : sizeof buf;
            proven_err_t e = proven_ws_deflate_step_(z, (proven_mem_view_t){ .ptr = g_plain + at, .size = end - at }, &used, true, p == parts - 1,
                                                     (proven_mem_mut_t){ .ptr = buf, .size = space }, &made, tail, &tail_len, &done);
            PROVEN_TEST_ASSERT(e == PROVEN_OK && made <= space, "a step of the compressor", "Check proven_ws_deflate_step_ in src/proven/ws_deflate.c.");
            PROVEN_TEST_ASSERT(wire + made <= sizeof g_wire, "the output fits the test's buffer", "");
            memcpy(g_wire + wire, buf, made);
            wire += made;
            at += used;
        }
    }
    proven_deflate_destroy(z);
    return wire;
}

/* One whole message through a compressor kept from message to message (takeover): its bytes
 * on the wire, the four closing ones left off, into `wire`. */
static proven_size_t deflate_kept(proven_deflate_t *z, const proven_byte_t *msg, proven_size_t n, proven_byte_t *wire, proven_size_t cap) {
    proven_byte_t tail[4];
    proven_u8 tail_len = 0;
    proven_size_t out = 0, at = 0;
    bool done = false;
    while (!done) {
        proven_size_t used = 0, made = 0;
        proven_byte_t buf[512];
        proven_err_t e = proven_ws_deflate_step_(z, (proven_mem_view_t){ .ptr = msg + at, .size = n - at }, &used, true, true,
                                                 (proven_mem_mut_t){ .ptr = buf, .size = sizeof buf }, &made, tail, &tail_len, &done);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && out + made <= cap, "a step of a kept compressor", "Check proven_ws_deflate_step_ in src/proven/ws_deflate.c.");
        memcpy(wire + out, buf, made);
        out += made;
        at += used;
    }
    return out;
}

/* One whole message through a decompressor kept from message to message: its decoded size, or
 * an error. */
static proven_err_t inflate_kept(proven_inflate_t **kept, const proven_byte_t *bytes, proven_size_t n, bool text, proven_size_t *size) {
    proven_ws_inflate_t_ m = { 0 };
    *size = 0;
    proven_err_t e = proven_ws_inflate_begin_(&m, g_heap, text, kept);
    bool done = false;
    proven_size_t at = 0;
    while (e == PROVEN_OK && !done) {
        proven_size_t used = 0, made = 0;
        e = proven_ws_inflate_step_(&m, (proven_mem_view_t){ .ptr = bytes + at, .size = n - at }, &used, true,
                                    (proven_mem_mut_t){ .ptr = g_back + *size, .size = sizeof g_back - *size }, &made, 0, &done);
        at += used;
        *size += made;
        if (e == PROVEN_OK && !done && used == 0 && made == 0) e = PROVEN_ERR_NEED_MORE;
    }
    proven_ws_inflate_end_(&m);
    return e;
}

int main(void) {
    PROVEN_TEST_SUITE("ws: permessage-deflate below the drivers",
        "The reserved bit that marks a compressed message, the decoder's rules for it, the negotiation of the extension, and the transform of one message - against the examples of RFC 7692.",
        "Inspect src/proven/ws.c (proven_ws_frame_parse, proven_ws_decoder_feed, proven_ws_deflate_accept, ws_check_response) and src/proven/ws_deflate.c.");
    g_heap = proven_heap_allocator();

    PROVEN_TEST_SECTION("the frame bit", "RSV1 is parsed and written where a message begins, and refused everywhere else.", "Check proven_ws_frame_parse and proven_ws_frame_write.");
    {
        static const struct { proven_byte_t b0, b1; proven_err_t want; bool rsv1; } rows[] = {
            { 0xc1, 0x07, PROVEN_OK, true },                   /* RFC: FIN, RSV1, text */
            { 0x41, 0x03, PROVEN_OK, true },                   /* RFC: RSV1, text, more to come */
            { 0xc2, 0x00, PROVEN_OK, true },                   /* binary */
            { 0x81, 0x05, PROVEN_OK, false },
            { 0x80, 0x04, PROVEN_OK, false },                  /* RFC: the continuation, without the bit */
            { 0xc0, 0x04, PROVEN_ERR_INVALID_FORMAT, false },  /* RSV1 on a continuation */
            { 0xc8, 0x00, PROVEN_ERR_INVALID_FORMAT, false },  /* on a close */
            { 0xc9, 0x00, PROVEN_ERR_INVALID_FORMAT, false },  /* on a ping */
            { 0xca, 0x00, PROVEN_ERR_INVALID_FORMAT, false },  /* on a pong */
            { 0xa1, 0x00, PROVEN_ERR_INVALID_FORMAT, false },  /* RSV2 */
            { 0x91, 0x00, PROVEN_ERR_INVALID_FORMAT, false },  /* RSV3 */
            { 0xf1, 0x00, PROVEN_ERR_INVALID_FORMAT, false },  /* all three */
        };
        for (proven_size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
            proven_byte_t h[2] = { rows[i].b0, rows[i].b1 };
            proven_ws_frame_t f = { 0 };
            proven_size_t hs = 0;
            proven_err_t e = proven_ws_frame_parse((proven_mem_view_t){ h, 2 }, &f, &hs);
            PROVEN_TEST_ASSERT(e == rows[i].want, "a header with reserved bits is parsed or refused by where the bit is", "");
            if (e != PROVEN_OK) continue;
            PROVEN_TEST_ASSERT(f.rsv1 == rows[i].rsv1 && hs == 2, "and RSV1 is reported", "");
            proven_byte_t back[PROVEN_WS_MAX_FRAME_HEADER];
            proven_size_t len = 0;
            PROVEN_TEST_ASSERT(proven_ws_frame_write((proven_mem_mut_t){ back, sizeof back }, &len, &f) == PROVEN_OK && len == 2 && back[0] == h[0] && back[1] == h[1], "and written back as it was", "");
        }
        proven_byte_t out[PROVEN_WS_MAX_FRAME_HEADER];
        proven_size_t len = 0;
        proven_ws_frame_t bad = { .fin = true, .opcode = PROVEN_WS_PING, .rsv1 = true };
        PROVEN_TEST_ASSERT(proven_ws_frame_write((proven_mem_mut_t){ out, sizeof out }, &len, &bad) == PROVEN_ERR_INVALID_ARG && len == 0, "the writer will not put the bit on a control frame", "");
        bad = (proven_ws_frame_t){ .fin = true, .opcode = PROVEN_WS_CONTINUATION, .rsv1 = true };
        PROVEN_TEST_ASSERT(proven_ws_frame_write((proven_mem_mut_t){ out, sizeof out }, &len, &bad) == PROVEN_ERR_INVALID_ARG && len == 0, "nor on a continuation", "");
    }

    PROVEN_TEST_SECTION("the decoder", "A compressed message is passed through as compressed bytes, marked, when the extension was agreed - and refused when it was not.", "Check proven_ws_decoder_feed.");
    {
        /* RFC 7692, 7.2.3.1: "Hello" in one frame, and the same in two. */
        proven_byte_t one[] = { 0xc1, 0x07, 0xf2, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00 };
        proven_byte_t two[] = { 0x41, 0x03, 0xf2, 0x48, 0xcd, 0x80, 0x04, 0xc9, 0xc9, 0x07, 0x00 };
        proven_ws_decoder_t d;
        proven_ws_event_t ev;
        proven_size_t used = 0;
        proven_ws_decoder_init(&d, false, 0);
        PROVEN_TEST_ASSERT(proven_ws_decoder_feed(&d, (proven_mem_mut_t){ one, sizeof one }, &used, &ev) == PROVEN_ERR_INVALID_FORMAT, "without the extension, RSV1 is PROVEN_ERR_INVALID_FORMAT as it always was", "");

        proven_byte_t again[] = { 0xc1, 0x07, 0xf2, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00 };
        proven_ws_decoder_init(&d, false, 0);
        proven_ws_decoder_allow_compressed(&d);
        PROVEN_TEST_ASSERT(proven_ws_decoder_feed(&d, (proven_mem_mut_t){ again, sizeof again }, &used, &ev) == PROVEN_OK && used == sizeof again &&
                           ev.kind == PROVEN_WS_EVENT_DATA && ev.compressed && ev.text && ev.first && ev.last && ev.data.size == 7 && ev.data.ptr[0] == 0xf2,
            "with it, the message arrives marked compressed, its bytes untouched and unchecked", "");

        proven_ws_decoder_init(&d, false, 0);
        proven_ws_decoder_allow_compressed(&d);
        PROVEN_TEST_ASSERT(proven_ws_decoder_feed(&d, (proven_mem_mut_t){ two, sizeof two }, &used, &ev) == PROVEN_OK && used == 5 && ev.compressed && ev.first && !ev.last && ev.data.size == 3,
            "a fragmented one: the first piece is marked", "");
        proven_size_t more = 0;
        PROVEN_TEST_ASSERT(proven_ws_decoder_feed(&d, (proven_mem_mut_t){ two + used, sizeof two - used }, &more, &ev) == PROVEN_OK && ev.compressed && !ev.first && ev.last && ev.data.size == 4,
            "and so is the continuation, though its frame does not carry the bit", "");

        /* An uncompressed message on a connection that agreed the extension is still checked. */
        proven_byte_t plain_bad[] = { 0x81, 0x02, 0xc3, 0x28 };
        proven_ws_decoder_init(&d, false, 0);
        proven_ws_decoder_allow_compressed(&d);
        PROVEN_TEST_ASSERT(proven_ws_decoder_feed(&d, (proven_mem_mut_t){ plain_bad, sizeof plain_bad }, &used, &ev) == PROVEN_ERR_INVALID_ENCODING, "text sent uncompressed is checked as before", "");
        proven_byte_t plain_ok[] = { 0x81, 0x02, 'h', 'i' };
        proven_ws_decoder_init(&d, false, 0);
        proven_ws_decoder_allow_compressed(&d);
        PROVEN_TEST_ASSERT(proven_ws_decoder_feed(&d, (proven_mem_mut_t){ plain_ok, sizeof plain_ok }, &used, &ev) == PROVEN_OK && !ev.compressed && ev.data.size == 2, "and is not marked", "");

        /* The limit counts compressed bytes here; the decoded ones are the driver's to count. */
        proven_byte_t limited[] = { 0xc1, 0x07, 0xf2, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00 };
        proven_ws_decoder_init(&d, false, 6);
        proven_ws_decoder_allow_compressed(&d);
        PROVEN_TEST_ASSERT(proven_ws_decoder_feed(&d, (proven_mem_mut_t){ limited, sizeof limited }, &used, &ev) == PROVEN_ERR_OUT_OF_BOUNDS, "seven compressed bytes pass no limit of six", "");
    }

    PROVEN_TEST_SECTION("the negotiation", "What a server answers to each offer, and what a client makes of each answer.", "Check ws_deflate_element, proven_ws_deflate_accept and ws_check_response in src/proven/ws.c.");
    {
        #define BOTH "permessage-deflate; server_no_context_takeover; client_no_context_takeover"
        static const struct { const char *offer; const char *answer; int bits; } rows[] = {
            { "permessage-deflate", BOTH, 15 },
            { PROVEN_WS_DEFLATE_OFFER, BOTH, 15 },
            { "permessage-deflate; client_max_window_bits", BOTH, 15 },
            { "permessage-deflate; client_max_window_bits=10", BOTH, 15 },
            { "permessage-deflate; server_max_window_bits=12", BOTH "; server_max_window_bits=12", 12 },
            { "permessage-deflate; server_max_window_bits=\"10\"", BOTH "; server_max_window_bits=10", 10 },
            { "permessage-deflate;server_max_window_bits=9;client_no_context_takeover", BOTH "; server_max_window_bits=9", 9 },
            { "permessage-deflate; server_max_window_bits=15", BOTH "; server_max_window_bits=15", 15 },
            { "PerMessage-Deflate ; Server_No_Context_Takeover", BOTH, 15 },
            { "permessage-deflate; server_max_window_bits=8, permessage-deflate", BOTH, 15 },       /* the first cannot be met: the second is */
            { "x-webkit-deflate-frame, permessage-deflate; client_max_window_bits", BOTH, 15 },
            { "permessage-deflate; unknown, permessage-deflate; server_max_window_bits=11", BOTH "; server_max_window_bits=11", 11 },
            { "permessage-deflate; server_max_window_bits=8", NULL, 0 },                            /* this compressor's smallest window is 9 bits */
            { "permessage-deflate; server_max_window_bits", NULL, 0 },                              /* a value is required */
            { "permessage-deflate; server_max_window_bits=16", NULL, 0 },
            { "permessage-deflate; server_max_window_bits=7", NULL, 0 },
            { "permessage-deflate; server_max_window_bits=012", NULL, 0 },
            { "permessage-deflate; server_max_window_bits=12; server_max_window_bits=12", NULL, 0 },
            { "permessage-deflate; server_no_context_takeover; server_no_context_takeover", NULL, 0 },
            { "permessage-deflate; server_no_context_takeover=1", NULL, 0 },
            { "permessage-deflate; client_max_window_bits=20", NULL, 0 },
            { "permessage-deflate; something_else", NULL, 0 },
            { "permessage-deflateX", NULL, 0 },
            { "x-webkit-deflate-frame", NULL, 0 },
            { "", NULL, 0 },
        };
        for (proven_size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
            proven_http_header_t h[2] = { { PROVEN_LIT("Host"), PROVEN_LIT("x") }, { PROVEN_LIT("sec-websocket-EXTENSIONS"), sv(rows[i].offer) } };
            proven_http_request_t req = { .headers = h, .header_count = 2 };
            proven_byte_t out[PROVEN_WS_DEFLATE_ANSWER_MAX];
            proven_size_t len = 0;
            int bits = 0;
            proven_err_t e = proven_ws_deflate_accept(&req, (proven_mem_mut_t){ out, sizeof out }, &len, &bits);
            if (rows[i].answer) {
                PROVEN_TEST_ASSERT(e == PROVEN_OK && len == strlen(rows[i].answer) && memcmp(out, rows[i].answer, len) == 0 && bits == rows[i].bits,
                    "an offer that can be met is answered with both no-takeover parameters, and the window it asked for", rows[i].offer);
                proven_byte_t small[20];
                proven_size_t l2 = 3;
                PROVEN_TEST_ASSERT(proven_ws_deflate_accept(&req, (proven_mem_mut_t){ small, sizeof small }, &l2, &bits) == PROVEN_ERR_OUT_OF_BOUNDS && l2 == 3, "too little room is PROVEN_ERR_OUT_OF_BOUNDS with nothing written", "");
            } else {
                PROVEN_TEST_ASSERT(e == PROVEN_ERR_NOT_FOUND && len == 0, "an offer that cannot be met is passed over: PROVEN_ERR_NOT_FOUND", rows[i].offer);
            }
        }
        {
            proven_http_header_t h[3] = { { PROVEN_LIT("Sec-WebSocket-Extensions"), PROVEN_LIT("foo") }, { PROVEN_LIT("Host"), PROVEN_LIT("x") },
                                          { PROVEN_LIT("Sec-WebSocket-Extensions"), PROVEN_LIT("permessage-deflate; server_max_window_bits=13") } };
            proven_http_request_t req = { .headers = h, .header_count = 3 };
            proven_byte_t out[PROVEN_WS_DEFLATE_ANSWER_MAX];
            proven_size_t len = 0;
            int bits = 0;
            PROVEN_TEST_ASSERT(proven_ws_deflate_accept(&req, (proven_mem_mut_t){ out, sizeof out }, &len, &bits) == PROVEN_OK && bits == 13, "offers in a second header field are read too", "");
            proven_http_request_t none = { .headers = h + 1, .header_count = 1 };
            PROVEN_TEST_ASSERT(proven_ws_deflate_accept(&none, (proven_mem_mut_t){ out, sizeof out }, &len, &bits) == PROVEN_ERR_NOT_FOUND, "no field at all: PROVEN_ERR_NOT_FOUND", "");
        }

        /* The client's side: the key and its answer are the pair of RFC 6455 section 1.3. */
        static const struct { const char *answer; proven_err_t want; bool deflate; } answers[] = {
            { NULL, PROVEN_OK, false },                                             /* the server did not take it */
            { BOTH, PROVEN_OK, true },
            { "permessage-deflate; server_no_context_takeover", PROVEN_OK, true },
            { "permessage-deflate; server_no_context_takeover; server_max_window_bits=10", PROVEN_OK, true },
            { "permessage-deflate; server_max_window_bits=8; server_no_context_takeover", PROVEN_OK, true },
            { "permessage-deflate", PROVEN_ERR_INVALID_FORMAT, false },             /* it would keep its window between messages */
            { "permessage-deflate; client_no_context_takeover", PROVEN_ERR_INVALID_FORMAT, false },
            { BOTH "; client_max_window_bits=10", PROVEN_ERR_INVALID_FORMAT, false },   /* a parameter that was not offered */
            { BOTH "; client_max_window_bits", PROVEN_ERR_INVALID_FORMAT, false },
            { BOTH ", " BOTH, PROVEN_ERR_INVALID_FORMAT, false },                   /* twice */
            { BOTH ", foo", PROVEN_ERR_INVALID_FORMAT, false },
            { "foo", PROVEN_ERR_INVALID_FORMAT, false },
            { BOTH "; server_max_window_bits=16", PROVEN_ERR_INVALID_FORMAT, false },
            { BOTH "; mystery", PROVEN_ERR_INVALID_FORMAT, false },
            { "", PROVEN_ERR_INVALID_FORMAT, false },
        };
        for (proven_size_t i = 0; i < sizeof answers / sizeof answers[0]; ++i) {
            proven_http_header_t h[4] = { { PROVEN_LIT("Upgrade"), PROVEN_LIT("websocket") }, { PROVEN_LIT("Connection"), PROVEN_LIT("Upgrade") },
                                          { PROVEN_LIT("Sec-WebSocket-Accept"), PROVEN_LIT("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") },
                                          { PROVEN_LIT("Sec-WebSocket-Extensions"), sv(answers[i].answer ? answers[i].answer : "") } };
            proven_http_response_t res = { .status = 101, .headers = h, .header_count = answers[i].answer ? 4 : 3 };
            proven_u8str_view_t protocol;
            bool deflate = true;
            proven_err_t e = proven_ws_check_response_deflate(&res, PROVEN_LIT("dGhlIHNhbXBsZSBub25jZQ=="), PROVEN_LIT(""), &protocol, &deflate);
            PROVEN_TEST_ASSERT(e == answers[i].want && deflate == answers[i].deflate, "a client that offered accepts the one form it offered and nothing else", answers[i].answer ? answers[i].answer : "(no field)");
            if (answers[i].answer) {
                PROVEN_TEST_ASSERT(proven_ws_check_response(&res, PROVEN_LIT("dGhlIHNhbXBsZSBub25jZQ=="), PROVEN_LIT(""), &protocol) == PROVEN_ERR_INVALID_FORMAT,
                    "and a client that offered nothing still refuses every extension", answers[i].answer);
            }
        }
        PROVEN_TEST_ASSERT(strcmp(PROVEN_WS_DEFLATE_OFFER, "permessage-deflate; client_no_context_takeover; server_no_context_takeover") == 0, "the offer a client sends asks both sides to keep nothing between messages", "");
        #undef BOTH
    }

    PROVEN_TEST_SECTION("the negotiation with takeover", "What a server that keeps its window answers, what it agreed, and what a client that offered takeover accepts.", "Check ws_deflate_answer, proven_ws_deflate_accept_takeover and ws_check_response in src/proven/ws.c.");
    {
        static const struct { const char *offer; const char *answer; bool server_takeover, client_takeover; int server_bits; } rows[] = {
            { "permessage-deflate", "permessage-deflate", true, true, 15 },
            { PROVEN_WS_DEFLATE_OFFER_TAKEOVER, "permessage-deflate", true, true, 15 },
            { "permessage-deflate; client_max_window_bits=10", "permessage-deflate", true, true, 15 },
            { "permessage-deflate; server_no_context_takeover", "permessage-deflate; server_no_context_takeover", false, true, 15 },
            { "permessage-deflate; client_no_context_takeover", "permessage-deflate; client_no_context_takeover", true, false, 15 },
            { PROVEN_WS_DEFLATE_OFFER, "permessage-deflate; server_no_context_takeover; client_no_context_takeover", false, false, 15 },
            { "permessage-deflate; server_max_window_bits=10", "permessage-deflate; server_max_window_bits=10", true, true, 10 },
            { "permessage-deflate; server_max_window_bits=8, permessage-deflate; client_no_context_takeover", "permessage-deflate; client_no_context_takeover", true, false, 15 },
            { "permessage-deflate; client_no_context_takeover; server_no_context_takeover; server_max_window_bits=9",
              "permessage-deflate; server_no_context_takeover; client_no_context_takeover; server_max_window_bits=9", false, false, 9 },
            { "permessage-deflate; mystery, permessage-deflate", "permessage-deflate", true, true, 15 },
            { "permessage-deflate; server_max_window_bits=8", NULL, false, false, 0 },
            { "x-webkit-deflate-frame", NULL, false, false, 0 },
        };
        for (proven_size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
            proven_http_header_t h[1] = { { PROVEN_LIT("Sec-WebSocket-Extensions"), sv(rows[i].offer) } };
            proven_http_request_t req = { .headers = h, .header_count = 1 };
            proven_byte_t out[PROVEN_WS_DEFLATE_ANSWER_MAX];
            proven_size_t len = 0;
            proven_ws_deflate_terms_t t = { 0 };
            proven_err_t e = proven_ws_deflate_accept_takeover(&req, (proven_mem_mut_t){ out, sizeof out }, &len, &t);
            if (!rows[i].answer) {
                PROVEN_TEST_ASSERT(e == PROVEN_ERR_NOT_FOUND && len == 0, "an offer that cannot be met is passed over", rows[i].offer);
                continue;
            }
            PROVEN_TEST_ASSERT(e == PROVEN_OK && len == strlen(rows[i].answer) && memcmp(out, rows[i].answer, len) == 0,
                "takeover is forbidden in the answer only where the offer asked for it", rows[i].offer);
            PROVEN_TEST_ASSERT(t.server_takeover == rows[i].server_takeover && t.client_takeover == rows[i].client_takeover &&
                               t.server_window_bits == rows[i].server_bits && t.client_window_bits == 15, "and the terms say what was agreed", rows[i].offer);
        }
        {
            proven_http_request_t none = { 0 };
            proven_byte_t out[8];
            proven_size_t len = 0;
            proven_ws_deflate_terms_t t;
            PROVEN_TEST_ASSERT(proven_ws_deflate_accept_takeover(&none, (proven_mem_mut_t){ out, sizeof out }, &len, NULL) == PROVEN_ERR_INVALID_ARG, "no place for the terms is PROVEN_ERR_INVALID_ARG", "");
            proven_http_header_t h[1] = { { PROVEN_LIT("Sec-WebSocket-Extensions"), PROVEN_LIT("permessage-deflate; server_no_context_takeover") } };
            proven_http_request_t req = { .headers = h, .header_count = 1 };
            len = 2;
            PROVEN_TEST_ASSERT(proven_ws_deflate_accept_takeover(&req, (proven_mem_mut_t){ out, sizeof out }, &len, &t) == PROVEN_ERR_OUT_OF_BOUNDS && len == 2,
                "too little room is PROVEN_ERR_OUT_OF_BOUNDS with nothing written", "");
        }
        PROVEN_TEST_ASSERT(strcmp(PROVEN_WS_DEFLATE_OFFER_TAKEOVER, "permessage-deflate; client_max_window_bits") == 0,
            "the offer of takeover leaves each side its window and lets the server bound the client's", "");

        static const struct { const char *answer; proven_err_t want; bool deflate, server_takeover, client_takeover; int server_bits, client_bits; } answers[] = {
            { NULL, PROVEN_OK, false, false, false, 0, 0 },
            { "permessage-deflate", PROVEN_OK, true, true, true, 15, 15 },
            { "permessage-deflate; server_no_context_takeover", PROVEN_OK, true, false, true, 15, 15 },
            { "permessage-deflate; client_no_context_takeover", PROVEN_OK, true, true, false, 15, 15 },
            { "permessage-deflate; server_no_context_takeover; client_no_context_takeover", PROVEN_OK, true, false, false, 15, 15 },
            { "permessage-deflate; client_max_window_bits=10", PROVEN_OK, true, true, true, 15, 10 },
            { "permessage-deflate; client_max_window_bits=8", PROVEN_OK, true, true, true, 15, 8 },
            { "permessage-deflate; server_max_window_bits=9; client_max_window_bits=\"12\"", PROVEN_OK, true, true, true, 9, 12 },
            { "permessage-deflate; client_max_window_bits", PROVEN_ERR_INVALID_FORMAT, false, false, false, 0, 0 },     /* an answer gives the value */
            { "permessage-deflate; client_no_context_takeover; client_no_context_takeover", PROVEN_ERR_INVALID_FORMAT, false, false, false, 0, 0 },
            { "permessage-deflate; client_max_window_bits=16", PROVEN_ERR_INVALID_FORMAT, false, false, false, 0, 0 },
            { "permessage-deflate, permessage-deflate", PROVEN_ERR_INVALID_FORMAT, false, false, false, 0, 0 },
            { "permessage-deflate; mystery", PROVEN_ERR_INVALID_FORMAT, false, false, false, 0, 0 },
            { "foo", PROVEN_ERR_INVALID_FORMAT, false, false, false, 0, 0 },
        };
        for (proven_size_t i = 0; i < sizeof answers / sizeof answers[0]; ++i) {
            proven_http_header_t h[4] = { { PROVEN_LIT("Upgrade"), PROVEN_LIT("websocket") }, { PROVEN_LIT("Connection"), PROVEN_LIT("Upgrade") },
                                          { PROVEN_LIT("Sec-WebSocket-Accept"), PROVEN_LIT("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") },
                                          { PROVEN_LIT("Sec-WebSocket-Extensions"), sv(answers[i].answer ? answers[i].answer : "") } };
            proven_http_response_t res = { .status = 101, .headers = h, .header_count = answers[i].answer ? 4 : 3 };
            proven_u8str_view_t protocol;
            bool deflate = true;
            proven_ws_deflate_terms_t t = { .server_takeover = true, .client_window_bits = 99 };
            proven_err_t e = proven_ws_check_response_takeover(&res, PROVEN_LIT("dGhlIHNhbXBsZSBub25jZQ=="), PROVEN_LIT(""), &protocol, &deflate, &t);
            const char *what = answers[i].answer ? answers[i].answer : "(no field)";
            PROVEN_TEST_ASSERT(e == answers[i].want && deflate == answers[i].deflate, "a client that offered takeover accepts every legal answer and nothing else", what);
            PROVEN_TEST_ASSERT(t.server_takeover == answers[i].server_takeover && t.client_takeover == answers[i].client_takeover &&
                               t.server_window_bits == answers[i].server_bits && t.client_window_bits == answers[i].client_bits, "and reports what was agreed - nothing, unless agreed", what);
        }
        proven_http_response_t bad = { .status = 200 };
        proven_u8str_view_t protocol;
        bool deflate = false;
        PROVEN_TEST_ASSERT(proven_ws_check_response_takeover(&bad, PROVEN_LIT("dGhlIHNhbXBsZSBub25jZQ=="), PROVEN_LIT(""), &protocol, &deflate, NULL) == PROVEN_ERR_INVALID_ARG,
            "no place for the terms is PROVEN_ERR_INVALID_ARG", "");
    }

    PROVEN_TEST_SECTION("the examples of RFC 7692", "Messages somebody else compressed, read at every step size.", "Check proven_ws_inflate_step_ in src/proven/ws_deflate.c.");
    {
        static const proven_byte_t hello[] = { 0xf2, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00 };                     /* 7.2.3.1 */
        static const proven_byte_t stored[] = { 0x00, 0x05, 0x00, 0xfa, 0xff, 0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x00 };   /* 7.2.3.3: a block with no compression */
        static const proven_byte_t final_block[] = { 0xf3, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00, 0x00 };          /* 7.2.3.4: BFINAL set */
        static const proven_byte_t two_blocks[] = { 0xf2, 0x48, 0x05, 0x00, 0x00, 0x00, 0xff, 0xff, 0xca, 0xc9, 0xc9, 0x07, 0x00 };   /* 7.2.3.5 */
        static const struct { const char *name; const proven_byte_t *bytes; proven_size_t size; } rows[] = {
            { "one block", hello, sizeof hello }, { "a block with no compression", stored, sizeof stored },
            { "a final block", final_block, sizeof final_block }, { "two blocks", two_blocks, sizeof two_blocks },
        };
        for (proven_size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
            for (proven_size_t step = 1; step <= 16; step = step == 1 ? 2 : step * 8) {
                for (proven_size_t room = 1; room <= 64; room *= 8) {
                    proven_size_t n = 0;
                    proven_err_t e = inflate_one(rows[i].bytes, rows[i].size, true, step, room, &n);
                    PROVEN_TEST_ASSERT(e == PROVEN_OK && n == 5 && memcmp(g_back, "Hello", 5) == 0, "the RFC's message decodes to Hello at every step size", rows[i].name);
                }
            }
        }
        /* 7.2.3.1 again as the two fragments the RFC shows. */
        proven_mem_view_t pieces[2] = { { hello, 3 }, { hello + 3, 4 } };
        proven_size_t n = 0;
        PROVEN_TEST_ASSERT(inflate_pieces(pieces, 2, true, 16, 64, 0, &n) == PROVEN_OK && n == 5 && memcmp(g_back, "Hello", 5) == 0, "and in two fragments", "");
        /* An empty message is the one byte 00. */
        static const proven_byte_t empty[] = { 0x00 };
        PROVEN_TEST_ASSERT(inflate_one(empty, 1, true, 1, 16, &n) == PROVEN_OK && n == 0, "the one byte 00 is an empty message", "");
    }

    PROVEN_TEST_SECTION("the transform, both ways", "What this side compresses, this side reads back - whole and in parts, through small buffers.", "Check proven_ws_deflate_step_ and proven_ws_inflate_step_.");
    {
        proven_u32 x = 88172645u;
        for (proven_size_t i = 0; i < sizeof g_plain; ++i) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            /* Words, with a stretch that does not compress in the middle. */
            g_plain[i] = (i > 100000 && i < 140000) ? (proven_byte_t)(x >> 9) : (proven_byte_t)("the quick brown fox \n"[(i + (x >> 28)) % 21]);
        }
        static const proven_size_t sizes[] = { 1, 2, 5, 63, 64, 1000, 65535, 65536, 70000, 300000 };
        static const int parts_of[] = { 1, 2, 7 };
        for (proven_size_t s = 0; s < sizeof sizes / sizeof sizes[0]; ++s) {
            for (proven_size_t p = 0; p < 3; ++p) {
                proven_size_t n = sizes[s];
                int parts = parts_of[p];
                if ((proven_size_t)parts > n) continue;
                proven_size_t wire = deflate_parts(n, parts, p == 2 ? 16 : 4096, (int)(1 + s % 9), s % 2 ? 0 : 9 + (int)(s % 7));
                if (parts == 1) PROVEN_TEST_ASSERT(wire < 4 || memcmp(g_wire + wire - 4, "\x00\x00\xff\xff", 4) != 0, "the four closing bytes are not sent", "");
                proven_size_t back = 0;
                proven_err_t e = inflate_one(g_wire, wire, false, s % 3 == 0 ? 1 : 5000, s % 2 ? 7 : 16384, &back);
                PROVEN_TEST_ASSERT(e == PROVEN_OK && back == n && memcmp(g_back, g_plain, n) == 0, "a message compressed here is read back here, byte for byte", "");
            }
        }
        /* A message in parts whose last part is empty: the receiver must still see an end. */
        {
            proven_deflate_t *z = NULL;
            PROVEN_TEST_ASSERT(proven_ws_deflate_begin_(g_heap, 0, 0, 0, &z) == PROVEN_OK, "a compressor", "");
            proven_byte_t tail[4], buf[256];
            proven_u8 tail_len = 0;
            proven_size_t used = 0, made = 0, wire = 0;
            bool done = false;
            PROVEN_TEST_ASSERT(proven_ws_deflate_step_(z, (proven_mem_view_t){ g_plain, 100 }, &used, true, false, (proven_mem_mut_t){ buf, sizeof buf }, &made, tail, &tail_len, &done) == PROVEN_OK && done && used == 100,
                "a first part, flushed", "");
            PROVEN_TEST_ASSERT(made >= 4 && memcmp(buf + made - 4, "\x00\x00\xff\xff", 4) == 0, "keeps its four bytes: only the end of a message loses them", "");
            memcpy(g_wire, buf, made);
            wire = made;
            PROVEN_TEST_ASSERT(proven_ws_deflate_step_(z, (proven_mem_view_t){ g_plain, 0 }, &used, true, true, (proven_mem_mut_t){ buf, sizeof buf }, &made, tail, &tail_len, &done) == PROVEN_OK && done && made == 1 && buf[0] == 0x00,
                "an empty last part is the one byte that begins an empty block", "");
            g_wire[wire++] = buf[0];
            proven_deflate_destroy(z);
            proven_size_t back = 0;
            PROVEN_TEST_ASSERT(inflate_one(g_wire, wire, false, 3, 64, &back) == PROVEN_OK && back == 100 && memcmp(g_back, g_plain, 100) == 0, "and the message reads back whole", "");
        }
    }

    PROVEN_TEST_SECTION("context takeover", "A window kept from one message to the next, on both sides: the RFC's shared-window example, a final block between messages, and similar short messages.", "Check proven_ws_inflate_begin_ with a kept decompressor, the restart in proven_ws_inflate_step_, and proven_inflate_restart in src/proven/deflate.c.");
    {
        /* RFC 7692 7.2.3.2: "Hello", then "Hello" again as a match five bytes back - into the
         * message before. */
        static const proven_byte_t first[] = { 0xf2, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00 };
        static const proven_byte_t again[] = { 0xf2, 0x00, 0x11, 0x00, 0x00 };
        /* 7.2.3.4: "Hello" in a final block, and the byte that starts the next stream. */
        static const proven_byte_t final_block[] = { 0xf3, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00, 0x00 };
        proven_inflate_t *kept = NULL;
        proven_size_t n = 0;
        PROVEN_TEST_ASSERT(inflate_kept(&kept, first, sizeof first, true, &n) == PROVEN_OK && n == 5 && memcmp(g_back, "Hello", 5) == 0 && kept != NULL,
            "the first message, through a decompressor made for the connection", "");
        PROVEN_TEST_ASSERT(inflate_kept(&kept, again, sizeof again, true, &n) == PROVEN_OK && n == 5 && memcmp(g_back, "Hello", 5) == 0,
            "the second refers back into the first, and decodes", "");
        PROVEN_TEST_ASSERT(inflate_one(again, sizeof again, true, 16, 64, &n) == PROVEN_ERR_INVALID_FORMAT, "where a decompressor of one message finds nothing to refer to", "");
        proven_inflate_destroy(kept);

        kept = NULL;
        PROVEN_TEST_ASSERT(inflate_kept(&kept, final_block, sizeof final_block, true, &n) == PROVEN_OK && n == 5 && memcmp(g_back, "Hello", 5) == 0,
            "a message that ends in a final block (7.2.3.4)", "");
        PROVEN_TEST_ASSERT(inflate_kept(&kept, again, sizeof again, true, &n) == PROVEN_OK && n == 5 && memcmp(g_back, "Hello", 5) == 0,
            "and the next still refers back across it: the stream restarts with its window", "Check that the `ended` branch calls proven_inflate_restart, not reset, for a kept decompressor.");
        PROVEN_TEST_ASSERT(inflate_kept(&kept, again, sizeof again, true, &n) == PROVEN_OK && n == 5 && memcmp(g_back, "Hello", 5) == 0, "and the one after", "");
        proven_inflate_destroy(kept);

        /* proven_inflate_restart itself, and that reset is not it. */
        {
            proven_inflate_t *z = NULL;
            PROVEN_TEST_ASSERT(proven_inflate_create(g_heap, PROVEN_DEFLATE_RAW, &z) == PROVEN_OK, "a raw decompressor", "");
            static const proven_byte_t hello_final[] = { 0xf3, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00 };   /* "Hello", BFINAL */
            static const proven_byte_t again_final[] = { 0xf3, 0x00, 0x11, 0x00, 0x00 };          /* a match 5 back, BFINAL: the RFC's second Hello with its first bit set */
            proven_byte_t out[16];
            proven_size_t used = 0, made = 0;
            bool done = false;
            PROVEN_TEST_ASSERT(proven_inflate(z, (proven_mem_view_t){ hello_final, sizeof hello_final }, &used, (proven_mem_mut_t){ out, sizeof out }, &made, &done) == PROVEN_OK &&
                               done && made == 5, "a stream that ends", "");
            proven_inflate_restart(z);
            PROVEN_TEST_ASSERT(proven_inflate(z, (proven_mem_view_t){ again_final, sizeof again_final }, &used, (proven_mem_mut_t){ out, sizeof out }, &made, &done) == PROVEN_OK &&
                               done && made == 5 && memcmp(out, "Hello", 5) == 0, "after proven_inflate_restart, the next stream reaches back into it", "");
            proven_inflate_reset(z);
            PROVEN_TEST_ASSERT(proven_inflate(z, (proven_mem_view_t){ again_final, sizeof again_final }, &used, (proven_mem_mut_t){ out, sizeof out }, &made, &done) == PROVEN_ERR_INVALID_FORMAT,
                "after reset it cannot", "");
            proven_inflate_restart(NULL);
            proven_inflate_destroy(z);
        }

        /* Two hundred short, similar messages, through one compressor and one decompressor -
         * and the same through a compressor for each, to compare. */
        {
            proven_deflate_t *z = NULL;
            PROVEN_TEST_ASSERT(proven_ws_deflate_begin_(g_heap, 0, PROVEN_WS_DEFLATE_TAKEOVER_BITS, 0, &z) == PROVEN_OK, "a compressor for the connection", "");
            proven_inflate_t *in = NULL;
            proven_size_t raw = 0, kept_bytes = 0, alone_bytes = 0;
            bool all_back = true;
            static const char *const words[] = { "ready", "steady", "go", "again", "later", "now" };
            for (int i = 0; i < 200; ++i) {
                char msg[128];
                int len = snprintf(msg, sizeof msg, "{\"type\":\"chat\",\"room\":\"lobby\",\"user\":%d,\"text\":\"%s %s\",\"seq\":%d}",
                                   i % 7, words[i % 6], words[(i * 5) % 6], i);
                raw += (proven_size_t)len;
                proven_size_t w = deflate_kept(z, (const proven_byte_t *)msg, (proven_size_t)len, g_wire, sizeof g_wire);
                kept_bytes += w;
                proven_size_t back = 0;
                if (inflate_kept(&in, g_wire, w, true, &back) != PROVEN_OK || back != (proven_size_t)len || memcmp(g_back, msg, back) != 0) all_back = false;
                proven_deflate_t *one = NULL;
                PROVEN_TEST_ASSERT(proven_ws_deflate_begin_(g_heap, 0, 0, (proven_size_t)len, &one) == PROVEN_OK, "a compressor for one message", "");
                alone_bytes += deflate_kept(one, (const proven_byte_t *)msg, (proven_size_t)len, g_wire, sizeof g_wire);
                proven_deflate_destroy(one);
            }
            fprintf(stderr, "[PROVEN][TEST][INFO] 200 chat messages, %u bytes: %u with takeover, %u each on its own\n", (unsigned)raw, (unsigned)kept_bytes, (unsigned)alone_bytes);
            PROVEN_TEST_ASSERT(all_back, "every message comes back, byte for byte, through the kept decompressor", "");
            PROVEN_TEST_ASSERT(kept_bytes * 3 < raw && kept_bytes * 2 < alone_bytes, "and takeover sends under a third of the bytes, and under half of what each on its own would", "");
            proven_deflate_destroy(z);
            proven_inflate_destroy(in);
        }
    }

    PROVEN_TEST_SECTION("what is refused", "The limit on decoded bytes, text that is not text, and bytes that are not DEFLATE.", "Check proven_ws_inflate_step_.");
    {
        memset(g_plain, 'a', 200000);
        proven_size_t wire = deflate_parts(200000, 1, 4096, 6, 0);
        PROVEN_TEST_ASSERT(wire < 1000, "two hundred thousand of one letter are a few hundred bytes on the wire", "");
        proven_mem_view_t piece = { g_wire, wire };
        proven_size_t n = 0;
        PROVEN_TEST_ASSERT(inflate_pieces(&piece, 1, true, 4096, 16384, 200000, &n) == PROVEN_OK && n == 200000, "at a limit of exactly their size they pass", "");
        PROVEN_TEST_ASSERT(inflate_pieces(&piece, 1, true, 4096, 16384, 199999, &n) == PROVEN_ERR_OUT_OF_BOUNDS && n <= 199999 + 16384, "one byte under it: PROVEN_ERR_OUT_OF_BOUNDS, long before the buffer is at risk", "");

        static const proven_byte_t not_utf8[] = { 0xc3, 0x28, 'x', 'x', 'x', 'x', 'x', 'x' };
        memcpy(g_plain, not_utf8, sizeof not_utf8);
        wire = deflate_parts(sizeof not_utf8, 1, 4096, 6, 0);
        PROVEN_TEST_ASSERT(inflate_one(g_wire, wire, true, 4096, 4096, &n) == PROVEN_ERR_INVALID_ENCODING, "text that inflates to bytes that are not UTF-8 is PROVEN_ERR_INVALID_ENCODING", "");
        PROVEN_TEST_ASSERT(inflate_one(g_wire, wire, false, 4096, 4096, &n) == PROVEN_OK && n == sizeof not_utf8, "and the same bytes as a binary message are fine", "");
        static const proven_byte_t cut_char[] = { 'a', 'b', 0xe2, 0x82 };              /* the first two bytes of a three-byte character */
        memcpy(g_plain, cut_char, sizeof cut_char);
        wire = deflate_parts(sizeof cut_char, 1, 4096, 6, 0);
        PROVEN_TEST_ASSERT(inflate_one(g_wire, wire, true, 4096, 4096, &n) == PROVEN_ERR_INVALID_ENCODING, "text that ends in the middle of a character is refused at the end", "");
        static const proven_byte_t whole_char[] = { 'a', 0xe2, 0x82, 0xac, 'b' };
        memcpy(g_plain, whole_char, sizeof whole_char);
        wire = deflate_parts(sizeof whole_char, 1, 4096, 6, 0);
        PROVEN_TEST_ASSERT(inflate_one(g_wire, wire, true, 1, 1, &n) == PROVEN_OK && n == sizeof whole_char, "a character split across one-byte pieces of output is still one character", "");

        static const proven_byte_t reserved_type[] = { 0x06, 0x00 };                    /* block type 3 */
        static const proven_byte_t bad_stored[] = { 0x00, 0x05, 0x00, 0x00, 0x00, 'H' };   /* a length and its complement that disagree */
        PROVEN_TEST_ASSERT(inflate_one(reserved_type, sizeof reserved_type, false, 16, 64, &n) == PROVEN_ERR_INVALID_FORMAT, "a block type that does not exist is PROVEN_ERR_INVALID_FORMAT", "");
        PROVEN_TEST_ASSERT(inflate_one(bad_stored, sizeof bad_stored, false, 16, 64, &n) == PROVEN_ERR_INVALID_FORMAT, "and so is a stored block whose length is wrong", "");
    }

    PROVEN_TEST_PASS("the bit, the rules, the negotiation and the transform of permessage-deflate hold.");
    return 0;
}
