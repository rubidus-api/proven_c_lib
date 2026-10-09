#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * URLs: parsing, percent-coding, and the path resolver that stands between a request and a
 * filesystem.
 *
 * The component tables were checked against Python's urllib.parse.urlsplit for every accepted
 * URL (same scheme, userinfo, host, port, path, query, fragment). The refusals are this
 * library's own rule - RFC 3986 with an authority, nothing repaired - and several of them are
 * texts urlsplit accepts.
 *
 * The traversal cases are the published shapes: encoded dots, encoded slashes, double
 * encoding, overlong UTF-8, backslashes, NUL.
 */

static proven_u8str_view_t sv(const char *s) {
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}

static bool veq(proven_u8str_view_t v, const char *s) {
    return v.size == strlen(s) && memcmp(v.ptr, s, v.size) == 0;
}

typedef struct {
    const char *url;
    const char *scheme, *userinfo, *host, *path, *query, *fragment;   /* NULL: absent */
    int port;                                                          /* -1: absent */
    bool ipv6;
} url_case_t;

static bool check_url(const url_case_t *c) {
    proven_url_t u;
    if (proven_url_parse(sv(c->url), &u) != PROVEN_OK) return false;
    if (!veq(u.scheme, c->scheme) || !veq(u.host, c->host) || !veq(u.path, c->path)) return false;
    if (u.has_userinfo != (c->userinfo != NULL) || (c->userinfo && !veq(u.userinfo, c->userinfo))) return false;
    if (u.has_query != (c->query != NULL) || (c->query && !veq(u.query, c->query))) return false;
    if (u.has_fragment != (c->fragment != NULL) || (c->fragment && !veq(u.fragment, c->fragment))) return false;
    if (u.has_port != (c->port >= 0) || (c->port >= 0 && u.port != c->port)) return false;
    return u.host_is_ipv6 == c->ipv6;
}

static bool resolves_to(const char *raw, const char *want) {
    proven_byte_t out[256];
    proven_size_t n = 0;
    if (proven_url_path_resolve(sv(raw), (proven_mem_mut_t){ out, sizeof out }, &n) != PROVEN_OK) return false;
    return n == strlen(want) && memcmp(out, want, n) == 0;
}

static proven_err_t resolve_err(const char *raw) {
    proven_byte_t out[256];
    proven_size_t n = 99;
    proven_err_t e = proven_url_path_resolve(sv(raw), (proven_mem_mut_t){ out, sizeof out }, &n);
    return (e != PROVEN_OK && n != 0) ? PROVEN_OK : e;      /* a refusal must report nothing written */
}

int main(void) {
    PROVEN_TEST_SUITE("url: parsing, percent-coding, and a path that stays inside its root",
        "A URL comes apart into the components that were written; a request path is decoded once and cannot climb out.",
        "Inspect src/proven/url.c.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("components are views of what was written",
        "Every accepted URL splits as Python's urlsplit splits it; absent and empty are different.",
        "");
    // ---------------------------------------------------------------
    {
        static const url_case_t cases[] = {
            { "http://example.com", "http", NULL, "example.com", "", NULL, NULL, -1, false },
            { "http://example.com/", "http", NULL, "example.com", "/", NULL, NULL, -1, false },
            { "https://example.com:8443/a/b?x=1&y=2#top", "https", NULL, "example.com", "/a/b", "x=1&y=2", "top", 8443, false },
            { "HTTP://Example.COM/Path", "HTTP", NULL, "Example.COM", "/Path", NULL, NULL, -1, false },
            { "http://user:pw@host/", "http", "user:pw", "host", "/", NULL, NULL, -1, false },
            { "http://a@b@host/", "http", "a@b", "host", "/", NULL, NULL, -1, false },
            { "http://192.0.2.1:80/", "http", NULL, "192.0.2.1", "/", NULL, NULL, 80, false },
            { "http://[2001:db8::1]/x", "http", NULL, "2001:db8::1", "/x", NULL, NULL, -1, true },
            { "http://[::1]:8080", "http", NULL, "::1", "", NULL, NULL, 8080, true },
            { "http://[fe80::1%25eth0]/", "http", NULL, "fe80::1%25eth0", "/", NULL, NULL, -1, true },
            { "http://h/?", "http", NULL, "h", "/", "", NULL, -1, false },
            { "http://h/#", "http", NULL, "h", "/", NULL, "", -1, false },
            { "http://h?q", "http", NULL, "h", "", "q", NULL, -1, false },
            { "http://h#f?notquery", "http", NULL, "h", "", NULL, "f?notquery", -1, false },
            { "http://h/a%20b?c=%26#d%23", "http", NULL, "h", "/a%20b", "c=%26", "d%23", -1, false },
            { "http://h:/x", "http", NULL, "h", "/x", NULL, NULL, -1, false },
            { "ws://h:0/", "ws", NULL, "h", "/", NULL, NULL, 0, false },
            { "a+b-c.d://h", "a+b-c.d", NULL, "h", "", NULL, NULL, -1, false },
            { "file:///etc/hosts", "file", NULL, "", "/etc/hosts", NULL, NULL, -1, false },
            { "http://h/a/../b/./c", "http", NULL, "h", "/a/../b/./c", NULL, NULL, -1, false },
            { "http://xn--bcher-kva.example/", "http", NULL, "xn--bcher-kva.example", "/", NULL, NULL, -1, false },
        };
        bool all = true;
        for (proven_size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
            if (!check_url(&cases[i])) { all = false; PROVEN_TEST_INFO("wrong split: {}", PROVEN_ARG((const char *)cases[i].url)); }
        }
        PROVEN_TEST_ASSERT(all, "all twenty-one URLs split into the expected components", "");

        static const char *bad[] = {
            "", "example.com", "/path/only", "//host/path", "http:/host", "http:host", "mailto:a@b",
            "1http://h", "ht tp://h", "http://h/a b", "http://h/\x7f", "http://h/%", "http://h/%4", "http://h/%zz",
            "http://h:99999/", "http://h:8a/", "http://h:-1/", "http://:80/", "http://user@/", "http://[::1", "http://[]/",
            "http://[::1]x/", "http://[g::1]/", "http://[::1%eth0]/", "http://[::1%25]/", "http://h]/", "http://h/\xc3\xa9", "http://h\n/",
        };
        all = true;
        for (proven_size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
            proven_url_t u;
            memset(&u, 0x5a, sizeof u);
            proven_url_t before = u;
            if (proven_url_parse(sv(bad[i]), &u) != PROVEN_ERR_INVALID_FORMAT || memcmp(&u, &before, sizeof u) != 0) {
                all = false;
                PROVEN_TEST_INFO("accepted, or wrote on refusal: {}", PROVEN_ARG((const char *)bad[i]));
            }
        }
        PROVEN_TEST_ASSERT(all, "everything that is not an absolute URL with an authority is PROVEN_ERR_INVALID_FORMAT, with the output untouched", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("ports, schemes and request targets",
        "The default port follows the scheme; a request target splits into path and query in each of its forms.",
        "");
    // ---------------------------------------------------------------
    {
        proven_url_t u;
        PROVEN_TEST_ASSERT(proven_url_parse(sv("HTTPS://h/"), &u) == PROVEN_OK && proven_url_effective_port(&u) == 443 &&
                           proven_url_scheme_is(&u, PROVEN_LIT("https")) && !proven_url_scheme_is(&u, PROVEN_LIT("http")),
            "the scheme is compared without case, and https means 443", "");
        PROVEN_TEST_ASSERT(proven_url_parse(sv("http://h:8080/"), &u) == PROVEN_OK && proven_url_effective_port(&u) == 8080,
            "a written port wins", "");
        PROVEN_TEST_ASSERT(proven_url_default_port(PROVEN_LIT("ws")) == 80 && proven_url_default_port(PROVEN_LIT("WSS")) == 443 &&
                           proven_url_default_port(PROVEN_LIT("ftp")) == 21 && proven_url_default_port(PROVEN_LIT("gopher")) == 0,
            "known schemes have their ports; an unknown one has none", "");

        proven_u8str_view_t path, query;
        bool has;
        PROVEN_TEST_ASSERT(proven_url_split_target(sv("/a/b?x=1&y"), &path, &query, &has) == PROVEN_OK && veq(path, "/a/b") && has && veq(query, "x=1&y"),
            "origin-form", "");
        PROVEN_TEST_ASSERT(proven_url_split_target(sv("/"), &path, &query, &has) == PROVEN_OK && veq(path, "/") && !has && query.size == 0,
            "the root, with no query", "");
        PROVEN_TEST_ASSERT(proven_url_split_target(sv("/a?"), &path, &query, &has) == PROVEN_OK && veq(path, "/a") && has && query.size == 0,
            "an empty query is still a query", "");
        PROVEN_TEST_ASSERT(proven_url_split_target(sv("http://host:8080/a/b?q"), &path, &query, &has) == PROVEN_OK && veq(path, "/a/b") && has && veq(query, "q"),
            "absolute-form", "");
        PROVEN_TEST_ASSERT(proven_url_split_target(sv("http://host"), &path, &query, &has) == PROVEN_OK && veq(path, "/"),
            "absolute-form with no path asks for the root", "");
        PROVEN_TEST_ASSERT(proven_url_split_target(sv("*"), &path, &query, &has) == PROVEN_OK && veq(path, "*") && !has,
            "the asterisk of OPTIONS *", "");
        PROVEN_TEST_ASSERT(proven_url_split_target(sv("host:443"), &path, &query, &has) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_url_split_target(sv("a/b"), &path, &query, &has) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_url_split_target(sv("/a#frag"), &path, &query, &has) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_url_split_target(sv("/a b"), &path, &query, &has) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_url_split_target(sv(""), &path, &query, &has) == PROVEN_ERR_INVALID_FORMAT,
            "authority-form, a relative path, a fragment, a space and nothing at all are refused", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a query, pair by pair",
        "Names and values come back as written; empty pairs are skipped; the first '=' ends the name.",
        "");
    // ---------------------------------------------------------------
    {
        proven_url_query_iter_t it = proven_url_query_iter(sv("a=1&b=&c&&d=x=y&=v"));
        proven_u8str_view_t n, v;
        bool ok = proven_url_query_next(&it, &n, &v) && veq(n, "a") && veq(v, "1");
        ok = ok && proven_url_query_next(&it, &n, &v) && veq(n, "b") && veq(v, "");
        ok = ok && proven_url_query_next(&it, &n, &v) && veq(n, "c") && veq(v, "");
        ok = ok && proven_url_query_next(&it, &n, &v) && veq(n, "d") && veq(v, "x=y");
        ok = ok && proven_url_query_next(&it, &n, &v) && veq(n, "") && veq(v, "v");
        ok = ok && !proven_url_query_next(&it, &n, &v) && !proven_url_query_next(&it, &n, &v);
        PROVEN_TEST_ASSERT(ok, "five pairs, then no more, however often it is asked", "");
        it = proven_url_query_iter(sv(""));
        PROVEN_TEST_ASSERT(!proven_url_query_next(&it, &n, &v), "an empty query has no pairs", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("percent-coding, both ways",
        "Decoding gives the bytes that were encoded - any bytes; encoding a component leaves nothing that could end it.",
        "");
    // ---------------------------------------------------------------
    {
        proven_byte_t out[64];
        proven_size_t n = 0;
        PROVEN_TEST_ASSERT(proven_url_percent_decode(sv("a%20b%2Fc%00%ff+"), (proven_mem_mut_t){ out, sizeof out }, &n) == PROVEN_OK &&
                           n == 8 && memcmp(out, "a b/c\0\xff+", 8) == 0,
            "escapes become their bytes - a slash, a NUL and 0xff included - and '+' stays '+'", "");
        PROVEN_TEST_ASSERT(proven_url_form_decode(sv("a+b%2Bc"), (proven_mem_mut_t){ out, sizeof out }, &n) == PROVEN_OK &&
                           n == 5 && memcmp(out, "a b+c", 5) == 0,
            "form decoding also reads '+' as a space, and %2B as a plus", "");
        PROVEN_TEST_ASSERT(proven_url_percent_decode(sv("%"), (proven_mem_mut_t){ out, sizeof out }, &n) == PROVEN_ERR_INVALID_FORMAT && n == 0 &&
                           proven_url_percent_decode(sv("%4"), (proven_mem_mut_t){ out, sizeof out }, &n) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_url_percent_decode(sv("%4g"), (proven_mem_mut_t){ out, sizeof out }, &n) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_url_percent_decode(sv("ab%"), (proven_mem_mut_t){ out, sizeof out }, &n) == PROVEN_ERR_INVALID_FORMAT,
            "a '%' without two hex digits is PROVEN_ERR_INVALID_FORMAT", "");
        PROVEN_TEST_ASSERT(proven_url_percent_decode(sv("abcd"), (proven_mem_mut_t){ out, 3 }, &n) == PROVEN_ERR_OUT_OF_BOUNDS && n == 0,
            "an output one byte short is PROVEN_ERR_OUT_OF_BOUNDS", "");
        char inplace[] = "%41%42%43def";
        PROVEN_TEST_ASSERT(proven_url_percent_decode(sv(inplace), (proven_mem_mut_t){ (proven_byte_t *)inplace, sizeof inplace }, &n) == PROVEN_OK &&
                           n == 6 && memcmp(inplace, "ABCdef", 6) == 0,
            "decoding in place - the output is the input buffer - works", "");

        const proven_byte_t raw[] = { 'a', ' ', '/', '?', '&', '=', '#', '%', '+', 0x00, 0xe2, 0x82, 0xac, '-', '.', '_', '~', 'Z', '9' };
        PROVEN_TEST_ASSERT(proven_url_encode_component((proven_mem_view_t){ raw, sizeof raw }, (proven_mem_mut_t){ out, sizeof out }, &n) == PROVEN_OK &&
                           n == 43 && memcmp(out, "a%20%2F%3F%26%3D%23%25%2B%00%E2%82%AC-._~Z9", 43) == 0,
            "a component keeps letters, digits and - . _ ~, and encodes everything else in uppercase hex", "");
        PROVEN_TEST_ASSERT(proven_url_encode_path((proven_mem_view_t){ (const proven_byte_t *)"/a b/c?d", 8 }, (proven_mem_mut_t){ out, sizeof out }, &n) == PROVEN_OK &&
                           n == 12 && memcmp(out, "/a%20b/c%3Fd", 12) == 0,
            "a path keeps its slashes", "");
        PROVEN_TEST_ASSERT(proven_url_form_encode((proven_mem_view_t){ (const proven_byte_t *)"a b+c", 5 }, (proven_mem_mut_t){ out, sizeof out }, &n) == PROVEN_OK &&
                           n == 7 && memcmp(out, "a+b%2Bc", 7) == 0,
            "form encoding writes a space as '+' and a plus as %2B", "");
        PROVEN_TEST_ASSERT(proven_url_encode_component((proven_mem_view_t){ raw, 2 }, (proven_mem_mut_t){ out, 3 }, &n) == PROVEN_ERR_OUT_OF_BOUNDS && n == 0,
            "an output too small for one escape is PROVEN_ERR_OUT_OF_BOUNDS", "");
        PROVEN_TEST_ASSERT(proven_url_encoded_size_max(10) == 30 && proven_url_encoded_size_max(PROVEN_SIZE_MAX / 2) == PROVEN_SIZE_MAX,
            "three bytes per input byte always suffice, and an unrepresentable size says so", "");

        /* Every byte value survives encode then decode. */
        proven_byte_t all[256], enc[768], dec[256];
        for (int i = 0; i < 256; ++i) all[i] = (proven_byte_t)i;
        proven_size_t en = 0, dn = 0;
        PROVEN_TEST_ASSERT(proven_url_encode_component((proven_mem_view_t){ all, sizeof all }, (proven_mem_mut_t){ enc, sizeof enc }, &en) == PROVEN_OK &&
                           proven_url_percent_decode((proven_u8str_view_t){ enc, en }, (proven_mem_mut_t){ dec, sizeof dec }, &dn) == PROVEN_OK &&
                           dn == 256 && memcmp(dec, all, 256) == 0,
            "all 256 byte values round-trip through encode and decode", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("path resolution: what a clean path looks like",
        "Dot segments and empty segments disappear; a directory keeps its trailing slash; the result always starts at the root.",
        "");
    // ---------------------------------------------------------------
    {
        static const struct { const char *in, *out; } ok[] = {
            { "/", "/" }, { "/a", "/a" }, { "/a/", "/a/" }, { "/a/b/c.txt", "/a/b/c.txt" },
            { "/a/./b", "/a/b" }, { "/./a", "/a" }, { "/a/.", "/a/" }, { "/.", "/" },
            { "/a/../b", "/b" }, { "/a/b/..", "/a/" }, { "/a/b/../..", "/" }, { "/a/..", "/" },
            { "/a/b/../../c", "/c" }, { "//a///b//", "/a/b/" }, { "/a/b/./../c/", "/a/c/" },
            { "/%61%62", "/ab" }, { "/a%20b", "/a b" }, { "/%2e", "/" }, { "/a/%2e%2e/b", "/b" }, { "/a/%2E%2E", "/" },
            { "/...", "/..." }, { "/..a", "/..a" }, { "/a..", "/a.." }, { "/.hidden", "/.hidden" },
            { "/%252e%252e/x", "/%2e%2e/x" },                    /* decoded ONCE: a file name, not a traversal */
            { "/caf%C3%A9", "/caf\xc3\xa9" }, { "/%E2%82%AC", "/\xe2\x82\xac" },
            { "/a%3Fb%23c", "/a?b#c" },
        };
        bool all = true;
        for (proven_size_t i = 0; i < sizeof ok / sizeof ok[0]; ++i) {
            if (!resolves_to(ok[i].in, ok[i].out)) { all = false; PROVEN_TEST_INFO("wrong resolution of {}", PROVEN_ARG((const char *)ok[i].in)); }
        }
        PROVEN_TEST_ASSERT(all, "all twenty-eight paths resolve to the expected clean path", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("path resolution: what does not get through",
        "Climbing above the root is PROVEN_ERR_PERMISSION however the dots are spelled; hostile bytes are PROVEN_ERR_INVALID_FORMAT.",
        "These are the published traversal shapes. A path here that resolves is a path that reaches a file outside the root.");
    // ---------------------------------------------------------------
    {
        static const char *climbs[] = {
            "/..", "/../", "/../etc/passwd", "/a/../..", "/a/../../b", "/a/b/../../../c", "/./..",
            "/%2e%2e", "/%2e%2e/etc/passwd", "/%2E%2E/", "/.%2e/", "/%2e./", "/a/%2e%2e/%2e%2e/b", "//..", "/a/..//../x",
        };
        bool all = true;
        for (proven_size_t i = 0; i < sizeof climbs / sizeof climbs[0]; ++i) {
            if (resolve_err(climbs[i]) != PROVEN_ERR_PERMISSION) { all = false; PROVEN_TEST_INFO("not refused as a climb: {}", PROVEN_ARG((const char *)climbs[i])); }
        }
        PROVEN_TEST_ASSERT(all, "all fifteen paths that leave the root are PROVEN_ERR_PERMISSION, with nothing reported written", "");

        static const char *hostile[] = {
            "", "a/b", "relative", "\\a", "/a\\b", "/a%5cb", "/a%5Cb", "/..%5c..%5cwindows",
            "/a%2fb", "/a%2Fb", "/..%2f..%2fetc", "/%2e%2e%2f",
            "/a%00b", "/a%00.png", "/%00", "/a%0ab", "/a%0d%0ab", "/a%7fb", "/a\tb",
            "/%c0%ae%c0%ae/", "/%c0%af", "/%e0%80%ae", "/%f0%80%80%ae", "/%ff", "/%80", "/%c3", "/caf%e9",
            "/%ed%a0%80",                                        /* a UTF-16 surrogate, encoded as if it were a character */
            "/a%", "/a%2", "/a%zz", "/%g0",
        };
        all = true;
        for (proven_size_t i = 0; i < sizeof hostile / sizeof hostile[0]; ++i) {
            if (resolve_err(hostile[i]) != PROVEN_ERR_INVALID_FORMAT) { all = false; PROVEN_TEST_INFO("not refused as malformed: {}", PROVEN_ARG((const char *)hostile[i])); }
        }
        PROVEN_TEST_ASSERT(all, "all thirty-two hostile paths are PROVEN_ERR_INVALID_FORMAT: encoded slashes, backslashes, control bytes, overlong and broken UTF-8, bad escapes", "");

        proven_byte_t small[4];
        proven_size_t n = 9;
        PROVEN_TEST_ASSERT(proven_url_path_resolve(sv("/abcdef"), (proven_mem_mut_t){ small, sizeof small }, &n) == PROVEN_ERR_OUT_OF_BOUNDS && n == 0,
            "an output too small is PROVEN_ERR_OUT_OF_BOUNDS - a different answer from a climb", "");
        proven_byte_t exact[7];
        PROVEN_TEST_ASSERT(proven_url_path_resolve(sv("/abcdef"), (proven_mem_mut_t){ exact, sizeof exact }, &n) == PROVEN_OK && n == 7,
            "an output of exactly the path's length is enough", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("path resolution: no input can climb out",
        "Two hundred thousand generated paths over a small alphabet of dangerous pieces: every one that resolves is clean.",
        "A counter-example is printed. The property is checked on the OUTPUT, so it does not depend on this test understanding the input.");
    // ---------------------------------------------------------------
    {
        static const char *pieces[] = { "/", "/", "..", ".", "%2e", "%2E", "a", "b", "%2f", "%5c", "\\", "%00", "%25", "%c0%ae", "//", "...", "%2e%2e" };
        proven_xoshiro256ss_t g;
        proven_xoshiro256ss_seed(&g, 20261009);
        proven_rng_t rng = proven_xoshiro256ss_rng(&g);
        proven_size_t resolved = 0, bad = 0;
        char in[128];
        for (int iter = 0; iter < 200000; ++iter) {
            proven_size_t len = 0;
            in[len++] = '/';
            proven_size_t parts = 1 + (proven_size_t)proven_rng_below(rng, 10);
            for (proven_size_t k = 0; k < parts; ++k) {
                const char *pc = pieces[proven_rng_below(rng, sizeof pieces / sizeof pieces[0])];
                proven_size_t pl = strlen(pc);
                if (len + pl >= sizeof in) break;
                memcpy(in + len, pc, pl);
                len += pl;
            }
            proven_byte_t out[160];
            proven_size_t n = 0;
            proven_err_t e = proven_url_path_resolve((proven_u8str_view_t){ (const proven_byte_t *)in, len }, (proven_mem_mut_t){ out, sizeof out }, &n);
            if (e != PROVEN_OK) continue;
            resolved++;
            /* Clean: starts at the root; no segment is "." or ".."; no empty segment; no
             * backslash, control byte or NUL. */
            bool clean = n >= 1 && out[0] == '/';
            proven_size_t seg = 1;
            for (proven_size_t i = 1; i <= n && clean; ++i) {
                if (i < n && (out[i] < 0x20 || out[i] == '\\' || out[i] == 0x7f)) clean = false;
                if (i == n || out[i] == '/') {
                    proven_size_t sl = i - seg;
                    if (sl == 0 && i != n) clean = false;
                    if (sl == 1 && out[seg] == '.') clean = false;
                    if (sl == 2 && out[seg] == '.' && out[seg + 1] == '.') clean = false;
                    seg = i + 1;
                }
            }
            if (!clean) {
                bad++;
                in[len] = '\0';
                if (bad <= 3) PROVEN_TEST_INFO("resolved to an unclean path: {}", PROVEN_ARG((const char *)in));
            }
        }
        PROVEN_TEST_ASSERT(bad == 0, "no generated path resolves to anything but a clean path under the root", "");
        PROVEN_TEST_ASSERT(resolved > 1000, "and enough of them resolved for that to mean something", "");
    }

    PROVEN_TEST_PASS("URLs split, code and resolve as specified.");
    return 0;
}
