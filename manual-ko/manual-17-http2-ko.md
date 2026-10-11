# 17장: HTTP/2 - 프레임과 헤더 압축

**5부 — 운영체제와 대화하기. 선행 조건: 요청과 응답이 무엇인지는 [10장](manual-10-http-ko.md), 압축
테이블을 만들 때 쓰는 할당자(allocator) - 예제에서는 힙(heap) 할당자 - 는
[2장](manual-02-allocation-ko.md).**
**이 장을 마치면** HTTP/2가 무엇을 바꾸고 무엇을 바꾸지 않는지 말할 수 있고, 헤더 블록을 인코딩하고
디코딩하며, 프레임을 읽고 쓰고, 잘 만들어진 요청과 전달해서는 안 되는 요청을 가려낼 수 있다 - 그리고
이 가운데 이 라이브러리가 지금 하는 것이 어디까지인지 말할 수 있다.

이 장은 `hpack.h`와 `h2.h`를 다룬다. 둘 다 순수한 계산이고
[프리스탠딩(freestanding)](manual-freestanding-ko.md) 빌드에서 쓸 수 있다: HPACK은 인코더에 테이블
하나, 디코더에 테이블 하나를 할당하고, 프레임 함수는 아무것도 할당하지 않는다. 입력과 출력은 여러분의
메모리를 가리키는 뷰(view)로 주고받는다.

**여기 있는 것은 코덱이지 아직 프로토콜이 아니다.** 이 두 헤더는 HTTP/2 연결 위를 오가는 것을 읽고
쓴다. 연결을 하나로 묶어 주지는 않는다 - 어떤 스트림이 열려 있는지, 얼마나 보낼 수 있는지, 무엇에
무엇으로 답하는지 - 그리고 11장과 15장의 서버와 클라이언트는 HTTP/1.1로 말한다. 그 선이 정확히 어디인지는
5절에 있다.

## 목차

1. [HTTP/2가 바꾸는 것, 한 쪽으로](#1-http2가-바꾸는-것-한-쪽으로)
2. [헤더 압축](#2-헤더-압축)
3. [프레임](#3-프레임)
4. [메시지로서의 헤더 목록](#4-메시지로서의-헤더-목록)
5. [무엇을 받아들이고, 무엇이 없으며, 어떻게 시험했는가](#5-무엇을-받아들이고-무엇이-없으며-어떻게-시험했는가)

## 1. HTTP/2가 바꾸는 것, 한 쪽으로

HTTP/2(RFC 9113)는 HTTP/1.1과 같은 요청과 응답 - 같은 메서드, 상태 코드, 헤더 필드 - 을 다른 모양으로
실어 나른다. 세 가지가 다르다:

- **모든 것이 프레임이다.** 헤더 아홉 바이트 - 길이, 종류, 플래그, 스트림 번호 - 와 페이로드. 파싱할
  텍스트도 끝낼 줄도 없다: 메시지는 HEADERS 프레임과 DATA 프레임이다.
- **여러 교환이 한 연결을 나눠 쓴다.** 요청 하나와 그 응답이 번호가 붙은 *스트림*이다. 서로 다른
  스트림의 프레임이 섞여 오가므로, 느린 응답이 뒤의 것들을 붙잡지 않는다. 스트림마다, 그리고 연결 전체에
  *윈도*가 있다: 상대가 받겠다고 말한 DATA 바이트 수다.
- **헤더 필드는 압축된다.** 헤더를 위해 만든 방식(HPACK, RFC 7541)으로. 같은 필드가 요청마다 다시
  전송되기 때문이다.

HTTP/1.1이 첫 줄에서 말하던 것은 이름이 콜론으로 시작하는 *유사 헤더 필드*가 말한다: 요청에는
`:method`, `:scheme`, `:authority`, `:path`, 응답에는 `:status`. `Host`는 없고(`:authority`가 그것이다)
`Connection`도 `Transfer-Encoding`도 없다: 그것들이 하던 일은 프레이밍이 한다.

연결은 클라이언트가 보내는 고정된 24바이트(`PROVEN_H2_PREFACE`)로 시작하고, 이어서 양쪽이 SETTINGS
프레임을 하나씩 보낸다.

## 2. 헤더 압축

```c
proven_err_t proven_hpack_encoder_create(proven_allocator_t alloc, proven_size_t max_table_bytes, proven_hpack_encoder_t **out);
proven_err_t proven_hpack_encode(proven_hpack_encoder_t *encoder, const proven_http_header_t *headers, proven_size_t count,
                                 const bool *sensitive, proven_mem_mut_t out, proven_size_t *len);

proven_err_t proven_hpack_decoder_create(proven_allocator_t alloc, proven_size_t max_table_bytes, proven_hpack_decoder_t **out);
proven_err_t proven_hpack_decode(proven_hpack_decoder_t *decoder, proven_mem_view_t block,
                                 proven_http_header_t *headers, proven_size_t header_cap,
                                 proven_mem_mut_t scratch, proven_size_t max_list_bytes, proven_size_t *count);
```

HPACK은 필드를 세 가지 방법 가운데 하나로 보낸다: 흔한 필드 61개가 담긴 고정 테이블의 항목을 가리키는
번호로(`:method: GET`은 2번 항목이다), 이 연결이 전에 보낸 필드를 담은 *동적 테이블*의 항목을 가리키는
번호로, 또는 리터럴로 - 리터럴은 보내면서 동적 테이블에 넣을 수 있고, 그 글자는 허프만 코딩할 수 있다.
처음에 65바이트인 요청이 두 번째에는 22바이트다.

**양쪽은 같은 테이블을 유지해야 한다.** 모든 블록은 보낸 순서대로, 그 연결 그 방향의 디코더 하나가
디코딩한다. 블록 하나를 건너뛰거나 두 번 디코딩하면 그 뒤의 모든 블록이 다른 뜻이 된다. 잘못된 블록을
만난 디코더가 그 뒤의 것을 모두 거부하는 까닭, 그리고 HTTP/2가 그 일로 연결을 끝내는 까닭이 이것이다.

**테이블 크기는 여러분이 정하고**, 비용은 정확히 그 크기다: 기본 4,096바이트
(`PROVEN_HPACK_DEFAULT_TABLE_BYTES`)이며 첫 항목이 추가될 때 할당된다. 0으로 만든 인코더는 테이블을 두지
않아 상태가 없고, 0으로 만든 디코더는 상대에게 테이블을 두지 말라고 알린다.

### 예제: 두 번 보낸 요청, 테이블에 넣지 않은 비밀, 그리고 한도

<!-- example: manual/examples/ko/ex_17_hpack.c -->
```c
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
    proven_size_t n = 0;
    EXAMPLE_REQUIRE(proven_hpack_encode(encoder, more, 1, sensitive, (proven_mem_mut_t){ block, sizeof block }, &n) == PROVEN_OK &&
                    proven_hpack_decode(decoder, (proven_mem_view_t){ block, n }, fields, 16, (proven_mem_mut_t){ scratch, sizeof scratch }, 4096, &count) == PROVEN_OK &&
                    proven_hpack_decoder_table_count(decoder) == 3, "a field marked sensitive adds nothing to the table");

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
```

### 참조

| API | 의도 | 반환 |
|---|---|---|
| `proven_hpack_encoder_create(alloc, max_table_bytes, &enc)` | 테이블을 많아야 이만큼 쓰는 인코더(0: 없음). | `proven_err_t`: `INVALID_ARG`, `NOMEM`. |
| `proven_hpack_encode(enc, headers, count, sensitive, out, &len)` | 이 필드들의 헤더 블록 하나를 덧붙인다. `sensitive`는 선택 사항. | `proven_err_t`: `out`의 자리가 bound보다 적으면 `OUT_OF_BOUNDS` - 아무것도 쓰지 않고 아무것도 바뀌지 않는다. |
| `proven_hpack_encode_bound(headers, count)` | 언제나 충분한 자리. | `proven_size_t`. |
| `proven_hpack_encoder_set_peer_table_bytes(enc, n)` | 상대의 한도가 바뀌었다. 다음 블록에서 알린다. | 없음. |
| `proven_hpack_decoder_create(alloc, max_table_bytes, &dec)` | 상대에게 이만큼의 테이블을 허용하는 디코더. | `proven_err_t`. |
| `proven_hpack_decode(dec, block, headers, cap, scratch, max_list_bytes, &count)` | 블록 하나를 통째로 디코딩한다. | `proven_err_t`: `OUT_OF_BOUNDS` - 목록이 너무 크거나, 필드가 너무 많거나, 스크래치가 너무 작다. 디코더는 여전히 쓸 수 있다. `INVALID_FORMAT` - HPACK이 아니다. 디코더는 끝났다. |
| `proven_hpack_decoder_set_max_table_bytes(dec, n)` | 이쪽의 한도가 바뀌었다. 낮췄다면 상대의 다음 블록이 따랐다고 말해야 한다. | 없음. |
| `proven_hpack_decoder_table_count`, `_table_bytes`, `_table_entry` | 디코더의 테이블을 들여다본다. | 개수. `bool`. |
| `proven_hpack_huffman_size`, `_huffman_encode`, `_huffman_decode` | 문자열 코드만 따로. | 크기. `proven_err_t`. |
| `proven_hpack_encoder_destroy`, `proven_hpack_decoder_destroy` | 해제한다. 널을 받는다. | 없음. |

### 주의사항, 그리고 무엇이 잘못되는가

- **블록의 크기는 그것이 말하는 내용의 크기가 아니다.** 다섯 바이트가 기억된 큰 필드를 다섯 번 가리킬 수
  있고, 오천 바이트는 오천 번 가리킬 수 있다. `max_list_bytes`를 늘 줄 것: 풀린 목록 - RFC가 세는 대로
  이름, 값, 그리고 필드마다 32바이트 - 을 묶고, 그 크기의 `scratch`면 언제나 충분하다.
- **한도 초과는 연결의 끝이 아니고, 잘못된 형식은 끝이다.** `OUT_OF_BOUNDS`는 블록을 끝까지 읽었고
  테이블이 맞아 있으며 이 메시지만 거절한다는 뜻이다. `INVALID_FORMAT`은 두 테이블이 달라졌을 수 있다는
  뜻이고, 디코더는 그 뒤로 계속 같은 답을 한다.
- **뷰, 세 가지.** 디코딩된 이름이나 값은 블록 안, 라이브러리의 고정 테이블 안, 또는 여러분이 빌려준
  임시 작업용(scratch) 버퍼 - 아래에서는 스크래치 - 안을 가리킨다. 셋 모두 필드를 쓰는 동안 살아 있어야 한다. 간직할 것은 복사한다.
- **비밀은 테이블에 넣지 않는다.** 테이블은 압축 문맥이고, 그 안의 것은 공격자가 영향을 주는 뒤따르는
  블록의 크기로 한 바이트씩 추측될 수 있다. 인코더는 `authorization`과 `proxy-authorization`의 값을
  기억하지 않고, 어느 중간 지점도 기억하지 않도록 표시해서 보낸다. `cookie`와 `set-cookie`도 기억하지
  않는다. `sensitive`는 여러분이 지목한 필드까지 그렇게 다룬다.
- **이름은 인코더에 닿기 전에 소문자여야 한다.** 인코더는 받은 대로 인코딩한다. 목록이 보내도 되는
  것인지 검사하는 일은 4절의 것이다.
- **인코더 하나, 디코더 하나, 방향 하나, 순서대로.** 요청마다가 아니고 스트림마다가 아니다.

## 3. 프레임

```c
proven_err_t proven_h2_frame_parse(proven_mem_view_t data, proven_h2_frame_t *out);
proven_err_t proven_h2_frame_check(const proven_h2_frame_t *frame, proven_u32 max_frame_size, proven_h2_error_t *code, bool *stream_error);
proven_err_t proven_h2_frame_write(proven_mem_mut_t out, proven_size_t *len, const proven_h2_frame_t *frame);
```

프레임은 아홉 바이트와 페이로드다:

| 필드 | 비트 | 뜻 |
|---|---|---|
| 길이 | 24 | 페이로드의 바이트 수. 모든 끝점은 16,384를 받는다. 더 큰 것은 그렇게 말한 경우에만 |
| 종류 | 8 | 열 가지 가운데 하나. 그 밖의 것은 건너뛴다 |
| 플래그 | 8 | 뜻은 종류에 따라 다르다 |
| 스트림 | 31(그리고 예약 비트 하나) | 0은 연결 자체. 클라이언트는 홀수 스트림을 연다 |

`proven_h2_frame_check`는 페이로드를 보기 전에 종류마다 있는 규칙을 적용한다:

| 종류 | 스트림 | 크기 | 싣는 것 |
|---|---|---|---|
| `DATA` | 필요 | 아무 크기 | 본문의 한 조각. 패딩될 수 있다. `END_STREAM`은 보낸 쪽을 끝낸다 |
| `HEADERS` | 필요 | 아무 크기 | 헤더 블록, 또는 그 시작. 패딩될 수 있고, 우선순위 다섯 바이트를 실을 수 있는데 그것은 건너뛴다 |
| `CONTINUATION` | 필요 | 아무 크기 | `END_HEADERS`가 나올 때까지 헤더 블록의 나머지 |
| `RST_STREAM` | 필요 | 4 | 오류 코드: 이 스트림은 끝났다 |
| `SETTINGS` | 0이어야 한다 | 6의 배수. `ACK`면 0 | 식별자와 값의 쌍 |
| `PING` | 0이어야 한다 | 8 | `ACK`와 함께 돌려보낼 여덟 바이트 |
| `GOAWAY` | 0이어야 한다 | 8 이상 | 처리한 마지막 스트림, 오류 코드, 사람이 읽을 글 |
| `WINDOW_UPDATE` | 어느 쪽이든 | 4 | 몇 바이트를 더 보내도 되는가 |
| `PRIORITY` | 필요 | 5 | 폐기된 것. 검사하고 무시한다 |
| `PUSH_PROMISE` | 필요 | 아무 크기 | 서버 푸시. 이 라이브러리는 언제나 사양한다 |

**오류에는 두 가지 크기가 있다.** *연결 오류*는 연결을 끝낸다: 코드를 실은 GOAWAY를 보내고 닫는다.
*스트림 오류*는 스트림 하나를 끝낸다: RST_STREAM을 보내고 계속한다. 검사는 코드와 어느 쪽인지를 알려 준다.

### 예제: 서버가 읽듯이 읽은, 연결의 첫 바이트들

<!-- example: manual/examples/ko/ex_17_frames.c -->
```c
#include <stdio.h>
#include <string.h>

/*
 * HTTP/2 프레임과 그 안의 헤더 목록 - 서버가 연결의 첫 바이트들을 읽듯이, 여기서는 버퍼에서
 * 읽는다.
 *
 * h2.h에는 연결을 하나로 묶어 주는 것이 없다. 프레임을 읽고 쓰고, 프레임 종류마다 있는 규칙을
 * 적용하고, 풀린 헤더 목록이 요청인지 검사한다. 어떤 스트림이 열려 있고 얼마나 보낼 수 있는지는
 * 이것들을 쓰는 쪽의 일이다.
 */

/* 클라이언트가 연결을 열고 페이지 하나를 요청할 때 보낼 바이트. */
static proven_size_t client_bytes(proven_allocator_t heap, proven_byte_t *wire, proven_size_t cap) {
    proven_mem_mut_t out = { wire, cap };
    proven_size_t len = 0;
    memcpy(wire, PROVEN_H2_PREFACE, PROVEN_H2_PREFACE_SIZE);       /* 고정된 24바이트가 먼저 온다 */
    len = PROVEN_H2_PREFACE_SIZE;

    /* 다음은 클라이언트의 설정: 여기서는 "푸시하지 말 것"과 더 큰 윈도. */
    proven_h2_setting_t settings[] = {
        { PROVEN_H2_SETTINGS_ENABLE_PUSH, 0 },
        { PROVEN_H2_SETTINGS_INITIAL_WINDOW_SIZE, 1048576 },
    };
    if (proven_h2_write_settings(out, &len, settings, 2) != PROVEN_OK) return 0;

    /* 요청은 새 스트림 - 클라이언트의 스트림은 홀수 - 위의 HEADERS 프레임이고, HPACK
     * 블록을 싣는다. END_HEADERS: 블록이 온전하다. END_STREAM: 본문이 뒤따르지 않는다. */
    proven_http_header_t request[] = {
        { PROVEN_LIT(":method"), PROVEN_LIT("GET") },
        { PROVEN_LIT(":scheme"), PROVEN_LIT("https") },
        { PROVEN_LIT(":path"), PROVEN_LIT("/") },
        { PROVEN_LIT(":authority"), PROVEN_LIT("www.example.com") },
        { PROVEN_LIT("accept"), PROVEN_LIT("text/html") },
    };
    proven_hpack_encoder_t *encoder = NULL;
    proven_byte_t block[256];
    proven_size_t block_len = 0;
    if (proven_hpack_encoder_create(heap, PROVEN_HPACK_DEFAULT_TABLE_BYTES, &encoder) != PROVEN_OK) return 0;
    proven_err_t err = proven_hpack_encode(encoder, request, 5, NULL, (proven_mem_mut_t){ block, sizeof block }, &block_len);
    proven_hpack_encoder_destroy(encoder);
    if (err != PROVEN_OK) return 0;
    proven_h2_frame_t headers = { .length = (proven_u32)block_len, .type = PROVEN_H2_HEADERS,
                                  .flags = PROVEN_H2_FLAG_END_HEADERS | PROVEN_H2_FLAG_END_STREAM, .stream_id = 1 };
    if (proven_h2_frame_write(out, &len, &headers) != PROVEN_OK) return 0;
    memcpy(wire + len, block, block_len);
    len += block_len;

    /* 그리고 예제를 위해, 규칙을 어기는 프레임 하나: PING은 여덟 바이트여야 한다. */
    proven_h2_frame_t bad_ping = { .length = 4, .type = PROVEN_H2_PING };
    if (proven_h2_frame_write(out, &len, &bad_ping) != PROVEN_OK) return 0;
    memset(wire + len, 0, 4);
    return len + 4;
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    proven_byte_t wire[1024];
    proven_size_t size = client_bytes(heap, wire, sizeof wire);
    EXAMPLE_REQUIRE(size > 0, "the client's bytes");

    // ---- 서버 쪽 -------------------------------------------------------------------
    EXAMPLE_REQUIRE(size >= PROVEN_H2_PREFACE_SIZE && memcmp(wire, PROVEN_H2_PREFACE, PROVEN_H2_PREFACE_SIZE) == 0, "the preface: this is HTTP/2");
    proven_size_t at = PROVEN_H2_PREFACE_SIZE;

    proven_hpack_decoder_t *decoder = NULL;
    EXAMPLE_REQUIRE(proven_hpack_decoder_create(heap, PROVEN_HPACK_DEFAULT_TABLE_BYTES, &decoder) == PROVEN_OK, "the connection's decoder");
    int frames = 0, requests = 0;
    proven_h2_error_t refused_with = PROVEN_H2_NO_ERROR;

    while (at < size) {
        /* 헤더 아홉 바이트가 프레임의 길이, 종류, 그리고 어느 스트림의 것인지를 말한다. */
        proven_h2_frame_t frame;
        proven_err_t err = proven_h2_frame_parse((proven_mem_view_t){ wire + at, size - at }, &frame);
        if (err == PROVEN_ERR_NEED_MORE) break;                    /* 실제 서버라면 더 읽을 것이다 */
        EXAMPLE_REQUIRE(err == PROVEN_OK, "a frame header");

        /* 페이로드를 보기 전에: 이 프레임은 자기가 말하는 그것이어도 되는가? 크기 한도는
         * 이쪽이 알린 값이다(여기서는 프로토콜의 최솟값). */
        proven_h2_error_t code;
        bool stream_error;
        if (proven_h2_frame_check(&frame, PROVEN_H2_MIN_MAX_FRAME_SIZE, &code, &stream_error) != PROVEN_OK) {
            /* 실제 서버는 GOAWAY로 답한다(스트림만 잘못됐으면 RST_STREAM). */
            refused_with = code;
            EXAMPLE_REQUIRE(!stream_error, "a PING of the wrong size ends the connection");
            break;
        }
        proven_mem_view_t payload = { wire + at + PROVEN_H2_FRAME_HEADER_SIZE, frame.length };
        EXAMPLE_REQUIRE(PROVEN_H2_FRAME_HEADER_SIZE + frame.length <= size - at, "the whole frame is here");
        frames++;

        if (frame.type == PROVEN_H2_SETTINGS) {
            proven_size_t pos = 0;
            proven_h2_setting_t setting;
            while ((err = proven_h2_settings_next(payload, &pos, &setting, &code)) == PROVEN_OK) {
                if (setting.id == PROVEN_H2_SETTINGS_INITIAL_WINDOW_SIZE) printf("the client's window: %u bytes a stream\n", (unsigned)setting.value);
                /* 이쪽이 모르는 설정은 그저 따르지 않는다. */
            }
            EXAMPLE_REQUIRE(err == PROVEN_ERR_EOF, "every setting was in range");
        } else if (frame.type == PROVEN_H2_HEADERS) {
            /* 헤더 블록은 패딩과 옛 우선순위 필드를 걷어 낸 나머지다. (END_HEADERS가 꺼져
             * 있으면 CONTINUATION 프레임들이 나머지를 싣는다.) */
            proven_mem_view_t block;
            EXAMPLE_REQUIRE(proven_h2_frame_content(&frame, payload, &block) == PROVEN_OK && (frame.flags & PROVEN_H2_FLAG_END_HEADERS) != 0, "a whole header block");
            proven_http_header_t fields[32];
            proven_byte_t scratch[2048];
            proven_size_t count = 0;
            EXAMPLE_REQUIRE(proven_hpack_decode(decoder, block, fields, 32, (proven_mem_mut_t){ scratch, sizeof scratch }, 16384, &count) == PROVEN_OK, "decoded");

            /* 필드 목록이 아직 요청인 것은 아니다. 대문자가 든 이름, 값 안의 줄바꿈, 빠진
             * :path, "connection" 헤더가 여기서 걸린다. */
            proven_h2_request_head_t head;
            EXAMPLE_REQUIRE(proven_h2_request_head(fields, count, &head) == PROVEN_OK, "a well-formed request");
            printf("stream %u: %.*s %.*s for %.*s, %u more fields\n", (unsigned)frame.stream_id, (int)head.method.size, (const char *)head.method.ptr,
                   (int)head.path.size, (const char *)head.path.ptr, (int)head.authority.size, (const char *)head.authority.ptr, (unsigned)head.field_count);
            EXAMPLE_REQUIRE(proven_u8str_view_eq(head.path, PROVEN_LIT("/")) && head.field_count == 1 && proven_u8str_view_eq(head.fields[0].name, PROVEN_LIT("accept")), "its parts");
            requests++;
        }
        at += PROVEN_H2_FRAME_HEADER_SIZE + frame.length;
    }
    EXAMPLE_REQUIRE(frames == 2 && requests == 1 && refused_with == PROVEN_H2_FRAME_SIZE_ERROR, "two frames taken, one request, and the third frame refused with FRAME_SIZE_ERROR");
    proven_hpack_decoder_destroy(decoder);

    // ---- 서버가 써 보내는 것 -------------------------------------------------------
    proven_byte_t reply[256];
    proven_mem_mut_t out = { reply, sizeof reply };
    proven_size_t len = 0;
    static const proven_byte_t opaque[8] = { 'p', 'r', 'o', 'v', 'e', 'n', '!', '!' };
    EXAMPLE_REQUIRE(proven_h2_write_settings(out, &len, NULL, 0) == PROVEN_OK &&       /* 자기 설정: 모두 기본값 */
                    proven_h2_write_settings_ack(out, &len) == PROVEN_OK &&            /* "그쪽 설정이 적용되었다" */
                    proven_h2_write_window_update(out, &len, 0, 1 << 20) == PROVEN_OK &&   /* 연결에 1메가바이트를 더 보내도 된다 */
                    proven_h2_write_ping(out, &len, opaque, false) == PROVEN_OK &&     /* 거기 있는가? */
                    proven_h2_write_rst_stream(out, &len, 3, PROVEN_H2_REFUSED_STREAM) == PROVEN_OK &&   /* 그 스트림은 지금은 안 된다 */
                    proven_h2_write_goaway(out, &len, 1, PROVEN_H2_FRAME_SIZE_ERROR, proven_mem_view_from_u8(PROVEN_LIT("a PING is eight bytes"))) == PROVEN_OK,
                    "six frames a server might send");

    /* 클라이언트가 하듯이 다시 읽는다. */
    at = 0;
    int seen = 0;
    while (at < len) {
        proven_h2_frame_t frame;
        proven_h2_error_t code;
        bool stream_error;
        EXAMPLE_REQUIRE(proven_h2_frame_parse((proven_mem_view_t){ reply + at, len - at }, &frame) == PROVEN_OK &&
                        proven_h2_frame_check(&frame, PROVEN_H2_MIN_MAX_FRAME_SIZE, &code, &stream_error) == PROVEN_OK, "each passes its own rule");
        proven_mem_view_t payload = { reply + at + PROVEN_H2_FRAME_HEADER_SIZE, frame.length };
        proven_u32 a = 0, b = 0;
        proven_mem_view_t debug;
        if (frame.type == PROVEN_H2_WINDOW_UPDATE) EXAMPLE_REQUIRE(proven_h2_read_window_update(payload, &a) == PROVEN_OK && a == 1 << 20, "the increment");
        if (frame.type == PROVEN_H2_RST_STREAM) EXAMPLE_REQUIRE(proven_h2_read_rst_stream(payload, &a) == PROVEN_OK && a == PROVEN_H2_REFUSED_STREAM && frame.stream_id == 3, "the reset");
        if (frame.type == PROVEN_H2_GOAWAY) EXAMPLE_REQUIRE(proven_h2_read_goaway(payload, &a, &b, &debug) == PROVEN_OK && a == 1 && b == PROVEN_H2_FRAME_SIZE_ERROR && debug.size == 21, "the goodbye");
        seen++;
        at += PROVEN_H2_FRAME_HEADER_SIZE + frame.length;
    }
    EXAMPLE_REQUIRE(seen == 6, "six frames read back");

    // ---- 응답과 트레일러 -----------------------------------------------------------
    proven_http_header_t response[] = { { PROVEN_LIT(":status"), PROVEN_LIT("200") }, { PROVEN_LIT("content-type"), PROVEN_LIT("text/html") } };
    proven_u16 status = 0;
    const proven_http_header_t *rest = NULL;
    proven_size_t rest_count = 0;
    EXAMPLE_REQUIRE(proven_h2_response_head(response, 2, &status, &rest, &rest_count) == PROVEN_OK && status == 200 && rest_count == 1, "a response has one pseudo-header field: its status");
    proven_http_header_t smuggle[] = { { PROVEN_LIT(":status"), PROVEN_LIT("200") }, { PROVEN_LIT("x-note"), PROVEN_LIT("fine\r\ntransfer-encoding: chunked") } };
    EXAMPLE_REQUIRE(proven_h2_response_head(smuggle, 2, &status, &rest, &rest_count) == PROVEN_ERR_INVALID_FORMAT, "a value with a line break in it is no response at all");
    proven_http_header_t trailers[] = { { PROVEN_LIT("x-checksum"), PROVEN_LIT("9f86d081") } };
    EXAMPLE_REQUIRE(proven_h2_trailers_check(trailers, 1) == PROVEN_OK, "trailers are ordinary fields");
    return EXAMPLE_OK();
}
```

### 참조

| API | 의도 | 반환 |
|---|---|---|
| `proven_h2_frame_parse(data, &frame)` | 프레임 헤더 아홉 바이트를 읽는다. | `proven_err_t`: 아홉 바이트 미만이면 `NEED_MORE`. |
| `proven_h2_frame_write(out, &len, &frame)` | 프레임 헤더를 덧붙인다. | `proven_err_t`: `INVALID_ARG`(길이가 2^24-1 초과, 스트림이 2^31-1 초과), `OUT_OF_BOUNDS`. |
| `proven_h2_frame_check(&frame, max_frame_size, &code, &stream_error)` | 위의 표를 적용한다. | `proven_err_t`: `INVALID_FORMAT`과 함께 보낼 코드, 그리고 스트림이 끝나는지 연결이 끝나는지. |
| `proven_h2_frame_content(&frame, payload, &content)` | 패딩과 우선순위를 뺀 DATA나 HEADERS의 내용. | `proven_err_t`: 맞지 않는 패딩은 `INVALID_FORMAT`(연결 오류, `PROTOCOL_ERROR`). |
| `proven_h2_settings_next(payload, &pos, &setting, &code)` | 다음 설정을 읽는다. | `proven_err_t`: 끝에서 `EOF`. 범위를 벗어난 값은 코드와 함께 `INVALID_FORMAT`. |
| `proven_h2_write_settings`, `_settings_ack`, `_ping`, `_window_update`, `_rst_stream`, `_goaway` | 프레임 하나를 통째로 덧붙인다. | `proven_err_t`: 아무것도 쓰지 않은 채 `OUT_OF_BOUNDS`. |
| `proven_h2_read_window_update`, `_rst_stream`, `_goaway` | 검사를 통과한 프레임의 페이로드를 읽는다. | `proven_err_t`. 윈도 증가분 0은 `INVALID_FORMAT`. |

### 주의사항, 그리고 무엇이 잘못되는가

- **읽기 전에 검사한다.** `proven_h2_frame_parse`는 아홉 바이트에서 실패할 수 없다. 그 바이트들이
  있어도 되는 프레임을 말하는지는 검사가 답한다. 여러분이 알린 크기보다 긴 프레임은 무엇이라고 주장하든
  연결 오류다.
- **흐름 제어는 페이로드 전체를 센다.** `proven_h2_frame_content`는 패딩된 DATA 프레임의 내용이 어디
  있는지 알려 준다. 윈도에서는 패딩까지 차감된다.
- **헤더 블록은 여러 프레임에 걸칠 수 있고**, 그 사이에 다른 것은 올 수 없다: `END_HEADERS`가 없는
  HEADERS 뒤에는 같은 스트림의 CONTINUATION 프레임이, 그 플래그가 나올 때까지 이어진다. 모으는 양에
  한도를 둘 것 - 헤더 목록 한도의 두 배면 정직한 블록에는 충분하다 - 그러지 않으면 상대가 보내고 싶은
  만큼을 쥐게 된다.
- **모르는 것은 틀린 것이 아니다.** 이 라이브러리가 모르는 프레임 종류나 설정은, 프로토콜이 요구하는
  대로 검사를 통과한다. 건너뛴다.
- **설정에는 범위가 있고**, 벗어난 값은 연결 오류다: `ENABLE_PUSH`는 0 또는 1, 초기 윈도는 많아야
  2^31-1, 프레임 크기는 16,384에서 2^24-1까지.

## 4. 메시지로서의 헤더 목록

```c
proven_err_t proven_h2_request_head(const proven_http_header_t *headers, proven_size_t count, proven_h2_request_head_t *out);
proven_err_t proven_h2_response_head(const proven_http_header_t *headers, proven_size_t count, proven_u16 *status,
                                     const proven_http_header_t **fields, proven_size_t *field_count);
proven_err_t proven_h2_trailers_check(const proven_http_header_t *headers, proven_size_t count);
```

디코딩된 헤더 목록은 바이트 문자열의 목록이다. 그것이 요청인가는 별개의 질문이고, 그 답은 HTTP/2가
HTTP/1.1과 만나는 곳에서 가장 중요하다: 줄바꿈이 든 값이 텍스트로 전달되면 헤더 두 줄이 된다 - 받는
서버가 도착하는 것을 본 적 없는 요청이다. 그래서 다음과 같은 목록은 *잘못된 형식*이며, 고치거나
전달하지 말고 거절해야 한다:

| 규칙 | 까닭 |
|---|---|
| 필드 이름에 대문자, 공백, 제어 바이트, `0x7e`보다 큰 바이트가 있거나, 맨 앞이 아닌 곳에 콜론이 있다 | 이름은 소문자 토큰이다. 콜론은 유사 헤더 필드를 시작할 뿐이다 |
| 값에 NUL, CR, LF가 있거나, 값이 공백이나 탭으로 시작하거나 끝난다 | 이것들은 HTTP/1.1의 구분자다 |
| 유사 헤더 필드가 보통 필드 뒤에 오거나, 두 번 나오거나, 아무도 정의하지 않은 것이다 | 그것들은 메시지의 첫 줄이다: 한 번, 맨 먼저 |
| 요청에 `:method`, `:scheme`, `:path`가 없다. `http`나 `https`인데 `:path`가 비었다. 요청에 `:status`가 있다 | 첫 줄은 온전해야 한다. (`CONNECT`에는 `:method`와 `:authority`만 있다) |
| 응답에 `:status`가 없거나 세 자리 숫자가 아니다 | 위와 같다 |
| `connection`, `proxy-connection`, `keep-alive`, `transfer-encoding`, `upgrade`가 있다. `te`가 `trailers`가 아닌 것을 말한다 | 이것들은 HTTP/1.1 연결을 기술한다. 여기서는 남의 연결을 기술하려는 시도다 |
| 트레일러에 유사 헤더 필드가 있다 | 트레일러는 보통 필드다 |

`proven_h2_request_head`는 이 규칙들을 적용하고 부분들을 돌려준다: 메서드, 스킴, authority, 경로,
그리고 뒤따르는 보통 필드들. 잘못된 형식의 메시지는 `PROTOCOL_ERROR` 종류의 스트림 오류다: 응답을 아직
보낼 수 있으면 `400`으로 답하고, 아니면 스트림을 리셋한다.

## 5. 무엇을 받아들이고, 무엇이 없으며, 어떻게 시험했는가

**디코더가 받아들이는 것은 RFC 7541이 정의한 HPACK이다.** 블록 맨 앞의 테이블 크기 갱신은 몇 개든
(RFC는 많아야 둘이라고 약속한다), 정수는 연속 바이트 다섯 개까지, 리터럴의 허프만 패딩은 1 비트 일곱
개까지이고 그 밖의 것은 안 된다.

**인코더가 쓰는 것**은 흔한 선택을 하는 유효한 HPACK이다: 테이블에 있으면 참조로, 허프만 코딩은 더 짧을
때만(길이가 같으면 하지 않는다), 새 필드는 비밀이거나 테이블보다 크지 않으면 테이블에 넣는다.

**여기에 없는 것:**

- **연결.** 스트림 상태, 흐름 제어 윈도, 프레임의 순서, 설정과 그 승인, 무엇에 무엇으로 답하는가:
  어느 것도 없다. 이 라이브러리의 HTTP 서버와 클라이언트는 HTTP/1.1만 말한다.
- **서버 푸시**: `PUSH_PROMISE`는 거절할 수 있도록 알아보기만 한다.
- **우선순위**(RFC 9113이 폐기한 RFC 7540의 방식): 건너뛴다.
- RFC 9113이 없앤 **`Upgrade: h2c`**, **HTTP/2 위의 WebSocket**(RFC 8441), **HTTP/3**.
- **디코딩된 필드의 never-indexed 표시**: 프록시라면 그것을 전달하겠지만, 여기에는 그럴 것이 없다.

**어떻게 시험했는가.** 정적 테이블과 허프만 코드는 손으로 입력한 것이 아니다: 비공개 프로그램이 RFC
7541의 공개된 본문에서 읽어 오며, 항목이 61개이고 코드 257개가 서로 접두사가 아니며 코드 공간을 정확히
채우지 않으면 멈춘다. 등록된 시험은 링크된 테이블에 대해 같은 것을 검사한다. 그 시험은 이어서 RFC 부록
C의 예제 열여섯 개를 모두 디코더에 통과시키며 단계마다 동적 테이블을 RFC의 것과 비교하고, 인코더가
같은 선택을 하는 예제를 바이트 단위로 재현하게 하고, 잘못된 블록 열세 종류를 거부하고, 각 한도를
지키고, RFC 블록의 모든 앞부분과 모든 한 비트 변경을 디코딩한다. 두 번째 시험은 프레임 종류마다의
규칙을 경계의 양쪽에서, 그리고 잘못된 요청 서른한 개를 시험한다. 등록된 시험 밖에서는: 이 디코더가
다른 구현 열네 개가 같은 헤더 목록에 대해 만든 블록 47,142개를 읽었고(nghttp2, Go, Node, Python,
Haskell, Swift 인코더. 테이블 크기가 바뀌는 순서 포함), Node의 HTTP/2 서버 - nghttp2다 - 가 이 인코더가
공유 연결 위의 요청 1,290개에 대해 쓴 블록을 모두 디코딩했으며, 손상된 블록 오백만 개를 버퍼 밖의
읽기나 쓰기를 보고하는 도구 아래에서 디코딩했고 보고된 것은 없었다.

**하지 않은 것:** 커버리지 기반 퍼징 없음. 속도 측정 없음(허프만 디코더는 한 번에 한 비트를 읽는다).
외부 검토 없음.
