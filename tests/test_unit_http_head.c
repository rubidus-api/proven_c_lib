#include "proven.h"
#include "proven_test.h"
#include <stdlib.h>
#include <string.h>

/*
 * The HTTP/1.1 head parser: what it accepts, what it refuses, and the two properties callers
 * build on.
 *
 * Property 1 - every proper prefix of a valid head is PROVEN_ERR_NEED_MORE: never an error and
 * never a success. A reader may therefore always answer NEED_MORE by reading more.
 *
 * Property 2 - whatever the bytes, the parser stays inside them. Three hundred thousand mutated
 * heads are parsed from buffers of exactly their own size, so under AddressSanitizer one byte
 * read past the end is a failure; and every head that is accepted has its views inside the
 * buffer and its line endings intact.
 *
 * The refusal table is the request-smuggling catalogue: each row is a spelling that some
 * deployed parser accepts and another does not.
 */

#define CAP 32

static proven_mem_view_t mv(const char *s) {
    return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}

static bool veq(proven_u8str_view_t v, const char *s) {
    return v.size == strlen(s) && memcmp(v.ptr, s, v.size) == 0;
}

static proven_err_t req_err(const char *raw) {
    proven_http_header_t h[CAP];
    proven_http_request_t r;
    proven_size_t n = 0;
    return proven_http_parse_request(mv(raw), h, CAP, 0, &r, &n);
}

static proven_err_t res_err(const char *raw) {
    proven_http_header_t h[CAP];
    proven_http_response_t r;
    proven_size_t n = 0;
    return proven_http_parse_response(mv(raw), h, CAP, 0, &r, &n);
}

/* Every proper prefix is NEED_MORE; the whole is OK and its head ends where the text does. */
static bool prefixes_need_more(const char *raw, bool request) {
    proven_size_t len = strlen(raw);
    proven_http_header_t h[CAP];
    proven_size_t n = 0;
    for (proven_size_t cut = 0; cut < len; ++cut) {
        /* An exact-size copy: a read past the prefix is a read past an allocation. */
        proven_byte_t *copy = malloc(cut ? cut : 1);
        memcpy(copy, raw, cut);
        proven_err_t e;
        if (request) { proven_http_request_t r; e = proven_http_parse_request((proven_mem_view_t){ copy, cut }, h, CAP, 0, &r, &n); }
        else { proven_http_response_t r; e = proven_http_parse_response((proven_mem_view_t){ copy, cut }, h, CAP, 0, &r, &n); }
        free(copy);
        if (e != PROVEN_ERR_NEED_MORE) return false;
    }
    proven_err_t e;
    if (request) { proven_http_request_t r; e = proven_http_parse_request(mv(raw), h, CAP, 0, &r, &n); }
    else { proven_http_response_t r; e = proven_http_parse_response(mv(raw), h, CAP, 0, &r, &n); }
    return e == PROVEN_OK && n == len;
}

static const char *const valid_requests[] = {
    "GET / HTTP/1.1\r\n\r\n",
    "GET / HTTP/1.0\r\n\r\n",
    "GET /index.html HTTP/1.1\r\nHost: example.com\r\n\r\n",
    "POST /submit?x=1&y=2 HTTP/1.1\r\nHost: example.com\r\nContent-Length: 5\r\nContent-Type: text/plain\r\n\r\n",
    "OPTIONS * HTTP/1.1\r\nHost: h\r\n\r\n",
    "CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n",
    "GET http://example.com/a?b HTTP/1.1\r\nHost: example.com\r\n\r\n",
    "PROPFIND /dav HTTP/1.1\r\nHost: h\r\nDepth: 1\r\n\r\n",
    "GET / HTTP/1.1\r\nX-Empty:\r\nX-Spaces:   padded value \t \r\nX-Tab:\tv\r\n\r\n",
    "GET / HTTP/1.1\r\nX-Latin: caf\xe9\r\nX-Odd!#$%&'*+-.^_`|~: ok\r\n\r\n",
    "GET / HTTP/1.1\r\nSet-Cookie: a=1\r\nSet-Cookie: b=2\r\nset-cookie: c=3\r\n\r\n",
    "\r\nGET / HTTP/1.1\r\nHost: h\r\n\r\n",
    "\r\n\r\nGET / HTTP/1.1\r\n\r\n",
    "DELETE /a%20b/%E2%82%AC HTTP/1.1\r\nHost: h\r\n\r\n",
};

static const char *const valid_responses[] = {
    "HTTP/1.1 200 OK\r\n\r\n",
    "HTTP/1.0 404 Not Found\r\n\r\n",
    "HTTP/1.1 200 OK\r\nContent-Length: 12\r\nContent-Type: text/html; charset=utf-8\r\n\r\n",
    "HTTP/1.1 204 \r\n\r\n",
    "HTTP/1.1 204\r\n\r\n",
    "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n",
    "HTTP/1.1 599 Custom reason with \t tab and caf\xe9\r\nX: y\r\n\r\n",
    "HTTP/1.1 301 Moved Permanently\r\nLocation: https://example.com/\r\n\r\n",
};

int main(void) {
    PROVEN_TEST_SUITE("http: the head parser",
        "Requests and responses parse into views of the bytes given; malformed and ambiguous heads are refused; a prefix is always NEED_MORE.",
        "Inspect http_fields and the two parse functions in src/proven/http.c.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a request head, field by field",
        "Method, target, version and each header come back as written, with values trimmed of surrounding whitespace.",
        "");
    // ---------------------------------------------------------------
    {
        proven_http_header_t h[CAP];
        proven_http_request_t r;
        proven_size_t n = 0;
        const char *raw = "POST /submit?x=1&y=2 HTTP/1.1\r\nHost: example.com\r\nContent-Length: 5\r\nX-Pad:  \t two words \t \r\nX-Empty:\r\n\r\nhello";
        PROVEN_TEST_ASSERT(proven_http_parse_request(mv(raw), h, CAP, 0, &r, &n) == PROVEN_OK, "the head parses although the body follows it", "");
        PROVEN_TEST_ASSERT(n == strlen(raw) - 5 && memcmp(raw + n, "hello", 5) == 0, "head_size is where the body begins", "");
        PROVEN_TEST_ASSERT(r.method == PROVEN_HTTP_POST && veq(r.method_text, "POST") && veq(r.target, "/submit?x=1&y=2") && r.version_minor == 1,
            "method, target and version", "");
        PROVEN_TEST_ASSERT(r.header_count == 4 && r.headers == h &&
                           veq(h[0].name, "Host") && veq(h[0].value, "example.com") &&
                           veq(h[1].name, "Content-Length") && veq(h[1].value, "5") &&
                           veq(h[2].name, "X-Pad") && veq(h[2].value, "two words") &&
                           veq(h[3].name, "X-Empty") && veq(h[3].value, ""),
            "four headers, in order, the padded value trimmed and the empty one empty", "");
        PROVEN_TEST_ASSERT((const proven_byte_t *)raw <= r.target.ptr && r.target.ptr + r.target.size <= (const proven_byte_t *)raw + n,
            "views point into the buffer that was parsed: nothing is copied", "");

        PROVEN_TEST_ASSERT(proven_http_parse_request(mv("PROPFIND /dav HTTP/1.0\r\n\r\n"), h, CAP, 0, &r, &n) == PROVEN_OK &&
                           r.method == PROVEN_HTTP_METHOD_OTHER && veq(r.method_text, "PROPFIND") && r.version_minor == 0 && r.header_count == 0,
            "a method the enum has no name for is OTHER, with its text kept; HTTP/1.0 is minor 0", "");
        PROVEN_TEST_ASSERT(proven_http_parse_request(mv("\r\n\r\nGET /x HTTP/1.1\r\n\r\n"), h, CAP, 0, &r, &n) == PROVEN_OK &&
                           n == 23 && veq(r.target, "/x"),
            "empty lines before the request line are skipped and counted in head_size", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("every prefix of a valid head is NEED_MORE",
        "Fourteen requests and eight responses, cut at every byte: need more, need more, ..., then OK at exactly the full length.",
        "A prefix that is an error makes a server answer 400 to a request that was merely slow to arrive.");
    // ---------------------------------------------------------------
    {
        bool all = true;
        for (proven_size_t i = 0; i < sizeof valid_requests / sizeof valid_requests[0]; ++i) {
            if (!prefixes_need_more(valid_requests[i], true)) { all = false; PROVEN_TEST_INFO("request {} breaks the prefix property", PROVEN_ARG((int)i)); }
        }
        PROVEN_TEST_ASSERT(all, "holds for every valid request", "");
        all = true;
        for (proven_size_t i = 0; i < sizeof valid_responses / sizeof valid_responses[0]; ++i) {
            if (!prefixes_need_more(valid_responses[i], false)) { all = false; PROVEN_TEST_INFO("response {} breaks the prefix property", PROVEN_ARG((int)i)); }
        }
        PROVEN_TEST_ASSERT(all, "and for every valid response", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("requests that are refused",
        "Each row is malformed, or is read differently by different parsers - which is worse.",
        "PROVEN_ERR_INVALID_FORMAT means answer 400. Nothing here is repaired.");
    // ---------------------------------------------------------------
    {
        static const struct { const char *raw; const char *why; } bad[] = {
            { "GET / HTTP/1.1\n\n", "bare LF line endings" },
            { "GET / HTTP/1.1\r\nHost: h\n\r\n", "a bare LF ending one header line" },
            { "GET / HTTP/1.1\r\nHost: h\r\r\n", "a bare CR inside a line" },
            { "GET / HTTP/1.1\r\nHost : h\r\n\r\n", "a space between the header name and the colon" },
            { "GET / HTTP/1.1\r\nHost\t: h\r\n\r\n", "a tab between the header name and the colon" },
            { "GET / HTTP/1.1\r\nHost: h\r\n continued\r\n\r\n", "obsolete line folding with a space" },
            { "GET / HTTP/1.1\r\nHost: h\r\n\tcontinued\r\n\r\n", "obsolete line folding with a tab" },
            { "GET / HTTP/1.1\r\n Host: h\r\n\r\n", "whitespace before the first header name" },
            { "GET / HTTP/1.1\r\nHost h\r\n\r\n", "a header line with no colon" },
            { "GET / HTTP/1.1\r\n: value\r\n\r\n", "an empty header name" },
            { "GET / HTTP/1.1\r\nBad Name: v\r\n\r\n", "a space inside a header name" },
            { "GET / HTTP/1.1\r\nBad(Name): v\r\n\r\n", "a delimiter inside a header name" },
            { "GET / HTTP/1.1\r\nX: a\x01" "b\r\n\r\n", "a control character in a value" },
            { "GET / HTTP/1.1\r\nX: a\x7f" "b\r\n\r\n", "DEL in a value" },
            { "GET  / HTTP/1.1\r\n\r\n", "two spaces after the method" },
            { "GET /  HTTP/1.1\r\n\r\n", "two spaces after the target" },
            { "GET / HTTP/1.1 \r\n\r\n", "a space after the version" },
            { "GET\t/ HTTP/1.1\r\n\r\n", "a tab as the separator" },
            { " GET / HTTP/1.1\r\n\r\n", "a space before the method" },
            { "GET HTTP/1.1\r\n\r\n", "no target" },
            { "GET /\r\n\r\n", "no version (HTTP/0.9 style)" },
            { "GET / http/1.1\r\n\r\n", "a lowercase protocol name" },
            { "GET / HTTP/1.1x\r\n\r\n", "junk after the version" },
            { "GET / HTTP/11\r\n\r\n", "a version with no dot" },
            { "GET / HTTP/1.\r\n\r\n", "a version with no minor number" },
            { "GET / HTTPS/1.1\r\n\r\n", "a protocol that is not HTTP" },
            { "G@T / HTTP/1.1\r\n\r\n", "a method that is not a token" },
            { "GET /a\x01" "b HTTP/1.1\r\n\r\n", "a control character in the target" },
            { "GET /caf\xc3\xa9 HTTP/1.1\r\n\r\n", "bytes above ASCII in the target" },
            { "\nGET / HTTP/1.1\r\n\r\n", "a bare LF before the request line" },
            { "\r\rGET / HTTP/1.1\r\n\r\n", "a bare CR before the request line" },
        };
        bool all = true;
        for (proven_size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
            if (req_err(bad[i].raw) != PROVEN_ERR_INVALID_FORMAT) { all = false; PROVEN_TEST_INFO("not refused: {}", PROVEN_ARG((const char *)bad[i].why)); }
        }
        PROVEN_TEST_ASSERT(all, "all thirty-one malformed requests are PROVEN_ERR_INVALID_FORMAT", "");

        /* A NUL cannot sit in a C string literal table, so it gets its own case. */
        const proven_byte_t nul_value[] = "GET / HTTP/1.1\r\nX: a\0b\r\n\r\n";
        proven_http_header_t h[CAP];
        proven_http_request_t r;
        proven_size_t n = 0;
        PROVEN_TEST_ASSERT(proven_http_parse_request((proven_mem_view_t){ nul_value, sizeof nul_value - 1 }, h, CAP, 0, &r, &n) == PROVEN_ERR_INVALID_FORMAT,
            "a NUL byte in a header value is refused", "");

        PROVEN_TEST_ASSERT(req_err("GET / HTTP/2.0\r\n\r\n") == PROVEN_ERR_UNSUPPORTED &&
                           req_err("GET / HTTP/0.9\r\n\r\n") == PROVEN_ERR_UNSUPPORTED &&
                           req_err("GET / HTTP/1.2\r\n\r\n") == PROVEN_ERR_UNSUPPORTED &&
                           req_err("GET / HTTP/3.0\r\n\r\n") == PROVEN_ERR_UNSUPPORTED,
            "a well-formed version that is not 1.0 or 1.1 is PROVEN_ERR_UNSUPPORTED (505), not malformed", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("limits",
        "A head larger than the limit is refused as soon as the limit is reached - while it is still incomplete - and so is one field too many.",
        "Without this an endless header line would be read for ever, one NEED_MORE at a time.");
    // ---------------------------------------------------------------
    {
        proven_http_header_t h[CAP];
        proven_http_request_t r;
        proven_size_t n = 0;
        enum { LIMIT = 256 };
        proven_byte_t buf[LIMIT + 64];
        const char *start = "GET / HTTP/1.1\r\nX-Endless: ";
        proven_size_t sl = strlen(start);
        memcpy(buf, start, sl);
        memset(buf + sl, 'a', sizeof buf - sl);

        PROVEN_TEST_ASSERT(proven_http_parse_request((proven_mem_view_t){ buf, LIMIT - 1 }, h, CAP, LIMIT, &r, &n) == PROVEN_ERR_NEED_MORE,
            "one byte under the limit with no end yet: NEED_MORE", "");
        PROVEN_TEST_ASSERT(proven_http_parse_request((proven_mem_view_t){ buf, LIMIT }, h, CAP, LIMIT, &r, &n) == PROVEN_ERR_OUT_OF_BOUNDS,
            "at the limit with no end: PROVEN_ERR_OUT_OF_BOUNDS, decided without waiting for more", "");
        PROVEN_TEST_ASSERT(proven_http_parse_request((proven_mem_view_t){ buf, sizeof buf }, h, CAP, LIMIT, &r, &n) == PROVEN_ERR_OUT_OF_BOUNDS,
            "past the limit: the same", "");

        /* A head of exactly the limit is accepted; one byte longer is not. */
        proven_size_t value = LIMIT - sl - 4;
        memcpy(buf + sl + value, "\r\n\r\n", 4);
        PROVEN_TEST_ASSERT(proven_http_parse_request((proven_mem_view_t){ buf, LIMIT }, h, CAP, LIMIT, &r, &n) == PROVEN_OK && n == LIMIT,
            "a head of exactly the limit parses", "");
        memset(buf + sl, 'a', sizeof buf - sl);
        memcpy(buf + sl + value + 1, "\r\n\r\n", 4);
        PROVEN_TEST_ASSERT(proven_http_parse_request((proven_mem_view_t){ buf, LIMIT + 1 }, h, CAP, LIMIT, &r, &n) == PROVEN_ERR_OUT_OF_BOUNDS,
            "one byte more does not", "");

        const char *three = "GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n";
        PROVEN_TEST_ASSERT(proven_http_parse_request(mv(three), h, 3, 0, &r, &n) == PROVEN_OK && r.header_count == 3,
            "three fields fit an array of three", "");
        PROVEN_TEST_ASSERT(proven_http_parse_request(mv(three), h, 2, 0, &r, &n) == PROVEN_ERR_OUT_OF_BOUNDS,
            "and do not fit an array of two: PROVEN_ERR_OUT_OF_BOUNDS", "");
        PROVEN_TEST_ASSERT(proven_http_parse_request(mv("GET / HTTP/1.1\r\n\r\n"), NULL, 0, 0, &r, &n) == PROVEN_OK,
            "a head with no fields needs no array", "");

        /* The default limit when 0 is passed. */
        static proven_byte_t big[PROVEN_HTTP_DEFAULT_MAX_HEAD + 8];
        memcpy(big, start, sl);
        memset(big + sl, 'a', sizeof big - sl);
        PROVEN_TEST_ASSERT(proven_http_parse_request((proven_mem_view_t){ big, PROVEN_HTTP_DEFAULT_MAX_HEAD - 1 }, h, CAP, 0, &r, &n) == PROVEN_ERR_NEED_MORE &&
                           proven_http_parse_request((proven_mem_view_t){ big, PROVEN_HTTP_DEFAULT_MAX_HEAD }, h, CAP, 0, &r, &n) == PROVEN_ERR_OUT_OF_BOUNDS,
            "a limit of 0 means PROVEN_HTTP_DEFAULT_MAX_HEAD", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("response heads",
        "A status line with or without a reason phrase; the same field rules as a request.",
        "");
    // ---------------------------------------------------------------
    {
        proven_http_header_t h[CAP];
        proven_http_response_t r;
        proven_size_t n = 0;
        PROVEN_TEST_ASSERT(proven_http_parse_response(mv("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"), h, CAP, 0, &r, &n) == PROVEN_OK &&
                           r.status == 404 && veq(r.reason, "Not Found") && r.version_minor == 1 && r.header_count == 1 && veq(h[0].name, "Content-Length"),
            "status, reason, version and a header", "");
        PROVEN_TEST_ASSERT(proven_http_parse_response(mv("HTTP/1.1 204\r\n\r\n"), h, CAP, 0, &r, &n) == PROVEN_OK && r.status == 204 && r.reason.size == 0,
            "a status line with no reason phrase and no space", "");
        PROVEN_TEST_ASSERT(proven_http_parse_response(mv("HTTP/1.1 204 \r\n\r\n"), h, CAP, 0, &r, &n) == PROVEN_OK && r.reason.size == 0,
            "and one with the space and an empty phrase", "");

        static const char *bad[] = {
            "HTTP/1.1 20 OK\r\n\r\n", "HTTP/1.1 2000 OK\r\n\r\n", "HTTP/1.1 abc OK\r\n\r\n", "HTTP/1.1  200 OK\r\n\r\n",
            "HTTP/1.1\t200 OK\r\n\r\n", "HTTP/1.1 099 Low\r\n\r\n", "HTTP/1.1 200 OK\n\n", "http/1.1 200 OK\r\n\r\n",
            "HTTP/1.1 200 OK\r\nBad : v\r\n\r\n", "HTTP/1.1 200 OK\r\n folded\r\n\r\n", "HTTP/1.1 200 O\x01K\r\n\r\n",
            "ICY 200 OK\r\n\r\n", "200 OK\r\n\r\n", " HTTP/1.1 200 OK\r\n\r\n",
        };
        bool all = true;
        for (proven_size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
            if (res_err(bad[i]) != PROVEN_ERR_INVALID_FORMAT) { all = false; PROVEN_TEST_INFO("not refused: response {}", PROVEN_ARG((int)i)); }
        }
        PROVEN_TEST_ASSERT(all, "all fourteen malformed responses are PROVEN_ERR_INVALID_FORMAT", "");
        PROVEN_TEST_ASSERT(res_err("HTTP/2.0 200 OK\r\n\r\n") == PROVEN_ERR_UNSUPPORTED, "an HTTP/2.0 status line is PROVEN_ERR_UNSUPPORTED", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("looking fields up",
        "Names compare without case; a repeated field is counted; a token is found inside a list.",
        "");
    // ---------------------------------------------------------------
    {
        proven_http_header_t h[CAP];
        proven_http_request_t r;
        proven_size_t n = 0;
        const char *raw = "GET / HTTP/1.1\r\nHost: h\r\nSet-Cookie: a=1\r\nCONNECTION: keep-alive , Upgrade\r\nset-cookie: b=2\r\nConnection:close\r\nX-List: ,a,, b ,\r\n\r\n";
        PROVEN_TEST_ASSERT(proven_http_parse_request(mv(raw), h, CAP, 0, &r, &n) == PROVEN_OK, "a head with repeated fields", "");
        proven_u8str_view_t v;
        PROVEN_TEST_ASSERT(proven_http_header_find(h, r.header_count, PROVEN_LIT("host"), &v) && veq(v, "h") &&
                           proven_http_header_find(h, r.header_count, PROVEN_LIT("HOST"), NULL),
            "find ignores the case of the name, and the value is optional", "");
        PROVEN_TEST_ASSERT(proven_http_header_find(h, r.header_count, PROVEN_LIT("Set-Cookie"), &v) && veq(v, "a=1"),
            "find returns the FIRST field of that name", "");
        PROVEN_TEST_ASSERT(!proven_http_header_find(h, r.header_count, PROVEN_LIT("Cookie"), &v) &&
                           !proven_http_header_find(h, r.header_count, PROVEN_LIT("Hos"), &v),
            "a name that is absent, or only a prefix of one that is present, is not found", "");
        PROVEN_TEST_ASSERT(proven_http_header_count(h, r.header_count, PROVEN_LIT("set-cookie")) == 2 &&
                           proven_http_header_count(h, r.header_count, PROVEN_LIT("Connection")) == 2 &&
                           proven_http_header_count(h, r.header_count, PROVEN_LIT("Nope")) == 0,
            "count sees every repetition", "");
        PROVEN_TEST_ASSERT(proven_http_header_has_token(h, r.header_count, PROVEN_LIT("Connection"), PROVEN_LIT("upgrade")) &&
                           proven_http_header_has_token(h, r.header_count, PROVEN_LIT("connection"), PROVEN_LIT("CLOSE")) &&
                           proven_http_header_has_token(h, r.header_count, PROVEN_LIT("Connection"), PROVEN_LIT("keep-alive")) &&
                           !proven_http_header_has_token(h, r.header_count, PROVEN_LIT("Connection"), PROVEN_LIT("keep")) &&
                           !proven_http_header_has_token(h, r.header_count, PROVEN_LIT("Connection"), PROVEN_LIT("upgrade2")),
            "a token is found across both Connection fields, without case, and only as a whole element", "");
        PROVEN_TEST_ASSERT(proven_http_header_has_token(h, r.header_count, PROVEN_LIT("X-List"), PROVEN_LIT("a")) &&
                           proven_http_header_has_token(h, r.header_count, PROVEN_LIT("X-List"), PROVEN_LIT("b")),
            "empty list elements are skipped over", "");

        PROVEN_TEST_ASSERT(proven_http_method_from_text(PROVEN_LIT("GET")) == PROVEN_HTTP_GET &&
                           proven_http_method_from_text(PROVEN_LIT("get")) == PROVEN_HTTP_METHOD_OTHER &&
                           proven_http_method_from_text(PROVEN_LIT("PATCH")) == PROVEN_HTTP_PATCH &&
                           veq(proven_http_method_text(PROVEN_HTTP_DELETE), "DELETE") &&
                           proven_http_method_text(PROVEN_HTTP_METHOD_OTHER).size == 0,
            "methods map both ways, and are case-sensitive", "");
        PROVEN_TEST_ASSERT(veq(proven_http_reason_phrase(200), "OK") && veq(proven_http_reason_phrase(404), "Not Found") &&
                           veq(proven_http_reason_phrase(431), "Request Header Fields Too Large") && veq(proven_http_reason_phrase(799), "Unknown"),
            "reason phrases", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("does the connection stay open",
        "HTTP/1.1 persists unless told to close; HTTP/1.0 closes unless told to keep alive.",
        "");
    // ---------------------------------------------------------------
    {
        static const struct { const char *raw; bool keep; } q[] = {
            { "GET / HTTP/1.1\r\n\r\n", true },
            { "GET / HTTP/1.1\r\nConnection: close\r\n\r\n", false },
            { "GET / HTTP/1.1\r\nConnection: Keep-Alive, Close\r\n\r\n", false },
            { "GET / HTTP/1.1\r\nConnection: keep-alive\r\n\r\n", true },
            { "GET / HTTP/1.0\r\n\r\n", false },
            { "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", true },
            { "GET / HTTP/1.0\r\nConnection: close\r\n\r\n", false },
            { "GET / HTTP/1.1\r\nConnection: closed\r\n\r\n", true },
        };
        bool all = true;
        for (proven_size_t i = 0; i < sizeof q / sizeof q[0]; ++i) {
            proven_http_header_t h[CAP];
            proven_http_request_t r;
            proven_size_t n = 0;
            if (proven_http_parse_request(mv(q[i].raw), h, CAP, 0, &r, &n) != PROVEN_OK || proven_http_request_keep_alive(&r) != q[i].keep) all = false;
        }
        PROVEN_TEST_ASSERT(all, "all eight request cases", "");
        proven_http_header_t h[CAP];
        proven_http_response_t r;
        proven_size_t n = 0;
        PROVEN_TEST_ASSERT(proven_http_parse_response(mv("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n"), h, CAP, 0, &r, &n) == PROVEN_OK &&
                           !proven_http_response_keep_alive(&r) &&
                           proven_http_parse_response(mv("HTTP/1.0 200 OK\r\n\r\n"), h, CAP, 0, &r, &n) == PROVEN_OK &&
                           !proven_http_response_keep_alive(&r) &&
                           proven_http_parse_response(mv("HTTP/1.1 200 OK\r\n\r\n"), h, CAP, 0, &r, &n) == PROVEN_OK &&
                           proven_http_response_keep_alive(&r),
            "and the same rule for responses", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("no bytes can lead the parser astray",
        "Three hundred thousand mutated heads, each parsed from a buffer of exactly its own size.",
        "Run under ASan this catches a read past the end. Every accepted head must have its views inside the buffer and every CR paired with an LF.");
    // ---------------------------------------------------------------
    {
        proven_xoshiro256ss_t g;
        proven_xoshiro256ss_seed(&g, 9112);
        proven_rng_t rng = proven_xoshiro256ss_rng(&g);
        static const proven_byte_t interesting[] = { '\r', '\n', ' ', '\t', ':', 0, 0x7f, 0x80, 0xff, '/', '1', ',', 'H' };
        proven_size_t accepted = 0, violations = 0;
        for (int iter = 0; iter < 300000; ++iter) {
            bool request = proven_rng_below(rng, 2) == 0;
            const char *base = request ? valid_requests[proven_rng_below(rng, sizeof valid_requests / sizeof valid_requests[0])]
                                       : valid_responses[proven_rng_below(rng, sizeof valid_responses / sizeof valid_responses[0])];
            proven_size_t len = strlen(base);
            proven_byte_t work[256];
            memcpy(work, base, len);
            proven_size_t edits = 1 + (proven_size_t)proven_rng_below(rng, 3);
            for (proven_size_t k = 0; k < edits && len > 0; ++k) {
                proven_size_t at = (proven_size_t)proven_rng_below(rng, len);
                proven_byte_t b = proven_rng_below(rng, 2) ? interesting[proven_rng_below(rng, sizeof interesting)] : (proven_byte_t)proven_rng_below(rng, 256);
                switch (proven_rng_below(rng, 4)) {
                    case 0: work[at] = b; break;                                              /* replace */
                    case 1: memmove(work + at, work + at + 1, len - at - 1); len--; break;    /* delete */
                    case 2: if (len < sizeof work - 1) { memmove(work + at + 1, work + at, len - at); work[at] = b; len++; } break;  /* insert */
                    default: len = at; break;                                                 /* truncate */
                }
            }
            proven_byte_t *exact = malloc(len ? len : 1);
            memcpy(exact, work, len);
            proven_http_header_t h[CAP];
            proven_size_t n = 0;
            proven_err_t e;
            proven_u8str_view_t first = {0};
            proven_size_t count = 0;
            if (request) {
                proven_http_request_t r;
                e = proven_http_parse_request((proven_mem_view_t){ exact, len }, h, CAP, 0, &r, &n);
                if (e == PROVEN_OK) { first = r.target; count = r.header_count; }
            } else {
                proven_http_response_t r;
                e = proven_http_parse_response((proven_mem_view_t){ exact, len }, h, CAP, 0, &r, &n);
                if (e == PROVEN_OK) { first = r.reason; count = r.header_count; }
            }
            bool known = e == PROVEN_OK || e == PROVEN_ERR_NEED_MORE || e == PROVEN_ERR_INVALID_FORMAT ||
                         e == PROVEN_ERR_UNSUPPORTED || e == PROVEN_ERR_OUT_OF_BOUNDS;
            if (!known) violations++;
            if (e == PROVEN_OK) {
                accepted++;
                bool ok = n <= len && n >= 4 && memcmp(exact + n - 4, "\r\n\r\n", 4) == 0;
                ok = ok && first.ptr >= exact && first.ptr + first.size <= exact + n;
                for (proven_size_t i = 0; i < count && ok; ++i) {
                    ok = h[i].name.ptr >= exact && h[i].name.ptr + h[i].name.size <= exact + n &&
                         h[i].value.ptr >= exact && h[i].value.ptr + h[i].value.size <= exact + n && h[i].name.size > 0;
                }
                /* Inside an accepted head no CR stands without its LF and no LF without its CR. */
                for (proven_size_t i = 0; i < n && ok; ++i) {
                    if (exact[i] == '\r' && (i + 1 >= n || exact[i + 1] != '\n')) ok = false;
                    if (exact[i] == '\n' && (i == 0 || exact[i - 1] != '\r')) ok = false;
                    if (exact[i] == 0) ok = false;
                }
                if (!ok) violations++;
            }
            free(exact);
        }
        PROVEN_TEST_ASSERT(violations == 0, "no mutated head produced an unknown error, an out-of-buffer view, or an accepted head with a broken line ending", "");
        PROVEN_TEST_ASSERT(accepted > 1000, "and enough mutations were still valid heads for the acceptance checks to have run", "");
    }

    PROVEN_TEST_PASS("the head parser accepts what is specified and refuses the rest.");
    return 0;
}
