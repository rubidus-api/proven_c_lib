#include "proven.h"
#include "proven_test.h"
#include <stdio.h>
#include <string.h>

/*
 * HTTP/2 without a connection: frames, and the rules a header list must pass.
 *
 * Nothing here needs a peer. Frame headers are read and written; each frame type's rule for
 * size and stream is tried on both sides of its edge; padding and settings are taken apart;
 * and header lists are run through the checks that stand between HTTP/2 and a request
 * smuggled into whatever the message is translated to. That real frames from another
 * implementation are read as that implementation meant is the engine's evidence, and a
 * private program's.
 */

static proven_u8str_view_t sv(const char *s) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }

static proven_byte_t g_out[70000];

static proven_err_t check(proven_u8 type, proven_u8 flags, proven_u32 stream, proven_u32 length, proven_h2_error_t *code, bool *on_stream) {
    proven_h2_frame_t f = { .length = length, .type = type, .flags = flags, .stream_id = stream };
    return proven_h2_frame_check(&f, PROVEN_H2_MIN_MAX_FRAME_SIZE, code, on_stream);
}

int main(void) {
    PROVEN_TEST_SUITE("h2: HTTP/2 frames and header lists",
        "Frame headers read and written, each frame type's rules on both sides of their edges, padding, settings and their ranges, the frames this side writes read back, and the checks a header list must pass to be a request, a response or trailers.",
        "Inspect src/proven/h2.c: proven_h2_frame_check for the per-type rules, proven_h2_frame_content for padding, proven_h2_settings_next for the ranges, h2_field_ok and h2_ordinary_ok for the header rules.");

    PROVEN_TEST_SECTION("frame headers", "Nine bytes, both ways.", "Check proven_h2_frame_parse and proven_h2_frame_write.");
    {
        PROVEN_TEST_ASSERT(sizeof PROVEN_H2_PREFACE - 1 == PROVEN_H2_PREFACE_SIZE && memcmp(PROVEN_H2_PREFACE, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", 24) == 0, "the client preface is the 24 bytes of RFC 9113 section 3.4", "");
        /* A SETTINGS acknowledgement, and a HEADERS frame of 0x012345 bytes on stream 0x7fffffff with flags 0x25. */
        static const proven_byte_t ack[] = { 0, 0, 0, 4, 1, 0, 0, 0, 0 };
        static const proven_byte_t big[] = { 0x01, 0x23, 0x45, 1, 0x25, 0xff, 0xff, 0xff, 0xff };
        proven_h2_frame_t f;
        PROVEN_TEST_ASSERT(proven_h2_frame_parse((proven_mem_view_t){ ack, 9 }, &f) == PROVEN_OK && f.length == 0 && f.type == PROVEN_H2_SETTINGS && f.flags == PROVEN_H2_FLAG_ACK && f.stream_id == 0, "a SETTINGS acknowledgement parses", "");
        PROVEN_TEST_ASSERT(proven_h2_frame_parse((proven_mem_view_t){ big, 9 }, &f) == PROVEN_OK && f.length == 0x012345 && f.type == PROVEN_H2_HEADERS && f.flags == 0x25 && f.stream_id == 0x7fffffff,
            "length, type, flags and stream are where the RFC puts them, and the reserved bit is dropped", "");
        for (proven_size_t n = 0; n < 9; ++n) {
            PROVEN_TEST_ASSERT(proven_h2_frame_parse((proven_mem_view_t){ big, n }, &f) == PROVEN_ERR_NEED_MORE, "fewer than nine bytes is PROVEN_ERR_NEED_MORE", "");
        }
        proven_size_t len = 0;
        proven_h2_frame_t w = { .length = 0x012345, .type = PROVEN_H2_HEADERS, .flags = 0x25, .stream_id = 0x7fffffff };
        PROVEN_TEST_ASSERT(proven_h2_frame_write((proven_mem_mut_t){ g_out, 9 }, &len, &w) == PROVEN_OK && len == 9 && memcmp(g_out, big, 5) == 0 && g_out[5] == 0x7f && g_out[8] == 0xff,
            "and written back the same, with the reserved bit clear", "");
        len = 0;
        PROVEN_TEST_ASSERT(proven_h2_frame_write((proven_mem_mut_t){ g_out, 8 }, &len, &w) == PROVEN_ERR_OUT_OF_BOUNDS && len == 0, "eight bytes of room is PROVEN_ERR_OUT_OF_BOUNDS with nothing written", "");
        w.length = 0x1000000;
        PROVEN_TEST_ASSERT(proven_h2_frame_write((proven_mem_mut_t){ g_out, 9 }, &len, &w) == PROVEN_ERR_INVALID_ARG, "a length of 2^24 cannot be written", "");
        w.length = 0; w.stream_id = 0x80000000u;
        PROVEN_TEST_ASSERT(proven_h2_frame_write((proven_mem_mut_t){ g_out, 9 }, &len, &w) == PROVEN_ERR_INVALID_ARG, "nor a stream identifier of 2^31", "");
    }

    PROVEN_TEST_SECTION("the rule of each frame type", "Size and stream, on both sides of each edge, with the code to send and whether the stream or the connection ends.", "Check proven_h2_frame_check against RFC 9113 section 6.");
    {
        static const struct { const char *what; proven_u8 type, flags; proven_u32 stream, length; proven_h2_error_t code; bool on_stream; } rows[] = {
            { "DATA on a stream", PROVEN_H2_DATA, 0, 1, 100, PROVEN_H2_NO_ERROR, false },
            { "DATA of no bytes", PROVEN_H2_DATA, 1, 1, 0, PROVEN_H2_NO_ERROR, false },
            { "DATA on stream 0", PROVEN_H2_DATA, 0, 0, 100, PROVEN_H2_PROTOCOL_ERROR, false },
            { "DATA of exactly the largest size", PROVEN_H2_DATA, 0, 1, 16384, PROVEN_H2_NO_ERROR, false },
            { "DATA one byte over it", PROVEN_H2_DATA, 0, 1, 16385, PROVEN_H2_FRAME_SIZE_ERROR, false },
            { "HEADERS on a stream", PROVEN_H2_HEADERS, 4, 3, 20, PROVEN_H2_NO_ERROR, false },
            { "HEADERS on stream 0", PROVEN_H2_HEADERS, 4, 0, 20, PROVEN_H2_PROTOCOL_ERROR, false },
            { "CONTINUATION on stream 0", PROVEN_H2_CONTINUATION, 4, 0, 20, PROVEN_H2_PROTOCOL_ERROR, false },
            { "CONTINUATION on a stream", PROVEN_H2_CONTINUATION, 4, 5, 0, PROVEN_H2_NO_ERROR, false },
            { "PRIORITY of five bytes", PROVEN_H2_PRIORITY, 0, 1, 5, PROVEN_H2_NO_ERROR, false },
            { "PRIORITY of four bytes", PROVEN_H2_PRIORITY, 0, 1, 4, PROVEN_H2_FRAME_SIZE_ERROR, true },
            { "PRIORITY of six bytes", PROVEN_H2_PRIORITY, 0, 1, 6, PROVEN_H2_FRAME_SIZE_ERROR, true },
            { "PRIORITY on stream 0", PROVEN_H2_PRIORITY, 0, 0, 5, PROVEN_H2_PROTOCOL_ERROR, false },
            { "RST_STREAM of four bytes", PROVEN_H2_RST_STREAM, 0, 1, 4, PROVEN_H2_NO_ERROR, false },
            { "RST_STREAM of five bytes", PROVEN_H2_RST_STREAM, 0, 1, 5, PROVEN_H2_FRAME_SIZE_ERROR, false },
            { "RST_STREAM on stream 0", PROVEN_H2_RST_STREAM, 0, 0, 4, PROVEN_H2_PROTOCOL_ERROR, false },
            { "SETTINGS of three settings", PROVEN_H2_SETTINGS, 0, 0, 18, PROVEN_H2_NO_ERROR, false },
            { "SETTINGS of no settings", PROVEN_H2_SETTINGS, 0, 0, 0, PROVEN_H2_NO_ERROR, false },
            { "SETTINGS of seven bytes", PROVEN_H2_SETTINGS, 0, 0, 7, PROVEN_H2_FRAME_SIZE_ERROR, false },
            { "SETTINGS on a stream", PROVEN_H2_SETTINGS, 0, 1, 6, PROVEN_H2_PROTOCOL_ERROR, false },
            { "a SETTINGS acknowledgement", PROVEN_H2_SETTINGS, 1, 0, 0, PROVEN_H2_NO_ERROR, false },
            { "a SETTINGS acknowledgement with a payload", PROVEN_H2_SETTINGS, 1, 0, 6, PROVEN_H2_FRAME_SIZE_ERROR, false },
            { "PING of eight bytes", PROVEN_H2_PING, 0, 0, 8, PROVEN_H2_NO_ERROR, false },
            { "PING of seven bytes", PROVEN_H2_PING, 0, 0, 7, PROVEN_H2_FRAME_SIZE_ERROR, false },
            { "PING on a stream", PROVEN_H2_PING, 0, 1, 8, PROVEN_H2_PROTOCOL_ERROR, false },
            { "GOAWAY of eight bytes", PROVEN_H2_GOAWAY, 0, 0, 8, PROVEN_H2_NO_ERROR, false },
            { "GOAWAY with debug data", PROVEN_H2_GOAWAY, 0, 0, 40, PROVEN_H2_NO_ERROR, false },
            { "GOAWAY of seven bytes", PROVEN_H2_GOAWAY, 0, 0, 7, PROVEN_H2_FRAME_SIZE_ERROR, false },
            { "GOAWAY on a stream", PROVEN_H2_GOAWAY, 0, 1, 8, PROVEN_H2_PROTOCOL_ERROR, false },
            { "WINDOW_UPDATE on the connection", PROVEN_H2_WINDOW_UPDATE, 0, 0, 4, PROVEN_H2_NO_ERROR, false },
            { "WINDOW_UPDATE on a stream", PROVEN_H2_WINDOW_UPDATE, 0, 9, 4, PROVEN_H2_NO_ERROR, false },
            { "WINDOW_UPDATE of three bytes", PROVEN_H2_WINDOW_UPDATE, 0, 9, 3, PROVEN_H2_FRAME_SIZE_ERROR, false },
            { "PUSH_PROMISE on stream 0", PROVEN_H2_PUSH_PROMISE, 4, 0, 10, PROVEN_H2_PROTOCOL_ERROR, false },
            { "a frame type nobody defined", 0x42, 0xff, 7, 300, PROVEN_H2_NO_ERROR, false },
            { "an unknown type over the size limit", 0x42, 0, 0, 16385, PROVEN_H2_FRAME_SIZE_ERROR, false },
        };
        for (proven_size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
            proven_h2_error_t code = PROVEN_H2_CANCEL;
            bool on_stream = true;
            proven_err_t e = check(rows[i].type, rows[i].flags, rows[i].stream, rows[i].length, &code, &on_stream);
            PROVEN_TEST_ASSERT((e == PROVEN_OK) == (rows[i].code == PROVEN_H2_NO_ERROR) && (e == PROVEN_OK || e == PROVEN_ERR_INVALID_FORMAT), "a frame is taken or refused as RFC 9113 section 6 says", rows[i].what);
            PROVEN_TEST_ASSERT(code == rows[i].code && on_stream == rows[i].on_stream, "with the code to send, and whether it ends the stream or the connection", rows[i].what);
        }
        proven_h2_error_t code;
        bool on_stream;
        proven_h2_frame_t large = { .length = 100000, .type = PROVEN_H2_DATA, .stream_id = 1 };
        PROVEN_TEST_ASSERT(proven_h2_frame_check(&large, 1 << 20, &code, &on_stream) == PROVEN_OK && proven_h2_frame_check(&large, 99999, &code, &on_stream) == PROVEN_ERR_INVALID_FORMAT,
            "the size limit is the one given, not a constant", "");
    }

    PROVEN_TEST_SECTION("padding and priority", "Where the content of a DATA or HEADERS payload is.", "Check proven_h2_frame_content.");
    {
        proven_byte_t payload[32];
        for (int i = 0; i < 32; ++i) payload[i] = (proven_byte_t)(0x40 + i);
        proven_mem_view_t content;
        proven_h2_frame_t f = { .length = 10, .type = PROVEN_H2_DATA, .stream_id = 1 };
        PROVEN_TEST_ASSERT(proven_h2_frame_content(&f, (proven_mem_view_t){ payload, 10 }, &content) == PROVEN_OK && content.ptr == payload && content.size == 10, "without flags the payload is the content", "");
        f.flags = PROVEN_H2_FLAG_PADDED;
        payload[0] = 4;
        PROVEN_TEST_ASSERT(proven_h2_frame_content(&f, (proven_mem_view_t){ payload, 10 }, &content) == PROVEN_OK && content.ptr == payload + 1 && content.size == 5, "padded: a length byte in front, that many bytes off the end", "");
        payload[0] = 9;
        PROVEN_TEST_ASSERT(proven_h2_frame_content(&f, (proven_mem_view_t){ payload, 10 }, &content) == PROVEN_OK && content.size == 0, "padding may be all there is", "");
        payload[0] = 10;
        PROVEN_TEST_ASSERT(proven_h2_frame_content(&f, (proven_mem_view_t){ payload, 10 }, &content) == PROVEN_ERR_INVALID_FORMAT, "padding longer than what follows its length byte is PROVEN_ERR_INVALID_FORMAT", "");
        f.length = 0;
        PROVEN_TEST_ASSERT(proven_h2_frame_content(&f, (proven_mem_view_t){ payload, 0 }, &content) == PROVEN_ERR_INVALID_FORMAT, "and so is a padded frame with no room for the length byte", "");
        f = (proven_h2_frame_t){ .length = 20, .type = PROVEN_H2_HEADERS, .stream_id = 1, .flags = PROVEN_H2_FLAG_PRIORITY | PROVEN_H2_FLAG_PADDED };
        payload[0] = 3;
        PROVEN_TEST_ASSERT(proven_h2_frame_content(&f, (proven_mem_view_t){ payload, 20 }, &content) == PROVEN_OK && content.ptr == payload + 6 && content.size == 11, "HEADERS: the five bytes of priority are skipped as well", "");
        f.length = 8;
        payload[0] = 3;
        PROVEN_TEST_ASSERT(proven_h2_frame_content(&f, (proven_mem_view_t){ payload, 8 }, &content) == PROVEN_ERR_INVALID_FORMAT, "and a payload too short to hold them is refused", "");
        f = (proven_h2_frame_t){ .length = 4, .type = PROVEN_H2_PING };
        PROVEN_TEST_ASSERT(proven_h2_frame_content(&f, (proven_mem_view_t){ payload, 4 }, &content) == PROVEN_ERR_INVALID_ARG, "another frame type has no such content: PROVEN_ERR_INVALID_ARG", "");
    }

    PROVEN_TEST_SECTION("settings", "A list read, each known setting's range held, the unknown passed through.", "Check proven_h2_settings_next and proven_h2_write_settings.");
    {
        proven_h2_setting_t list[] = {
            { PROVEN_H2_SETTINGS_HEADER_TABLE_SIZE, 8192 }, { PROVEN_H2_SETTINGS_ENABLE_PUSH, 0 }, { PROVEN_H2_SETTINGS_MAX_CONCURRENT_STREAMS, 100 },
            { PROVEN_H2_SETTINGS_INITIAL_WINDOW_SIZE, 0x7fffffff }, { PROVEN_H2_SETTINGS_MAX_FRAME_SIZE, 16384 }, { PROVEN_H2_SETTINGS_MAX_HEADER_LIST_SIZE, 0xffffffffu }, { 0x99, 7 },
        };
        proven_size_t len = 0;
        PROVEN_TEST_ASSERT(proven_h2_write_settings((proven_mem_mut_t){ g_out, sizeof g_out }, &len, list, 7) == PROVEN_OK && len == 9 + 42, "seven settings are written as one frame of 42 bytes", "");
        proven_h2_frame_t f;
        proven_h2_error_t code;
        bool on_stream;
        PROVEN_TEST_ASSERT(proven_h2_frame_parse((proven_mem_view_t){ g_out, len }, &f) == PROVEN_OK && f.type == PROVEN_H2_SETTINGS && f.length == 42 && f.stream_id == 0 && f.flags == 0 &&
                           proven_h2_frame_check(&f, 16384, &code, &on_stream) == PROVEN_OK, "which passes its own check", "");
        proven_size_t pos = 0;
        for (int i = 0; i < 7; ++i) {
            proven_h2_setting_t s;
            PROVEN_TEST_ASSERT(proven_h2_settings_next((proven_mem_view_t){ g_out + 9, 42 }, &pos, &s, &code) == PROVEN_OK && s.id == list[i].id && s.value == list[i].value, "and is read back setting for setting, the unknown one included", "");
        }
        proven_h2_setting_t s;
        PROVEN_TEST_ASSERT(proven_h2_settings_next((proven_mem_view_t){ g_out + 9, 42 }, &pos, &s, &code) == PROVEN_ERR_EOF, "then PROVEN_ERR_EOF", "");
        static const struct { const char *what; proven_u16 id; proven_u32 value; proven_h2_error_t code; } bad[] = {
            { "ENABLE_PUSH of 2", PROVEN_H2_SETTINGS_ENABLE_PUSH, 2, PROVEN_H2_PROTOCOL_ERROR },
            { "ENABLE_CONNECT_PROTOCOL of 2", PROVEN_H2_SETTINGS_ENABLE_CONNECT_PROTOCOL, 2, PROVEN_H2_PROTOCOL_ERROR },
            { "an initial window of 2^31", PROVEN_H2_SETTINGS_INITIAL_WINDOW_SIZE, 0x80000000u, PROVEN_H2_FLOW_CONTROL_ERROR },
            { "a frame size of 16383", PROVEN_H2_SETTINGS_MAX_FRAME_SIZE, 16383, PROVEN_H2_PROTOCOL_ERROR },
            { "a frame size of 2^24", PROVEN_H2_SETTINGS_MAX_FRAME_SIZE, 16777216, PROVEN_H2_PROTOCOL_ERROR },
        };
        for (proven_size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
            proven_h2_setting_t one = { bad[i].id, bad[i].value };
            len = 0;
            pos = 0;
            PROVEN_TEST_ASSERT(proven_h2_write_settings((proven_mem_mut_t){ g_out, sizeof g_out }, &len, &one, 1) == PROVEN_OK &&
                               proven_h2_settings_next((proven_mem_view_t){ g_out + 9, 6 }, &pos, &s, &code) == PROVEN_ERR_INVALID_FORMAT && code == bad[i].code && pos == 0,
                "a setting outside its range is refused with the code the RFC names", bad[i].what);
        }
        proven_h2_setting_t edge[] = { { PROVEN_H2_SETTINGS_MAX_FRAME_SIZE, 16777215 }, { PROVEN_H2_SETTINGS_ENABLE_PUSH, 1 } };
        len = 0; pos = 0;
        PROVEN_TEST_ASSERT(proven_h2_write_settings((proven_mem_mut_t){ g_out, sizeof g_out }, &len, edge, 2) == PROVEN_OK &&
                           proven_h2_settings_next((proven_mem_view_t){ g_out + 9, 12 }, &pos, &s, &code) == PROVEN_OK && proven_h2_settings_next((proven_mem_view_t){ g_out + 9, 12 }, &pos, &s, &code) == PROVEN_OK,
            "while the largest frame size and ENABLE_PUSH of 1 are in range", "");
        pos = 0;
        PROVEN_TEST_ASSERT(proven_h2_settings_next((proven_mem_view_t){ g_out + 9, 5 }, &pos, &s, &code) == PROVEN_ERR_INVALID_FORMAT && code == PROVEN_H2_FRAME_SIZE_ERROR, "five bytes are not a setting", "");
        len = 0;
        PROVEN_TEST_ASSERT(proven_h2_write_settings((proven_mem_mut_t){ g_out, 14 }, &len, list, 1) == PROVEN_ERR_OUT_OF_BOUNDS && len == 0, "a frame that does not fit is not begun", "");
    }

    PROVEN_TEST_SECTION("the small frames", "What this side writes, byte for byte, and read back.", "Check the proven_h2_write_ and proven_h2_read_ functions.");
    {
        proven_size_t len = 0;
        static const proven_byte_t ack[] = { 0, 0, 0, 4, 1, 0, 0, 0, 0 };
        PROVEN_TEST_ASSERT(proven_h2_write_settings_ack((proven_mem_mut_t){ g_out, sizeof g_out }, &len) == PROVEN_OK && len == 9 && memcmp(g_out, ack, 9) == 0, "a SETTINGS acknowledgement is nine fixed bytes", "");
        static const proven_byte_t opaque[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        static const proven_byte_t ping[] = { 0, 0, 8, 6, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8 };
        len = 0;
        PROVEN_TEST_ASSERT(proven_h2_write_ping((proven_mem_mut_t){ g_out, sizeof g_out }, &len, opaque, false) == PROVEN_OK && len == 17 && memcmp(g_out, ping, 17) == 0, "a PING carries its eight bytes", "");
        len = 0;
        PROVEN_TEST_ASSERT(proven_h2_write_ping((proven_mem_mut_t){ g_out, sizeof g_out }, &len, opaque, true) == PROVEN_OK && g_out[4] == PROVEN_H2_FLAG_ACK && memcmp(g_out + 9, opaque, 8) == 0, "and its acknowledgement the same eight, with the flag", "");
        static const proven_byte_t wu[] = { 0, 0, 4, 8, 0, 0, 0, 0, 5, 0, 1, 0, 0 };
        proven_u32 inc = 0, code = 0, last = 0;
        len = 0;
        PROVEN_TEST_ASSERT(proven_h2_write_window_update((proven_mem_mut_t){ g_out, sizeof g_out }, &len, 5, 65536) == PROVEN_OK && len == 13 && memcmp(g_out, wu, 13) == 0 &&
                           proven_h2_read_window_update((proven_mem_view_t){ g_out + 9, 4 }, &inc) == PROVEN_OK && inc == 65536, "a WINDOW_UPDATE of 65,536 for stream 5", "");
        PROVEN_TEST_ASSERT(proven_h2_write_window_update((proven_mem_mut_t){ g_out, sizeof g_out }, &len, 5, 0) == PROVEN_ERR_INVALID_ARG && proven_h2_write_window_update((proven_mem_mut_t){ g_out, sizeof g_out }, &len, 5, 0x80000000u) == PROVEN_ERR_INVALID_ARG,
            "an increment of zero or of 2^31 is not written", "");
        static const proven_byte_t zero[4] = { 0x80, 0, 0, 0 };
        PROVEN_TEST_ASSERT(proven_h2_read_window_update((proven_mem_view_t){ zero, 4 }, &inc) == PROVEN_ERR_INVALID_FORMAT, "and one of zero that arrives - reserved bit or not - is PROVEN_ERR_INVALID_FORMAT", "");
        len = 0;
        PROVEN_TEST_ASSERT(proven_h2_write_rst_stream((proven_mem_mut_t){ g_out, sizeof g_out }, &len, 7, PROVEN_H2_CANCEL) == PROVEN_OK && len == 13 && g_out[3] == PROVEN_H2_RST_STREAM && g_out[8] == 7 &&
                           proven_h2_read_rst_stream((proven_mem_view_t){ g_out + 9, 4 }, &code) == PROVEN_OK && code == PROVEN_H2_CANCEL, "a RST_STREAM with CANCEL for stream 7", "");
        PROVEN_TEST_ASSERT(proven_h2_write_rst_stream((proven_mem_mut_t){ g_out, sizeof g_out }, &len, 0, PROVEN_H2_CANCEL) == PROVEN_ERR_INVALID_ARG, "a connection cannot be reset this way", "");
        len = 0;
        proven_mem_view_t debug;
        PROVEN_TEST_ASSERT(proven_h2_write_goaway((proven_mem_mut_t){ g_out, sizeof g_out }, &len, 41, PROVEN_H2_ENHANCE_YOUR_CALM, (proven_mem_view_t){ (const proven_byte_t *)"too many resets", 15 }) == PROVEN_OK && len == 9 + 8 + 15 &&
                           proven_h2_read_goaway((proven_mem_view_t){ g_out + 9, 23 }, &last, &code, &debug) == PROVEN_OK && last == 41 && code == PROVEN_H2_ENHANCE_YOUR_CALM && debug.size == 15 && memcmp(debug.ptr, "too many resets", 15) == 0,
            "a GOAWAY with the last stream, the code, and words for a human", "");
        len = 3;
        PROVEN_TEST_ASSERT(proven_h2_write_goaway((proven_mem_mut_t){ g_out, 19 }, &len, 41, PROVEN_H2_NO_ERROR, (proven_mem_view_t){ 0 }) == PROVEN_ERR_OUT_OF_BOUNDS && len == 3, "none of them begins a frame it has no room to finish", "");
    }

    PROVEN_TEST_SECTION("a header list as a request", "What makes one, and each way it is malformed.", "Check proven_h2_request_head, h2_field_ok and h2_ordinary_ok against RFC 9113 sections 8.2 and 8.3.");
    {
        proven_h2_request_head_t head;
        proven_http_header_t get[] = { { sv(":method"), sv("GET") }, { sv(":scheme"), sv("https") }, { sv(":path"), sv("/a?b=c") }, { sv(":authority"), sv("example.com") }, { sv("accept"), sv("*/*") }, { sv("te"), sv("trailers") } };
        PROVEN_TEST_ASSERT(proven_h2_request_head(get, 6, &head) == PROVEN_OK && proven_u8str_view_eq(head.method, sv("GET")) && proven_u8str_view_eq(head.scheme, sv("https")) &&
                           proven_u8str_view_eq(head.path, sv("/a?b=c")) && proven_u8str_view_eq(head.authority, sv("example.com")) && head.fields == get + 4 && head.field_count == 2,
            "a GET: the four pseudo-header fields taken apart, the ordinary fields after them", "");
        proven_http_header_t no_authority[] = { { sv(":method"), sv("POST") }, { sv(":path"), sv("/") }, { sv(":scheme"), sv("http") } };
        PROVEN_TEST_ASSERT(proven_h2_request_head(no_authority, 3, &head) == PROVEN_OK && head.authority.size == 0 && head.field_count == 0, "the authority may be absent, and the pseudo-header fields come in any order", "");
        proven_http_header_t options[] = { { sv(":method"), sv("OPTIONS") }, { sv(":scheme"), sv("https") }, { sv(":path"), sv("*") }, { sv(":authority"), sv("example.com") } };
        PROVEN_TEST_ASSERT(proven_h2_request_head(options, 4, &head) == PROVEN_OK && proven_u8str_view_eq(head.path, sv("*")), "OPTIONS for the server as a whole has the path *", "");
        proven_http_header_t connect[] = { { sv(":method"), sv("CONNECT") }, { sv(":authority"), sv("example.com:443") } };
        PROVEN_TEST_ASSERT(proven_h2_request_head(connect, 2, &head) == PROVEN_OK && head.scheme.size == 0 && head.path.size == 0, "CONNECT has a method and an authority and nothing else", "");
        proven_http_header_t extended[] = { { sv(":method"), sv("CONNECT") }, { sv(":protocol"), sv("websocket") }, { sv(":scheme"), sv("https") },
                                            { sv(":authority"), sv("example.com") }, { sv(":path"), sv("/chat") }, { sv("sec-websocket-version"), sv("13") } };
        PROVEN_TEST_ASSERT(proven_h2_request_head(extended, 6, &head) == PROVEN_OK && head.protocol.size == 9 && memcmp(head.protocol.ptr, "websocket", 9) == 0 &&
                           head.path.size == 5 && head.authority.size == 11 && head.field_count == 1,
            "an extended CONNECT (RFC 8441): :protocol with CONNECT, and scheme, authority and path all there", "");
        PROVEN_TEST_ASSERT(proven_h2_request_head(connect, 2, &head) == PROVEN_OK && head.protocol.size == 0, "a plain CONNECT names no protocol", "");
        proven_http_header_t other_scheme[] = { { sv(":method"), sv("GET") }, { sv(":scheme"), sv("ftp") }, { sv(":path"), sv("") } };
        PROVEN_TEST_ASSERT(proven_h2_request_head(other_scheme, 3, &head) == PROVEN_OK, "an empty path is malformed only for http and https", "");

        #define M ":method", "GET"
        #define S ":scheme", "https"
        #define P ":path", "/"
        static const struct { const char *what; const char *f[5][2]; proven_size_t n; } bad[] = {
            { "no :method", { { S }, { P } }, 2 },
            { "no :scheme", { { M }, { P } }, 2 },
            { "no :path", { { M }, { S } }, 2 },
            { "an empty :path for https", { { M }, { S }, { ":path", "" } }, 3 },
            { "an empty :method", { { ":method", "" }, { S }, { P } }, 3 },
            { ":method twice", { { M }, { M }, { S }, { P } }, 4 },
            { ":path twice", { { M }, { S }, { P }, { P } }, 4 },
            { ":status in a request", { { M }, { S }, { P }, { ":status", "200" } }, 4 },
            { "a pseudo-header field nobody defined", { { M }, { S }, { P }, { ":tunnel", "x" } }, 4 },
            { ":protocol with a method other than CONNECT", { { M }, { S }, { ":authority", "x" }, { P }, { ":protocol", "websocket" } }, 5 },
            { "an empty :protocol", { { ":method", "CONNECT" }, { S }, { ":authority", "x" }, { P }, { ":protocol", "" } }, 5 },
            { ":protocol twice", { { ":method", "CONNECT" }, { ":protocol", "websocket" }, { S }, { ":authority", "x" }, { ":protocol", "websocket" } }, 5 },
            { "an extended CONNECT with no :path", { { ":method", "CONNECT" }, { ":protocol", "websocket" }, { S }, { ":authority", "x" } }, 4 },
            { "an extended CONNECT with no :scheme", { { ":method", "CONNECT" }, { ":protocol", "websocket" }, { ":authority", "x" }, { P } }, 4 },
            { "an extended CONNECT with no :authority", { { ":method", "CONNECT" }, { ":protocol", "websocket" }, { S }, { P } }, 4 },
            { "a pseudo-header field after an ordinary one", { { M }, { S }, { "accept", "*/*" }, { P } }, 4 },
            { "CONNECT with a path", { { ":method", "CONNECT" }, { ":authority", "x:443" }, { P } }, 3 },
            { "CONNECT with a scheme", { { ":method", "CONNECT" }, { ":authority", "x:443" }, { S } }, 3 },
            { "CONNECT with no authority", { { ":method", "CONNECT" } }, 1 },
            { "an uppercase letter in a name", { { M }, { S }, { P }, { "Accept", "*/*" } }, 4 },
            { "a space in a name", { { M }, { S }, { P }, { "x y", "z" } }, 4 },
            { "a colon inside a name", { { M }, { S }, { P }, { "x:y", "z" } }, 4 },
            { "a control byte in a name", { { M }, { S }, { P }, { "x\x01y", "z" } }, 4 },
            { "a byte above 0x7e in a name", { { M }, { S }, { P }, { "x\xc3\xa9", "z" } }, 4 },
            { "an empty name", { { M }, { S }, { P }, { "", "z" } }, 4 },
            { "a line feed in a value", { { M }, { S }, { P }, { "x", "a\nb" } }, 4 },
            { "a carriage return in a value", { { M }, { S }, { P }, { "x", "a\rb" } }, 4 },
            { "a value that begins with a space", { { M }, { S }, { P }, { "x", " a" } }, 4 },
            { "a value that ends with a tab", { { M }, { S }, { P }, { "x", "a\t" } }, 4 },
            { "a line feed in :path", { { M }, { S }, { ":path", "/a\n" } }, 3 },
            { "connection", { { M }, { S }, { P }, { "connection", "close" } }, 4 },
            { "keep-alive", { { M }, { S }, { P }, { "keep-alive", "timeout=5" } }, 4 },
            { "proxy-connection", { { M }, { S }, { P }, { "proxy-connection", "keep-alive" } }, 4 },
            { "transfer-encoding", { { M }, { S }, { P }, { "transfer-encoding", "chunked" } }, 4 },
            { "upgrade", { { M }, { S }, { P }, { "upgrade", "websocket" } }, 4 },
            { "te with something other than trailers", { { M }, { S }, { P }, { "te", "gzip" } }, 4 },
        };
        for (proven_size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
            proven_http_header_t list[5];
            for (proven_size_t k = 0; k < bad[i].n; ++k) list[k] = (proven_http_header_t){ sv(bad[i].f[k][0]), sv(bad[i].f[k][1]) };
            PROVEN_TEST_ASSERT(proven_h2_request_head(list, bad[i].n, &head) == PROVEN_ERR_INVALID_FORMAT, "a malformed request is PROVEN_ERR_INVALID_FORMAT", bad[i].what);
        }
        /* NUL cannot be written in a C string of the table above. */
        proven_http_header_t nul[] = { { sv(":method"), sv("GET") }, { sv(":scheme"), sv("https") }, { sv(":path"), sv("/") }, { sv("x"), { (const proven_byte_t *)"a\0b", 3 } } };
        PROVEN_TEST_ASSERT(proven_h2_request_head(nul, 4, &head) == PROVEN_ERR_INVALID_FORMAT, "and so is a NUL in a value", "");
        proven_http_header_t fine[] = { { sv(":method"), sv("GET") }, { sv(":scheme"), sv("https") }, { sv(":path"), sv("/") }, { sv("x-odd_name.1~"), sv("a b\tc \xc3\xa9") }, { sv("empty"), sv("") } };
        PROVEN_TEST_ASSERT(proven_h2_request_head(fine, 5, &head) == PROVEN_OK, "while spaces and tabs inside a value, bytes above 0x7f in a value, and an empty value are all allowed", "");
        PROVEN_TEST_ASSERT(proven_h2_request_head(NULL, 0, &head) == PROVEN_ERR_INVALID_FORMAT, "no fields at all is no request", "");
        #undef M
        #undef S
        #undef P
    }

    PROVEN_TEST_SECTION("a header list as a response, and as trailers", "One pseudo-header field, or none.", "Check proven_h2_response_head and proven_h2_trailers_check.");
    {
        proven_u16 status = 0;
        const proven_http_header_t *fields = NULL;
        proven_size_t count = 0;
        proven_http_header_t ok[] = { { sv(":status"), sv("204") }, { sv("server"), sv("x") } };
        PROVEN_TEST_ASSERT(proven_h2_response_head(ok, 2, &status, &fields, &count) == PROVEN_OK && status == 204 && fields == ok + 1 && count == 1, "a response: its status, and the fields after it", "");
        static const struct { const char *what; const char *f[3][2]; proven_size_t n; } bad[] = {
            { "no :status", { { "server", "x" } }, 1 },
            { "nothing", { { "", "" } }, 0 },
            { ":status after an ordinary field", { { "server", "x" }, { ":status", "200" } }, 2 },
            { ":status twice", { { ":status", "200" }, { ":status", "200" } }, 2 },
            { "a request pseudo-header field", { { ":status", "200" }, { ":path", "/" } }, 2 },
            { "a status of two digits", { { ":status", "20" } }, 1 },
            { "a status of four digits", { { ":status", "2000" } }, 1 },
            { "a status that is not a number", { { ":status", "2x0" } }, 1 },
            { "a status below 100", { { ":status", "099" } }, 1 },
            { "connection", { { ":status", "200" }, { "connection", "close" } }, 2 },
            { "te in a response", { { ":status", "200" }, { "te", "trailers" } }, 2 },
            { "an uppercase name", { { ":status", "200" }, { "Server", "x" } }, 2 },
        };
        for (proven_size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
            proven_http_header_t list[3];
            for (proven_size_t k = 0; k < bad[i].n; ++k) list[k] = (proven_http_header_t){ sv(bad[i].f[k][0]), sv(bad[i].f[k][1]) };
            PROVEN_TEST_ASSERT(proven_h2_response_head(list, bad[i].n, &status, &fields, &count) == PROVEN_ERR_INVALID_FORMAT && status == 0 && count == 0, "a malformed response is PROVEN_ERR_INVALID_FORMAT", bad[i].what);
        }
        proven_http_header_t trailers[] = { { sv("x-checksum"), sv("abc") }, { sv("x-took"), sv("12ms") } };
        PROVEN_TEST_ASSERT(proven_h2_trailers_check(trailers, 2) == PROVEN_OK && proven_h2_trailers_check(NULL, 0) == PROVEN_OK, "trailers are ordinary fields, or none", "");
        proven_http_header_t pseudo[] = { { sv("x-checksum"), sv("abc") }, { sv(":status"), sv("200") } };
        proven_http_header_t conn[] = { { sv("connection"), sv("close") } };
        PROVEN_TEST_ASSERT(proven_h2_trailers_check(pseudo, 2) == PROVEN_ERR_INVALID_FORMAT && proven_h2_trailers_check(conn, 1) == PROVEN_ERR_INVALID_FORMAT, "with no pseudo-header field and no connection-specific one among them", "");
    }

    PROVEN_TEST_PASS("frames are read, written and judged by their type's rule, and header lists by the rules that keep a message whole.");
    return 0;
}
