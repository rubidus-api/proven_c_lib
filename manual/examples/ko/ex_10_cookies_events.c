#include "example.h"

/*
 * 클라이언트가 메시지와 메시지 사이에 간직하는 두 가지 상태: 서버가 심은 쿠키와,
 * 서버 전송 이벤트 스트림에서의 자기 위치. 어느 쪽도 소켓을 건드리지 않는다.
 */

static bool view_is(proven_u8str_view_t v, const char *text) {
    return proven_u8str_view_eq(v, proven_u8str_view_from_cstr(text));
}

int main(void) {
    // ---- 쿠키 보관함 ---------------------------------------------------------
    proven_http_cookie_jar_t jar;
    EXAMPLE_REQUIRE(proven_http_cookie_jar_init(&jar, proven_heap_allocator(), 32) == PROVEN_OK, "a jar for up to 32 cookies");
    proven_time_t now = proven_time_now();

    /* 응답의 Set-Cookie 헤더 하나하나를, 그것이 어디서 왔는지와 함께 저장한다. */
    proven_u8str_view_t host = PROVEN_LIT("shop.example.com");
    EXAMPLE_REQUIRE(proven_http_cookie_jar_store(&jar, host, PROVEN_LIT("/"), true, PROVEN_LIT("sid=abc123; Path=/; Secure; HttpOnly"), now) == PROVEN_OK, "a session cookie");
    EXAMPLE_REQUIRE(proven_http_cookie_jar_store(&jar, host, PROVEN_LIT("/cart/view"), true, PROVEN_LIT("items=3; Max-Age=3600"), now) == PROVEN_OK,
                    "one with no Path: it belongs to /cart");
    /* 남의 도메인을 위한 쿠키는 거절된다. 이 보관함은 브라우저보다 더 엄격해서, 모든 쿠키를
     * 그것을 심은 바로 그 호스트에만 돌려준다. */
    EXAMPLE_REQUIRE(proven_http_cookie_jar_store(&jar, host, PROVEN_LIT("/"), true, PROVEN_LIT("track=1; Domain=ads.example.net"), now) == PROVEN_ERR_PERMISSION,
                    "not this host's to set");
    EXAMPLE_REQUIRE(proven_http_cookie_jar_count(&jar) == 2, "two cookies");

    /* 요청을 보낼 때 보관함이 Cookie 헤더의 값을 써 준다. */
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

    /* 시간이 흐르면 Max-Age가 다한다. 서버는 쿠키를 만료시켜서 지운다. */
    proven_time_t later = now + (proven_time_t)7200 * 1000000000;
    EXAMPLE_REQUIRE(proven_http_cookie_jar_header(&jar, host, PROVEN_LIT("/cart"), true, later, out, &n) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ buf, n }, "sid=abc123"), "two hours on, items has expired");
    EXAMPLE_REQUIRE(proven_http_cookie_jar_store(&jar, host, PROVEN_LIT("/"), true, PROVEN_LIT("sid=; Path=/; Max-Age=0"), later) == PROVEN_OK &&
                    proven_http_cookie_jar_count(&jar) == 0, "logged out: the jar is empty");

    proven_http_cookie_jar_clear(&jar);       /* 또는 직접 비운다 */
    proven_http_cookie_jar_destroy(&jar);

    // ---- 서버 전송 이벤트 ---------------------------------------------------
    /* 파서에는 도착한 것을 도착한 조각 그대로 넣고, 파서는 온전한 이벤트를 돌려준다. */
    proven_byte_t work[512];
    proven_sse_t sse;
    EXAMPLE_REQUIRE(proven_sse_init(&sse, (proven_mem_mut_t){ work, sizeof work }) == PROVEN_OK, "a parser with room for 256-byte events");

    static const char *const arrived[] = {
        ": keep-alive\n\n",                              /* 주석 줄: 이벤트가 아니다 */
        "id: 41\nevent: price\ndata: {\"sym\":\"X\",",   /* 중간에서 끊긴 이벤트 */
        "\ndata:  \"p\":12.5}\n\n",                      /* ...그리고 그 끝 */
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
            if (!have) continue;           /* 전부 소비했고 아직 이벤트는 없다 */
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
    /* 연결이 끊긴 뒤 이것을 Last-Event-ID로 보내면 서버가 거기서부터 이어 줄 수 있다. */
    EXAMPLE_REQUIRE(view_is(proven_sse_last_id(&sse), "41"), "where to resume");

    return EXAMPLE_OK();
}
