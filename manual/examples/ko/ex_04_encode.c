#include "example.h"

/*
 * 바이트를 글자로, 쓰임새별로. 규칙은 해시와 같다. 할 일마다 함수 하나, 그리고 위험은
 * 할 일을 잘못 고르는 것. 사람이 읽을 것에는 hex, URL 에 들어갈 것에는 Base64URL,
 * 선로로 나갈 것에는 표준 Base64.
 */

int main(void) {
    proven_mem_view_t data = proven_mem_view_from_u8(PROVEN_LIT("the quick brown fox"));

    /* 할 일 1: 사람이 읽거나 붙여 넣을 다이제스트 - hex, sha256sum 과 git 이 쓰는 표기. */
    proven_byte_t hex[64];   /* proven_hex_encoded_size(19) = 38 */
    proven_size_t hn = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_hex_encode(data, hex, sizeof hex, &hn)),
                    "hex encode into a buffer sized by proven_hex_encoded_size");
    EXAMPLE_REQUIRE(hn == proven_hex_encoded_size(data.size), "two hex chars per byte");

    /* 할 일 2: URL 에 들어갈 토큰 - Base64URL. 퍼센트 이스케이프가 필요 없고, 파서가
     * 걸려 넘어질 '=' 채움도 없다. */
    proven_byte_t token_bytes[16] = { 0 };   /* 실제 코드에서는: proven_random_bytes(token_bytes, 16) */
    proven_byte_t url[32];
    proven_size_t un = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_base64url_encode(
                        (proven_mem_view_t){ token_bytes, sizeof token_bytes }, url, sizeof url, &un)),
                    "base64url encode a token");
    /* URL 안전 토큰에는 '=' 가 없다. */
    bool has_pad = false;
    for (proven_size_t i = 0; i < un; ++i) if (url[i] == '=') has_pad = true;
    EXAMPLE_REQUIRE(!has_pad, "the URL form emits no padding");

    /* 할 일 3: 선로로 나가는 바이트 - 표준 Base64, HTTP 와 MIME 이 기대하는 +/= 문자표. */
    proven_byte_t b64[64];
    proven_size_t bn = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_base64_encode(data, b64, sizeof b64, &bn)),
                    "standard base64 encode");

    /* 그리고 왕복한다. 디코드는 바이트를 그대로 되돌려 준다. 두 문자표와 채움이 있든
     * 없든 받아 주는 디코더는 일부러 그렇게 만든 것이다 - 실제 입력은 온갖 모양으로 온다. */
    proven_byte_t back[32];
    proven_size_t dn = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_base64_decode(
                        (proven_mem_view_t){ b64, bn }, back, sizeof back, &dn)),
                    "decode the base64 back");
    EXAMPLE_REQUIRE(dn == data.size && proven_memcmp(back, data.ptr, dn) == 0,
                    "what comes back is exactly what went in");

    /* 검증하는 디코더의 요점: 쓰레기는 추측하지 않고 거부한다. 이것을 두 줄짜리 반복문에
     * 먹였다면 끝을 넘어 읽거나 조용히 짧은 결과를 얻었을 것이다. */
    proven_err_t bad = proven_base64_decode(
        proven_mem_view_from_u8(PROVEN_LIT("not valid base64!!")), back, sizeof back, &dn);
    EXAMPLE_REQUIRE(bad == PROVEN_ERR_INVALID_ENCODING,
                    "a stray character is INVALID_ENCODING, with nothing committed");

    /* 그리고 한 바이트 모자란 버퍼는 거부된다. 잘리는 일은 없다. */
    proven_byte_t tiny[4];
    EXAMPLE_REQUIRE(proven_hex_encode(data, tiny, sizeof tiny, &hn) == PROVEN_ERR_OUT_OF_BOUNDS,
                    "a too-small output buffer is OUT_OF_BOUNDS, not a truncated prefix");

    /* --- 짐작하지 말고 목적지 버퍼의 크기를 셈하기 ------------------------ */

    /* 위의 인코드와 디코드는 모두 넉넉한 것이 뻔한 고정 배열에 썼다. 실제 코드는
     * 목적지를 할당하고, 그러면 크기는 기억해 내는 것이 아니라 셈해야 한다. 크기 함수
     * 넷이 그것을 위한 것이다 - 인코딩마다, 방향마다 하나씩. */
    proven_allocator_t alloc = proven_heap_allocator();

    proven_size_t need = proven_base64_encoded_size(data.size);
    EXAMPLE_REQUIRE(need > 0, "an encoded size must be computed, not guessed");

    proven_result_mem_mut_t out = alloc.alloc_fn(alloc.ctx, need, 1);
    EXAMPLE_REQUIRE(proven_is_ok(out.err), "allocating exactly the encoded size must succeed");

    proven_size_t wrote = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_base64_encode(data, out.value.ptr, out.value.size, &wrote)),
                    "encoding into a buffer sized by the size function must fit exactly");
    EXAMPLE_REQUIRE(wrote == need, "and use all of it: this size is exact for standard base64");

    /* 디코드 방향은 정확한 개수가 아니라 *상한*이다. base64 글에는 채움이 있을 수
     * 있으므로, 디코더가 실제로 몇 바이트를 썼는지 알려 준다. 버퍼는 상한으로 잡고,
     * 그다음에는 알려 준 개수를 쓸 것. */
    proven_size_t bound = proven_base64_decoded_size(wrote);
    EXAMPLE_REQUIRE(bound >= data.size, "the decoded bound must cover the real output");

    proven_result_mem_mut_t plain = alloc.alloc_fn(alloc.ctx, bound, 1);
    EXAMPLE_REQUIRE(proven_is_ok(plain.err), "allocating the decode bound must succeed");

    proven_size_t got = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_base64_decode(
                        (proven_mem_view_t){ out.value.ptr, wrote }, plain.value.ptr, plain.value.size, &got)),
                    "decoding back must succeed");
    EXAMPLE_REQUIRE(got == data.size && proven_memcmp(plain.value.ptr, data.ptr, got) == 0,
                    "and reproduce the original bytes exactly");

    /* hex 도 같은 짝을 갖는다. 입력 길이가 짝수일 때 디코드 상한은 정확하고, 올바른
     * hex 는 언제나 짝수다 - 홀수 길이는 잘못된 입력이고, 디코더는 마지막 자리를
     * 버리는 대신 그렇다고 말한다. */
    proven_size_t hex_len = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_hex_encode(data, hex, sizeof hex, &hex_len)),
                    "re-encode to hex, since the refused call above left its count unusable");

    proven_size_t hex_bound = proven_hex_decoded_size(hex_len);
    EXAMPLE_REQUIRE(hex_bound == data.size, "two hex characters decode to one byte");

    proven_byte_t from_hex[32];
    proven_size_t hex_got = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_hex_decode(
                        (proven_mem_view_t){ hex, hex_len }, from_hex, sizeof from_hex, &hex_got)),
                    "hex decodes back to the original bytes");
    EXAMPLE_REQUIRE(hex_got == data.size && proven_memcmp(from_hex, data.ptr, hex_got) == 0,
                    "and the round trip is exact");

    /* hex 는 들어올 때 대소문자를 가리지 않는다. 도구마다 같은 다이제스트를 다른
     * 대소문자로 찍기 때문에 중요하다. */
    EXAMPLE_REQUIRE(proven_is_ok(proven_hex_decode(
                        proven_mem_view_from_u8(PROVEN_LIT("DEADBEEF")), from_hex, sizeof from_hex, &hex_got)),
                    "uppercase hex decodes too");
    EXAMPLE_REQUIRE(hex_got == 4, "and yields one byte per two characters");

    /* 글자 수가 홀수면 온전한 바이트 수가 될 수 없다. */
    EXAMPLE_REQUIRE(proven_hex_decode(proven_mem_view_from_u8(PROVEN_LIT("abc")),
                                      from_hex, sizeof from_hex, &hex_got) == PROVEN_ERR_INVALID_ENCODING,
                    "an odd-length hex string is malformed, not silently truncated");

    alloc.free_fn(alloc.ctx, plain.value.ptr);
    alloc.free_fn(alloc.ctx, out.value.ptr);

    (void)url; (void)un;
    return EXAMPLE_OK();
}
