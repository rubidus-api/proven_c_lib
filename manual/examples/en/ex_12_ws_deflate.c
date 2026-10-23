#include "example.h"
#include <stdio.h>
#include <string.h>

/*
 * Compressed WebSocket messages with the codec alone: no socket, no driver.
 *
 * Three things make up the extension called permessage-deflate. The handshake agrees on it.
 * One bit in a frame's header says "this message is compressed". And the payload is DEFLATE
 * with its last four bytes left off. The codec does the first two; the third is deflate.h.
 *
 * A program that uses ws_conn.h or ws_event.h sets one field and sees none of this.
 */

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    // ---- the handshake ---------------------------------------------------------------
    /* A client asks with one header. PROVEN_WS_DEFLATE_OFFER is the simpler of the two forms
     * this library speaks: each message compressed on its own, in both directions. The other,
     * with context takeover, is at the end. */
    proven_http_header_t asked[] = {
        { PROVEN_LIT("Sec-WebSocket-Extensions"), PROVEN_LIT(PROVEN_WS_DEFLATE_OFFER) },
    };
    proven_http_request_t request = { .headers = asked, .header_count = 1 };

    /* A server looks at what was offered and writes its answer - or finds nothing it can
     * meet (PROVEN_ERR_NOT_FOUND), and the connection goes on uncompressed. */
    proven_byte_t answer[PROVEN_WS_DEFLATE_ANSWER_MAX];
    proven_size_t answer_len = 0;
    int window_bits = 0;
    EXAMPLE_REQUIRE(proven_ws_deflate_accept(&request, (proven_mem_mut_t){ answer, sizeof answer }, &answer_len, &window_bits) == PROVEN_OK, "the offer can be met");
    printf("the server answers: %.*s\n", (int)answer_len, (const char *)answer);
    EXAMPLE_REQUIRE(window_bits == 15, "and may compress with the full window");

    /* A browser's offer looks different, and is met as well. */
    proven_http_header_t browser[] = { { PROVEN_LIT("Sec-WebSocket-Extensions"), PROVEN_LIT("permessage-deflate; client_max_window_bits") } };
    proven_http_request_t from_browser = { .headers = browser, .header_count = 1 };
    proven_size_t len2 = 0;
    EXAMPLE_REQUIRE(proven_ws_deflate_accept(&from_browser, (proven_mem_mut_t){ answer, sizeof answer }, &len2, &window_bits) == PROVEN_OK, "a browser's offer too");

    /* The client checks the whole answer: the usual things, and that the extension came back
     * in the form it asked for. (The key and the accept value are the pair in RFC 6455.) */
    proven_http_header_t answered[] = {
        { PROVEN_LIT("Upgrade"), PROVEN_LIT("websocket") },
        { PROVEN_LIT("Connection"), PROVEN_LIT("Upgrade") },
        { PROVEN_LIT("Sec-WebSocket-Accept"), PROVEN_LIT("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") },
        { PROVEN_LIT("Sec-WebSocket-Extensions"), { answer, answer_len } },
    };
    proven_http_response_t response = { .status = 101, .headers = answered, .header_count = 4 };
    proven_u8str_view_t protocol;
    bool compressed = false;
    EXAMPLE_REQUIRE(proven_ws_check_response_deflate(&response, PROVEN_LIT("dGhlIHNhbXBsZSBub25jZQ=="), PROVEN_LIT(""), &protocol, &compressed) == PROVEN_OK && compressed,
                    "the client finds the extension agreed");

    // ---- a compressed message arrives -------------------------------------------------
    /* This frame is in RFC 7692: the text "Hello", compressed, from a server. Its first
     * byte is c1 where an uncompressed text frame has 81: the extra bit is RSV1. */
    proven_byte_t wire[] = { 0xc1, 0x07, 0xf2, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00 };

    proven_ws_decoder_t decoder;
    proven_ws_event_t event;
    proven_size_t used = 0;
    /* A decoder that was not told about the extension refuses the bit, as it must. */
    proven_byte_t copy[sizeof wire];
    memcpy(copy, wire, sizeof wire);
    proven_ws_decoder_init(&decoder, false, 0);
    EXAMPLE_REQUIRE(proven_ws_decoder_feed(&decoder, (proven_mem_mut_t){ copy, sizeof copy }, &used, &event) == PROVEN_ERR_INVALID_FORMAT, "without the extension, the bit is an error");

    proven_ws_decoder_init(&decoder, false, 0);
    proven_ws_decoder_allow_compressed(&decoder);
    EXAMPLE_REQUIRE(proven_ws_decoder_feed(&decoder, (proven_mem_mut_t){ wire, sizeof wire }, &used, &event) == PROVEN_OK &&
                    event.kind == PROVEN_WS_EVENT_DATA && event.compressed && event.last, "with it, the message is handed over, marked compressed");

    /* The decoder does not inflate: it allocates nothing. The bytes it hands over are raw
     * DEFLATE whose last four bytes - always 00 00 ff ff - the sender left off. Put them
     * back and inflate. The limit on what comes out is the size of your buffer. */
    proven_inflate_t *z = NULL;
    EXAMPLE_REQUIRE(proven_inflate_create(heap, PROVEN_DEFLATE_RAW, &z) == PROVEN_OK, "a decompressor for raw DEFLATE");
    static const proven_byte_t tail[4] = { 0x00, 0x00, 0xff, 0xff };
    proven_byte_t text[64];
    proven_size_t taken = 0, made = 0, total = 0;
    bool ended = false;
    EXAMPLE_REQUIRE(proven_inflate(z, event.data, &taken, (proven_mem_mut_t){ text, sizeof text }, &made, &ended) == PROVEN_OK && taken == event.data.size, "the payload");
    total = made;
    EXAMPLE_REQUIRE(proven_inflate(z, (proven_mem_view_t){ tail, 4 }, &taken, (proven_mem_mut_t){ text + total, sizeof text - total }, &made, &ended) == PROVEN_OK && taken == 4, "then the four bytes");
    total += made;
    EXAMPLE_REQUIRE(total == 5 && memcmp(text, "Hello", 5) == 0, "and the message is Hello");
    printf("the message: %.*s\n", (int)total, (const char *)text);
    /* Now - not before - it is text, and yours to check: the decoder could not. */
    proven_inflate_destroy(z);

    // ---- a compressed message is sent --------------------------------------------------
    /* The other way: compress with a sync flush, which ends in those same four bytes, and
     * leave them off. The frame that carries it has `rsv1` set. */
    static const char said[] = "to be, or not to be, that is the question: to be, or not to be";
    proven_deflate_options_t options = { .alloc = heap, .format = PROVEN_DEFLATE_RAW };
    proven_deflate_t *c = NULL;
    EXAMPLE_REQUIRE(proven_deflate_create(&options, &c) == PROVEN_OK, "a compressor");
    proven_byte_t packed[128];
    EXAMPLE_REQUIRE(proven_deflate(c, (proven_mem_view_t){ (const proven_byte_t *)said, sizeof said - 1 }, &taken, (proven_mem_mut_t){ packed, sizeof packed }, &made,
                                   PROVEN_DEFLATE_FLUSH_SYNC, &ended) == PROVEN_OK && taken == sizeof said - 1 && made > 4, "compressed, with a sync flush");
    EXAMPLE_REQUIRE(memcmp(packed + made - 4, tail, 4) == 0, "which ends in 00 00 ff ff");
    proven_size_t payload = made - 4;
    proven_deflate_destroy(c);

    proven_ws_frame_t frame = { .fin = true, .opcode = PROVEN_WS_TEXT, .rsv1 = true, .length = payload };
    proven_byte_t header[PROVEN_WS_MAX_FRAME_HEADER];
    proven_size_t header_len = 0;
    EXAMPLE_REQUIRE(proven_ws_frame_write((proven_mem_mut_t){ header, sizeof header }, &header_len, &frame) == PROVEN_OK && header[0] == 0xc1, "a frame header with the bit");
    printf("%u bytes of text go out as %u\n", (unsigned)(sizeof said - 1), (unsigned)(header_len + payload));
    EXAMPLE_REQUIRE(header_len + payload < sizeof said - 1, "fewer than were said");

    // ---- with context takeover ---------------------------------------------------------
    /* The other form: each side keeps its window from one message to the next, so a message
     * refers back to the ones before it. A client offers PROVEN_WS_DEFLATE_OFFER_TAKEOVER; a
     * server answers with proven_ws_deflate_accept_takeover, which forbids takeover only
     * where the offer asked, and says what was agreed. */
    proven_http_header_t asked_take[] = { { PROVEN_LIT("Sec-WebSocket-Extensions"), PROVEN_LIT(PROVEN_WS_DEFLATE_OFFER_TAKEOVER) } };
    proven_http_request_t request_take = { .headers = asked_take, .header_count = 1 };
    proven_ws_deflate_terms_t terms;
    answer_len = 0;
    EXAMPLE_REQUIRE(proven_ws_deflate_accept_takeover(&request_take, (proven_mem_mut_t){ answer, sizeof answer }, &answer_len, &terms) == PROVEN_OK &&
                    terms.server_takeover && terms.client_takeover, "takeover is agreed both ways");
    printf("the server answers: %.*s\n", (int)answer_len, (const char *)answer);
    answered[3].value = (proven_u8str_view_t){ answer, answer_len };
    proven_ws_deflate_terms_t seen;
    EXAMPLE_REQUIRE(proven_ws_check_response_takeover(&response, PROVEN_LIT("dGhlIHNhbXBsZSBub25jZQ=="), PROVEN_LIT(""), &protocol, &compressed, &seen) == PROVEN_OK &&
                    compressed && seen.server_takeover && seen.client_takeover, "and the client sees the same terms");

    /* A side that takes over keeps one compressor for the connection and ends every message
     * with a sync flush; the side that reads keeps one decompressor. The same line twice: the
     * second goes out as a reference to the first. */
    EXAMPLE_REQUIRE(proven_deflate_create(&options, &c) == PROVEN_OK && proven_inflate_create(heap, PROVEN_DEFLATE_RAW, &z) == PROVEN_OK, "one of each, kept");
    for (int round = 0; round < 2; ++round) {
        EXAMPLE_REQUIRE(proven_deflate(c, (proven_mem_view_t){ (const proven_byte_t *)said, sizeof said - 1 }, &taken, (proven_mem_mut_t){ packed, sizeof packed }, &made,
                                       PROVEN_DEFLATE_FLUSH_SYNC, &ended) == PROVEN_OK && made > 4, "a message through the kept compressor");
        printf("message %d goes out as %u bytes\n", round + 1, (unsigned)(made - 4));
        if (round == 1) EXAMPLE_REQUIRE(made - 4 < 10, "the second is a few bytes: a reference into the first");
        /* The reader: the payload as it arrived - the four bytes left off, then put back. */
        proven_byte_t back[128];
        proven_size_t got = 0;
        EXAMPLE_REQUIRE(proven_inflate(z, (proven_mem_view_t){ packed, made }, &taken, (proven_mem_mut_t){ back, sizeof back }, &got, &ended) == PROVEN_OK &&
                        got == sizeof said - 1 && memcmp(back, said, got) == 0, "and the kept decompressor reads it");
        /* A sender may end a message with a final block instead (RFC 7692, 7.2.3.4): the
         * decompressor then reports `ended`, and the reader goes on with
         * proven_inflate_restart, which keeps the window - not reset, which forgets it. */
        if (ended) proven_inflate_restart(z);
    }
    proven_deflate_destroy(c);
    proven_inflate_destroy(z);
    return EXAMPLE_OK();
}
