#include "example.h"
#include <stdio.h>
#include <string.h>

/*
 * HPACK: HTTP/2가 헤더 필드를 보내는 방식.
 *
 * 인코더는 필드 목록을 바이트 블록으로 바꾸고, 반대편의 디코더는 블록을 다시 목록으로 바꾼다.
 * 양쪽 모두 앞서 지나간 필드의 테이블을 유지하며, 같은 필드를 두 번째로 보낼 때는 한 바이트다.
 * 두 테이블은 같은 상태여야 한다 - 그래서 연결에는 방향마다 인코더 하나와 디코더 하나가 있고,
 * 순서대로 쓴다.
 */

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    proven_hpack_encoder_t *encoder = NULL;
    proven_hpack_decoder_t *decoder = NULL;
    /* 테이블 4096바이트가 프로토콜의 기본값이다. 양쪽이 다른 크기에 합의할 수 있다. */
    EXAMPLE_REQUIRE(proven_hpack_encoder_create(heap, PROVEN_HPACK_DEFAULT_TABLE_BYTES, &encoder) == PROVEN_OK &&
                    proven_hpack_decoder_create(heap, PROVEN_HPACK_DEFAULT_TABLE_BYTES, &decoder) == PROVEN_OK, "an encoder here, a decoder there");

    // ---- 요청 하나를 두 번 ---------------------------------------------------------
    /* HTTP/2에서 이름은 소문자이고, HTTP/1이 첫 줄에 적던 것은 여기서 이름이 콜론으로
     * 시작하는 필드가 된다. */
    proven_http_header_t request[] = {
        { PROVEN_LIT(":method"), PROVEN_LIT("GET") },
        { PROVEN_LIT(":scheme"), PROVEN_LIT("https") },
        { PROVEN_LIT(":path"), PROVEN_LIT("/reports/2026/october") },
        { PROVEN_LIT(":authority"), PROVEN_LIT("www.example.com") },
        { PROVEN_LIT("user-agent"), PROVEN_LIT("example-client/1.0") },
        { PROVEN_LIT("authorization"), PROVEN_LIT("Bearer 5f2c1e9a7b3d") },
    };
    proven_byte_t block[512];
    EXAMPLE_REQUIRE(proven_hpack_encode_bound(request, 6) <= sizeof block, "the block will fit");
    proven_size_t first = 0;
    EXAMPLE_REQUIRE(proven_hpack_encode(encoder, request, 6, NULL, (proven_mem_mut_t){ block, sizeof block }, &first) == PROVEN_OK, "the request, encoded");

    /* 디코더는 필드를 뷰로 돌려준다: 블록 안, 고정 테이블 안, 또는 빌려준 스크래치 안을
     * 가리킨다. */
    proven_http_header_t fields[16];
    proven_byte_t scratch[1024];
    proven_size_t count = 0;
    EXAMPLE_REQUIRE(proven_hpack_decode(decoder, (proven_mem_view_t){ block, first }, fields, 16, (proven_mem_mut_t){ scratch, sizeof scratch }, 4096, &count) == PROVEN_OK && count == 6,
                    "and decoded");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(fields[2].name, PROVEN_LIT(":path")) && proven_u8str_view_eq(fields[2].value, PROVEN_LIT("/reports/2026/october")), "field for field");

    /* 같은 요청을 다시 보내면 비용은 일부분이다: 양쪽이 기억하고 있다. */
    proven_size_t second = 0;
    EXAMPLE_REQUIRE(proven_hpack_encode(encoder, request, 6, NULL, (proven_mem_mut_t){ block, sizeof block }, &second) == PROVEN_OK, "the same request again");
    printf("the first time: %u bytes; the second: %u\n", (unsigned)first, (unsigned)second);
    EXAMPLE_REQUIRE(second < first / 2, "less than half");
    EXAMPLE_REQUIRE(proven_hpack_decode(decoder, (proven_mem_view_t){ block, second }, fields, 16, (proven_mem_mut_t){ scratch, sizeof scratch }, 4096, &count) == PROVEN_OK && count == 6 &&
                    proven_u8str_view_eq(fields[4].value, PROVEN_LIT("example-client/1.0")), "and the decoder still gives the whole list");

    // ---- 기억하지 않는 것 ----------------------------------------------------------
    /* 테이블은 압축 문맥이고, 그 안의 비밀은 뒤따르는 블록의 크기로 추측될 수 있다.
     * 그래서 authorization 값은 테이블에 들어간 적이 없다 - 어느 쪽에서도: */
    proven_size_t entries = proven_hpack_decoder_table_count(decoder);
    bool secret_kept = false;
    for (proven_size_t i = 0; i < entries; ++i) {
        proven_u8str_view_t name, value;
        EXAMPLE_REQUIRE(proven_hpack_decoder_table_entry(decoder, i, &name, &value), "an entry of the table");
        if (proven_u8str_view_eq(name, PROVEN_LIT("authorization"))) secret_kept = true;
    }
    printf("the table holds %u entries, %u bytes by the protocol's count\n", (unsigned)entries, (unsigned)proven_hpack_decoder_table_bytes(decoder));
    EXAMPLE_REQUIRE(entries == 3 && !secret_kept, "the path, the authority and the user agent - not the secret");
    /* 어떤 필드든 같은 취급을 받게 할 수 있다. */
    proven_http_header_t more[] = { { PROVEN_LIT("x-api-key"), PROVEN_LIT("k-0123456789") } };
    bool sensitive[] = { true };
    bool marked[16] = { false };
    proven_size_t n = 0;
    /* 디코더는 어느 필드가 표시와 함께 왔는지 알려 줄 수 있다 - 필드를 넘기는 쪽에 필요한 것. */
    EXAMPLE_REQUIRE(proven_hpack_encode(encoder, more, 1, sensitive, (proven_mem_mut_t){ block, sizeof block }, &n) == PROVEN_OK &&
                    proven_hpack_decode_marked(decoder, (proven_mem_view_t){ block, n }, fields, marked, 16, (proven_mem_mut_t){ scratch, sizeof scratch }, 4096, &count) == PROVEN_OK &&
                    proven_hpack_decoder_table_count(decoder) == 3 && marked[0], "a field marked sensitive adds nothing to the table, and arrives marked");

    // ---- 한도, 그리고 그것의 쓸모 --------------------------------------------------
    /* 블록의 크기는 그것이 말하는 내용의 크기가 아니다. 이 블록은 다섯 바이트이고, 기억된
     * 같은 필드를 다섯 번 가리킨다: */
    static const proven_byte_t repeat[] = { 0xbe, 0xbe, 0xbe, 0xbe, 0xbe };       /* "62번 항목" - 테이블에서 가장 새것 */
    EXAMPLE_REQUIRE(proven_hpack_decode(decoder, (proven_mem_view_t){ repeat, sizeof repeat }, fields, 16, (proven_mem_mut_t){ scratch, sizeof scratch }, 4096, &count) == PROVEN_OK && count == 5,
                    "five bytes, five fields");
    /* 그래서 한도는 블록이 아니라 풀린 목록 - 이름, 값, 그리고 필드마다 32 - 에 걸린다.
     * 넘으면 필드는 내주지 않지만, 디코더는 블록을 끝까지 읽었고 여전히 쓸 수 있다:
     * 그 요청 하나만 거절하고 연결은 유지한다. */
    EXAMPLE_REQUIRE(proven_hpack_decode(decoder, (proven_mem_view_t){ repeat, sizeof repeat }, fields, 16, (proven_mem_mut_t){ scratch, sizeof scratch }, 100, &count) == PROVEN_ERR_OUT_OF_BOUNDS && count == 0,
                    "a list limit of 100 bytes refuses it");
    EXAMPLE_REQUIRE(proven_hpack_decode(decoder, (proven_mem_view_t){ repeat, 1 }, fields, 16, (proven_mem_mut_t){ scratch, sizeof scratch }, 100, &count) == PROVEN_OK && count == 1, "and the next block is decoded as if nothing had happened");

    /* 아예 HPACK이 아닌 블록은 다른 문제다: 두 테이블이 일치한다고 더는 믿을 수 없고,
     * 디코더는 그때부터 그렇게 말한다. HTTP/2에서는 연결이 끝난다. */
    static const proven_byte_t nonsense[] = { 0x80 };                            /* "0번 항목": 그런 것은 없다 */
    EXAMPLE_REQUIRE(proven_hpack_decode(decoder, (proven_mem_view_t){ nonsense, 1 }, fields, 16, (proven_mem_mut_t){ scratch, sizeof scratch }, 4096, &count) == PROVEN_ERR_INVALID_FORMAT &&
                    proven_hpack_decode(decoder, (proven_mem_view_t){ repeat, 1 }, fields, 16, (proven_mem_mut_t){ scratch, sizeof scratch }, 4096, &count) == PROVEN_ERR_INVALID_FORMAT,
                    "a malformed block is refused, and so is everything after it");

    // ---- 테이블 크기는 협상된다 ---------------------------------------------------
    /* 상대가 더 작은 테이블을 알리면 인코더에게 전하고, 인코더는 다음 블록의 맨 앞에서
     * 따랐다고 말한다. 자기 한도가 낮아진 디코더는 그 말을 요구한다. */
    proven_hpack_decoder_destroy(decoder);
    EXAMPLE_REQUIRE(proven_hpack_decoder_create(heap, PROVEN_HPACK_DEFAULT_TABLE_BYTES, &decoder) == PROVEN_OK, "a fresh decoder");
    proven_hpack_decoder_set_max_table_bytes(decoder, 0);          /* "이쪽은 테이블을 두지 않겠다" ... */
    proven_hpack_encoder_set_peer_table_bytes(encoder, 0);         /* ... 인코더가 그것을 들었다 */
    n = 0;
    EXAMPLE_REQUIRE(proven_hpack_encode(encoder, request, 6, NULL, (proven_mem_mut_t){ block, sizeof block }, &n) == PROVEN_OK && block[0] == 0x20, "the next block opens by saying: table size 0");
    EXAMPLE_REQUIRE(proven_hpack_decode(decoder, (proven_mem_view_t){ block, n }, fields, 16, (proven_mem_mut_t){ scratch, sizeof scratch }, 4096, &count) == PROVEN_OK && count == 6 &&
                    proven_hpack_decoder_table_count(decoder) == 0, "and nothing is remembered on either side");

    // ---- 허프만 코딩만 따로 ------------------------------------------------------
    /* 리터럴 문자열은 흔한 문자에 더 적은 비트를 주는 코드로 보낸다. */
    proven_u8str_view_t text = PROVEN_LIT("www.example.com");
    proven_byte_t coded[32], back[32];
    proven_size_t coded_len = 0, back_len = 0;
    EXAMPLE_REQUIRE(proven_hpack_huffman_size(text) == 12 && proven_hpack_huffman_encode(text, (proven_mem_mut_t){ coded, sizeof coded }, &coded_len) == PROVEN_OK && coded_len == 12, "fifteen characters in twelve bytes");
    EXAMPLE_REQUIRE(proven_hpack_huffman_decode((proven_mem_view_t){ coded, coded_len }, (proven_mem_mut_t){ back, sizeof back }, &back_len) == PROVEN_OK && back_len == 15 && memcmp(back, "www.example.com", 15) == 0, "and back");

    proven_hpack_encoder_destroy(encoder);
    proven_hpack_decoder_destroy(decoder);
    return EXAMPLE_OK();
}
