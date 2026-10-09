#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * The small pure pieces an HTTP client is assembled from: resolving a reference against a
 * base URL, building form data, byte ranges, multipart bodies, and the Basic and Digest
 * authentication schemes.
 *
 * Reference resolution is checked against the normal and abnormal examples of RFC 3986
 * section 5.4, which were also run through Python's urljoin. The Digest responses are the ones
 * printed in RFC 7616 section 3.9.1 and RFC 2617 section 3.5, recomputed independently with
 * Python's hashlib before this test was run.
 */

static proven_u8str_view_t sv(const char *s) {
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}

static proven_mem_view_t mv(const char *s) {
    return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}

static bool resolves(const char *base, const char *ref, const char *want) {
    proven_byte_t out[256];
    proven_size_t n = 0;
    if (proven_url_resolve(sv(base), sv(ref), (proven_mem_mut_t){ out, sizeof out }, &n) != PROVEN_OK) return false;
    return n == strlen(want) && memcmp(out, want, n) == 0;
}

static bool buf_is(const proven_byte_t *buf, proven_size_t n, const char *want) {
    return n == strlen(want) && memcmp(buf, want, n) == 0;
}

int main(void) {
    PROVEN_TEST_SUITE("http helpers: references, forms, ranges, multipart, authentication",
        "Each is pure text handling with a published answer to compare against.",
        "Inspect proven_url_resolve in src/proven/url.c, the range and multipart writers in src/proven/http.c, and src/proven/http_auth.c.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("resolving a reference: the examples of RFC 3986 section 5.4",
        "Against the base http://a/b/c/d;p?q - the normal examples, then the abnormal ones.",
        "A redirect's Location is resolved with this. A wrong answer sends the next request to the wrong place.");
    // ---------------------------------------------------------------
    {
        static const struct { const char *ref, *want; } v[] = {
            { "g", "http://a/b/c/g" }, { "./g", "http://a/b/c/g" }, { "g/", "http://a/b/c/g/" }, { "/g", "http://a/g" },
            { "//g", "http://g" }, { "?y", "http://a/b/c/d;p?y" }, { "g?y", "http://a/b/c/g?y" }, { "#s", "http://a/b/c/d;p?q#s" },
            { "g#s", "http://a/b/c/g#s" }, { "g?y#s", "http://a/b/c/g?y#s" }, { ";x", "http://a/b/c/;x" }, { "g;x", "http://a/b/c/g;x" },
            { "g;x?y#s", "http://a/b/c/g;x?y#s" }, { "", "http://a/b/c/d;p?q" }, { ".", "http://a/b/c/" }, { "./", "http://a/b/c/" },
            { "..", "http://a/b/" }, { "../", "http://a/b/" }, { "../g", "http://a/b/g" }, { "../..", "http://a/" },
            { "../../", "http://a/" }, { "../../g", "http://a/g" },
            /* abnormal: more ".." than there are segments stops at the root */
            { "../../../g", "http://a/g" }, { "../../../../g", "http://a/g" }, { "/./g", "http://a/g" }, { "/../g", "http://a/g" },
            { "g.", "http://a/b/c/g." }, { ".g", "http://a/b/c/.g" }, { "g..", "http://a/b/c/g.." }, { "..g", "http://a/b/c/..g" },
            { "./../g", "http://a/b/g" }, { "./g/.", "http://a/b/c/g/" }, { "g/./h", "http://a/b/c/g/h" }, { "g/../h", "http://a/b/c/h" },
            { "g;x=1/./y", "http://a/b/c/g;x=1/y" }, { "g;x=1/../y", "http://a/b/c/y" },
            /* dots after '?' or '#' are data, not segments */
            { "g?y/./x", "http://a/b/c/g?y/./x" }, { "g?y/../x", "http://a/b/c/g?y/../x" },
            { "g#s/./x", "http://a/b/c/g#s/./x" }, { "g#s/../x", "http://a/b/c/g#s/../x" },
            { "https://other.example/x?y", "https://other.example/x?y" },
        };
        bool all = true;
        for (proven_size_t i = 0; i < sizeof v / sizeof v[0]; ++i) {
            if (!resolves("http://a/b/c/d;p?q", v[i].ref, v[i].want)) { all = false; PROVEN_TEST_INFO("wrong for reference \"{}\"", PROVEN_ARG((const char *)v[i].ref)); }
        }
        PROVEN_TEST_ASSERT(all, "all forty-one references resolve as the RFC lists them", "");

        PROVEN_TEST_ASSERT(resolves("http://h", "x", "http://h/x") && resolves("http://h", "?q", "http://h?q") &&
                           resolves("https://u:p@h:8443/a/b?old#frag", "c", "https://u:p@h:8443/a/c"),
            "a base with no path, and one with userinfo and a port: the authority is kept as written, the base's fragment is not", "");

        proven_byte_t out[64];
        proven_size_t n = 9;
        PROVEN_TEST_ASSERT(proven_url_resolve(sv("/not/absolute"), sv("x"), (proven_mem_mut_t){ out, sizeof out }, &n) == PROVEN_ERR_INVALID_FORMAT && n == 0 &&
                           proven_url_resolve(sv("http://a/b"), sv("a b"), (proven_mem_mut_t){ out, sizeof out }, &n) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_url_resolve(sv("http://a/b"), sv("%zz"), (proven_mem_mut_t){ out, sizeof out }, &n) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_url_resolve(sv("http://a/b"), sv("mailto:x@y"), (proven_mem_mut_t){ out, sizeof out }, &n) == PROVEN_ERR_INVALID_FORMAT,
            "a base that is not absolute, a reference with a space or a broken escape, and a result with no authority are refused", "");
        PROVEN_TEST_ASSERT(proven_url_resolve(sv("http://a/b/c"), sv("d"), (proven_mem_mut_t){ out, 11 }, &n) == PROVEN_ERR_OUT_OF_BOUNDS &&
                           proven_url_resolve(sv("http://a/b/c"), sv("d"), (proven_mem_mut_t){ out, 12 }, &n) == PROVEN_OK && n == 12,
            "an output one byte short is PROVEN_ERR_OUT_OF_BOUNDS; exactly enough is enough", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("form data, pair by pair",
        "Each call appends one encoded name=value, with '&' between pairs.",
        "");
    // ---------------------------------------------------------------
    {
        proven_byte_t buf[64];
        proven_mem_mut_t out = { buf, sizeof buf };
        proven_size_t len = 0;
        PROVEN_TEST_ASSERT(proven_url_form_append(out, &len, mv("q"), mv("two words")) == PROVEN_OK &&
                           proven_url_form_append(out, &len, mv("a&b"), mv("1=2")) == PROVEN_OK &&
                           proven_url_form_append(out, &len, mv("empty"), mv("")) == PROVEN_OK &&
                           buf_is(buf, len, "q=two+words&a%26b=1%3D2&empty="),
            "three pairs, with the separators inside a name and a value encoded", "");
        proven_url_query_iter_t it = proven_url_query_iter((proven_u8str_view_t){ buf, len });
        proven_u8str_view_t n, v;
        proven_byte_t dn[16], dv[16];
        proven_size_t nl = 0, vl = 0;
        bool ok = proven_url_query_next(&it, &n, &v) && proven_url_query_next(&it, &n, &v) &&
                  proven_url_form_decode(n, (proven_mem_mut_t){ dn, sizeof dn }, &nl) == PROVEN_OK &&
                  proven_url_form_decode(v, (proven_mem_mut_t){ dv, sizeof dv }, &vl) == PROVEN_OK &&
                  buf_is(dn, nl, "a&b") && buf_is(dv, vl, "1=2");
        PROVEN_TEST_ASSERT(ok, "and the query iterator with the form decoder reads the second pair back exactly", "");
        proven_size_t before = len;
        PROVEN_TEST_ASSERT(proven_url_form_append((proven_mem_mut_t){ buf, len + 3 }, &len, mv("k"), mv("vv")) == PROVEN_ERR_OUT_OF_BOUNDS && len == before,
            "a pair that does not fit is PROVEN_ERR_OUT_OF_BOUNDS and the length does not move", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("byte ranges",
        "A request asks for part of a resource; a server works out which bytes that is, or refuses.",
        "");
    // ---------------------------------------------------------------
    {
        proven_byte_t buf[96];
        proven_mem_mut_t out = { buf, sizeof buf };
        proven_size_t len = 0;
        PROVEN_TEST_ASSERT(proven_http_write_range(out, &len, 100, 199) == PROVEN_OK && buf_is(buf, len, "Range: bytes=100-199\r\n"), "a closed range", "");
        len = 0;
        PROVEN_TEST_ASSERT(proven_http_write_range(out, &len, 100, PROVEN_HTTP_RANGE_TO_END) == PROVEN_OK && buf_is(buf, len, "Range: bytes=100-\r\n"), "an open-ended range", "");
        len = 0;
        PROVEN_TEST_ASSERT(proven_http_write_range(out, &len, 5, 4) == PROVEN_ERR_INVALID_ARG && len == 0, "a range that ends before it starts is refused", "");
        PROVEN_TEST_ASSERT(proven_http_write_content_range(out, &len, 100, 199, 1000) == PROVEN_OK && buf_is(buf, len, "Content-Range: bytes 100-199/1000\r\n"), "Content-Range", "");
        len = 0;
        PROVEN_TEST_ASSERT(proven_http_write_content_range(out, &len, 100, 1000, 1000) == PROVEN_ERR_INVALID_ARG &&
                           proven_http_write_content_range(out, &len, 200, 100, 1000) == PROVEN_ERR_INVALID_ARG,
            "a Content-Range that runs past the total, or backwards, is refused", "");

        static const struct { const char *value; proven_u64 size; proven_err_t err; proven_u64 first, last; } v[] = {
            { "bytes=0-99", 1000, PROVEN_OK, 0, 99 }, { "bytes=100-199", 1000, PROVEN_OK, 100, 199 },
            { "bytes=900-", 1000, PROVEN_OK, 900, 999 }, { "bytes=900-5000", 1000, PROVEN_OK, 900, 999 },
            { "bytes=-100", 1000, PROVEN_OK, 900, 999 }, { "bytes=-5000", 1000, PROVEN_OK, 0, 999 },
            { "bytes=0-0", 1, PROVEN_OK, 0, 0 }, { "BYTES=0-9", 100, PROVEN_OK, 0, 9 }, { "bytes= 0-9 ", 100, PROVEN_OK, 0, 9 },
            { "bytes=1000-", 1000, PROVEN_ERR_OUT_OF_BOUNDS, 0, 0 }, { "bytes=1000-1001", 1000, PROVEN_ERR_OUT_OF_BOUNDS, 0, 0 },
            { "bytes=-0", 1000, PROVEN_ERR_OUT_OF_BOUNDS, 0, 0 }, { "bytes=0-9", 0, PROVEN_ERR_OUT_OF_BOUNDS, 0, 0 },
            { "bytes=0-9,20-29", 100, PROVEN_ERR_UNSUPPORTED, 0, 0 }, { "items=0-9", 100, PROVEN_ERR_UNSUPPORTED, 0, 0 },
            { "bytes=9-0", 100, PROVEN_ERR_INVALID_FORMAT, 0, 0 }, { "bytes=a-b", 100, PROVEN_ERR_INVALID_FORMAT, 0, 0 },
            { "bytes=", 100, PROVEN_ERR_INVALID_FORMAT, 0, 0 }, { "bytes=5", 100, PROVEN_ERR_INVALID_FORMAT, 0, 0 },
            { "bytes", 100, PROVEN_ERR_INVALID_FORMAT, 0, 0 }, { "bytes=0-9x", 100, PROVEN_ERR_INVALID_FORMAT, 0, 0 },
            { "bytes=99999999999999999999-", 100, PROVEN_ERR_INVALID_FORMAT, 0, 0 }, { "=0-9", 100, PROVEN_ERR_INVALID_FORMAT, 0, 0 },
        };
        bool all = true;
        for (proven_size_t i = 0; i < sizeof v / sizeof v[0]; ++i) {
            proven_u64 first = 77, last = 77;
            proven_err_t e = proven_http_range_parse(sv(v[i].value), v[i].size, &first, &last);
            if (e != v[i].err || (e == PROVEN_OK && (first != v[i].first || last != v[i].last))) { all = false; PROVEN_TEST_INFO("wrong for \"{}\"", PROVEN_ARG((const char *)v[i].value)); }
        }
        PROVEN_TEST_ASSERT(all, "twenty-three Range values: nine satisfied and clamped, four unsatisfiable (416), two unsupported, eight malformed", "");

        proven_u64 a = 0, b = 0, t = 0;
        bool has = false;
        PROVEN_TEST_ASSERT(proven_http_content_range_parse(sv("bytes 100-199/1000"), &a, &b, &t, &has) == PROVEN_OK && a == 100 && b == 199 && t == 1000 && has, "Content-Range with a total", "");
        PROVEN_TEST_ASSERT(proven_http_content_range_parse(sv("bytes 0-9/*"), &a, &b, &t, &has) == PROVEN_OK && a == 0 && b == 9 && !has, "Content-Range with an unknown total", "");
        PROVEN_TEST_ASSERT(proven_http_content_range_parse(sv("bytes 9-0/100"), &a, &b, &t, &has) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_http_content_range_parse(sv("bytes 0-100/100"), &a, &b, &t, &has) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_http_content_range_parse(sv("bytes */100"), &a, &b, &t, &has) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_http_content_range_parse(sv("items 0-9/100"), &a, &b, &t, &has) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_http_content_range_parse(sv("bytes 0-9/100 "), &a, &b, &t, &has) == PROVEN_ERR_INVALID_FORMAT,
            "contradictory or malformed Content-Range values are refused", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("multipart form data",
        "A body of parts separated by a boundary, with names that cannot break out of their quotes.",
        "");
    // ---------------------------------------------------------------
    {
        proven_byte_t random[16];
        for (int i = 0; i < 16; ++i) random[i] = (proven_byte_t)(i * 17);
        proven_byte_t boundary[PROVEN_HTTP_BOUNDARY_SIZE];
        proven_http_multipart_boundary(random, boundary);
        PROVEN_TEST_ASSERT(memcmp(boundary, "--proven00112233445566778899aabbccddeeff", 40) == 0,
            "a boundary is a fixed prefix and the 16 random bytes in hex", "");
        proven_u8str_view_t b = { boundary, sizeof boundary };

        proven_byte_t buf[512];
        proven_mem_mut_t out = { buf, sizeof buf };
        proven_size_t len = 0;
        PROVEN_TEST_ASSERT(proven_http_multipart_write_content_type(out, &len, b) == PROVEN_OK &&
                           buf_is(buf, len, "Content-Type: multipart/form-data; boundary=--proven00112233445566778899aabbccddeeff\r\n"),
            "the Content-Type header carries the boundary", "");
        len = 0;
        bool ok = proven_http_multipart_write_part(out, &len, b, PROVEN_LIT("title"), PROVEN_LIT(""), PROVEN_LIT("")) == PROVEN_OK;
        memcpy(buf + len, "Hello", 5); len += 5;
        ok = ok && proven_http_multipart_write_part_end(out, &len) == PROVEN_OK;
        ok = ok && proven_http_multipart_write_part(out, &len, b, PROVEN_LIT("file"), sv("my \"quoted\"\r\nname.txt"), PROVEN_LIT("text/plain")) == PROVEN_OK;
        memcpy(buf + len, "data", 4); len += 4;
        ok = ok && proven_http_multipart_write_part_end(out, &len) == PROVEN_OK && proven_http_multipart_write_end(out, &len, b) == PROVEN_OK;
        const char *want =
            "----proven00112233445566778899aabbccddeeff\r\n"
            "Content-Disposition: form-data; name=\"title\"\r\n\r\n"
            "Hello\r\n"
            "----proven00112233445566778899aabbccddeeff\r\n"
            "Content-Disposition: form-data; name=\"file\"; filename=\"my %22quoted%22%0D%0Aname.txt\"\r\n"
            "Content-Type: text/plain\r\n\r\n"
            "data\r\n"
            "----proven00112233445566778899aabbccddeeff--\r\n";
        PROVEN_TEST_ASSERT(ok && buf_is(buf, len, want),
            "two parts and the closing boundary, byte for byte - the quotes and the line break in the file name are percent-encoded", "");

        proven_size_t before = len;
        PROVEN_TEST_ASSERT(proven_http_multipart_write_part(out, &len, PROVEN_LIT("bad boundary"), PROVEN_LIT("n"), PROVEN_LIT(""), PROVEN_LIT("")) == PROVEN_ERR_INVALID_ARG &&
                           proven_http_multipart_write_part(out, &len, PROVEN_LIT(""), PROVEN_LIT("n"), PROVEN_LIT(""), PROVEN_LIT("")) == PROVEN_ERR_INVALID_ARG &&
                           proven_http_multipart_write_part(out, &len, b, PROVEN_LIT(""), PROVEN_LIT(""), PROVEN_LIT("")) == PROVEN_ERR_INVALID_ARG &&
                           proven_http_multipart_write_part(out, &len, b, PROVEN_LIT("n"), PROVEN_LIT(""), sv("text/plain\r\nX: y")) == PROVEN_ERR_INVALID_ARG &&
                           proven_http_multipart_write_end(out, &len, PROVEN_LIT("a\"b")) == PROVEN_ERR_INVALID_ARG && len == before,
            "a boundary with a space or a quote, an empty name, and a content type with a line break are refused, writing nothing", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("Basic authentication",
        "user:password in Base64, and the two things the format cannot carry.",
        "");
    // ---------------------------------------------------------------
    {
        proven_byte_t buf[128];
        proven_size_t n = 0;
        PROVEN_TEST_ASSERT(proven_http_basic_auth(PROVEN_LIT("Aladdin"), PROVEN_LIT("open sesame"), (proven_mem_mut_t){ buf, sizeof buf }, &n) == PROVEN_OK &&
                           buf_is(buf, n, "Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ=="),
            "the example of RFC 7617 section 2", "");
        static const struct { const char *u, *p, *want; } v[] = {
            { "", "", "Basic Og==" }, { "a", "", "Basic YTo=" }, { "", "b", "Basic OmI=" }, { "ab", "c", "Basic YWI6Yw==" },
            { "user", "pass:with:colons", "Basic dXNlcjpwYXNzOndpdGg6Y29sb25z" },
        };
        bool all = true;
        for (proven_size_t i = 0; i < sizeof v / sizeof v[0]; ++i) {
            if (proven_http_basic_auth(sv(v[i].u), sv(v[i].p), (proven_mem_mut_t){ buf, sizeof buf }, &n) != PROVEN_OK || !buf_is(buf, n, v[i].want)) all = false;
        }
        PROVEN_TEST_ASSERT(all, "every length of user and password lands on the right Base64 padding; a colon in the password is fine", "");
        PROVEN_TEST_ASSERT(proven_http_basic_auth(PROVEN_LIT("a:b"), PROVEN_LIT("c"), (proven_mem_mut_t){ buf, sizeof buf }, &n) == PROVEN_ERR_INVALID_ARG && n == 0 &&
                           proven_http_basic_auth(sv("a\nb"), PROVEN_LIT("c"), (proven_mem_mut_t){ buf, sizeof buf }, &n) == PROVEN_ERR_INVALID_ARG &&
                           proven_http_basic_auth(PROVEN_LIT("a"), sv("c\r\nX: y"), (proven_mem_mut_t){ buf, sizeof buf }, &n) == PROVEN_ERR_INVALID_ARG,
            "a colon in the user name, and a control character in either, are refused", "");
        PROVEN_TEST_ASSERT(proven_http_basic_auth(PROVEN_LIT("Aladdin"), PROVEN_LIT("open sesame"), (proven_mem_mut_t){ buf, 33 }, &n) == PROVEN_ERR_OUT_OF_BOUNDS &&
                           proven_http_basic_auth(PROVEN_LIT("Aladdin"), PROVEN_LIT("open sesame"), (proven_mem_mut_t){ buf, 34 }, &n) == PROVEN_OK && n == 34,
            "thirty-four bytes hold that header value and thirty-three do not", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("Digest authentication: the worked examples of the RFCs",
        "RFC 7616 section 3.9.1 with SHA-256 and with MD5, and the older example of RFC 2617.",
        "The response strings are the ones the RFCs print. A mismatch is a wrong A1, A2 or a wrong order of the colon-separated fields.");
    // ---------------------------------------------------------------
    {
        const char *www =
            "Digest realm=\"http-auth@example.org\", qop=\"auth, auth-int\", algorithm=SHA-256, "
            "nonce=\"7ypf/xlj9XXwfDPEoM4URrv/xwf94BcCAzFZH4GiTo0v\", opaque=\"FQhe/qaU925kfnzjCev0ciny7QMkPqMAFRtzCUYo5tdS\", "
            "Digest realm=\"http-auth@example.org\", qop=\"auth, auth-int\", algorithm=MD5, "
            "nonce=\"7ypf/xlj9XXwfDPEoM4URrv/xwf94BcCAzFZH4GiTo0v\", opaque=\"FQhe/qaU925kfnzjCev0ciny7QMkPqMAFRtzCUYo5tdS\"";
        proven_http_digest_challenge_t ch;
        PROVEN_TEST_ASSERT(proven_http_digest_challenge_parse(sv(www), &ch) == PROVEN_OK && ch.algorithm == PROVEN_HTTP_DIGEST_SHA256 &&
                           ch.qop_auth && ch.has_opaque && !ch.stale &&
                           proven_u8str_view_eq(ch.realm, PROVEN_LIT("http-auth@example.org")) &&
                           proven_u8str_view_eq(ch.nonce, PROVEN_LIT("7ypf/xlj9XXwfDPEoM4URrv/xwf94BcCAzFZH4GiTo0v")),
            "of the two challenges the server offers, SHA-256 is chosen over MD5", "");
        PROVEN_TEST_ASSERT(proven_http_auth_offers(sv(www), PROVEN_LIT("digest")) && !proven_http_auth_offers(sv(www), PROVEN_LIT("Basic")),
            "the value offers Digest, in any case, and does not offer Basic", "");

        proven_byte_t buf[512];
        proven_size_t n = 0;
        proven_u8str_view_t cnonce = PROVEN_LIT("f2/wE4q74E6zIJEtWaHKaf5wv/H5QzzpXusqGemxURZJ");
        PROVEN_TEST_ASSERT(proven_http_digest_auth(&ch, PROVEN_LIT("Mufasa"), PROVEN_LIT("Circle of Life"), PROVEN_LIT("GET"), PROVEN_LIT("/dir/index.html"),
                                                   1, cnonce, (proven_mem_mut_t){ buf, sizeof buf }, &n) == PROVEN_OK &&
                           buf_is(buf, n,
                               "Digest username=\"Mufasa\", realm=\"http-auth@example.org\", nonce=\"7ypf/xlj9XXwfDPEoM4URrv/xwf94BcCAzFZH4GiTo0v\", "
                               "uri=\"/dir/index.html\", algorithm=SHA-256, response=\"753927fa0e85d155564e2e272a28d1802ca10daf4496794697cf8db5856cb6c1\", "
                               "qop=auth, nc=00000001, cnonce=\"f2/wE4q74E6zIJEtWaHKaf5wv/H5QzzpXusqGemxURZJ\", opaque=\"FQhe/qaU925kfnzjCev0ciny7QMkPqMAFRtzCUYo5tdS\""),
            "the SHA-256 response of RFC 7616", "");
        ch.algorithm = PROVEN_HTTP_DIGEST_MD5;
        PROVEN_TEST_ASSERT(proven_http_digest_auth(&ch, PROVEN_LIT("Mufasa"), PROVEN_LIT("Circle of Life"), PROVEN_LIT("GET"), PROVEN_LIT("/dir/index.html"),
                                                   1, cnonce, (proven_mem_mut_t){ buf, sizeof buf }, &n) == PROVEN_OK &&
                           strstr((memcpy(buf + n, "", 1), (const char *)buf), "response=\"8ca523f5e9506fed4657c9700eebdbec\"") != NULL,
            "the MD5 response of RFC 7616", "");

        const char *old = "Digest realm=\"testrealm@host.com\", qop=\"auth,auth-int\", nonce=\"dcd98b7102dd2f0e8b11d0f600bfb0c093\", opaque=\"5ccc069c403ebaf9f0171e9517f40e41\"";
        PROVEN_TEST_ASSERT(proven_http_digest_challenge_parse(sv(old), &ch) == PROVEN_OK && ch.algorithm == PROVEN_HTTP_DIGEST_MD5 &&
                           proven_http_digest_auth(&ch, PROVEN_LIT("Mufasa"), PROVEN_LIT("Circle Of Life"), PROVEN_LIT("GET"), PROVEN_LIT("/dir/index.html"),
                                                   1, PROVEN_LIT("0a4f113b"), (proven_mem_mut_t){ buf, sizeof buf }, &n) == PROVEN_OK &&
                           strstr((memcpy(buf + n, "", 1), (const char *)buf), "response=\"6629fae49393a05397450978507c4ef1\"") != NULL,
            "with no algorithm named, MD5 is assumed: the response of RFC 2617 section 3.5", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("Digest authentication: reading challenges, and refusing",
        "Mixed schemes, unknown algorithms, missing fields, and a user name that would break the quoting.",
        "");
    // ---------------------------------------------------------------
    {
        proven_http_digest_challenge_t ch;
        PROVEN_TEST_ASSERT(proven_http_digest_challenge_parse(sv("Basic realm=\"x\", Digest realm=\"r\", nonce=\"n\", stale=TRUE, Bearer"), &ch) == PROVEN_OK &&
                           proven_u8str_view_eq(ch.realm, PROVEN_LIT("r")) && ch.stale && !ch.qop_auth && !ch.has_opaque,
            "a Digest challenge is found between other schemes; stale is read; no qop means the old form", "");
        PROVEN_TEST_ASSERT(proven_http_auth_offers(sv("Basic realm=\"x\", Digest realm=\"r\", nonce=\"n\", Bearer"), PROVEN_LIT("Basic")) &&
                           proven_http_auth_offers(sv("Basic realm=\"x\", Digest realm=\"r\", nonce=\"n\", Bearer"), PROVEN_LIT("bearer")) &&
                           !proven_http_auth_offers(sv("Digest realm=\"Basic\", nonce=\"n\""), PROVEN_LIT("Basic")),
            "a scheme is found wherever it stands, and a parameter VALUE that spells a scheme is not one", "");
        PROVEN_TEST_ASSERT(proven_http_digest_challenge_parse(sv("Basic realm=\"x\""), &ch) == PROVEN_ERR_NOT_FOUND &&
                           proven_http_digest_challenge_parse(sv(""), &ch) == PROVEN_ERR_NOT_FOUND,
            "no Digest challenge is PROVEN_ERR_NOT_FOUND", "");
        PROVEN_TEST_ASSERT(proven_http_digest_challenge_parse(sv("Digest realm=\"r\", nonce=\"n\", algorithm=SHA-512-256"), &ch) == PROVEN_ERR_UNSUPPORTED &&
                           proven_http_digest_challenge_parse(sv("Digest realm=\"r\", nonce=\"n\", qop=\"auth-int\""), &ch) == PROVEN_ERR_UNSUPPORTED &&
                           proven_http_digest_challenge_parse(sv("Digest realm=\"r\\\"x\", nonce=\"n\""), &ch) == PROVEN_ERR_UNSUPPORTED,
            "an algorithm or qop this library does not implement, and an escaped quote in the realm, are PROVEN_ERR_UNSUPPORTED", "");
        PROVEN_TEST_ASSERT(proven_http_digest_challenge_parse(sv("Digest realm=\"r\", nonce=\"n\", algorithm=SHA-512-256, Digest realm=\"r\", nonce=\"n\", algorithm=MD5"), &ch) == PROVEN_OK &&
                           ch.algorithm == PROVEN_HTTP_DIGEST_MD5,
            "an unsupported challenge beside a supported one: the supported one is used", "");
        PROVEN_TEST_ASSERT(proven_http_digest_challenge_parse(sv("Digest nonce=\"n\""), &ch) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_http_digest_challenge_parse(sv("Digest realm=\"r\""), &ch) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_http_digest_challenge_parse(sv("Digest realm=\"r, nonce=\"n\""), &ch) == PROVEN_ERR_INVALID_FORMAT,
            "a challenge with no realm, no nonce, or an unterminated quote is PROVEN_ERR_INVALID_FORMAT", "");

        proven_byte_t buf[256];
        proven_size_t n = 0;
        PROVEN_TEST_ASSERT(proven_http_digest_challenge_parse(sv("Digest realm=\"r\", nonce=\"n\", qop=auth, algorithm=MD5-sess"), &ch) == PROVEN_OK &&
                           ch.algorithm == PROVEN_HTTP_DIGEST_MD5_SESS, "an unquoted qop and a -sess algorithm are read", "");
        PROVEN_TEST_ASSERT(proven_http_digest_auth(&ch, sv("us\"er"), PROVEN_LIT("p"), PROVEN_LIT("GET"), PROVEN_LIT("/"), 1, PROVEN_LIT("c"), (proven_mem_mut_t){ buf, sizeof buf }, &n) == PROVEN_ERR_INVALID_ARG &&
                           proven_http_digest_auth(&ch, PROVEN_LIT("u"), PROVEN_LIT("p"), PROVEN_LIT("GET"), PROVEN_LIT("/"), 1, sv("c\"x"), (proven_mem_mut_t){ buf, sizeof buf }, &n) == PROVEN_ERR_INVALID_ARG &&
                           proven_http_digest_auth(&ch, PROVEN_LIT("u"), PROVEN_LIT("p"), PROVEN_LIT("G T"), PROVEN_LIT("/"), 1, PROVEN_LIT("c"), (proven_mem_mut_t){ buf, sizeof buf }, &n) == PROVEN_ERR_INVALID_ARG &&
                           proven_http_digest_auth(&ch, PROVEN_LIT("u"), PROVEN_LIT("p"), PROVEN_LIT("GET"), sv("/a\r\nb"), 1, PROVEN_LIT("c"), (proven_mem_mut_t){ buf, sizeof buf }, &n) == PROVEN_ERR_INVALID_ARG && n == 0,
            "a quote in the user name or cnonce, a method that is not a token, and a line break in the uri are refused", "");
        PROVEN_TEST_ASSERT(proven_http_digest_auth(&ch, PROVEN_LIT("u"), PROVEN_LIT("p"), PROVEN_LIT("GET"), PROVEN_LIT("/"), 255, PROVEN_LIT("c"), (proven_mem_mut_t){ buf, sizeof buf }, &n) == PROVEN_OK &&
                           strstr((memcpy(buf + n, "", 1), (const char *)buf), "nc=000000ff") != NULL && strstr((const char *)buf, "algorithm=MD5-sess") != NULL,
            "the nonce count is eight hex digits, and a -sess algorithm is named in the answer", "");
        PROVEN_TEST_ASSERT(proven_http_digest_auth(&ch, PROVEN_LIT("u"), PROVEN_LIT("p"), PROVEN_LIT("GET"), PROVEN_LIT("/"), 1, PROVEN_LIT("c"), (proven_mem_mut_t){ buf, 20 }, &n) == PROVEN_ERR_OUT_OF_BOUNDS && n == 0,
            "an output too small is PROVEN_ERR_OUT_OF_BOUNDS", "");
    }

    PROVEN_TEST_PASS("references, forms, ranges, multipart and authentication behave as specified.");
    return 0;
}
