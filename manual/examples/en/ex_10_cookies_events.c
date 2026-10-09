#include "example.h"

/*
 * Two pieces of state a client keeps between messages: the cookies a server set, and its
 * place in a stream of server-sent events. Neither touches a socket.
 */

static bool view_is(proven_u8str_view_t v, const char *text) {
    return proven_u8str_view_eq(v, proven_u8str_view_from_cstr(text));
}

int main(void) {
    // ---- A cookie jar -------------------------------------------------------
    proven_http_cookie_jar_t jar;
    EXAMPLE_REQUIRE(proven_http_cookie_jar_init(&jar, proven_heap_allocator(), 32) == PROVEN_OK, "a jar for up to 32 cookies");
    proven_time_t now = proven_time_now();

    /* Each Set-Cookie header of a response is stored with where it came from. */
    proven_u8str_view_t host = PROVEN_LIT("shop.example.com");
    EXAMPLE_REQUIRE(proven_http_cookie_jar_store(&jar, host, PROVEN_LIT("/"), true, PROVEN_LIT("sid=abc123; Path=/; Secure; HttpOnly"), now) == PROVEN_OK, "a session cookie");
    EXAMPLE_REQUIRE(proven_http_cookie_jar_store(&jar, host, PROVEN_LIT("/cart/view"), true, PROVEN_LIT("items=3; Max-Age=3600"), now) == PROVEN_OK,
                    "one with no Path: it belongs to /cart");
    /* A cookie for somebody else's domain is refused. This jar goes further than a browser
     * and keeps every cookie for the exact host that set it. */
    EXAMPLE_REQUIRE(proven_http_cookie_jar_store(&jar, host, PROVEN_LIT("/"), true, PROVEN_LIT("track=1; Domain=ads.example.net"), now) == PROVEN_ERR_PERMISSION,
                    "not this host's to set");
    EXAMPLE_REQUIRE(proven_http_cookie_jar_count(&jar) == 2, "two cookies");

    /* For a request, the jar writes the value of the Cookie header. */
    proven_byte_t buf[256];
    proven_mem_mut_t out = { buf, sizeof buf };
    proven_size_t n = 0;
    EXAMPLE_REQUIRE(proven_http_cookie_jar_header(&jar, host, PROVEN_LIT("/cart/checkout"), true, now, out, &n) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ buf, n }, "items=3; sid=abc123"), "under /cart: both, the longer path first");
    EXAMPLE_REQUIRE(proven_http_cookie_jar_header(&jar, host, PROVEN_LIT("/account"), true, now, out, &n) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ buf, n }, "sid=abc123"), "elsewhere: only the one for /");
    EXAMPLE_REQUIRE(proven_http_cookie_jar_header(&jar, host, PROVEN_LIT("/account"), false, now, out, &n) == PROVEN_OK && n == 0,
                    "over plain HTTP: nothing - sid is Secure. n == 0 means send no header");
    EXAMPLE_REQUIRE(proven_http_cookie_jar_header(&jar, PROVEN_LIT("example.com"), PROVEN_LIT("/"), true, now, out, &n) == PROVEN_OK && n == 0,
                    "another host: nothing");

    /* Time passes; Max-Age runs out. And a server deletes a cookie by expiring it. */
    proven_time_t later = now + (proven_time_t)7200 * 1000000000;
    EXAMPLE_REQUIRE(proven_http_cookie_jar_header(&jar, host, PROVEN_LIT("/cart"), true, later, out, &n) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ buf, n }, "sid=abc123"), "two hours on, items has expired");
    EXAMPLE_REQUIRE(proven_http_cookie_jar_store(&jar, host, PROVEN_LIT("/"), true, PROVEN_LIT("sid=; Path=/; Max-Age=0"), later) == PROVEN_OK &&
                    proven_http_cookie_jar_count(&jar) == 0, "logged out: the jar is empty");

    proven_http_cookie_jar_clear(&jar);       /* or empty it yourself */
    proven_http_cookie_jar_destroy(&jar);

    // ---- Server-sent events -------------------------------------------------
    /* The parser is fed whatever arrived, in whatever pieces, and hands back whole events. */
    proven_byte_t work[512];
    proven_sse_t sse;
    EXAMPLE_REQUIRE(proven_sse_init(&sse, (proven_mem_mut_t){ work, sizeof work }) == PROVEN_OK, "a parser with room for 256-byte events");

    static const char *const arrived[] = {
        ": keep-alive\n\n",                              /* a comment: no event */
        "id: 41\nevent: price\ndata: {\"sym\":\"X\",",   /* an event, cut in the middle */
        "\ndata:  \"p\":12.5}\n\n",                      /* ...and its end */
        "data: plain\n\n",
    };
    int events = 0;
    for (proven_size_t i = 0; i < 4; ++i) {
        proven_mem_view_t in = proven_mem_view_from_u8(proven_u8str_view_from_cstr(arrived[i]));
        while (in.size > 0) {
            proven_size_t used = 0;
            proven_sse_event_t ev;
            bool have = false;
            EXAMPLE_REQUIRE(proven_sse_feed(&sse, in, &used, &ev, &have) == PROVEN_OK, "the stream parses");
            in.ptr += used;
            in.size -= used;
            if (!have) continue;           /* everything consumed, no event yet */
            events++;
            if (events == 1) {
                EXAMPLE_REQUIRE(view_is(ev.event, "price") && view_is(ev.id, "41"), "a typed event with an id");
                EXAMPLE_REQUIRE(view_is(ev.data, "{\"sym\":\"X\",\n \"p\":12.5}"), "two data lines, joined by a line feed");
            } else {
                EXAMPLE_REQUIRE(ev.event.size == 0 && view_is(ev.data, "plain"), "no type given: a \"message\"");
                EXAMPLE_REQUIRE(view_is(ev.id, "41"), "the id stays until another replaces it");
            }
        }
    }
    EXAMPLE_REQUIRE(events == 2, "two events from four pieces");
    /* After a disconnect, send this as Last-Event-ID and the server can resume from there. */
    EXAMPLE_REQUIRE(view_is(proven_sse_last_id(&sse), "41"), "where to resume");

    return EXAMPLE_OK();
}
