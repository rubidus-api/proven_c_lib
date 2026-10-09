#include "proven.h"
#include "proven_test.h"
#include <stdio.h>
#include <string.h>

/*
 * The cookie jar: what it stores, what it gives back, and - the part that is security - what
 * it refuses to store and to whom it refuses to give.
 *
 * Every cookie here is host-only (see http_cookie.h for why). The cases below fix that rule
 * from both sides: a parent-domain cookie is kept but never leaves the host that set it, and a
 * cookie for a domain the server is not part of is not kept at all.
 */

static proven_u8str_view_t sv(const char *s) {
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}

static const proven_time_t NS = 1000000000;
static const proven_time_t NOW = 1791504000 * (proven_time_t)1000000000;      /* 2026-10-09 00:00:00 GMT */

static proven_err_t store(proven_http_cookie_jar_t *jar, const char *host, const char *path, bool secure, const char *value) {
    return proven_http_cookie_jar_store(jar, sv(host), sv(path), secure, sv(value), NOW);
}

/* The Cookie header for a request, or NULL-like "!" on error. */
static const char *header(proven_http_cookie_jar_t *jar, const char *host, const char *path, bool secure, proven_time_t now) {
    static char buf[512];
    proven_size_t n = 0;
    if (proven_http_cookie_jar_header(jar, sv(host), sv(path), secure, now, (proven_mem_mut_t){ (proven_byte_t *)buf, sizeof buf - 1 }, &n) != PROVEN_OK) return "!";
    buf[n] = '\0';
    return buf;
}

int main(void) {
    PROVEN_TEST_SUITE("http: the cookie jar",
        "Cookies are stored per host and path, expire, are replaced and evicted - and never cross to another host.",
        "Inspect src/proven/http_cookie.c.");

    proven_allocator_t heap = proven_heap_allocator();
    proven_http_cookie_jar_t jar;
    PROVEN_TEST_ASSERT(proven_http_cookie_jar_init(&jar, heap, 8) == PROVEN_OK && proven_http_cookie_jar_count(&jar) == 0, "an empty jar for eight cookies", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("store and return",
        "A cookie goes back to the host that set it, for the paths it covers.",
        "");
    // ---------------------------------------------------------------
    {
        PROVEN_TEST_ASSERT(store(&jar, "example.com", "/", false, "sid=abc123") == PROVEN_OK, "a plain cookie is stored", "");
        PROVEN_TEST_ASSERT(strcmp(header(&jar, "example.com", "/", false, NOW), "sid=abc123") == 0, "and returned to the same host", "");
        PROVEN_TEST_ASSERT(strcmp(header(&jar, "EXAMPLE.com", "/any/path", false, NOW), "sid=abc123") == 0, "host names compare without case; a cookie for / covers every path", "");
        PROVEN_TEST_ASSERT(strcmp(header(&jar, "other.com", "/", false, NOW), "") == 0, "another host gets nothing", "");

        PROVEN_TEST_ASSERT(store(&jar, "example.com", "/", false, "  theme = dark ; Path=/app; HttpOnly; SameSite=Lax") == PROVEN_OK, "whitespace around name and value is trimmed; unknown attributes are ignored", "");
        PROVEN_TEST_ASSERT(strcmp(header(&jar, "example.com", "/app/page", false, NOW), "theme=dark; sid=abc123") == 0,
            "under /app both apply, and the cookie with the longer path comes first", "");
        PROVEN_TEST_ASSERT(strcmp(header(&jar, "example.com", "/app", false, NOW), "theme=dark; sid=abc123") == 0, "/app itself is covered", "");
        PROVEN_TEST_ASSERT(strcmp(header(&jar, "example.com", "/application", false, NOW), "sid=abc123") == 0,
            "/application is NOT under /app: a path matches at a segment boundary", "");
        PROVEN_TEST_ASSERT(strcmp(header(&jar, "example.com", "/", false, NOW), "sid=abc123") == 0, "above /app the path cookie is not sent", "");

        PROVEN_TEST_ASSERT(store(&jar, "example.com", "/docs/guide/page.html", false, "tab=2") == PROVEN_OK &&
                           strcmp(header(&jar, "example.com", "/docs/guide/other", false, NOW), "tab=2; sid=abc123") == 0 &&
                           strcmp(header(&jar, "example.com", "/docs", false, NOW), "sid=abc123") == 0,
            "with no Path attribute the cookie belongs to the directory of the request that set it", "");
        PROVEN_TEST_ASSERT(store(&jar, "example.com", "/", false, "sid=replaced") == PROVEN_OK && proven_http_cookie_jar_count(&jar) == 3 &&
                           strcmp(header(&jar, "example.com", "/", false, NOW), "sid=replaced") == 0,
            "the same name, host and path replaces the earlier cookie", "");
        PROVEN_TEST_ASSERT(store(&jar, "example.com", "/", false, "q=\"quoted value\"") == PROVEN_OK &&
                           strstr(header(&jar, "example.com", "/", false, NOW), "q=\"quoted value\"") != NULL,
            "a quoted value is kept with its quotes, as browsers keep it", "");
        PROVEN_TEST_ASSERT(store(&jar, "example.com", "/", false, "empty=") == PROVEN_OK && strstr(header(&jar, "example.com", "/", false, NOW), "empty=") != NULL,
            "an empty value is a value", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("what is refused, and what never leaves its host",
        "A cookie for a domain the server is not part of, a Secure cookie over plain HTTP, and text that is not a cookie.",
        "Without a public-suffix list a Domain attribute cannot be honoured safely, so it never widens where a cookie goes.");
    // ---------------------------------------------------------------
    {
        proven_http_cookie_jar_clear(&jar);
        PROVEN_TEST_ASSERT(proven_http_cookie_jar_count(&jar) == 0, "clear empties the jar", "");

        proven_size_t before = proven_http_cookie_jar_count(&jar);
        PROVEN_TEST_ASSERT(store(&jar, "www.example.com", "/", false, "a=1; Domain=evil.test") == PROVEN_ERR_PERMISSION &&
                           store(&jar, "www.example.com", "/", false, "a=1; Domain=ample.com") == PROVEN_ERR_PERMISSION &&
                           store(&jar, "www.example.com", "/", false, "a=1; Domain=sub.www.example.com") == PROVEN_ERR_PERMISSION &&
                           store(&jar, "example.com", "/", false, "a=1; Domain=com.evil") == PROVEN_ERR_PERMISSION &&
                           proven_http_cookie_jar_count(&jar) == before,
            "a Domain that is unrelated, merely a text suffix, or a child of the setting host is PROVEN_ERR_PERMISSION and stores nothing", "");

        PROVEN_TEST_ASSERT(store(&jar, "www.example.com", "/", false, "wide=1; Domain=example.com") == PROVEN_OK &&
                           store(&jar, "www.example.com", "/", false, "dot=1; Domain=.EXAMPLE.com") == PROVEN_OK,
            "a Domain that is a parent of the setting host is accepted", "");
        PROVEN_TEST_ASSERT(strstr(header(&jar, "www.example.com", "/", false, NOW), "wide=1") != NULL &&
                           strcmp(header(&jar, "example.com", "/", false, NOW), "") == 0 &&
                           strcmp(header(&jar, "api.example.com", "/", false, NOW), "") == 0 &&
                           strcmp(header(&jar, "evil-example.com", "/", false, NOW), "") == 0,
            "and still goes back only to the host that set it - not to the parent, not to a sibling", "");

        PROVEN_TEST_ASSERT(store(&jar, "example.com", "/", false, "s=1; Secure") == PROVEN_ERR_PERMISSION, "a Secure cookie set over plain HTTP is refused", "");
        PROVEN_TEST_ASSERT(store(&jar, "example.com", "/", true, "s=1; Secure") == PROVEN_OK &&
                           strcmp(header(&jar, "example.com", "/", true, NOW), "s=1") == 0 &&
                           strcmp(header(&jar, "example.com", "/", false, NOW), "") == 0,
            "set over an encrypted connection it is stored, and is only ever sent over one", "");

        before = proven_http_cookie_jar_count(&jar);
        PROVEN_TEST_ASSERT(store(&jar, "example.com", "/", false, "novalue") == PROVEN_ERR_INVALID_FORMAT &&
                           store(&jar, "example.com", "/", false, "=onlyvalue") == PROVEN_ERR_INVALID_FORMAT &&
                           store(&jar, "example.com", "/", false, "") == PROVEN_ERR_INVALID_FORMAT &&
                           store(&jar, "example.com", "/", false, "a=b\r\nSet-Cookie: c=d") == PROVEN_ERR_INVALID_FORMAT &&
                           store(&jar, "example.com", "/", false, "; Path=/") == PROVEN_ERR_INVALID_FORMAT &&
                           proven_http_cookie_jar_count(&jar) == before,
            "no '=', an empty name, nothing at all, and a control character are PROVEN_ERR_INVALID_FORMAT, with the jar unchanged", "");

        static char big[PROVEN_HTTP_COOKIE_MAX_SIZE + 8];
        memset(big, 'x', sizeof big);
        big[0] = 'k'; big[1] = '=';
        big[PROVEN_HTTP_COOKIE_MAX_SIZE + 1] = '\0';                 /* name 1 + value 4095 = 4096 */
        PROVEN_TEST_ASSERT(store(&jar, "example.com", "/", false, big) == PROVEN_OK, "name plus value of exactly the limit is stored", "");
        big[PROVEN_HTTP_COOKIE_MAX_SIZE + 1] = 'x';
        big[PROVEN_HTTP_COOKIE_MAX_SIZE + 2] = '\0';
        PROVEN_TEST_ASSERT(store(&jar, "example.com", "/", false, big) == PROVEN_ERR_OUT_OF_BOUNDS, "one byte more is PROVEN_ERR_OUT_OF_BOUNDS", "");
        char small[64];
        proven_size_t n = 0;
        PROVEN_TEST_ASSERT(proven_http_cookie_jar_header(&jar, sv("example.com"), sv("/"), false, NOW, (proven_mem_mut_t){ (proven_byte_t *)small, sizeof small }, &n) == PROVEN_ERR_OUT_OF_BOUNDS && n == 0,
            "a header buffer too small for the cookies is PROVEN_ERR_OUT_OF_BOUNDS", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("expiry and deletion",
        "Max-Age and Expires bound a cookie's life; an already-expired cookie deletes the one it names.",
        "");
    // ---------------------------------------------------------------
    {
        proven_http_cookie_jar_clear(&jar);
        PROVEN_TEST_ASSERT(store(&jar, "h.test", "/", false, "short=1; Max-Age=60") == PROVEN_OK &&
                           store(&jar, "h.test", "/", false, "dated=1; Expires=Fri, 09 Oct 2026 01:00:00 GMT") == PROVEN_OK &&
                           store(&jar, "h.test", "/", false, "session=1") == PROVEN_OK &&
                           store(&jar, "h.test", "/", false, "forever=1; Expires=Fri, 31 Dec 9999 23:59:59 GMT") == PROVEN_OK &&
                           store(&jar, "h.test", "/", false, "both=1; Expires=Thu, 01 Jan 2026 00:00:00 GMT; Max-Age=3600") == PROVEN_OK,
            "cookies with a Max-Age, an Expires, neither, a date past the end of the clock, and both", "");
        PROVEN_TEST_ASSERT(strcmp(header(&jar, "h.test", "/", false, NOW + 59 * NS), "short=1; dated=1; session=1; forever=1; both=1") == 0,
            "59 seconds later all five are sent, in the order they were set - Max-Age won over the past Expires", "");
        PROVEN_TEST_ASSERT(strcmp(header(&jar, "h.test", "/", false, NOW + 60 * NS), "dated=1; session=1; forever=1; both=1") == 0,
            "at 60 seconds the Max-Age=60 cookie is gone", "");
        PROVEN_TEST_ASSERT(proven_http_cookie_jar_count(&jar) == 4, "and it was dropped from the jar, not merely skipped", "");
        PROVEN_TEST_ASSERT(strcmp(header(&jar, "h.test", "/", false, NOW + 3600 * NS), "session=1; forever=1") == 0,
            "an hour later the dated cookies are gone; the session cookie and the one dated 9999 remain", "");

        PROVEN_TEST_ASSERT(store(&jar, "h.test", "/", false, "session=; Max-Age=0") == PROVEN_OK &&
                           strcmp(header(&jar, "h.test", "/", false, NOW), "forever=1") == 0,
            "Max-Age=0 deletes the cookie of that name", "");
        PROVEN_TEST_ASSERT(store(&jar, "h.test", "/", false, "forever=; Expires=Thu, 01 Jan 1970 00:00:01 GMT") == PROVEN_OK &&
                           store(&jar, "h.test", "/", false, "never-was=; Max-Age=-1") == PROVEN_OK &&
                           proven_http_cookie_jar_count(&jar) == 0,
            "so does an Expires in the past; deleting a cookie that is not there is not an error", "");
        PROVEN_TEST_ASSERT(store(&jar, "h.test", "/", false, "odd=1; Expires=not a date; Max-Age=soon") == PROVEN_OK &&
                           strcmp(header(&jar, "h.test", "/", false, NOW + 1000000 * NS), "odd=1") == 0,
            "an Expires or Max-Age that cannot be read is ignored, leaving a session cookie", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a full jar",
        "The ninth cookie into a jar of eight pushes out the oldest; replacing one does not.",
        "");
    // ---------------------------------------------------------------
    {
        proven_http_cookie_jar_clear(&jar);
        char name[16];
        for (int i = 0; i < 8; ++i) {
            snprintf(name, sizeof name, "c%d=%d", i, i);
            PROVEN_TEST_ASSERT(store(&jar, "full.test", "/", false, name) == PROVEN_OK, "fill the jar", "");
        }
        PROVEN_TEST_ASSERT(store(&jar, "full.test", "/", false, "c3=again") == PROVEN_OK && proven_http_cookie_jar_count(&jar) == 8 &&
                           strstr(header(&jar, "full.test", "/", false, NOW), "c0=0") != NULL,
            "replacing an existing cookie in a full jar evicts nothing", "");
        PROVEN_TEST_ASSERT(store(&jar, "full.test", "/", false, "new=1") == PROVEN_OK && proven_http_cookie_jar_count(&jar) == 8, "a ninth cookie still leaves eight", "");
        const char *h = header(&jar, "full.test", "/", false, NOW);
        PROVEN_TEST_ASSERT(strstr(h, "c0=0") == NULL && strstr(h, "c1=1") != NULL && strstr(h, "new=1") != NULL && strstr(h, "c3=again") != NULL,
            "the oldest one (c0) made room; the replaced one counts as new", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("allocation failure and lifetime",
        "A store that cannot allocate changes nothing; destroy frees everything.",
        "Run under ASan this section is also the leak check for the whole test.");
    // ---------------------------------------------------------------
    {
        proven_http_cookie_jar_t bad;
        proven_allocator_t none = {0};
        PROVEN_TEST_ASSERT(proven_http_cookie_jar_init(&bad, none, 4) == PROVEN_ERR_INVALID_ARG &&
                           proven_http_cookie_jar_init(&bad, heap, 0) == PROVEN_ERR_INVALID_ARG,
            "an invalid allocator and a limit of zero are refused", "");
        proven_http_cookie_jar_destroy(&bad);                         /* safe on a zeroed jar */

        static proven_byte_t mem[512];
        proven_arena_t arena = proven_arena_create((proven_mem_mut_t){ mem, sizeof mem });
        proven_http_cookie_jar_t tiny;
        PROVEN_TEST_ASSERT(proven_http_cookie_jar_init(&tiny, proven_arena_as_allocator(&arena), 4) == PROVEN_OK, "a jar on a 512-byte arena", "");
        static char huge[1024];
        memset(huge, 'v', sizeof huge);
        memcpy(huge, "k=", 2);
        huge[sizeof huge - 1] = '\0';
        PROVEN_TEST_ASSERT(store(&tiny, "a.test", "/", false, "ok=1") == PROVEN_OK &&
                           store(&tiny, "a.test", "/", false, huge) == PROVEN_ERR_NOMEM &&
                           proven_http_cookie_jar_count(&tiny) == 1 && strcmp(header(&tiny, "a.test", "/", false, NOW), "ok=1") == 0,
            "a cookie the allocator cannot hold is PROVEN_ERR_NOMEM, and the jar keeps what it had", "");
        proven_http_cookie_jar_destroy(&tiny);
        proven_http_cookie_jar_destroy(&jar);
        PROVEN_TEST_ASSERT(proven_http_cookie_jar_count(&jar) == 0, "destroy leaves a zeroed jar", "");
    }

    PROVEN_TEST_PASS("the cookie jar stores, returns, expires and refuses as specified.");
    return 0;
}
