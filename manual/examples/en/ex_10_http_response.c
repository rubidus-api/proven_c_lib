#include "example.h"

/*
 * Writing a response and reading one back.
 *
 * Each writer appends at `len` and moves it. A value that could break the message - a line
 * break in a header - is refused instead of written, so a string taken from a request cannot
 * be used to add a header of the sender's choosing.
 */

int main(void) {
    proven_byte_t msg[512];
    proven_mem_mut_t out = { msg, sizeof msg };
    proven_size_t len = 0;

    /* The date header: the wall clock, in the one form HTTP writes. */
    proven_byte_t date[PROVEN_HTTP_DATE_SIZE];
    proven_time_t now = proven_time_now();
    EXAMPLE_REQUIRE(proven_http_date_format(now, date) == PROVEN_OK, "now, as an HTTP date");

    /* A response whose length is not known in advance goes out in chunks. */
    proven_err_t err = proven_http_write_status_line(out, &len, 200, PROVEN_LIT(""));     /* "" : the standard phrase */
    if (err == PROVEN_OK) err = proven_http_write_header(out, &len, PROVEN_LIT("Date"), (proven_u8str_view_t){ date, sizeof date });
    if (err == PROVEN_OK) err = proven_http_write_header(out, &len, PROVEN_LIT("Content-Type"), PROVEN_LIT("text/plain; charset=utf-8"));
    if (err == PROVEN_OK) err = proven_http_write_header(out, &len, PROVEN_LIT("Transfer-Encoding"), PROVEN_LIT("chunked"));
    if (err == PROVEN_OK) err = proven_http_write_header_u64(out, &len, PROVEN_LIT("X-Request-Id"), 4711);
    if (err == PROVEN_OK) err = proven_http_write_head_end(out, &len);
    EXAMPLE_REQUIRE(err == PROVEN_OK, "the head is written");
    proven_size_t head_len = len;

    static const char *const parts[] = { "written in ", "two chunks" };
    for (proven_size_t i = 0; i < 2 && err == PROVEN_OK; ++i) {
        proven_size_t n = proven_cstr_len(parts[i]);
        err = proven_http_write_chunk_begin(out, &len, n);
        if (err != PROVEN_OK || n > sizeof msg - len) { err = PROVEN_ERR_OUT_OF_BOUNDS; break; }
        for (proven_size_t k = 0; k < n; ++k) msg[len + k] = (proven_byte_t)parts[i][k];   /* the chunk's data */
        len += n;
        err = proven_http_write_chunk_end(out, &len);
    }
    if (err == PROVEN_OK) err = proven_http_write_last_chunk(out, &len);
    EXAMPLE_REQUIRE(err == PROVEN_OK, "the body is written");

    /* A header value with a line break in it is not written at all. */
    proven_size_t before = len;
    EXAMPLE_REQUIRE(proven_http_write_header(out, &len, PROVEN_LIT("Location"), PROVEN_LIT("/home\r\nSet-Cookie: session=stolen")) == PROVEN_ERR_INVALID_ARG &&
                    len == before, "response splitting is refused, and nothing was appended");

    /* --- the client's side: the same bytes, read back ------------------------------- */

    proven_http_header_t fields[16];
    proven_http_response_t res;
    proven_size_t head_size = 0;
    EXAMPLE_REQUIRE(proven_http_parse_response((proven_mem_view_t){ msg, len }, fields, 16, 0, &res, &head_size) == PROVEN_OK &&
                    head_size == head_len, "the head parses, and ends where it was written to end");
    EXAMPLE_REQUIRE(res.status == 200 && proven_u8str_view_eq(res.reason, proven_http_reason_phrase(200)), "200 OK");
    EXAMPLE_REQUIRE(proven_http_response_keep_alive(&res), "an HTTP/1.1 response leaves the connection open");

    /* The body's framing depends on the request it answers: a response to HEAD has none. */
    proven_http_framing_t framing;
    EXAMPLE_REQUIRE(proven_http_response_framing(&res, PROVEN_HTTP_HEAD, &framing) == PROVEN_OK && framing.kind == PROVEN_HTTP_BODY_NONE,
                    "had this answered HEAD, no body would follow");
    EXAMPLE_REQUIRE(proven_http_response_framing(&res, PROVEN_HTTP_GET, &framing) == PROVEN_OK && framing.kind == PROVEN_HTTP_BODY_CHUNKED,
                    "it answers GET, and the body is chunked");

    proven_http_body_t body;
    proven_byte_t text[64];
    proven_size_t text_len = 0;
    proven_size_t pos = head_size;
    bool done = false;
    EXAMPLE_REQUIRE(proven_http_body_init(&body, framing, sizeof text) == PROVEN_OK, "accept at most what the buffer holds");
    while (!done && pos < len) {
        proven_size_t used = 0;
        proven_mem_view_t payload;
        if (proven_http_body_feed(&body, (proven_mem_view_t){ msg + pos, len - pos }, &used, &payload, &done) != PROVEN_OK) break;
        for (proven_size_t i = 0; i < payload.size; ++i) text[text_len + i] = payload.ptr[i];
        text_len += payload.size;
        pos += used;
    }
    EXAMPLE_REQUIRE(done && pos == len && proven_u8str_view_eq((proven_u8str_view_t){ text, text_len }, PROVEN_LIT("written in two chunks")),
                    "the body, decoded");

    /* The date comes back as the time it was made from, to the second. */
    proven_u8str_view_t date_text;
    proven_time_t parsed = 0;
    EXAMPLE_REQUIRE(proven_http_header_find(fields, res.header_count, PROVEN_LIT("Date"), &date_text) &&
                    proven_http_date_parse(date_text, now, &parsed) == PROVEN_OK && parsed == now - now % 1000000000,
                    "the Date header parses back to the same second");

    /* A request is written the same way. */
    len = 0;
    err = proven_http_write_request_line(out, &len, PROVEN_LIT("GET"), PROVEN_LIT("/search?q=a%20b"));
    if (err == PROVEN_OK) err = proven_http_write_header(out, &len, PROVEN_LIT("Host"), PROVEN_LIT("example.com"));
    if (err == PROVEN_OK) err = proven_http_write_head_end(out, &len);
    EXAMPLE_REQUIRE(err == PROVEN_OK && proven_u8str_view_eq((proven_u8str_view_t){ msg, len },
                    PROVEN_LIT("GET /search?q=a%20b HTTP/1.1\r\nHost: example.com\r\n\r\n")), "a request head, byte for byte");

    return EXAMPLE_OK();
}
