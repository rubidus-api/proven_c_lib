#include "example.h"
#include <string.h>

/*
 * The small pieces a client and a server need around the message codec: resolving a
 * redirect, building a form, asking for part of a resource, uploading a file, and answering
 * an authentication challenge. All of it is text in, text out - no socket, no allocation.
 */

static bool view_is(proven_u8str_view_t v, const char *text) {
    return proven_u8str_view_eq(v, proven_u8str_view_from_cstr(text));
}

int main(void) {
    proven_byte_t buf[512];
    proven_mem_mut_t out = (proven_mem_mut_t){ buf, sizeof buf };
    proven_size_t len = 0;

    /* A Location header is a reference; where it leads depends on where you are. */
    proven_u8str_view_t base = PROVEN_LIT("http://example.com/docs/guide/intro.html?v=2");
    EXAMPLE_REQUIRE(proven_url_resolve(base, PROVEN_LIT("../img/logo.png"), out, &len) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ buf, len }, "http://example.com/docs/img/logo.png"), "a relative reference");
    EXAMPLE_REQUIRE(proven_url_resolve(base, PROVEN_LIT("/login?next=%2F"), out, &len) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ buf, len }, "http://example.com/login?next=%2F"), "an absolute path");
    EXAMPLE_REQUIRE(proven_url_resolve(base, PROVEN_LIT("//cdn.example.net/x"), out, &len) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ buf, len }, "http://cdn.example.net/x"), "another host, same scheme");

    /* A form body, one pair at a time; the separators and the encoding are done for you. */
    len = 0;
    EXAMPLE_REQUIRE(proven_url_form_append(out, &len, proven_mem_view_from_u8(PROVEN_LIT("q")), proven_mem_view_from_u8(PROVEN_LIT("tea & cake"))) == PROVEN_OK &&
                    proven_url_form_append(out, &len, proven_mem_view_from_u8(PROVEN_LIT("page")), proven_mem_view_from_u8(PROVEN_LIT("2"))) == PROVEN_OK,
                    "two fields");
    EXAMPLE_REQUIRE(view_is((proven_u8str_view_t){ buf, len }, "q=tea+%26+cake&page=2"), "application/x-www-form-urlencoded");

    /* Ranges. A client asks; a server reads the request against the size of what it has. */
    len = 0;
    EXAMPLE_REQUIRE(proven_http_write_range(out, &len, 100, PROVEN_HTTP_RANGE_TO_END) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ buf, len }, "Range: bytes=100-\r\n"), "from byte 100 to the end");
    proven_u64 first = 0, last = 0, total = 0;
    EXAMPLE_REQUIRE(proven_http_range_parse(PROVEN_LIT("bytes=100-"), 1000, &first, &last) == PROVEN_OK && first == 100 && last == 999, "of 1000 bytes: 100 through 999");
    EXAMPLE_REQUIRE(proven_http_range_parse(PROVEN_LIT("bytes=-50"), 1000, &first, &last) == PROVEN_OK && first == 950 && last == 999, "the last fifty");
    EXAMPLE_REQUIRE(proven_http_range_parse(PROVEN_LIT("bytes=2000-"), 1000, &first, &last) == PROVEN_ERR_OUT_OF_BOUNDS, "past the end: answer 416");
    EXAMPLE_REQUIRE(proven_http_range_parse(PROVEN_LIT("bytes=0-9,20-29"), 1000, &first, &last) == PROVEN_ERR_UNSUPPORTED, "several ranges: send the whole thing");
    len = 0;
    EXAMPLE_REQUIRE(proven_http_write_content_range(out, &len, 950, 999, 1000) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ buf, len }, "Content-Range: bytes 950-999/1000\r\n"), "the 206 says which part it is");
    bool has_total = false;
    EXAMPLE_REQUIRE(proven_http_content_range_parse(PROVEN_LIT("bytes 950-999/1000"), &first, &last, &total, &has_total) == PROVEN_OK &&
                    first == 950 && last == 999 && has_total && total == 1000, "and the client reads it back");

    /* multipart/form-data: a text field and a file. The boundary comes from random bytes
     * you supply, so it cannot be predicted and placed inside a part. */
    proven_byte_t random[16];
    EXAMPLE_REQUIRE(proven_random_bytes(random, sizeof random), "sixteen random bytes");
    proven_byte_t boundary_bytes[PROVEN_HTTP_BOUNDARY_SIZE];
    proven_http_multipart_boundary(random, boundary_bytes);
    proven_u8str_view_t boundary = (proven_u8str_view_t){ boundary_bytes, sizeof boundary_bytes };

    proven_byte_t head[128];
    proven_size_t head_len = 0;
    EXAMPLE_REQUIRE(proven_http_multipart_write_content_type((proven_mem_mut_t){ head, sizeof head }, &head_len, boundary) == PROVEN_OK, "the header that announces it");

    len = 0;
    proven_err_t err = proven_http_multipart_write_part(out, &len, boundary, PROVEN_LIT("title"), PROVEN_LIT(""), PROVEN_LIT(""));
    if (err == PROVEN_OK) { memcpy(buf + len, "Holiday", 7); len += 7; }      /* the part's bytes are yours to write */
    if (err == PROVEN_OK) err = proven_http_multipart_write_part_end(out, &len);
    if (err == PROVEN_OK) err = proven_http_multipart_write_part(out, &len, boundary, PROVEN_LIT("photo"), PROVEN_LIT("a \"b\".png"), PROVEN_LIT("image/png"));
    if (err == PROVEN_OK) { memcpy(buf + len, "\x89PNG", 4); len += 4; }
    if (err == PROVEN_OK) err = proven_http_multipart_write_part_end(out, &len);
    if (err == PROVEN_OK) err = proven_http_multipart_write_end(out, &len, boundary);
    EXAMPLE_REQUIRE(err == PROVEN_OK, "a body of two parts");
    buf[len] = '\0';
    EXAMPLE_REQUIRE(strstr((const char *)buf, "filename=\"a %22b%22.png\"") != NULL, "a quote in a file name cannot end the quoted string");

    /* Authentication. Basic is the two strings, encoded - not encrypted. */
    proven_size_t n = 0;
    EXAMPLE_REQUIRE(proven_http_basic_auth(PROVEN_LIT("Aladdin"), PROVEN_LIT("open sesame"), out, &n) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ buf, n }, "Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ=="), "the value of an Authorization header");

    /* Digest answers a challenge, so the password itself is never sent. */
    proven_u8str_view_t www = PROVEN_LIT("Digest realm=\"http-auth@example.org\", qop=\"auth, auth-int\", algorithm=SHA-256, "
                                         "nonce=\"7ypf/xlj9XXwfDPEoM4URrv/xwf94BcCAzFZH4GiTo0v\", opaque=\"FQhe/qaU925kfnzjCev0ciny7QMkPqMAFRtzCUYo5tdS\"");
    EXAMPLE_REQUIRE(proven_http_auth_offers(www, PROVEN_LIT("digest")) && !proven_http_auth_offers(www, PROVEN_LIT("Basic")), "what the server offers");
    proven_http_digest_challenge_t challenge;
    EXAMPLE_REQUIRE(proven_http_digest_challenge_parse(www, &challenge) == PROVEN_OK && challenge.algorithm == PROVEN_HTTP_DIGEST_SHA256 && challenge.qop_auth, "the challenge");
    EXAMPLE_REQUIRE(proven_http_digest_auth(&challenge, PROVEN_LIT("Mufasa"), PROVEN_LIT("Circle of Life"), PROVEN_LIT("GET"), PROVEN_LIT("/dir/index.html"),
                                            1, PROVEN_LIT("f2/wE4q74E6zIJEtWaHKaf5wv/H5QzzpXusqGemxURZJ"), out, &n) == PROVEN_OK, "the answer");
    buf[n] = '\0';
    /* The response RFC 7616 section 3.9.1 works out for exactly these inputs. */
    EXAMPLE_REQUIRE(strstr((const char *)buf, "response=\"753927fa0e85d155564e2e272a28d1802ca10daf4496794697cf8db5856cb6c1\"") != NULL, "is the one in the RFC");

    return EXAMPLE_OK();
}
