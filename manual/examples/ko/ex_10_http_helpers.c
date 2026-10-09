#include "example.h"
#include <string.h>

/*
 * 클라이언트와 서버가 메시지 코덱 둘레에서 필요로 하는 작은 조각들: 리다이렉트 풀기,
 * 폼 만들기, 리소스의 일부 요청하기, 파일 올리기, 인증 요구에 답하기. 모두 텍스트를 받아
 * 텍스트를 낸다 - 소켓도 할당도 없다.
 */

static bool view_is(proven_u8str_view_t v, const char *text) {
    return proven_u8str_view_eq(v, proven_u8str_view_from_cstr(text));
}

int main(void) {
    proven_byte_t buf[512];
    proven_mem_mut_t out = (proven_mem_mut_t){ buf, sizeof buf };
    proven_size_t len = 0;

    /* Location 헤더는 참조다. 그것이 어디로 이어지는지는 지금 어디에 있는지에 달려 있다. */
    proven_u8str_view_t base = PROVEN_LIT("http://example.com/docs/guide/intro.html?v=2");
    EXAMPLE_REQUIRE(proven_url_resolve(base, PROVEN_LIT("../img/logo.png"), out, &len) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ buf, len }, "http://example.com/docs/img/logo.png"), "a relative reference");
    EXAMPLE_REQUIRE(proven_url_resolve(base, PROVEN_LIT("/login?next=%2F"), out, &len) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ buf, len }, "http://example.com/login?next=%2F"), "an absolute path");
    EXAMPLE_REQUIRE(proven_url_resolve(base, PROVEN_LIT("//cdn.example.net/x"), out, &len) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ buf, len }, "http://cdn.example.net/x"), "another host, same scheme");

    /* 폼 본문을 한 쌍씩. 구분자와 인코딩은 알아서 처리된다. */
    len = 0;
    EXAMPLE_REQUIRE(proven_url_form_append(out, &len, proven_mem_view_from_u8(PROVEN_LIT("q")), proven_mem_view_from_u8(PROVEN_LIT("tea & cake"))) == PROVEN_OK &&
                    proven_url_form_append(out, &len, proven_mem_view_from_u8(PROVEN_LIT("page")), proven_mem_view_from_u8(PROVEN_LIT("2"))) == PROVEN_OK,
                    "two fields");
    EXAMPLE_REQUIRE(view_is((proven_u8str_view_t){ buf, len }, "q=tea+%26+cake&page=2"), "application/x-www-form-urlencoded");

    /* 범위. 클라이언트가 요청하고, 서버는 그 요청을 자기가 가진 것의 크기에 견주어 읽는다. */
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

    /* multipart/form-data: 텍스트 필드 하나와 파일 하나. 경계는 여러분이 준 무작위 바이트에서
     * 나오므로, 미리 짐작해 파트 안에 심어 둘 수 없다. */
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
    if (err == PROVEN_OK) { memcpy(buf + len, "Holiday", 7); len += 7; }      /* 파트의 바이트는 여러분이 쓴다 */
    if (err == PROVEN_OK) err = proven_http_multipart_write_part_end(out, &len);
    if (err == PROVEN_OK) err = proven_http_multipart_write_part(out, &len, boundary, PROVEN_LIT("photo"), PROVEN_LIT("a \"b\".png"), PROVEN_LIT("image/png"));
    if (err == PROVEN_OK) { memcpy(buf + len, "\x89PNG", 4); len += 4; }
    if (err == PROVEN_OK) err = proven_http_multipart_write_part_end(out, &len);
    if (err == PROVEN_OK) err = proven_http_multipart_write_end(out, &len, boundary);
    EXAMPLE_REQUIRE(err == PROVEN_OK, "a body of two parts");
    buf[len] = '\0';
    EXAMPLE_REQUIRE(strstr((const char *)buf, "filename=\"a %22b%22.png\"") != NULL, "a quote in a file name cannot end the quoted string");

    /* 인증. Basic은 두 문자열을 인코딩한 것이다 - 암호화한 것이 아니다. */
    proven_size_t n = 0;
    EXAMPLE_REQUIRE(proven_http_basic_auth(PROVEN_LIT("Aladdin"), PROVEN_LIT("open sesame"), out, &n) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ buf, n }, "Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ=="), "the value of an Authorization header");

    /* Digest는 요구에 답하는 방식이라 비밀번호 자체는 보내지 않는다. */
    proven_u8str_view_t www = PROVEN_LIT("Digest realm=\"http-auth@example.org\", qop=\"auth, auth-int\", algorithm=SHA-256, "
                                         "nonce=\"7ypf/xlj9XXwfDPEoM4URrv/xwf94BcCAzFZH4GiTo0v\", opaque=\"FQhe/qaU925kfnzjCev0ciny7QMkPqMAFRtzCUYo5tdS\"");
    EXAMPLE_REQUIRE(proven_http_auth_offers(www, PROVEN_LIT("digest")) && !proven_http_auth_offers(www, PROVEN_LIT("Basic")), "what the server offers");
    proven_http_digest_challenge_t challenge;
    EXAMPLE_REQUIRE(proven_http_digest_challenge_parse(www, &challenge) == PROVEN_OK && challenge.algorithm == PROVEN_HTTP_DIGEST_SHA256 && challenge.qop_auth, "the challenge");
    EXAMPLE_REQUIRE(proven_http_digest_auth(&challenge, PROVEN_LIT("Mufasa"), PROVEN_LIT("Circle of Life"), PROVEN_LIT("GET"), PROVEN_LIT("/dir/index.html"),
                                            1, PROVEN_LIT("f2/wE4q74E6zIJEtWaHKaf5wv/H5QzzpXusqGemxURZJ"), out, &n) == PROVEN_OK, "the answer");
    buf[n] = '\0';
    /* RFC 7616 3.9.1절이 바로 이 입력으로 계산해 보인 response 값. */
    EXAMPLE_REQUIRE(strstr((const char *)buf, "response=\"753927fa0e85d155564e2e272a28d1802ca10daf4496794697cf8db5856cb6c1\"") != NULL, "is the one in the RFC");

    return EXAMPLE_OK();
}
