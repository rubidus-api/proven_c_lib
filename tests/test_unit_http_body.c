#include "proven.h"
#include "proven_test.h"
#include <stdlib.h>
#include <string.h>

/*
 * HTTP/1.1 body framing, the body decoder, the writers, and dates.
 *
 * Framing is where request smuggling happens: every row of the framing tables is a head whose
 * body length two parsers could disagree about, and each must be refused rather than resolved.
 *
 * The decoder is checked against itself in the one way that matters: a chunked message is fed
 * whole, split in two at every position, and one byte at a time, and all three must deliver
 * the same bytes, stop at the same place, and leave what follows untouched.
 *
 * The writers are checked by parsing what they wrote, and by trying to make them write a line
 * break that was not asked for.
 */

#define CAP 16

static proven_mem_view_t mv(const char *s) {
    return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}

static proven_u8str_view_t sv(const char *s) {
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}

static proven_err_t req_framing(const char *raw, proven_http_framing_t *f) {
    proven_http_header_t h[CAP];
    proven_http_request_t r;
    proven_size_t n = 0;
    proven_err_t e = proven_http_parse_request(mv(raw), h, CAP, 0, &r, &n);
    if (e != PROVEN_OK) return PROVEN_ERR_INVALID_STATE;     /* the table is wrong, not the code */
    return proven_http_request_framing(&r, f);
}

static proven_err_t res_framing(const char *raw, proven_http_method_t method, proven_http_framing_t *f) {
    proven_http_header_t h[CAP];
    proven_http_response_t r;
    proven_size_t n = 0;
    proven_err_t e = proven_http_parse_response(mv(raw), h, CAP, 0, &r, &n);
    if (e != PROVEN_OK) return PROVEN_ERR_INVALID_STATE;
    return proven_http_response_framing(&r, method, f);
}

/* Decode `wire` in pieces of `step` bytes (0: all at once), collecting the payload. */
typedef struct { proven_err_t err; bool done; proven_size_t consumed; proven_byte_t out[512]; proven_size_t out_len; } decoded_t;

static decoded_t decode(proven_http_framing_t framing, proven_u64 max_body, const proven_byte_t *wire, proven_size_t len, proven_size_t first, proven_size_t step) {
    decoded_t d = {0};
    proven_http_body_t body;
    d.err = proven_http_body_init(&body, framing, max_body);
    if (d.err != PROVEN_OK) return d;
    proven_size_t pos = 0;
    proven_size_t piece = first ? first : (step ? step : len);
    while (pos < len && !d.done) {
        proven_size_t avail = len - pos < piece ? len - pos : piece;
        /* An exact-size copy of the piece, so a read past it is caught by the sanitizer. */
        proven_byte_t *copy = malloc(avail ? avail : 1);
        memcpy(copy, wire + pos, avail);
        proven_size_t off = 0;
        while (off < avail && !d.done) {
            proven_size_t used = 0;
            proven_mem_view_t payload;
            d.err = proven_http_body_feed(&body, (proven_mem_view_t){ copy + off, avail - off }, &used, &payload, &d.done);
            if (d.err != PROVEN_OK) { free(copy); d.consumed = pos + off; return d; }
            if (payload.size > 0) {
                bool inside = payload.ptr >= copy + off && payload.ptr + payload.size <= copy + off + used;
                if (!inside || d.out_len + payload.size > sizeof d.out) { d.err = PROVEN_ERR_INVALID_STATE; free(copy); return d; }
                memcpy(d.out + d.out_len, payload.ptr, payload.size);
                d.out_len += payload.size;
            }
            if (used == 0 && !d.done) { d.err = PROVEN_ERR_INVALID_STATE; free(copy); return d; }   /* no progress */
            off += used;
        }
        pos += off;
        free(copy);
        piece = step ? step : len;
    }
    d.consumed = pos;
    return d;
}

static bool same(const decoded_t *a, const decoded_t *b) {
    return a->err == b->err && a->done == b->done && a->consumed == b->consumed &&
           a->out_len == b->out_len && memcmp(a->out, b->out, a->out_len) == 0;
}

static proven_err_t chunked_err(const char *wire, proven_u64 max_body) {
    proven_http_framing_t f = { .kind = PROVEN_HTTP_BODY_CHUNKED };
    decoded_t whole = decode(f, max_body, (const proven_byte_t *)wire, strlen(wire), 0, 0);
    decoded_t bytes = decode(f, max_body, (const proven_byte_t *)wire, strlen(wire), 0, 1);
    /* However it is fed, the verdict must be the same. */
    return whole.err == bytes.err ? whole.err : PROVEN_ERR_INVALID_STATE;
}

int main(void) {
    PROVEN_TEST_SUITE("http: framing, the body decoder, writers and dates",
        "How long a body is cannot be a matter of opinion; a body decodes the same however it arrives; a writer cannot be made to write a line it was not asked for.",
        "Inspect http_framing_headers, proven_http_body_feed and the writers in src/proven/http.c.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("request framing",
        "Chunked, a length, or nothing - and every ambiguous combination refused.",
        "INVALID_FORMAT is a 400; UNSUPPORTED is a 501. Neither is ever resolved by picking one reading.");
    // ---------------------------------------------------------------
    {
        proven_http_framing_t f;
        PROVEN_TEST_ASSERT(req_framing("GET / HTTP/1.1\r\n\r\n", &f) == PROVEN_OK && f.kind == PROVEN_HTTP_BODY_NONE,
            "no framing header: no body", "");
        PROVEN_TEST_ASSERT(req_framing("POST / HTTP/1.1\r\nContent-Length: 1234\r\n\r\n", &f) == PROVEN_OK && f.kind == PROVEN_HTTP_BODY_LENGTH && f.length == 1234,
            "Content-Length", "");
        PROVEN_TEST_ASSERT(req_framing("POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n", &f) == PROVEN_OK && f.kind == PROVEN_HTTP_BODY_LENGTH && f.length == 0,
            "a length of zero is a length", "");
        PROVEN_TEST_ASSERT(req_framing("POST / HTTP/1.1\r\nContent-Length: 18446744073709551615\r\n\r\n", &f) == PROVEN_OK && f.length == UINT64_MAX,
            "the largest 64-bit length", "");
        PROVEN_TEST_ASSERT(req_framing("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", &f) == PROVEN_OK && f.kind == PROVEN_HTTP_BODY_CHUNKED,
            "Transfer-Encoding: chunked", "");
        PROVEN_TEST_ASSERT(req_framing("POST / HTTP/1.1\r\ntransfer-encoding:  \tCHUNKED \r\n\r\n", &f) == PROVEN_OK && f.kind == PROVEN_HTTP_BODY_CHUNKED,
            "in any case, with surrounding whitespace", "");
        PROVEN_TEST_ASSERT(req_framing("POST / HTTP/1.1\r\nTransfer-Encoding: ,chunked,\r\n\r\n", &f) == PROVEN_OK && f.kind == PROVEN_HTTP_BODY_CHUNKED,
            "empty list elements around it change nothing", "");

        static const struct { const char *raw; const char *why; } invalid[] = {
            { "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n", "TE and CL together" },
            { "POST / HTTP/1.1\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n", "CL and TE together, the other way round" },
            { "POST / HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n", "two Content-Length fields that agree" },
            { "POST / HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n", "two Content-Length fields that differ" },
            { "POST / HTTP/1.1\r\nContent-Length: 5, 5\r\n\r\n", "a Content-Length list" },
            { "POST / HTTP/1.1\r\nContent-Length: +5\r\n\r\n", "a plus sign" },
            { "POST / HTTP/1.1\r\nContent-Length: -5\r\n\r\n", "a minus sign" },
            { "POST / HTTP/1.1\r\nContent-Length: 5 5\r\n\r\n", "a space inside the number" },
            { "POST / HTTP/1.1\r\nContent-Length: 0x10\r\n\r\n", "hexadecimal" },
            { "POST / HTTP/1.1\r\nContent-Length: 1e3\r\n\r\n", "an exponent" },
            { "POST / HTTP/1.1\r\nContent-Length: 5.0\r\n\r\n", "a decimal point" },
            { "POST / HTTP/1.1\r\nContent-Length:\r\n\r\n", "an empty value" },
            { "POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n", "letters" },
            { "POST / HTTP/1.1\r\nContent-Length: 18446744073709551616\r\n\r\n", "one more than fits 64 bits" },
            { "POST / HTTP/1.1\r\nContent-Length: 99999999999999999999999\r\n\r\n", "far more than fits" },
            { "POST / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n", "Transfer-Encoding on HTTP/1.0" },
            { "POST / HTTP/1.1\r\nTransfer-Encoding:\r\n\r\n", "an empty Transfer-Encoding" },
            { "POST / HTTP/1.1\r\nTransfer-Encoding: ,\r\n\r\n", "a Transfer-Encoding of only commas" },
            { "POST / HTTP/1.1\r\nTransfer-Encoding: \"chunked\"\r\n\r\n", "a quoted coding" },
        };
        bool all = true;
        for (proven_size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
            if (req_framing(invalid[i].raw, &f) != PROVEN_ERR_INVALID_FORMAT) { all = false; PROVEN_TEST_INFO("not refused: {}", PROVEN_ARG((const char *)invalid[i].why)); }
        }
        PROVEN_TEST_ASSERT(all, "all nineteen ambiguous or malformed framings are PROVEN_ERR_INVALID_FORMAT", "");

        static const char *unsupported[] = {
            "POST / HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n",
            "POST / HTTP/1.1\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
            "POST / HTTP/1.1\r\nTransfer-Encoding: chunked, gzip\r\n\r\n",
            "POST / HTTP/1.1\r\nTransfer-Encoding: chunked, chunked\r\n\r\n",
            "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n",
            "POST / HTTP/1.1\r\nTransfer-Encoding: identity\r\n\r\n",
            "POST / HTTP/1.1\r\nTransfer-Encoding: xchunked\r\n\r\n",
            "POST / HTTP/1.1\r\nTransfer-Encoding: chunked;q=1\r\n\r\n",
            "POST / HTTP/1.1\r\nTransfer-Encoding: chunked x\r\n\r\n",
        };
        all = true;
        for (proven_size_t i = 0; i < sizeof unsupported / sizeof unsupported[0]; ++i) {
            if (req_framing(unsupported[i], &f) != PROVEN_ERR_UNSUPPORTED) { all = false; PROVEN_TEST_INFO("not unsupported: case {}", PROVEN_ARG((int)i)); }
        }
        PROVEN_TEST_ASSERT(all, "any transfer coding other than a single plain \"chunked\" is PROVEN_ERR_UNSUPPORTED", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("response framing",
        "The status and the request method can remove the body; otherwise chunked, a length, or until the peer closes.",
        "");
    // ---------------------------------------------------------------
    {
        proven_http_framing_t f;
        PROVEN_TEST_ASSERT(res_framing("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n", PROVEN_HTTP_GET, &f) == PROVEN_OK && f.kind == PROVEN_HTTP_BODY_LENGTH && f.length == 10, "a length", "");
        PROVEN_TEST_ASSERT(res_framing("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n", PROVEN_HTTP_GET, &f) == PROVEN_OK && f.kind == PROVEN_HTTP_BODY_CHUNKED, "chunked", "");
        PROVEN_TEST_ASSERT(res_framing("HTTP/1.1 200 OK\r\n\r\n", PROVEN_HTTP_GET, &f) == PROVEN_OK && f.kind == PROVEN_HTTP_BODY_UNTIL_CLOSE, "neither: until the peer closes", "");
        PROVEN_TEST_ASSERT(res_framing("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n", PROVEN_HTTP_HEAD, &f) == PROVEN_OK && f.kind == PROVEN_HTTP_BODY_NONE,
            "a response to HEAD has no body although it states a length", "");
        PROVEN_TEST_ASSERT(res_framing("HTTP/1.1 204 No Content\r\n\r\n", PROVEN_HTTP_GET, &f) == PROVEN_OK && f.kind == PROVEN_HTTP_BODY_NONE &&
                           res_framing("HTTP/1.1 304 Not Modified\r\nContent-Length: 10\r\n\r\n", PROVEN_HTTP_GET, &f) == PROVEN_OK && f.kind == PROVEN_HTTP_BODY_NONE &&
                           res_framing("HTTP/1.1 100 Continue\r\n\r\n", PROVEN_HTTP_POST, &f) == PROVEN_OK && f.kind == PROVEN_HTTP_BODY_NONE &&
                           res_framing("HTTP/1.1 101 Switching Protocols\r\n\r\n", PROVEN_HTTP_GET, &f) == PROVEN_OK && f.kind == PROVEN_HTTP_BODY_NONE,
            "204, 304 and every 1xx have no body", "");
        PROVEN_TEST_ASSERT(res_framing("HTTP/1.1 200 Connection established\r\n\r\n", PROVEN_HTTP_CONNECT, &f) == PROVEN_OK && f.kind == PROVEN_HTTP_BODY_NONE &&
                           res_framing("HTTP/1.1 403 Forbidden\r\nContent-Length: 3\r\n\r\n", PROVEN_HTTP_CONNECT, &f) == PROVEN_OK && f.kind == PROVEN_HTTP_BODY_LENGTH,
            "a successful CONNECT has no body - a tunnel follows; a refused one has", "");
        PROVEN_TEST_ASSERT(res_framing("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n", PROVEN_HTTP_GET, &f) == PROVEN_ERR_INVALID_FORMAT &&
                           res_framing("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n", PROVEN_HTTP_GET, &f) == PROVEN_ERR_INVALID_FORMAT &&
                           res_framing("HTTP/1.1 204 No Content\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n", PROVEN_HTTP_GET, &f) == PROVEN_ERR_INVALID_FORMAT,
            "contradictory framing is refused in a response too, even when the status has no body", "");
        PROVEN_TEST_ASSERT(res_framing("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n", PROVEN_HTTP_GET, &f) == PROVEN_ERR_UNSUPPORTED,
            "an unknown transfer coding is PROVEN_ERR_UNSUPPORTED", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a body of known length",
        "Exactly that many bytes are the body; what follows belongs to the next message.",
        "");
    // ---------------------------------------------------------------
    {
        const char *wire = "hello worldNEXT";
        proven_http_framing_t f = { .kind = PROVEN_HTTP_BODY_LENGTH, .length = 11 };
        decoded_t whole = decode(f, 100, (const proven_byte_t *)wire, strlen(wire), 0, 0);
        PROVEN_TEST_ASSERT(whole.err == PROVEN_OK && whole.done && whole.consumed == 11 && whole.out_len == 11 && memcmp(whole.out, "hello world", 11) == 0,
            "eleven bytes are delivered and the decoder stops before NEXT", "");
        bool all = true;
        for (proven_size_t cut = 1; cut < strlen(wire); ++cut) {
            decoded_t split = decode(f, 100, (const proven_byte_t *)wire, strlen(wire), cut, 0);
            if (!same(&whole, &split)) all = false;
        }
        decoded_t bytes = decode(f, 100, (const proven_byte_t *)wire, strlen(wire), 0, 1);
        PROVEN_TEST_ASSERT(all && same(&whole, &bytes), "split at any position, or fed one byte at a time, the result is identical", "");

        proven_http_body_t body;
        PROVEN_TEST_ASSERT(proven_http_body_init(&body, f, 10) == PROVEN_ERR_OUT_OF_BOUNDS,
            "a length above the limit is refused before a byte is read: PROVEN_ERR_OUT_OF_BOUNDS", "");
        PROVEN_TEST_ASSERT(proven_http_body_init(&body, f, 11) == PROVEN_OK, "a length of exactly the limit is accepted", "");
        PROVEN_TEST_ASSERT(proven_http_body_end(&body) == PROVEN_ERR_NEED_MORE,
            "a connection that closes before the body is complete is PROVEN_ERR_NEED_MORE: truncated, not finished", "");

        proven_http_framing_t zero = { .kind = PROVEN_HTTP_BODY_LENGTH, .length = 0 };
        proven_size_t used = 9;
        proven_mem_view_t payload;
        bool done = false;
        PROVEN_TEST_ASSERT(proven_http_body_init(&body, zero, 0) == PROVEN_OK &&
                           proven_http_body_feed(&body, mv("NEXT"), &used, &payload, &done) == PROVEN_OK && done && used == 0 && payload.size == 0 &&
                           proven_http_body_end(&body) == PROVEN_OK,
            "a body of length zero is done at once and consumes nothing", "");
        proven_http_framing_t none = { .kind = PROVEN_HTTP_BODY_NONE };
        PROVEN_TEST_ASSERT(proven_http_body_init(&body, none, 0) == PROVEN_OK &&
                           proven_http_body_feed(&body, mv("NEXT"), &used, &payload, &done) == PROVEN_OK && done && used == 0,
            "and so is no body at all", "");
        proven_http_framing_t bogus = { .kind = (proven_http_body_kind_t)99 };
        PROVEN_TEST_ASSERT(proven_http_body_init(&body, bogus, 0) == PROVEN_ERR_INVALID_ARG, "a framing kind that does not exist is PROVEN_ERR_INVALID_ARG", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a chunked body, however it arrives",
        "One message - extensions, a trailer, the next request behind it - decoded whole, split at every position, and byte by byte.",
        "The three must agree on every byte delivered and on where the body ends.");
    // ---------------------------------------------------------------
    {
        const char *wire = "4\r\nWiki\r\n"
                           "6;ext=1;other=\"q\"\r\npedia \r\n"
                           "E\r\nin \r\n\r\nchunks.\r\n"
                           "000a\r\n0123456789\r\n"
                           "0\r\n"
                           "X-Checksum: abc\r\nX-Other:\tv\r\n"
                           "\r\n"
                           "GET /next HTTP/1.1\r\n\r\n";
        proven_size_t body_end = strlen(wire) - strlen("GET /next HTTP/1.1\r\n\r\n");
        const char *want = "Wikipedia in \r\n\r\nchunks.0123456789";
        proven_http_framing_t f = { .kind = PROVEN_HTTP_BODY_CHUNKED };

        decoded_t whole = decode(f, 1000, (const proven_byte_t *)wire, strlen(wire), 0, 0);
        PROVEN_TEST_ASSERT(whole.err == PROVEN_OK && whole.done, "the message decodes", "");
        PROVEN_TEST_ASSERT(whole.out_len == strlen(want) && memcmp(whole.out, want, whole.out_len) == 0,
            "the payload is the chunks joined - CRLF inside a chunk is data", "");
        PROVEN_TEST_ASSERT(whole.consumed == body_end, "the decoder stops exactly where the next request begins", "");

        bool all = true;
        for (proven_size_t cut = 1; cut < strlen(wire); ++cut) {
            decoded_t split = decode(f, 1000, (const proven_byte_t *)wire, strlen(wire), cut, 0);
            if (!same(&whole, &split)) { all = false; PROVEN_TEST_INFO("differs when split at {}", PROVEN_ARG((int)cut)); break; }
        }
        PROVEN_TEST_ASSERT(all, "split in two at every one of its positions, the result is identical", "");
        for (proven_size_t step = 1; step <= 7; ++step) {
            decoded_t pieces = decode(f, 1000, (const proven_byte_t *)wire, strlen(wire), 0, step);
            if (!same(&whole, &pieces)) all = false;
        }
        PROVEN_TEST_ASSERT(all, "and fed in pieces of one to seven bytes", "");

        decoded_t exact = decode(f, strlen(want), (const proven_byte_t *)wire, strlen(wire), 0, 3);
        decoded_t tight = decode(f, strlen(want) - 1, (const proven_byte_t *)wire, strlen(wire), 0, 3);
        PROVEN_TEST_ASSERT(exact.err == PROVEN_OK && exact.done && tight.err == PROVEN_ERR_OUT_OF_BOUNDS,
            "a limit of exactly the body's size admits it; one byte less is PROVEN_ERR_OUT_OF_BOUNDS", "");

        proven_http_body_t body;
        proven_size_t used = 0;
        proven_mem_view_t payload;
        bool done = false;
        PROVEN_TEST_ASSERT(proven_http_body_init(&body, f, 1000) == PROVEN_OK &&
                           proven_http_body_feed(&body, mv("4\r\nWi"), &used, &payload, &done) == PROVEN_OK && used == 5 && payload.size == 2 && !done &&
                           proven_http_body_received(&body) == 2 && proven_http_body_end(&body) == PROVEN_ERR_NEED_MORE,
            "a connection that ends inside a chunk is PROVEN_ERR_NEED_MORE: the two bytes received are not the body", "");

        decoded_t empty = decode(f, 10, (const proven_byte_t *)"0\r\n\r\nNEXT", 9, 0, 1);
        PROVEN_TEST_ASSERT(empty.err == PROVEN_OK && empty.done && empty.out_len == 0 && empty.consumed == 5, "an empty chunked body is five bytes of framing", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("chunk framing that is refused",
        "Each is a spelling one chunk parser accepts and another does not.",
        "After any of these the decoder answers PROVEN_ERR_INVALID_STATE: the position of the next message is unknown, and the connection is finished.");
    // ---------------------------------------------------------------
    {
        static const struct { const char *wire; const char *why; } bad[] = {
            { "\r\nabc\r\n0\r\n\r\n", "no size at all" },
            { "g\r\nabc\r\n0\r\n\r\n", "a size that is not hex" },
            { "0x3\r\nabc\r\n0\r\n\r\n", "a 0x prefix" },
            { "3 \r\nabc\r\n0\r\n\r\n", "a space after the size" },
            { " 3\r\nabc\r\n0\r\n\r\n", "a space before the size" },
            { "3\t;x\r\nabc\r\n0\r\n\r\n", "a tab before the extension" },
            { "+3\r\nabc\r\n0\r\n\r\n", "a sign" },
            { "3\nabc\r\n0\r\n\r\n", "a bare LF after the size" },
            { "3\r\rabc\r\n0\r\n\r\n", "CR CR after the size" },
            { "3\r\nabc\n0\r\n\r\n", "a bare LF after the data" },
            { "3\r\nabcd\r\n0\r\n\r\n", "more data than the size says" },
            { "3\r\nab\r\n0\r\n\r\n", "less data than the size says" },
            { "3\r\nabc0\r\n\r\n", "no CRLF after the data" },
            { "11111111111111111\r\n", "seventeen hex digits" },
            { "3;a\x01" "b\r\nabc\r\n0\r\n\r\n", "a control character in an extension" },
            { "0\r\n bad: trailer\r\n\r\n", "a trailer line that starts with a space" },
            { "0\r\nbad trailer\r\n\r\n", "a trailer with no colon" },
            { "0\r\nX: v\n\r\n", "a bare LF ending a trailer" },
            { "0\r\nX: a\x01\r\n\r\n", "a control character in a trailer" },
            { "0\r\n\rX", "a CR not followed by LF at the very end" },
        };
        bool all = true;
        for (proven_size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
            if (chunked_err(bad[i].wire, 1000) != PROVEN_ERR_INVALID_FORMAT) { all = false; PROVEN_TEST_INFO("not refused: {}", PROVEN_ARG((const char *)bad[i].why)); }
        }
        PROVEN_TEST_ASSERT(all, "all twenty are PROVEN_ERR_INVALID_FORMAT, whether fed whole or byte by byte", "");

        PROVEN_TEST_ASSERT(chunked_err("ffffffffffffffff\r\n", 1000) == PROVEN_ERR_OUT_OF_BOUNDS &&
                           chunked_err("3e9\r\n", 1000) == PROVEN_ERR_OUT_OF_BOUNDS &&
                           chunked_err("3e8\r\n", 1000) == PROVEN_OK,
            "a chunk larger than the remaining allowance is refused when its size is read, not after its data arrives", "");
        PROVEN_TEST_ASSERT(chunked_err("3\r\nabc\r\n3e6\r\n", 1000) == PROVEN_ERR_OUT_OF_BOUNDS && chunked_err("3\r\nabc\r\n3e5\r\n", 1000) == PROVEN_OK,
            "the allowance counts the chunks already received", "");

        static char longline[400];
        memset(longline, 'x', sizeof longline);
        memcpy(longline, "3;", 2);
        longline[PROVEN_HTTP_MAX_CHUNK_LINE - 1] = '\r';
        longline[PROVEN_HTTP_MAX_CHUNK_LINE] = '\0';
        PROVEN_TEST_ASSERT(chunked_err(longline, 1000) == PROVEN_OK, "a chunk-size line of exactly the bound is accepted", "");
        longline[PROVEN_HTTP_MAX_CHUNK_LINE - 1] = 'x';
        longline[PROVEN_HTTP_MAX_CHUNK_LINE] = 'x';
        longline[PROVEN_HTTP_MAX_CHUNK_LINE + 1] = '\0';
        PROVEN_TEST_ASSERT(chunked_err(longline, 1000) == PROVEN_ERR_OUT_OF_BOUNDS, "one that runs past it is PROVEN_ERR_OUT_OF_BOUNDS: an endless extension costs 256 bytes", "");

        static char trailers[PROVEN_HTTP_MAX_TRAILER_BYTES + 64];
        memset(trailers, 'v', sizeof trailers);
        memcpy(trailers, "0\r\nX: ", 6);
        trailers[sizeof trailers - 1] = '\0';
        PROVEN_TEST_ASSERT(chunked_err(trailers, 1000) == PROVEN_ERR_OUT_OF_BOUNDS, "a trailer section past its bound is PROVEN_ERR_OUT_OF_BOUNDS", "");

        proven_http_body_t body;
        proven_http_framing_t f = { .kind = PROVEN_HTTP_BODY_CHUNKED };
        proven_size_t used = 0;
        proven_mem_view_t payload;
        bool done = false;
        PROVEN_TEST_ASSERT(proven_http_body_init(&body, f, 100) == PROVEN_OK &&
                           proven_http_body_feed(&body, mv("zz"), &used, &payload, &done) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_http_body_feed(&body, mv("0\r\n\r\n"), &used, &payload, &done) == PROVEN_ERR_INVALID_STATE &&
                           proven_http_body_end(&body) == PROVEN_ERR_INVALID_STATE,
            "after an error the decoder refuses everything: PROVEN_ERR_INVALID_STATE", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a body that runs until the peer closes",
        "Everything is payload, up to the limit; it is never done, and the close is its normal end.",
        "");
    // ---------------------------------------------------------------
    {
        proven_http_body_t body;
        proven_http_framing_t f = { .kind = PROVEN_HTTP_BODY_UNTIL_CLOSE };
        proven_size_t used = 0;
        proven_mem_view_t payload;
        bool done = true;
        PROVEN_TEST_ASSERT(proven_http_body_init(&body, f, 10) == PROVEN_OK &&
                           proven_http_body_feed(&body, mv("0\r\n\r\n"), &used, &payload, &done) == PROVEN_OK && used == 5 && payload.size == 5 && !done,
            "bytes that look like chunk framing are simply data", "");
        PROVEN_TEST_ASSERT(proven_http_body_feed(&body, mv("12345"), &used, &payload, &done) == PROVEN_OK && used == 5 && !done &&
                           proven_http_body_received(&body) == 10 && proven_http_body_end(&body) == PROVEN_OK,
            "ten bytes fit a limit of ten, and the close ends the body properly", "");
        PROVEN_TEST_ASSERT(proven_http_body_feed(&body, mv("x"), &used, &payload, &done) == PROVEN_ERR_OUT_OF_BOUNDS,
            "the eleventh byte is PROVEN_ERR_OUT_OF_BOUNDS", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("writers: what is written parses back",
        "A request and a response are built in a buffer and read by the parser; a chunked body written here is decoded here.",
        "");
    // ---------------------------------------------------------------
    {
        proven_byte_t buf[512];
        proven_mem_mut_t out = { buf, sizeof buf };
        proven_size_t len = 0;
        PROVEN_TEST_ASSERT(proven_http_write_request_line(out, &len, PROVEN_LIT("POST"), PROVEN_LIT("/upload?name=a%20b")) == PROVEN_OK &&
                           proven_http_write_header(out, &len, PROVEN_LIT("Host"), PROVEN_LIT("example.com")) == PROVEN_OK &&
                           proven_http_write_header(out, &len, PROVEN_LIT("X-Empty"), PROVEN_LIT("")) == PROVEN_OK &&
                           proven_http_write_header(out, &len, PROVEN_LIT("X-Text"), sv("two words\tand a tab; caf\xe9")) == PROVEN_OK &&
                           proven_http_write_header_u64(out, &len, PROVEN_LIT("Content-Length"), 18446744073709551615ull) == PROVEN_OK &&
                           proven_http_write_head_end(out, &len) == PROVEN_OK,
            "a request head is written field by field", "");
        const char *want = "POST /upload?name=a%20b HTTP/1.1\r\nHost: example.com\r\nX-Empty: \r\nX-Text: two words\tand a tab; caf\xe9\r\nContent-Length: 18446744073709551615\r\n\r\n";
        PROVEN_TEST_ASSERT(len == strlen(want) && memcmp(buf, want, len) == 0, "and is byte for byte what it should be", "");
        proven_http_header_t h[CAP];
        proven_http_request_t req;
        proven_size_t n = 0;
        proven_http_framing_t f;
        PROVEN_TEST_ASSERT(proven_http_parse_request((proven_mem_view_t){ buf, len }, h, CAP, 0, &req, &n) == PROVEN_OK && n == len &&
                           req.method == PROVEN_HTTP_POST && req.header_count == 4 &&
                           proven_http_request_framing(&req, &f) == PROVEN_OK && f.length == UINT64_MAX,
            "the parser reads back the same request", "");

        len = 0;
        PROVEN_TEST_ASSERT(proven_http_write_status_line(out, &len, 404, PROVEN_LIT("")) == PROVEN_OK &&
                           proven_http_write_header(out, &len, PROVEN_LIT("Transfer-Encoding"), PROVEN_LIT("chunked")) == PROVEN_OK &&
                           proven_http_write_head_end(out, &len) == PROVEN_OK,
            "a response head, with the standard reason phrase filled in", "");
        proven_size_t head_len = len;
        PROVEN_TEST_ASSERT(memcmp(buf, "HTTP/1.1 404 Not Found\r\n", 24) == 0, "404 is \"Not Found\"", "");
        static const char *parts[] = { "first ", "second, longer than fifteen bytes ", "x" };
        for (proven_size_t i = 0; i < 3; ++i) {
            proven_size_t pl = strlen(parts[i]);
            PROVEN_TEST_ASSERT(proven_http_write_chunk_begin(out, &len, pl) == PROVEN_OK, "a chunk begins", "");
            memcpy(buf + len, parts[i], pl);
            len += pl;
            PROVEN_TEST_ASSERT(proven_http_write_chunk_end(out, &len) == PROVEN_OK, "and ends", "");
        }
        PROVEN_TEST_ASSERT(proven_http_write_last_chunk(out, &len) == PROVEN_OK, "the last chunk", "");
        proven_http_response_t res;
        PROVEN_TEST_ASSERT(proven_http_parse_response((proven_mem_view_t){ buf, len }, h, CAP, 0, &res, &n) == PROVEN_OK && n == head_len && res.status == 404 &&
                           proven_http_response_framing(&res, PROVEN_HTTP_GET, &f) == PROVEN_OK && f.kind == PROVEN_HTTP_BODY_CHUNKED,
            "the parser reads the response head and finds a chunked body", "");
        decoded_t d = decode(f, 1000, buf + n, len - n, 0, 5);
        const char *joined = "first second, longer than fifteen bytes x";
        PROVEN_TEST_ASSERT(d.err == PROVEN_OK && d.done && d.consumed == len - n && d.out_len == strlen(joined) && memcmp(d.out, joined, d.out_len) == 0,
            "and the decoder reads back the three chunks, ending exactly at the end of what was written", "");

        len = 0;
        PROVEN_TEST_ASSERT(proven_http_write_status_line(out, &len, 299, PROVEN_LIT("Custom Phrase")) == PROVEN_OK && len == 28 &&
                           memcmp(buf, "HTTP/1.1 299 Custom Phrase\r\n", 28) == 0, "a reason phrase of the caller's choosing is used as given", "");
        len = 0;
        PROVEN_TEST_ASSERT(proven_http_write_status_line(out, &len, 799, PROVEN_LIT("")) == PROVEN_OK && memcmp(buf, "HTTP/1.1 799 Unknown\r\n", 22) == 0,
            "a status with no standard phrase gets \"Unknown\"", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("writers: no line that was not asked for",
        "A CR or LF in a name, a value, a target or a reason is refused, and a refusal writes nothing.",
        "This is response splitting. A value copied from a request into a response must not be able to end its field and start another.");
    // ---------------------------------------------------------------
    {
        proven_byte_t buf[64];
        proven_mem_mut_t out = { buf, sizeof buf };
        memset(buf, '#', sizeof buf);
        proven_size_t len = 3;
        static const char *bad_values[] = {
            "v\r\nSet-Cookie: stolen=1", "v\nX: y", "v\rX: y", "a\x01" "b", "a\x7f" "b", " leading", "trailing ", "\ttab-led", "trailing\t",
        };
        bool all = true;
        for (proven_size_t i = 0; i < sizeof bad_values / sizeof bad_values[0]; ++i) {
            if (proven_http_write_header(out, &len, PROVEN_LIT("X"), sv(bad_values[i])) != PROVEN_ERR_INVALID_ARG || len != 3) all = false;
        }
        const proven_byte_t nul_value[] = { 'a', 0, 'b' };
        if (proven_http_write_header(out, &len, PROVEN_LIT("X"), (proven_u8str_view_t){ nul_value, 3 }) != PROVEN_ERR_INVALID_ARG) all = false;
        PROVEN_TEST_ASSERT(all, "ten hostile or malformed values are PROVEN_ERR_INVALID_ARG, and the length does not move", "");

        static const char *bad_names[] = { "", "X Y", "X:", "X\r\nY", "X(1)", "caf\xc3\xa9" };
        all = true;
        for (proven_size_t i = 0; i < sizeof bad_names / sizeof bad_names[0]; ++i) {
            if (proven_http_write_header(out, &len, sv(bad_names[i]), PROVEN_LIT("v")) != PROVEN_ERR_INVALID_ARG || len != 3) all = false;
            if (proven_http_write_header_u64(out, &len, sv(bad_names[i]), 1) != PROVEN_ERR_INVALID_ARG || len != 3) all = false;
        }
        PROVEN_TEST_ASSERT(all, "six names that are not tokens are refused by both header writers", "");

        PROVEN_TEST_ASSERT(proven_http_write_request_line(out, &len, PROVEN_LIT("GET"), sv("/a b")) == PROVEN_ERR_INVALID_ARG &&
                           proven_http_write_request_line(out, &len, PROVEN_LIT("GET"), sv("/a\r\nX: y")) == PROVEN_ERR_INVALID_ARG &&
                           proven_http_write_request_line(out, &len, PROVEN_LIT("GET"), sv("")) == PROVEN_ERR_INVALID_ARG &&
                           proven_http_write_request_line(out, &len, PROVEN_LIT("GE T"), sv("/")) == PROVEN_ERR_INVALID_ARG &&
                           proven_http_write_request_line(out, &len, PROVEN_LIT(""), sv("/")) == PROVEN_ERR_INVALID_ARG && len == 3,
            "a target with a space or a line break, an empty target, and a method that is not a token", "");
        PROVEN_TEST_ASSERT(proven_http_write_status_line(out, &len, 200, sv("OK\r\nX: y")) == PROVEN_ERR_INVALID_ARG &&
                           proven_http_write_status_line(out, &len, 99, PROVEN_LIT("")) == PROVEN_ERR_INVALID_ARG &&
                           proven_http_write_status_line(out, &len, 1000, PROVEN_LIT("")) == PROVEN_ERR_INVALID_ARG && len == 3,
            "a reason with a line break, and a status outside 100-999", "");
        PROVEN_TEST_ASSERT(proven_http_write_chunk_begin(out, &len, 0) == PROVEN_ERR_INVALID_ARG && len == 3,
            "a chunk of size zero is the last chunk and has its own writer", "");
        bool untouched = true;
        for (proven_size_t i = 0; i < sizeof buf; ++i) if (buf[i] != '#') untouched = false;
        PROVEN_TEST_ASSERT(untouched, "and through all of that not one byte of the buffer was written", "");

        /* Room: exactly enough, and one byte too little. */
        proven_byte_t fit[7];
        proven_size_t flen = 0;
        PROVEN_TEST_ASSERT(proven_http_write_header((proven_mem_mut_t){ fit, 5 }, &flen, PROVEN_LIT("A"), PROVEN_LIT("b")) == PROVEN_ERR_OUT_OF_BOUNDS && flen == 0,
            "\"A: b\" and CRLF are six bytes, and five do not hold them", "");
        flen = 1;
        PROVEN_TEST_ASSERT(proven_http_write_header((proven_mem_mut_t){ fit, 7 }, &flen, PROVEN_LIT("A"), PROVEN_LIT("b")) == PROVEN_OK && flen == 7,
            "appended after one byte already there, six more fit exactly", "");
        flen = 2;
        PROVEN_TEST_ASSERT(proven_http_write_header((proven_mem_mut_t){ fit, 7 }, &flen, PROVEN_LIT("A"), PROVEN_LIT("b")) == PROVEN_ERR_OUT_OF_BOUNDS && flen == 2,
            "after two, they do not: PROVEN_ERR_OUT_OF_BOUNDS, and the length does not move", "");
        flen = 8;
        PROVEN_TEST_ASSERT(proven_http_write_head_end((proven_mem_mut_t){ fit, 7 }, &flen) == PROVEN_ERR_INVALID_ARG,
            "a length already past the buffer is PROVEN_ERR_INVALID_ARG", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("dates",
        "One form is written; three are read; a date that does not exist is refused.",
        "The RFC 9110 example - Sun, 06 Nov 1994 08:49:37 GMT - is 784111777 seconds after the epoch.");
    // ---------------------------------------------------------------
    {
        const proven_time_t ns = 1000000000;
        proven_byte_t d[PROVEN_HTTP_DATE_SIZE];
        PROVEN_TEST_ASSERT(proven_http_date_format(784111777 * ns, d) == PROVEN_OK && memcmp(d, "Sun, 06 Nov 1994 08:49:37 GMT", 29) == 0,
            "the RFC's example date", "");
        PROVEN_TEST_ASSERT(proven_http_date_format(0, d) == PROVEN_OK && memcmp(d, "Thu, 01 Jan 1970 00:00:00 GMT", 29) == 0, "the epoch was a Thursday", "");
        PROVEN_TEST_ASSERT(proven_http_date_format(1709251199 * ns, d) == PROVEN_OK && memcmp(d, "Thu, 29 Feb 2024 23:59:59 GMT", 29) == 0, "a leap day", "");
        PROVEN_TEST_ASSERT(proven_http_date_format(951782400 * ns, d) == PROVEN_OK && memcmp(d, "Tue, 29 Feb 2000 00:00:00 GMT", 29) == 0, "2000 was a leap year", "");
        PROVEN_TEST_ASSERT(proven_http_date_format(4107542400 * ns, d) == PROVEN_OK && memcmp(d, "Mon, 01 Mar 2100 00:00:00 GMT", 29) == 0, "2100 is not", "");
        PROVEN_TEST_ASSERT(proven_http_date_format(784111777 * ns + 999999999, d) == PROVEN_OK && memcmp(d, "Sun, 06 Nov 1994 08:49:37 GMT", 29) == 0,
            "a fraction of a second is dropped, not rounded up", "");
        PROVEN_TEST_ASSERT(proven_http_date_format(-1, d) == PROVEN_ERR_INVALID_ARG, "a time before the epoch has no HTTP date here", "");

        proven_time_t now = 1791504000 * ns;         /* 2026-10-09 */
        proven_time_t t = 0;
        PROVEN_TEST_ASSERT(proven_http_date_parse(sv("Sun, 06 Nov 1994 08:49:37 GMT"), now, &t) == PROVEN_OK && t == 784111777 * ns, "IMF-fixdate", "");
        PROVEN_TEST_ASSERT(proven_http_date_parse(sv("Sunday, 06-Nov-94 08:49:37 GMT"), now, &t) == PROVEN_OK && t == 784111777 * ns, "the RFC 850 form", "");
        PROVEN_TEST_ASSERT(proven_http_date_parse(sv("Sun Nov  6 08:49:37 1994"), now, &t) == PROVEN_OK && t == 784111777 * ns, "asctime, with its space-padded day", "");
        PROVEN_TEST_ASSERT(proven_http_date_parse(sv("Thu Feb 29 23:59:59 2024"), now, &t) == PROVEN_OK && t == 1709251199 * ns, "asctime with a two-digit day", "");

        /* A two-digit year more than fifty years ahead is in the past. From 2026: 76 is 2076, 77 is 1977. */
        PROVEN_TEST_ASSERT(proven_http_date_parse(sv("Thursday, 01-Jan-76 00:00:00 GMT"), now, &t) == PROVEN_ERR_INVALID_FORMAT,
            "1 January 2076 is a Wednesday, so \"Thursday\" there does not match - the year really was read as 2076", "");
        PROVEN_TEST_ASSERT(proven_http_date_parse(sv("Wednesday, 01-Jan-76 00:00:00 GMT"), now, &t) == PROVEN_OK && t == 3345062400 * ns, "76 is 2076 from 2026", "");
        PROVEN_TEST_ASSERT(proven_http_date_parse(sv("Saturday, 01-Jan-77 00:00:00 GMT"), now, &t) == PROVEN_OK && t == 220924800 * ns, "77 is 1977: 2077 would be 51 years ahead", "");

        static const char *bad[] = {
            "", "Sun, 06 Nov 1994 08:49:37", "Sun, 06 Nov 1994 08:49:37 UTC", "Sun, 06 Nov 1994 08:49:37 GMT ", " Sun, 06 Nov 1994 08:49:37 GMT",
            "sun, 06 Nov 1994 08:49:37 GMT", "Sun, 06 nov 1994 08:49:37 GMT", "Sun, 6 Nov 1994 08:49:37 GMT", "Sun, 06 Nov 94 08:49:37 GMT",
            "Mon, 06 Nov 1994 08:49:37 GMT",              /* it was a Sunday */
            "Fri, 30 Feb 2024 00:00:00 GMT", "Wed, 29 Feb 2023 00:00:00 GMT", "Sun, 31 Apr 2024 00:00:00 GMT", "Sun, 00 Nov 1994 08:49:37 GMT",
            "Sun, 06 Nov 1994 24:00:00 GMT", "Sun, 06 Nov 1994 08:60:00 GMT", "Sun, 06 Nov 1994 08:49:61 GMT", "Sun, 06 Nov 1994 8:49:37 GMT",
            "Sun, 06 Foo 1994 08:49:37 GMT", "Xyz, 06 Nov 1994 08:49:37 GMT", "Sun, 06 Nov 1969 08:49:37 GMT", "1994-11-06T08:49:37Z",
            "Sunday, 06-Nov-1994 08:49:37 GMT", "Sun Nov 6 08:49:37 1994", "Sun Nov  6 08:49:37 94", "Sunnyday, 06-Nov-94 08:49:37 GMT",
        };
        bool all = true;
        for (proven_size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
            if (proven_http_date_parse(sv(bad[i]), now, &t) != PROVEN_ERR_INVALID_FORMAT) { all = false; PROVEN_TEST_INFO("accepted: {}", PROVEN_ARG((const char *)bad[i])); }
        }
        PROVEN_TEST_ASSERT(all, "twenty-six texts that are not a valid HTTP date are PROVEN_ERR_INVALID_FORMAT", "");
        PROVEN_TEST_ASSERT(proven_http_date_parse(sv("Thu, 30 Jun 2016 23:59:60 GMT"), now, &t) == PROVEN_OK && t == 1467331200 * ns,
            "a leap second (:60) is accepted and read as the second after :59", "");

        /* Format then parse is the identity, for whole seconds across the whole range. */
        proven_xoshiro256ss_t g;
        proven_xoshiro256ss_seed(&g, 9110);
        proven_rng_t rng = proven_xoshiro256ss_rng(&g);
        bool round = true;
        for (int i = 0; i < 200000 && round; ++i) {
            proven_i64 sec = (proven_i64)proven_rng_below(rng, 9223372036ull + 1);
            if (i == 0) sec = 9223372036;                                    /* the last second a proven_time_t holds: 2262-04-11 */
            if (proven_http_date_format(sec * ns, d) != PROVEN_OK ||
                proven_http_date_parse((proven_u8str_view_t){ d, sizeof d }, now, &t) != PROVEN_OK || t != sec * ns) {
                round = false;
                PROVEN_TEST_INFO("round trip fails at second {}", PROVEN_ARG((proven_u64)sec));
            }
        }
        PROVEN_TEST_ASSERT(round, "two hundred thousand random moments from 1970 to 2262 - the whole range of the time type - format and parse back to themselves", "");
        PROVEN_TEST_ASSERT(proven_http_date_format(INT64_MAX, d) == PROVEN_OK && memcmp(d, "Fri, 11 Apr 2262 23:47:16 GMT", 29) == 0,
            "the last moment the time type holds", "");
        t = 7;
        PROVEN_TEST_ASSERT(proven_http_date_parse(sv("Fri, 11 Apr 2262 23:47:17 GMT"), now, &t) == PROVEN_ERR_OVERFLOW && t == 7,
            "one second later is a valid date the type cannot hold: PROVEN_ERR_OVERFLOW, and nothing is written", "");
        PROVEN_TEST_ASSERT(proven_http_date_parse(sv("Fri, 31 Dec 9999 23:59:59 GMT"), now, &t) == PROVEN_ERR_OVERFLOW,
            "so is the \"never expires\" date servers really send - told apart from a malformed one", "");
    }

    PROVEN_TEST_PASS("framing, bodies, writers and dates behave as specified.");
    return 0;
}
