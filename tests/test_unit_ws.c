#include "proven.h"
#include "proven_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The WebSocket codec against RFC 6455: its handshake example (section 1.3), its frame examples
 * (section 5.7), and the rules it gives a receiver.
 *
 * The refusal tables follow the categories of the Autobahn test suite - framing, reserved bits,
 * opcodes, fragmentation, UTF-8, close handling, limits - because that suite could not be run
 * here and its categories are the public catalogue of what implementations get wrong.
 *
 * As with the other decoders, the property that matters most is that the result does not
 * depend on how the stream was cut: each stream is decoded whole, split in two at every
 * position, and a byte at a time, and the transcripts must be identical.
 */

static proven_u8str_view_t sv(const char *s) {
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}

/* Decode `stream` in pieces and write what happened into `out` as text. */
static proven_err_t transcript(const proven_byte_t *stream, proven_size_t len, bool from_client, proven_u64 max,
                               proven_size_t first, proven_size_t step, char *out, proven_size_t cap) {
    proven_ws_decoder_t d;
    proven_ws_decoder_init(&d, from_client, max);
    proven_size_t pos = 0, n = 0;
    proven_size_t piece = first ? first : (step ? step : (len ? len : 1));
    out[0] = '\0';
    while (pos < len) {
        proven_size_t avail = len - pos < piece ? len - pos : piece;
        /* An exact-size copy, so a read past the piece is a sanitizer report. */
        proven_byte_t *copy = malloc(avail ? avail : 1);
        memcpy(copy, stream + pos, avail);
        proven_size_t off = 0;
        while (off < avail) {
            proven_size_t used = 0;
            proven_ws_event_t ev;
            proven_err_t e = proven_ws_decoder_feed(&d, (proven_mem_mut_t){ copy + off, avail - off }, &used, &ev);
            if (e != PROVEN_OK) { free(copy); return e; }
            off += used;
            if (ev.kind == PROVEN_WS_EVENT_NONE) continue;
            if (ev.kind == PROVEN_WS_EVENT_DATA) {
                if (ev.first) n += (proven_size_t)snprintf(out + n, cap - n, "%s<", ev.text ? "T" : "B");
                for (proven_size_t i = 0; i < ev.data.size && n + 4 < cap; ++i) n += (proven_size_t)snprintf(out + n, cap - n, "%02x", ev.data.ptr[i]);
                if (ev.last) n += (proven_size_t)snprintf(out + n, cap - n, ">");
            } else if (ev.kind == PROVEN_WS_EVENT_CLOSE) {
                n += (proven_size_t)snprintf(out + n, cap - n, "[close %u %.*s]", (unsigned)ev.close_code, (int)ev.data.size, (const char *)ev.data.ptr);
            } else {
                n += (proven_size_t)snprintf(out + n, cap - n, "[%s %.*s]", ev.kind == PROVEN_WS_EVENT_PING ? "ping" : "pong", (int)ev.data.size, (const char *)ev.data.ptr);
            }
        }
        free(copy);
        pos += avail;
        piece = step ? step : len;
    }
    return PROVEN_OK;
}

/* Decode every way; *same says whether all ways agreed. Returns the whole-stream error. */
static proven_err_t every_way(const proven_byte_t *stream, proven_size_t len, bool from_client, proven_u64 max, char *out, proven_size_t cap, bool *same) {
    static char other[8192];
    proven_err_t whole = transcript(stream, len, from_client, max, 0, 0, out, cap);
    *same = true;
    for (proven_size_t cut = 1; cut < len; ++cut) {
        proven_err_t e = transcript(stream, len, from_client, max, cut, 0, other, sizeof other);
        if (e != whole || (e == PROVEN_OK && strcmp(out, other) != 0)) *same = false;
    }
    proven_err_t e = transcript(stream, len, from_client, max, 0, 1, other, sizeof other);
    if (e != whole || (e == PROVEN_OK && strcmp(out, other) != 0)) *same = false;
    return whole;
}

/* Append one frame to a stream under construction. */
static proven_size_t put(proven_byte_t *buf, proven_size_t len, bool fin, proven_u8 opcode, bool masked, const void *payload, proven_size_t n) {
    static const proven_byte_t key[4] = { 0x37, 0xfa, 0x21, 0x3d };
    proven_ws_frame_t f = { .fin = fin, .opcode = opcode, .masked = masked, .length = n };
    memcpy(f.mask, key, 4);
    proven_err_t e = proven_ws_frame_write((proven_mem_mut_t){ buf, 8192 }, &len, &f);
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "a test frame header is written", "");
    memcpy(buf + len, payload, n);
    if (masked) proven_ws_mask((proven_mem_mut_t){ buf + len, n }, key, 0);
    return len + n;
}

int main(void) {
    PROVEN_TEST_SUITE("ws: the WebSocket codec",
        "The handshake values, the frame header, and the decoder's rules, against RFC 6455.",
        "Inspect src/proven/ws.c.");

    static proven_byte_t buf[8192];
    static char text[8192];
    bool same = false;

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the opening handshake",
        "The example of RFC 6455 section 1.3, and what each side must refuse.",
        "Inspect proven_ws_accept_key, proven_ws_check_request and proven_ws_check_response.");
    // ---------------------------------------------------------------
    {
        proven_byte_t accept[PROVEN_WS_ACCEPT_SIZE];
        PROVEN_TEST_ASSERT(proven_ws_accept_key(sv("dGhlIHNhbXBsZSBub25jZQ=="), accept) == PROVEN_OK &&
                           memcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", 28) == 0, "the RFC's key gives the RFC's accept value", "");
        memset(accept, 'x', sizeof accept);
        PROVEN_TEST_ASSERT(proven_ws_accept_key(sv("dGhlIHNhbXBsZSBub25jZQ="), accept) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_ws_accept_key(sv("dGhlIHNhbXBsZSBub25jZQ==="), accept) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_ws_accept_key(sv("dGhlIHNhbXBsZSBub25j!Q=="), accept) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_ws_accept_key(sv("dGhlIHNhbXBsZSBub25jZSE="), accept) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_ws_accept_key(sv(""), accept) == PROVEN_ERR_INVALID_FORMAT && accept[0] == 'x',
            "a key that is not 24 characters decoding to 16 bytes is refused, with nothing written", "");

        proven_byte_t random[16], key[PROVEN_WS_KEY_SIZE];
        for (int i = 0; i < 16; ++i) random[i] = (proven_byte_t)(i + 1);
        proven_ws_make_key(random, key);
        PROVEN_TEST_ASSERT(memcmp(key, "AQIDBAUGBwgJCgsMDQ4PEA==", 24) == 0 && proven_ws_accept_key((proven_u8str_view_t){ key, 24 }, accept) == PROVEN_OK,
            "a made key is the Base64 of its sixteen bytes, and is a key", "");

        static const char good[] = "GET /chat HTTP/1.1\r\nHost: server.example.com\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n"
                                   "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nOrigin: http://example.com\r\n"
                                   "Sec-WebSocket-Protocol: chat, superchat\r\nSec-WebSocket-Version: 13\r\n\r\n";
        proven_http_header_t h[16];
        proven_http_request_t req;
        proven_size_t head = 0;
        proven_u8str_view_t k;
        PROVEN_TEST_ASSERT(proven_http_parse_request((proven_mem_view_t){ (const proven_byte_t *)good, sizeof good - 1 }, h, 16, 0, &req, &head) == PROVEN_OK, "the RFC's request parses", "");
        PROVEN_TEST_ASSERT(proven_ws_check_request(&req, &k) == PROVEN_OK && proven_u8str_view_eq(k, sv("dGhlIHNhbXBsZSBub25jZQ==")), "and is an upgrade, with its key", "");
        PROVEN_TEST_ASSERT(proven_ws_request_offers(&req, sv("chat")) && proven_ws_request_offers(&req, sv("superchat")) &&
                           !proven_ws_request_offers(&req, sv("Chat")) && !proven_ws_request_offers(&req, sv("cha")) && !proven_ws_request_offers(&req, sv("")),
            "the subprotocols offered are those listed, compared exactly", "");

        static const struct { const char *request; proven_err_t want; const char *why; } rows[] = {
            { "GET / HTTP/1.1\r\nHost: h\r\n\r\n", PROVEN_ERR_NOT_FOUND, "no Upgrade header: an ordinary request" },
            { "GET / HTTP/1.1\r\nHost: h\r\nUpgrade: h2c\r\nConnection: Upgrade\r\n\r\n", PROVEN_ERR_NOT_FOUND, "an upgrade to something else" },
            { "GET / HTTP/1.1\r\nHost: h\r\nUpgrade: WebSocket\r\nConnection: upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n", PROVEN_OK, "token case does not matter" },
            { "POST / HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\nContent-Length: 0\r\n\r\n", PROVEN_ERR_INVALID_FORMAT, "not GET" },
            { "GET / HTTP/1.0\r\nHost: h\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n", PROVEN_ERR_INVALID_FORMAT, "HTTP/1.0" },
            { "GET / HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\nConnection: keep-alive\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n", PROVEN_ERR_INVALID_FORMAT, "Connection does not say Upgrade" },
            { "GET / HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n\r\n", PROVEN_ERR_INVALID_FORMAT, "no key" },
            { "GET / HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n", PROVEN_ERR_INVALID_FORMAT, "two keys" },
            { "GET / HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: c2hvcnQ=\r\nSec-WebSocket-Version: 13\r\n\r\n", PROVEN_ERR_INVALID_FORMAT, "a key that is not sixteen bytes" },
            { "GET / HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n", PROVEN_ERR_INVALID_FORMAT, "no version" },
            { "GET / HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 8\r\n\r\n", PROVEN_ERR_UNSUPPORTED, "version 8: answer 426" },
            { "GET / HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13, 8\r\n\r\n", PROVEN_ERR_UNSUPPORTED, "a list of versions is not 13" },
        };
        for (proven_size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
            PROVEN_TEST_ASSERT(proven_http_parse_request((proven_mem_view_t){ (const proven_byte_t *)rows[i].request, strlen(rows[i].request) }, h, 16, 0, &req, &head) == PROVEN_OK, "the row parses as HTTP", "");
            proven_err_t e = proven_ws_check_request(&req, &k);
            if (e != rows[i].want) PROVEN_TEST_INFO("row: {} -> {}", PROVEN_ARG(rows[i].why), PROVEN_ARG((int)e));
            PROVEN_TEST_ASSERT(e == rows[i].want, "a request is classified as its row says", "");
        }

        static const struct { const char *response; const char *offered; proven_err_t want; const char *protocol; const char *why; } answers[] = {
            { "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\nSec-WebSocket-Protocol: chat\r\n\r\n", "chat, superchat", PROVEN_OK, "chat", "the RFC's response" },
            { "HTTP/1.1 101 Switching Protocols\r\nUpgrade: WebSocket\r\nConnection: upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n", "chat", PROVEN_OK, "", "no subprotocol chosen" },
            { "HTTP/1.1 200 OK\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\nContent-Length: 0\r\n\r\n", "", PROVEN_ERR_INVALID_FORMAT, "", "not 101" },
            { "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n", "", PROVEN_ERR_INVALID_FORMAT, "", "no Upgrade" },
            { "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n", "", PROVEN_ERR_INVALID_FORMAT, "", "no Connection" },
            { "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n", "", PROVEN_ERR_INVALID_FORMAT, "", "no accept value" },
            { "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOO=\r\n\r\n", "", PROVEN_ERR_INVALID_FORMAT, "", "the wrong accept value" },
            { "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\nSec-WebSocket-Protocol: other\r\n\r\n", "chat", PROVEN_ERR_INVALID_FORMAT, "", "a subprotocol that was not offered" },
            { "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\nSec-WebSocket-Protocol: chat\r\n\r\n", "", PROVEN_ERR_INVALID_FORMAT, "", "a subprotocol when none was offered" },
            { "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\nSec-WebSocket-Extensions: permessage-deflate\r\n\r\n", "", PROVEN_ERR_INVALID_FORMAT, "", "an extension nobody asked for" },
        };
        proven_http_response_t res;
        for (proven_size_t i = 0; i < sizeof answers / sizeof answers[0]; ++i) {
            PROVEN_TEST_ASSERT(proven_http_parse_response((proven_mem_view_t){ (const proven_byte_t *)answers[i].response, strlen(answers[i].response) }, h, 16, 0, &res, &head) == PROVEN_OK, "the row parses as HTTP", "");
            proven_u8str_view_t chosen = sv("untouched");
            proven_err_t e = proven_ws_check_response(&res, sv("dGhlIHNhbXBsZSBub25jZQ=="), sv(answers[i].offered), &chosen);
            if (e != answers[i].want) PROVEN_TEST_INFO("row: {} -> {}", PROVEN_ARG(answers[i].why), PROVEN_ARG((int)e));
            PROVEN_TEST_ASSERT(e == answers[i].want && proven_u8str_view_eq(chosen, sv(answers[i].protocol)), "a response is judged as its row says", "");
        }
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("frame headers",
        "The examples of RFC 6455 section 5.7 parse to what the RFC says; every prefix is NEED_MORE; what is written parses back; malformed headers are refused.",
        "Inspect proven_ws_frame_parse and proven_ws_frame_write.");
    // ---------------------------------------------------------------
    {
        static const struct { const char *hex; bool fin; proven_u8 opcode; bool masked; proven_u64 length; proven_size_t header; const char *why; } ex[] = {
            { "810548656c6c6f", true, PROVEN_WS_TEXT, false, 5, 2, "a single-frame unmasked text message" },
            { "818537fa213d7f9f4d5158", true, PROVEN_WS_TEXT, true, 5, 6, "a single-frame masked text message" },
            { "010348656c", false, PROVEN_WS_TEXT, false, 3, 2, "the first fragment" },
            { "80026c6f", true, PROVEN_WS_CONTINUATION, false, 2, 2, "the last fragment" },
            { "890548656c6c6f", true, PROVEN_WS_PING, false, 5, 2, "an unmasked ping" },
            { "8a8537fa213d7f9f4d5158", true, PROVEN_WS_PONG, true, 5, 6, "a masked pong" },
            { "827e0100", true, PROVEN_WS_BINARY, false, 256, 4, "256 bytes of binary" },
            { "827f0000000000010000", true, PROVEN_WS_BINARY, false, 65536, 10, "64 KiB of binary" },
        };
        for (proven_size_t i = 0; i < sizeof ex / sizeof ex[0]; ++i) {
            proven_byte_t raw[32];
            proven_size_t n = 0;
            PROVEN_TEST_ASSERT(proven_hex_decode((proven_mem_view_t){ (const proven_byte_t *)ex[i].hex, strlen(ex[i].hex) }, raw, sizeof raw, &n) == PROVEN_OK, "the example decodes from hex", "");
            proven_ws_frame_t f;
            proven_size_t hs = 0;
            proven_err_t e = proven_ws_frame_parse((proven_mem_view_t){ raw, n }, &f, &hs);
            if (e != PROVEN_OK) PROVEN_TEST_INFO("example: {}", PROVEN_ARG(ex[i].why));
            PROVEN_TEST_ASSERT(e == PROVEN_OK && f.fin == ex[i].fin && f.opcode == ex[i].opcode && f.masked == ex[i].masked && f.length == ex[i].length && hs == ex[i].header,
                "an RFC example parses to the fields the RFC gives", "");
            for (proven_size_t cut = 0; cut < hs; ++cut) {
                proven_byte_t *exact = malloc(cut ? cut : 1);
                memcpy(exact, raw, cut);
                e = proven_ws_frame_parse((proven_mem_view_t){ exact, cut }, &f, &hs);
                free(exact);
                PROVEN_TEST_ASSERT(e == PROVEN_ERR_NEED_MORE, "every proper prefix of a header is NEED_MORE", "");
            }
            /* And the same header is what the writer writes. */
            proven_ws_frame_t again;
            PROVEN_TEST_ASSERT(proven_ws_frame_parse((proven_mem_view_t){ raw, n }, &again, &hs) == PROVEN_OK, "parse again", "");
            proven_byte_t out[PROVEN_WS_MAX_FRAME_HEADER];
            proven_size_t len = 0;
            PROVEN_TEST_ASSERT(proven_ws_frame_write((proven_mem_mut_t){ out, sizeof out }, &len, &again) == PROVEN_OK && len == hs && memcmp(out, raw, hs) == 0,
                "writing the parsed header gives the RFC's bytes back", "");
        }
        /* The masked example's payload is "Hello" under its key. */
        proven_byte_t hello[5] = { 0x7f, 0x9f, 0x4d, 0x51, 0x58 };
        static const proven_byte_t key[4] = { 0x37, 0xfa, 0x21, 0x3d };
        proven_ws_mask((proven_mem_mut_t){ hello, 5 }, key, 0);
        PROVEN_TEST_ASSERT(memcmp(hello, "Hello", 5) == 0, "unmasking the RFC's masked payload gives Hello", "");
        proven_ws_mask((proven_mem_mut_t){ hello, 2 }, key, 0);
        proven_ws_mask((proven_mem_mut_t){ hello + 2, 3 }, key, 2);
        PROVEN_TEST_ASSERT(memcmp(hello, "\x7f\x9f\x4d\x51\x58", 5) == 0, "masking in two pieces with the offset gives the same bytes as in one", "");

        static const proven_u64 lengths[] = { 0, 1, 125, 126, 127, 65535, 65536, 0xFFFFFFFFull, 0x100000000ull, 0x7FFFFFFFFFFFFFFFull };
        static const proven_size_t sizes[] = { 2, 2, 2, 4, 4, 4, 10, 10, 10, 10 };
        for (proven_size_t i = 0; i < sizeof lengths / sizeof lengths[0]; ++i) {
            for (int masked = 0; masked < 2; ++masked) {
                proven_ws_frame_t f = { .fin = true, .opcode = PROVEN_WS_BINARY, .masked = masked != 0, .mask = { 1, 2, 3, 4 }, .length = lengths[i] }, g;
                proven_byte_t out[PROVEN_WS_MAX_FRAME_HEADER];
                proven_size_t len = 0, hs = 0;
                PROVEN_TEST_ASSERT(proven_ws_frame_write((proven_mem_mut_t){ out, sizeof out }, &len, &f) == PROVEN_OK && len == sizes[i] + (masked ? 4u : 0u), "a length is written in its shortest form", "");
                PROVEN_TEST_ASSERT(proven_ws_frame_parse((proven_mem_view_t){ out, len }, &g, &hs) == PROVEN_OK && hs == len && g.length == lengths[i] && g.masked == f.masked &&
                                   (!masked || memcmp(g.mask, f.mask, 4) == 0), "and parses back to the same frame", "");
                len = 0;
                PROVEN_TEST_ASSERT(proven_ws_frame_write((proven_mem_mut_t){ out, sizes[i] + (masked ? 4u : 0u) - 1 }, &len, &f) == PROVEN_ERR_OUT_OF_BOUNDS && len == 0, "one byte short is OUT_OF_BOUNDS with nothing written", "");
            }
        }

        static const struct { const char *hex; const char *why; } bad[] = {
            { "c100", "RSV1 set" }, { "a100", "RSV2 set" }, { "9100", "RSV3 set" }, { "f100", "all reserved bits set" },
            { "8300", "opcode 3" }, { "8400", "opcode 4" }, { "8500", "opcode 5" }, { "8600", "opcode 6" }, { "8700", "opcode 7" },
            { "8b00", "opcode 11" }, { "8c00", "opcode 12" }, { "8d00", "opcode 13" }, { "8e00", "opcode 14" }, { "8f00", "opcode 15" },
            { "0900", "a fragmented ping" }, { "0a00", "a fragmented pong" }, { "0800", "a fragmented close" },
            { "897e007e", "a ping of 126 bytes" }, { "887e0080", "a close of 128 bytes" }, { "8a7f0000000000010000", "a pong with a 64-bit length" },
            { "827e007d", "125 written with the 16-bit form" }, { "827e0000", "0 written with the 16-bit form" },
            { "827f000000000000ffff", "65535 written with the 64-bit form" }, { "827f0000000000000000", "0 written with the 64-bit form" },
            { "827f8000000000000000", "a 64-bit length with its top bit set" },
        };
        for (proven_size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
            proven_byte_t raw[16];
            proven_size_t n = 0, hs = 99;
            PROVEN_TEST_ASSERT(proven_hex_decode((proven_mem_view_t){ (const proven_byte_t *)bad[i].hex, strlen(bad[i].hex) }, raw, sizeof raw, &n) == PROVEN_OK, "the row decodes from hex", "");
            proven_ws_frame_t f;
            proven_err_t e = proven_ws_frame_parse((proven_mem_view_t){ raw, n }, &f, &hs);
            if (e != PROVEN_ERR_INVALID_FORMAT) PROVEN_TEST_INFO("row: {} -> {}", PROVEN_ARG(bad[i].why), PROVEN_ARG((int)e));
            PROVEN_TEST_ASSERT(e == PROVEN_ERR_INVALID_FORMAT, "a malformed header is PROVEN_ERR_INVALID_FORMAT", "A header accepted here is a frame two implementations read differently.");
        }
        proven_byte_t out[PROVEN_WS_MAX_FRAME_HEADER];
        proven_size_t len = 0;
        proven_ws_frame_t f = { .fin = false, .opcode = PROVEN_WS_PING };
        PROVEN_TEST_ASSERT(proven_ws_frame_write((proven_mem_mut_t){ out, sizeof out }, &len, &f) == PROVEN_ERR_INVALID_ARG, "the writer refuses a fragmented ping", "");
        f = (proven_ws_frame_t){ .fin = true, .opcode = PROVEN_WS_CLOSE, .length = 126 };
        PROVEN_TEST_ASSERT(proven_ws_frame_write((proven_mem_mut_t){ out, sizeof out }, &len, &f) == PROVEN_ERR_INVALID_ARG, "a long close", "");
        f = (proven_ws_frame_t){ .fin = true, .opcode = 3 };
        PROVEN_TEST_ASSERT(proven_ws_frame_write((proven_mem_mut_t){ out, sizeof out }, &len, &f) == PROVEN_ERR_INVALID_ARG, "an opcode that does not exist", "");
        f = (proven_ws_frame_t){ .fin = true, .opcode = PROVEN_WS_BINARY, .length = 0x8000000000000000ull };
        PROVEN_TEST_ASSERT(proven_ws_frame_write((proven_mem_mut_t){ out, sizeof out }, &len, &f) == PROVEN_ERR_INVALID_ARG && len == 0, "and a length that cannot be written", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("close codes and close payloads",
        "Which codes may be sent, and the payload written and read back.",
        "Inspect proven_ws_close_code_is_valid, proven_ws_close_write and proven_ws_close_parse.");
    // ---------------------------------------------------------------
    {
        static const proven_u16 valid[] = { 1000, 1001, 1002, 1003, 1007, 1008, 1009, 1010, 1011, 1012, 1013, 1014, 3000, 3999, 4000, 4999 };
        static const proven_u16 invalid[] = { 0, 1, 999, 1004, 1005, 1006, 1015, 1016, 1100, 2000, 2999, 5000, 65535 };
        bool ok = true;
        for (proven_size_t i = 0; i < sizeof valid / sizeof valid[0]; ++i) ok = ok && proven_ws_close_code_is_valid(valid[i]);
        for (proven_size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) ok = ok && !proven_ws_close_code_is_valid(invalid[i]);
        PROVEN_TEST_ASSERT(ok, "sixteen codes that may be sent and thirteen that may not", "");

        proven_size_t len = 0;
        proven_u16 code = 0;
        proven_u8str_view_t reason;
        PROVEN_TEST_ASSERT(proven_ws_close_write((proven_mem_mut_t){ buf, 200 }, &len, 1001, sv("going away")) == PROVEN_OK && len == 12 && buf[0] == 0x03 && buf[1] == 0xe9,
            "a code and a reason are written, the code big-endian", "");
        PROVEN_TEST_ASSERT(proven_ws_close_parse((proven_mem_view_t){ buf, len }, &code, &reason) == PROVEN_OK && code == 1001 && proven_u8str_view_eq(reason, sv("going away")), "and read back", "");
        len = 0;
        PROVEN_TEST_ASSERT(proven_ws_close_write((proven_mem_mut_t){ buf, 200 }, &len, PROVEN_WS_CLOSE_NO_STATUS, sv("")) == PROVEN_OK && len == 0, "no status: an empty payload", "");
        PROVEN_TEST_ASSERT(proven_ws_close_parse((proven_mem_view_t){ buf, 0 }, &code, &reason) == PROVEN_OK && code == PROVEN_WS_CLOSE_NO_STATUS && reason.size == 0, "which reads back as no status", "");
        static char long_reason[125];
        memset(long_reason, 'r', 124);
        PROVEN_TEST_ASSERT(proven_ws_close_write((proven_mem_mut_t){ buf, 200 }, &len, PROVEN_WS_CLOSE_NO_STATUS, sv("why")) == PROVEN_ERR_INVALID_ARG &&
                           proven_ws_close_write((proven_mem_mut_t){ buf, 200 }, &len, 1005, sv("")) == PROVEN_OK &&
                           proven_ws_close_write((proven_mem_mut_t){ buf, 200 }, &len, 1006, sv("")) == PROVEN_ERR_INVALID_ARG &&
                           proven_ws_close_write((proven_mem_mut_t){ buf, 200 }, &len, 1000, sv(long_reason)) == PROVEN_ERR_INVALID_ARG &&
                           proven_ws_close_write((proven_mem_mut_t){ buf, 200 }, &len, 1000, sv("\xff")) == PROVEN_ERR_INVALID_ENCODING &&
                           proven_ws_close_write((proven_mem_mut_t){ buf, 3 }, &len, 1000, sv("ab")) == PROVEN_ERR_OUT_OF_BOUNDS && len == 0,
            "a reason without a code, a code that may not be sent, a reason of 124 bytes, one that is not UTF-8 and a buffer too small are refused", "");
        long_reason[123] = '\0';
        PROVEN_TEST_ASSERT(proven_ws_close_write((proven_mem_mut_t){ buf, 200 }, &len, 1000, sv(long_reason)) == PROVEN_OK && len == 125, "a reason of 123 bytes fills a control frame exactly", "");
        PROVEN_TEST_ASSERT(proven_ws_close_parse((proven_mem_view_t){ (const proven_byte_t *)"\x03", 1 }, &code, &reason) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_ws_close_parse((proven_mem_view_t){ (const proven_byte_t *)"\x03\xed", 2 }, &code, &reason) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_ws_close_parse((proven_mem_view_t){ (const proven_byte_t *)"\x03\xe8\xc3\x28", 4 }, &code, &reason) == PROVEN_ERR_INVALID_ENCODING,
            "a one-byte payload and code 1005 on the wire are INVALID_FORMAT; a reason that is not UTF-8 is INVALID_ENCODING", "");
        PROVEN_TEST_ASSERT(proven_ws_close_code_for(PROVEN_ERR_INVALID_FORMAT) == 1002 && proven_ws_close_code_for(PROVEN_ERR_INVALID_ENCODING) == 1007 &&
                           proven_ws_close_code_for(PROVEN_ERR_OUT_OF_BOUNDS) == 1009 && proven_ws_close_code_for(PROVEN_ERR_IO) == 1011, "each decoder error has its close code", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the decoder: messages out of a byte stream, however it is cut",
        "A conversation with fragments, control frames between them, empty frames and a close, decoded whole, split at every byte and one byte at a time.",
        "Inspect proven_ws_decoder_feed. A difference between the ways is state lost between calls.");
    // ---------------------------------------------------------------
    {
        static proven_byte_t blob[300];
        for (int i = 0; i < 300; ++i) blob[i] = (proven_byte_t)i;
        for (int from_client = 0; from_client < 2; ++from_client) {
            bool m = from_client != 0;
            proven_size_t n = 0;
            n = put(buf, n, true, PROVEN_WS_TEXT, m, "Hello", 5);
            n = put(buf, n, false, PROVEN_WS_TEXT, m, "Hel", 3);
            n = put(buf, n, true, PROVEN_WS_PING, m, "mid", 3);             /* a control frame inside a message */
            n = put(buf, n, false, PROVEN_WS_CONTINUATION, m, "", 0);       /* an empty fragment */
            n = put(buf, n, true, PROVEN_WS_CONTINUATION, m, "lo", 2);
            n = put(buf, n, true, PROVEN_WS_BINARY, m, blob, 300);
            n = put(buf, n, true, PROVEN_WS_TEXT, m, "", 0);                /* an empty message */
            n = put(buf, n, false, PROVEN_WS_BINARY, m, "", 0);
            n = put(buf, n, true, PROVEN_WS_CONTINUATION, m, "", 0);        /* an empty message in two empty frames */
            n = put(buf, n, false, PROVEN_WS_TEXT, m, "\xce\xba\xe1\xbd", 4);   /* kappa, and half of the next character */
            n = put(buf, n, true, PROVEN_WS_CONTINUATION, m, "\xb9\xf0\x9f\x98\x80", 5);
            n = put(buf, n, true, PROVEN_WS_PONG, m, "", 0);
            n = put(buf, n, true, PROVEN_WS_CLOSE, m, "\x03\xe8" "bye", 5);
            proven_err_t e = every_way(buf, n, m, 0, text, sizeof text, &same);
            PROVEN_TEST_ASSERT(e == PROVEN_OK && same, "the conversation decodes, identically every way", "");
            PROVEN_TEST_ASSERT(strncmp(text, "T<48656c6c6f>T<48656c[ping mid]6c6f>B<000102", 44) == 0, "messages, fragments and the ping between them come out in order", "");
            PROVEN_TEST_ASSERT(strstr(text, "ff0001020304") != NULL && strstr(text, "2a2b>T<>B<>T<cebae1bdb9f09f9880>[pong ][close 1000 bye]") != NULL,
                "the binary message, two empty messages, text cut inside a character, a pong and the close", "");
        }

        /* Bytes after a close frame are not frames. */
        proven_size_t n = put(buf, 0, true, PROVEN_WS_CLOSE, true, "", 0);
        n = put(buf, n, true, PROVEN_WS_TEXT, true, "late", 4);
        PROVEN_TEST_ASSERT(transcript(buf, n, true, 0, 0, 0, text, sizeof text) == PROVEN_ERR_EOF, "bytes after a close frame are PROVEN_ERR_EOF", "");
        n = put(buf, 0, true, PROVEN_WS_CLOSE, true, "", 0);
        PROVEN_TEST_ASSERT(transcript(buf, n, true, 0, 0, 0, text, sizeof text) == PROVEN_OK && strcmp(text, "[close 1005 ]") == 0, "a close with no payload reports code 1005", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the decoder: what a receiver must refuse",
        "Each row is a stream that RFC 6455 tells a receiver to fail, with the error that names its close code.",
        "Inspect the checks after the header in proven_ws_decoder_feed, and ws_text_feed. A row that decodes is a protocol violation accepted.");
    // ---------------------------------------------------------------
    {
        struct { proven_size_t n; proven_err_t want; bool from_client; proven_u64 max; const char *why; } rows[40];
        static proven_byte_t streams[40][512];
        int count = 0;
        #define ROW(err, client, limit, name) (rows[count].want = (err), rows[count].from_client = (client), rows[count].max = (limit), rows[count].why = (name), rows[count].n = 0)
        #define ADD(fin, op, masked, payload, len) (rows[count].n = put(streams[count], rows[count].n, (fin), (op), (masked), (payload), (len)))

        ROW(PROVEN_ERR_INVALID_FORMAT, true, 0, "an unmasked frame from a client"); ADD(true, PROVEN_WS_TEXT, false, "x", 1); count++;
        ROW(PROVEN_ERR_INVALID_FORMAT, false, 0, "a masked frame from a server"); ADD(true, PROVEN_WS_TEXT, true, "x", 1); count++;
        ROW(PROVEN_ERR_INVALID_FORMAT, true, 0, "an unmasked ping from a client"); ADD(true, PROVEN_WS_PING, false, "", 0); count++;
        ROW(PROVEN_ERR_INVALID_FORMAT, true, 0, "a continuation with nothing to continue"); ADD(true, PROVEN_WS_CONTINUATION, true, "x", 1); count++;
        ROW(PROVEN_ERR_INVALID_FORMAT, true, 0, "a continuation after a complete message"); ADD(true, PROVEN_WS_TEXT, true, "a", 1); ADD(true, PROVEN_WS_CONTINUATION, true, "b", 1); count++;
        ROW(PROVEN_ERR_INVALID_FORMAT, true, 0, "a new text message inside an unfinished one"); ADD(false, PROVEN_WS_TEXT, true, "a", 1); ADD(true, PROVEN_WS_TEXT, true, "b", 1); count++;
        ROW(PROVEN_ERR_INVALID_FORMAT, true, 0, "a binary message inside an unfinished text one"); ADD(false, PROVEN_WS_TEXT, true, "a", 1); ADD(true, PROVEN_WS_BINARY, true, "b", 1); count++;
        ROW(PROVEN_ERR_INVALID_FORMAT, true, 0, "a new message after a ping inside an unfinished one"); ADD(false, PROVEN_WS_BINARY, true, "a", 1); ADD(true, PROVEN_WS_PING, true, "", 0); ADD(false, PROVEN_WS_BINARY, true, "b", 1); count++;
        ROW(PROVEN_ERR_INVALID_FORMAT, true, 0, "a close with a one-byte payload"); ADD(true, PROVEN_WS_CLOSE, true, "\x03", 1); count++;
        ROW(PROVEN_ERR_INVALID_FORMAT, true, 0, "a close with code 999"); ADD(true, PROVEN_WS_CLOSE, true, "\x03\xe7", 2); count++;
        ROW(PROVEN_ERR_INVALID_FORMAT, true, 0, "a close with code 1004"); ADD(true, PROVEN_WS_CLOSE, true, "\x03\xec", 2); count++;
        ROW(PROVEN_ERR_INVALID_FORMAT, true, 0, "a close with code 1005"); ADD(true, PROVEN_WS_CLOSE, true, "\x03\xed", 2); count++;
        ROW(PROVEN_ERR_INVALID_FORMAT, true, 0, "a close with code 1006"); ADD(true, PROVEN_WS_CLOSE, true, "\x03\xee", 2); count++;
        ROW(PROVEN_ERR_INVALID_FORMAT, true, 0, "a close with code 1015"); ADD(true, PROVEN_WS_CLOSE, true, "\x03\xf7", 2); count++;
        ROW(PROVEN_ERR_INVALID_FORMAT, true, 0, "a close with code 2999"); ADD(true, PROVEN_WS_CLOSE, true, "\x0b\xb7", 2); count++;
        ROW(PROVEN_ERR_INVALID_FORMAT, true, 0, "a close with code 5000"); ADD(true, PROVEN_WS_CLOSE, true, "\x13\x88", 2); count++;
        ROW(PROVEN_ERR_INVALID_ENCODING, true, 0, "a close whose reason is not UTF-8"); ADD(true, PROVEN_WS_CLOSE, true, "\x03\xe8\xce\xba\xe1\xbd\xb9\xcf\x83\xce\xbc\xce\xb5\xed\xa0\x80\x65", 16); count++;
        ROW(PROVEN_ERR_INVALID_ENCODING, true, 0, "text with a lone continuation byte"); ADD(true, PROVEN_WS_TEXT, true, "a\x80z", 3); count++;
        ROW(PROVEN_ERR_INVALID_ENCODING, true, 0, "text with an overlong slash"); ADD(true, PROVEN_WS_TEXT, true, "\xc0\xaf", 2); count++;
        ROW(PROVEN_ERR_INVALID_ENCODING, true, 0, "text with a UTF-16 surrogate"); ADD(true, PROVEN_WS_TEXT, true, "\xed\xa0\x80", 3); count++;
        ROW(PROVEN_ERR_INVALID_ENCODING, true, 0, "text past U+10FFFF"); ADD(true, PROVEN_WS_TEXT, true, "\xf4\x90\x80\x80", 4); count++;
        ROW(PROVEN_ERR_INVALID_ENCODING, true, 0, "text that ends inside a character"); ADD(true, PROVEN_WS_TEXT, true, "ok\xe2\x82", 4); count++;
        ROW(PROVEN_ERR_INVALID_ENCODING, true, 0, "a character begun in one fragment and broken in the next"); ADD(false, PROVEN_WS_TEXT, true, "\xc3", 1); ADD(true, PROVEN_WS_CONTINUATION, true, "\x28", 1); count++;
        ROW(PROVEN_ERR_INVALID_ENCODING, true, 0, "a fragmented text that ends inside a character"); ADD(false, PROVEN_WS_TEXT, true, "\xf0\x9f", 2); ADD(true, PROVEN_WS_CONTINUATION, true, "\x98", 1); count++;
        ROW(PROVEN_ERR_INVALID_ENCODING, false, 0, "the same from a server"); ADD(true, PROVEN_WS_TEXT, false, "\xff", 1); count++;
        ROW(PROVEN_ERR_OUT_OF_BOUNDS, true, 10, "a message one byte past the limit"); ADD(true, PROVEN_WS_BINARY, true, "01234567890", 11); count++;
        ROW(PROVEN_ERR_OUT_OF_BOUNDS, true, 10, "fragments that pass the limit together"); ADD(false, PROVEN_WS_BINARY, true, "012345", 6); ADD(true, PROVEN_WS_CONTINUATION, true, "67890", 5); count++;
        #undef ROW
        #undef ADD
        for (int i = 0; i < count; ++i) {
            proven_err_t e = every_way(streams[i], rows[i].n, rows[i].from_client, rows[i].max, text, sizeof text, &same);
            if (e != rows[i].want || !same) PROVEN_TEST_INFO("row: {} -> {}", PROVEN_ARG(rows[i].why), PROVEN_ARG((int)e));
            PROVEN_TEST_ASSERT(e == rows[i].want && same, "a stream that must be failed is, with its error, however it is cut", "");
        }
        PROVEN_TEST_INFO("{} streams refused", PROVEN_ARG(count));

        /* Binary is not text: the same bytes pass as binary. And the limit is exact. */
        proven_size_t n = put(buf, 0, true, PROVEN_WS_BINARY, true, "\xc0\xaf\xff", 3);
        PROVEN_TEST_ASSERT(every_way(buf, n, true, 0, text, sizeof text, &same) == PROVEN_OK && same, "bytes that are not UTF-8 are a fine binary message", "");
        n = put(buf, 0, false, PROVEN_WS_BINARY, true, "01234", 5);
        n = put(buf, n, true, PROVEN_WS_CONTINUATION, true, "56789", 5);
        n = put(buf, n, true, PROVEN_WS_PING, true, "control frames are not counted", 30);
        n = put(buf, n, true, PROVEN_WS_BINARY, true, "0123456789", 10);
        PROVEN_TEST_ASSERT(every_way(buf, n, true, 10, text, sizeof text, &same) == PROVEN_OK && same, "messages of exactly the limit pass, each counted from zero", "");

        /* An error is final. */
        proven_ws_decoder_t d;
        proven_ws_event_t ev;
        proven_size_t used = 0;
        proven_ws_decoder_init(&d, true, 0);
        proven_byte_t bad[2] = { 0xc1, 0x80 };
        proven_byte_t good[6] = { 0x81, 0x80, 1, 2, 3, 4 };
        PROVEN_TEST_ASSERT(proven_ws_decoder_feed(&d, (proven_mem_mut_t){ bad, 2 }, &used, &ev) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_ws_decoder_feed(&d, (proven_mem_mut_t){ good, 6 }, &used, &ev) == PROVEN_ERR_INVALID_FORMAT && used == 0,
            "after an error the decoder repeats it and consumes nothing", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("generated conversations",
        "Random messages, fragmented at random with control frames between the fragments, decoded from random pieces: the payload that comes out is the payload that went in.",
        "A failure prints the round. Inspect the offset arithmetic of proven_ws_decoder_feed and proven_ws_mask.");
    // ---------------------------------------------------------------
    {
        proven_xoshiro256ss_t g;
        proven_xoshiro256ss_seed(&g, 6455);
        proven_rng_t rng = proven_xoshiro256ss_rng(&g);
        static proven_byte_t wire[70000], sent[40000], got[40000], piece[512];
        for (int round = 0; round < 3000; ++round) {
            bool masked = proven_rng_below(rng, 2) == 0;
            proven_size_t wn = 0, sn = 0, gn = 0, pings = 0, seen_pings = 0, messages = 0, seen_messages = 0;
            int nmsg = 1 + (int)proven_rng_below(rng, 4);
            for (int m = 0; m < nmsg; ++m) {
                proven_size_t total = (proven_size_t)proven_rng_below(rng, round % 50 == 0 ? 9000 : 300);
                proven_size_t done = 0;
                bool first = true;
                messages++;
                for (;;) {
                    proven_size_t n = (proven_size_t)proven_rng_below(rng, (proven_u64)(total - done) + 1);
                    bool fin = done + n == total && proven_rng_below(rng, 3) != 0;
                    proven_ws_frame_t f = { .fin = fin, .opcode = (proven_u8)(first ? PROVEN_WS_BINARY : PROVEN_WS_CONTINUATION), .masked = masked, .length = n };
                    for (int k = 0; k < 4; ++k) f.mask[k] = (proven_byte_t)proven_rng_below(rng, 256);
                    PROVEN_TEST_ASSERT(proven_ws_frame_write((proven_mem_mut_t){ wire, sizeof wire }, &wn, &f) == PROVEN_OK, "a generated frame is written", "");
                    for (proven_size_t k = 0; k < n; ++k) { sent[sn + k] = (proven_byte_t)proven_rng_below(rng, 256); wire[wn + k] = sent[sn + k]; }
                    if (masked) proven_ws_mask((proven_mem_mut_t){ wire + wn, n }, f.mask, 0);
                    wn += n; sn += n; done += n; first = false;
                    if (fin) break;
                    if (proven_rng_below(rng, 4) == 0) {
                        proven_ws_frame_t p = { .fin = true, .opcode = PROVEN_WS_PING, .masked = masked, .length = 0 };
                        PROVEN_TEST_ASSERT(proven_ws_frame_write((proven_mem_mut_t){ wire, sizeof wire }, &wn, &p) == PROVEN_OK, "a ping between fragments", "");
                        pings++;
                    }
                }
            }
            proven_ws_decoder_t d;
            proven_ws_decoder_init(&d, masked, 0);
            proven_size_t pos = 0;
            bool ok = true;
            while (pos < wn && ok) {
                proven_size_t n = 1 + (proven_size_t)proven_rng_below(rng, sizeof piece);
                if (n > wn - pos) n = wn - pos;
                memcpy(piece, wire + pos, n);
                proven_size_t off = 0;
                while (off < n && ok) {
                    proven_size_t used = 0;
                    proven_ws_event_t ev;
                    ok = proven_ws_decoder_feed(&d, (proven_mem_mut_t){ piece + off, n - off }, &used, &ev) == PROVEN_OK;
                    off += used;
                    if (ev.kind == PROVEN_WS_EVENT_DATA) {
                        memcpy(got + gn, ev.data.ptr, ev.data.size);
                        gn += ev.data.size;
                        if (ev.last) seen_messages++;
                    } else if (ev.kind == PROVEN_WS_EVENT_PING) {
                        seen_pings++;
                    } else if (ev.kind == PROVEN_WS_EVENT_NONE && used == 0) {
                        ok = false;         /* nothing reported and nothing consumed: a stall */
                    }
                }
                pos += n;
            }
            if (!ok || gn != sn || memcmp(got, sent, sn) != 0 || seen_pings != pings || seen_messages != messages) PROVEN_TEST_INFO("round {}", PROVEN_ARG(round));
            PROVEN_TEST_ASSERT(ok && gn == sn && memcmp(got, sent, sn) == 0 && seen_pings == pings && seen_messages == messages,
                "what was framed is what is decoded: every byte, every message end, every ping", "");
        }
    }

    PROVEN_TEST_PASS("the codec agrees with RFC 6455.");
    return 0;
}
