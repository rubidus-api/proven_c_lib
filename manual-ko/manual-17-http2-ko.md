# 17장: HTTP/2 - 프레임과 헤더 압축

**5부 — 운영체제와 대화하기. 선행 조건: 요청과 응답이 무엇인지는 [10장](manual-10-http-ko.md), 압축
테이블을 만들 때 쓰는 할당자(allocator) - 예제에서는 힙(heap) 할당자 - 는
[2장](manual-02-allocation-ko.md).**
**이 장을 마치면** HTTP/2가 무엇을 바꾸고 무엇을 바꾸지 않는지 말할 수 있고, 헤더 블록을 인코딩하고
디코딩하며, 프레임을 읽고 쓰고, 잘 만들어진 요청과 전달해서는 안 되는 요청을 가려내고, 직접 실어
나르는 바이트 위에서 연결 하나 - 스트림, 윈도, 리셋, 작별 - 를 몰 수 있다. 그리고 이 가운데 이
라이브러리가 지금 하는 것이 어디까지인지 말할 수 있다.

이 장은 `hpack.h`와 `h2.h`를 다룬다. 둘 다 순수한 계산이고
[프리스탠딩(freestanding)](manual-freestanding-ko.md) 빌드에서 쓸 수 있다: HPACK은 인코더에 테이블
하나, 디코더에 테이블 하나를 할당하고, 프레임 함수는 아무것도 할당하지 않으며, 연결은 설정이 정한
한도 안에서 할당한다. 입력과 출력은 여러분의 메모리를 가리키는 뷰(view)로 주고받는다.

**여기 있는 것은 소켓 없는 프로토콜이다.** 2절부터 4절까지는 코덱이다: HTTP/2 연결 위를 오가는 것을
읽고 쓴다. 5절은 연결 그 자체다 - 어떤 스트림이 열려 있는지, 얼마나 보낼 수 있는지, 무엇에 무엇으로
답하는지 - 바이트를 받고 바이트를 돌려주는 객체로서. 여기 있는 어느 것도 소켓을 열지 않으며, 11장과
15장의 서버와 클라이언트는 아직 HTTP/1.1로 말한다. 그 선이 정확히 어디인지는 6절에 있다.

## 목차

1. [HTTP/2가 바꾸는 것, 한 쪽으로](#1-http2가-바꾸는-것-한-쪽으로)
2. [헤더 압축](#2-헤더-압축)
3. [프레임](#3-프레임)
4. [메시지로서의 헤더 목록](#4-메시지로서의-헤더-목록)
5. [연결](#5-연결)
6. [무엇을 받아들이고, 무엇이 없으며, 어떻게 시험했는가](#6-무엇을-받아들이고-무엇이-없으며-어떻게-시험했는가)

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

## 5. 연결

```c
proven_err_t proven_h2_conn_create(const proven_h2_config_t *config, proven_h2_conn_t **out);
void proven_h2_conn_destroy(proven_h2_conn_t *conn);

proven_err_t proven_h2_conn_feed(proven_h2_conn_t *conn, proven_mem_view_t in, proven_size_t *consumed, proven_h2_event_t *event);
proven_mem_view_t proven_h2_conn_pending_output(const proven_h2_conn_t *conn);
void proven_h2_conn_output_sent(proven_h2_conn_t *conn, proven_size_t n);
```

`proven_h2_conn_t`는 어느 쪽 끝이든 될 수 있는 HTTP/2 연결 하나다. 2절부터 4절까지가 여러분에게 남겨
두었던 것을 모두 쥐고 있다: 양쪽의 설정, 열려 있는 스트림, 양방향의 윈도, 압축 테이블 둘, 그리고
프레임마다 프로토콜이 요구하는 답. **입출력을 하지 않고 시계도 읽지 않는다.** 모양은 14장의 TLS 엔진과
같다:

```text
   상대가 보낸 바이트  -->  proven_h2_conn_feed  -->  사건 하나: 요청, 본문의 한 조각, 리셋, ...
   여러분의 요청, 응답, 본문  -->  proven_h2_conn_submit_* / _send_data
   proven_h2_conn_pending_output  -->  상대에게 갈 바이트  -->  proven_h2_conn_output_sent
```

소켓, TLS, 모든 타임아웃은 호출자의 것이다. 그래서 객체 하나가 블로킹 프로그램에도, 이벤트 루프에도,
네트워크가 전혀 없는 시험에도 쓰인다 - 아래 예제는 서로 버퍼를 건네는 연결 둘이다.

### 사건

`proven_h2_conn_feed`는 알릴 사건이 **하나** 생길 때까지 `in`의 바이트를 쓰고, 몇 바이트를 썼는지
알려 준다. 나머지를 가지고 다시 호출한다. `PROVEN_H2_EVENT_NONE`은 모두 썼다는 뜻이다.

| 사건 | 뜻 | 필드 |
|---|---|---|
| `PROVEN_H2_EVENT_HEADERS` | 서버: 요청이 시작되었다. 이미 4절의 규칙으로 검사되고 나뉘어 있다. 클라이언트: 응답의 머리(`status`가 200 미만이면 중간 응답이고, 더 온다) | `stream_id`, `request` 또는 `status`와 `fields`, `end_stream`, `user` |
| `PROVEN_H2_EVENT_DATA` | 본문의 한 조각. `in` 안을 가리키는 뷰다 | `stream_id`, `data`, `end_stream`, `user` |
| `PROVEN_H2_EVENT_TRAILERS` | 트레일러 필드: 상대 쪽의 스트림은 끝났다 | `stream_id`, `fields`, `user` |
| `PROVEN_H2_EVENT_STREAM_RESET` | 스트림이 끝나기 전에 끝났다: 상대가 리셋했거나, 그 스트림에서 규칙을 어겼거나, 처리하지 않은 채 작별했다 | `stream_id`, `code`, `user` |
| `PROVEN_H2_EVENT_STREAM_WRITABLE` | 잘렸던 전송을 이어 갈 수 있다 | `stream_id`, `user` |
| `PROVEN_H2_EVENT_PONG` | `proven_h2_conn_ping`의 답 | `data`: 그 여덟 바이트 |
| `PROVEN_H2_EVENT_GOAWAY` | 상대가 새 스트림을 받지 않는다 | `code`, `last_stream_id` |
| `PROVEN_H2_EVENT_CLOSED` | 연결이 끝났고, 모든 스트림도 함께 끝났다. 남은 출력을 보내고 닫는다 | `code` |

이 표에서 세 가지는 다시 볼 만하다.

**본문은 복사되지 않는다.** DATA 사건은 여러분이 넣어 준 버퍼 안을 가리키므로, 어떤 크기의 본문이든
여러분의 읽기 한 번보다 크지 않은 조각으로 지나간다. 세 번의 읽기에 걸쳐 도착한 프레임은 사건 셋이고,
`end_stream`은 마지막 것에 붙는다.

**상대의 잘못은 오류가 아니라 사건이다.** 상대가 프로토콜을 어겨도 `proven_h2_conn_feed`는
`PROVEN_OK`를 돌려준다: 연결은 코드를 담은 GOAWAY 프레임을 쌓아 두고 `PROVEN_H2_EVENT_CLOSED`를
알린다. 스트림 하나의 잘못은 그 스트림의 `PROVEN_H2_EVENT_STREAM_RESET`일 뿐이다. 상대가 한 일 때문에
feed가 돌려주는 오류는 하나, `PROVEN_ERR_INVALID_FORMAT`이다: 서버인데 클라이언트가 24바이트 서문으로
시작하지 않았을 때. **그것은 HTTP/2가 아니다.** 상대에게 아무 말도 하지 않으며, 달라지는 첫 바이트에서
알린다 - 그래서 그 바이트들은 여전히 여러분의 것이고 HTTP/1.1 파서에 넘길 수 있다.

**입력이 필요 없는 사건이 있다.** 스트림이 다시 쓸 수 있게 되는 것은 무언가 도착했을 때가 아니라 출력이
*나갔을* 때다. 그러니 `proven_h2_conn_output_sent` 뒤에는 `proven_h2_conn_feed`를 빈 뷰로 한 번 더
불러 생긴 사건을 받는다.

### 보내기

```c
proven_err_t proven_h2_conn_submit_request(proven_h2_conn_t *conn, const proven_http_header_t *headers, proven_size_t count,
                                           bool end_stream, proven_u32 *stream_id);
proven_err_t proven_h2_conn_submit_response(proven_h2_conn_t *conn, proven_u32 stream_id, const proven_http_header_t *headers, proven_size_t count,
                                            bool end_stream);
proven_result_size_t proven_h2_conn_send_data(proven_h2_conn_t *conn, proven_u32 stream_id, proven_mem_view_t data, bool end_stream);
proven_err_t proven_h2_conn_send_trailers(proven_h2_conn_t *conn, proven_u32 stream_id, const proven_http_header_t *headers, proven_size_t count);
proven_err_t proven_h2_conn_reset(proven_h2_conn_t *conn, proven_u32 stream_id, proven_h2_error_t code);
```

클라이언트는 의사 헤더 필드 넷으로 시작하는 헤더 목록으로 스트림을 열고, 서버는 `:status`로 시작하는
목록으로 스트림에 답한다. 둘 다 한 바이트라도 쌓이기 전에 4절의 규칙으로 검사되므로, 이 라이브러리는
자기가 거부할 것을 보내지 않는다.

`proven_h2_conn_send_data`는 **내민 것보다 적게 가져갈 수 있고** 전혀 가져가지 않을 수도 있으며,
그것은 오류가 아니다. 가져가는 양은 세 가지 가운데 가장 작은 것이다: 스트림의 윈도, 연결의 윈도,
`max_buffered_output` 아래에 남은 자리. 나머지는 쥐고 있다가 `PROVEN_H2_EVENT_STREAM_WRITABLE`이 그
스트림을 가리키면 다시 내민다. `end_stream`은 내민 것이 모두 받아들여졌을 때 효력이 생긴다.

### 받는 쪽의 흐름 제어

```c
void proven_h2_conn_consumed(proven_h2_conn_t *conn, proven_u32 stream_id, proven_size_t n);
```

상대는 이쪽이 받겠다고 한 만큼만 보낼 수 있다: 스트림마다 `initial_window` 바이트, 연결 전체로도 그만큼.
**자리는 바이트가 도착했을 때가 아니라 처리했다고 말할 때 돌아온다** - `proven_h2_conn_consumed`로.
뒤처져서 이 호출을 멈춘 처리기는 많아야 윈도 하나만큼을 버퍼에 둔 채 상대를 멈춘다. 이것이 HTTP/2의
역압(backpressure)이고, 이 호출이 말 그대로일 때만 통한다: 바이트를 건네받았을 때가 아니라 가야 할
곳으로 보냈을 때 부른다.

아무도 소비하지 않는 것은 연결이 스스로 돌려준다: 패딩, 리셋된 스트림으로 온 데이터, 그리고 스트림이
끝날 때 아직 소비되지 않았던 것.

### 상대가 연결에 쥐게 하거나 하게 할 수 있는 것

모든 양에는 설정이 있고, 모든 설정에는 기본값이 있다:

| 설정 | 기본값 | 경계에서 |
|---|---|---|
| `max_concurrent_streams` | 100 | 넘는 스트림은 거절되고(`REFUSED_STREAM`) 알려지지 않는다 |
| `initial_window` | 65,535 | 스트림의 윈도를 넘는 DATA는 그 스트림의 `FLOW_CONTROL_ERROR`, 연결의 윈도를 넘으면 연결의 오류 |
| `max_frame_size` | 16,384 | 더 큰 프레임은 연결의 `FRAME_SIZE_ERROR` |
| `max_header_list_bytes` | 16 KiB | 더 큰 목록은 그 스트림의 `PROTOCOL_ERROR`. 이 값의 두 배를 넘는 헤더 블록은 연결을 끝낸다 |
| `header_table_bytes` | 4,096 | 방향마다의 HPACK 테이블. `set_header_table`과 0이면 없음 |
| `max_buffered_output` | 64 KiB | 얼마가 나갈 때까지 본문을 더 받지 않고 `submit` 호출은 `PROVEN_ERR_BUSY`로 답한다. 따로 센다: 읽지 않는 상대에게 프로토콜이 빚진 답이 이만큼을 넘으면 연결이 끝난다 |
| `max_reset_debt` | 100 | 리셋된 스트림 - 상대가 했거나 상대의 잘못 때문에 - 이 제대로 끝난 스트림보다 이만큼 많은 뒤: 하나 더 오면 `ENHANCE_YOUR_CALM`으로 연결이 끝난다 |

마지막 줄은, 서버가 일을 시작하게 해 놓고 버리게 하려고 스트림을 열자마자 리셋하기를 되도록 빨리
되풀이하는 클라이언트에 대한 답이다. 시계는 쓰지 않는다: 리셋 하나가 하나를 더하고, 제대로 끝난 스트림
하나가 하나를 덜며, 시작한 것을 대체로 끝내는 상대는 한도 근처에도 가지 않는다.

막 열린 연결은 많아야 400바이트인 블록 하나를 쥔다. 요청을 실어 나른 뒤 다시 한가해진 연결은 그것과,
마지막 스트림들이 어떻게 끝났는지의 기록과, 압축 테이블 둘을 쥔다: 기본 테이블로 8 KiB 미만, 테이블
없이는 1,280바이트 미만. 스트림이 열려 있는 동안에는 스트림마다 48바이트와, 헤더 목록 하나를 읽는 데
쓰는 버퍼 - `max_header_list_bytes`의 다섯 배 - 를 더 쥔다.

### 작별

```c
void proven_h2_conn_shutdown(proven_h2_conn_t *conn, proven_h2_error_t code);
proven_err_t proven_h2_conn_ping(proven_h2_conn_t *conn, const proven_byte_t opaque[8]);
bool proven_h2_conn_is_closed(const proven_h2_conn_t *conn);
```

`PROVEN_H2_NO_ERROR`로 부른 `proven_h2_conn_shutdown`은 GOAWAY 프레임을 쌓아 두고 열려 있는 스트림이
끝나게 둔다: 새 스트림은 받지도 시작하지도 않으며, 마지막 스트림이 끝나면 `PROVEN_H2_EVENT_CLOSED`가
따라온다. 상대의 GOAWAY도 같은 방식으로 처리되며, 클라이언트는 서버가 처리하지 않은 스트림이 어느 것인지
듣는다 - `PROVEN_H2_REFUSED_STREAM`이 담긴 `PROVEN_H2_EVENT_STREAM_RESET` - 그것들은 다른 연결에서 다시
보내도 안전하기 때문이다.

### 예제: 사이에 소켓이 없는, 연결의 양쪽 끝

<!-- example: manual/examples/ko/ex_17_conn.c -->
```c
#include <stdio.h>
#include <string.h>

/*
 * 소켓 없이 양쪽 끝을 모두 가진 HTTP/2 연결: 한쪽이 쌓아 둔 것을 마치 선을 건너온 것처럼
 * 다른 쪽에 넘겨준다.
 *
 * proven_h2_conn_t는 입출력을 하지 않는다. 상대가 보낸 바이트를 넣어 주면 사건을 한 번에
 * 하나씩 알려 주고, 보내 달라고 하면 프레임을 쌓아 두어 가져가게 한다. 실제 프로그램은
 * carry()가 있는 자리에 소켓(과 TLS)을 둔다.
 */

typedef struct {
    proven_h2_conn_t *conn;
    const char *name;
    /* 이쪽 끝이 알게 된 것. 아래의 검사에 쓴다. */
    int requests, responses, trailers, resets, writable, pongs, goaways, closed;
    proven_size_t body_bytes;
    proven_u32 last_stream, reset_code, close_code;
    bool hold;                             /* DATA를 소비했다고 알리지 않는다 */
} end_t;

/* `in`의 바이트에 든 사건을 모두 받는다. 한 번 호출에 사건 하나다: 사건도 없고 남은 것도 없을
 * 때까지 나머지를 가지고 다시 호출한다. */
static bool take_events(end_t *e, proven_mem_view_t in) {
    proven_size_t pos = 0;
    for (;;) {
        proven_size_t used = 0;
        proven_h2_event_t ev;
        if (proven_h2_conn_feed(e->conn, (proven_mem_view_t){ in.ptr + pos, in.size - pos }, &used, &ev) != PROVEN_OK) return false;
        pos += used;
        switch (ev.kind) {
            case PROVEN_H2_EVENT_NONE:
                if (pos == in.size) return true;           /* 다 썼다: 더 읽어 온다 */
                break;
            case PROVEN_H2_EVENT_HEADERS:
                e->last_stream = ev.stream_id;
                if (ev.status == 0) {                      /* 서버: 이미 검사되고 나뉜 요청 */
                    e->requests++;
                    printf("%s: stream %u asks %.*s %.*s\n", e->name, (unsigned)ev.stream_id, (int)ev.request.method.size, (const char *)ev.request.method.ptr,
                           (int)ev.request.path.size, (const char *)ev.request.path.ptr);
                } else {                                   /* 클라이언트: 응답의 머리 */
                    e->responses++;
                    printf("%s: stream %u answers %u%s\n", e->name, (unsigned)ev.stream_id, (unsigned)ev.status, ev.user ? " (the stream we marked)" : "");
                }
                break;
            case PROVEN_H2_EVENT_DATA:
                /* ev.data는 `in` 안을 가리킨다: 복사된 것이 없다. 소비했다고 알려야 상대가
                 * 더 보낼 수 있다. */
                e->body_bytes += ev.data.size;
                if (!e->hold) proven_h2_conn_consumed(e->conn, ev.stream_id, ev.data.size);
                break;
            case PROVEN_H2_EVENT_TRAILERS: e->trailers++; break;
            case PROVEN_H2_EVENT_STREAM_RESET: e->resets++; e->reset_code = ev.code; break;
            case PROVEN_H2_EVENT_STREAM_WRITABLE: e->writable++; break;
            case PROVEN_H2_EVENT_PONG: e->pongs++; break;
            case PROVEN_H2_EVENT_GOAWAY: e->goaways++; break;
            case PROVEN_H2_EVENT_CLOSED: e->closed++; e->close_code = ev.code; return true;
        }
    }
}

/* 선: `from`이 쌓아 둔 것이 `to`로 간다. 그런 다음 `from`에게 빈 입력으로 한 번 더 묻는다 -
 * 보낸 덕분에 생긴 사건, 곧 다시 보낼 수 있게 된 스트림이 있는지. */
static bool carry(end_t *from, end_t *to) {
    static proven_byte_t wire[200000];
    proven_mem_view_t out = proven_h2_conn_pending_output(from->conn);
    if (out.size > sizeof wire) return false;
    if (out.size > 0) memcpy(wire, out.ptr, out.size);
    proven_size_t n = out.size;
    proven_h2_conn_output_sent(from->conn, n);             /* 소켓이라면 몇 바이트를 받았는지 알려 줄 것이다 */
    if (!to->closed && !take_events(to, (proven_mem_view_t){ wire, n })) return false;
    return from->closed || take_events(from, (proven_mem_view_t){ wire, 0 });
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    end_t client = { .name = "client" }, server = { .name = "server" };

    /* 0은 모든 한계에서 기본값을 뜻한다. 꼭 말해야 하는 것은 `server` 하나다. */
    proven_h2_config_t ccfg = { .alloc = heap };
    proven_h2_config_t scfg = { .alloc = heap, .server = true, .max_concurrent_streams = 10 };
    EXAMPLE_REQUIRE(proven_h2_conn_create(&ccfg, &client.conn) == PROVEN_OK && proven_h2_conn_create(&scfg, &server.conn) == PROVEN_OK, "two ends");

    /* 여는 말은 이미 쌓여 있다: 클라이언트의 서문과 양쪽의 설정. */
    EXAMPLE_REQUIRE(carry(&client, &server) && carry(&server, &client) && carry(&client, &server), "settings cross and are acknowledged");

    // ---- 본문이 있는 요청과 그 응답 ----------------------------------------------------------
    proven_http_header_t request[] = {
        { PROVEN_LIT(":method"), PROVEN_LIT("POST") }, { PROVEN_LIT(":scheme"), PROVEN_LIT("https") },
        { PROVEN_LIT(":authority"), PROVEN_LIT("www.example.com") }, { PROVEN_LIT(":path"), PROVEN_LIT("/upload") },
        { PROVEN_LIT("content-type"), PROVEN_LIT("text/plain") },
    };
    proven_u32 stream = 0;
    int mark = 0;
    EXAMPLE_REQUIRE(proven_h2_conn_submit_request(client.conn, request, 5, false, &stream) == PROVEN_OK && stream == 1, "a client's first stream is 1");
    proven_h2_conn_stream_set_user(client.conn, stream, &mark);       /* 이 스트림의 사건에 실려 온다 */
    EXAMPLE_REQUIRE(proven_h2_conn_stream_user(client.conn, stream) == &mark, "and it can be asked for");
    proven_result_size_t sent = proven_h2_conn_send_data(client.conn, stream, proven_mem_view_from_u8(PROVEN_LIT("hello, ")), false);
    EXAMPLE_REQUIRE(sent.err == PROVEN_OK && sent.value == 7, "seven bytes of body");
    sent = proven_h2_conn_send_data(client.conn, stream, proven_mem_view_from_u8(PROVEN_LIT("world")), true);     /* true: 본문의 끝 */
    EXAMPLE_REQUIRE(sent.err == PROVEN_OK && sent.value == 5 && carry(&client, &server), "and five more, the last");
    EXAMPLE_REQUIRE(server.requests == 1 && server.body_bytes == 12 && proven_h2_conn_stream_count(server.conn) == 1, "the server has the request and its twelve bytes");

    proven_http_header_t response[] = { { PROVEN_LIT(":status"), PROVEN_LIT("200") }, { PROVEN_LIT("content-type"), PROVEN_LIT("text/plain") } };
    proven_http_header_t trailers[] = { { PROVEN_LIT("x-checksum"), PROVEN_LIT("9f86d081") } };
    EXAMPLE_REQUIRE(proven_h2_conn_submit_response(server.conn, stream, response, 2, false) == PROVEN_OK, "a response head");
    sent = proven_h2_conn_send_data(server.conn, stream, proven_mem_view_from_u8(PROVEN_LIT("thanks")), false);
    EXAMPLE_REQUIRE(sent.err == PROVEN_OK && sent.value == 6 && proven_h2_conn_send_trailers(server.conn, stream, trailers, 1) == PROVEN_OK, "a body, and trailers to end it");
    EXAMPLE_REQUIRE(carry(&server, &client), "back to the client");
    EXAMPLE_REQUIRE(client.responses == 1 && client.body_bytes == 6 && client.trailers == 1, "the client has all three parts");
    EXAMPLE_REQUIRE(proven_h2_conn_stream_count(client.conn) == 0 && proven_h2_conn_stream_count(server.conn) == 0, "and the stream is over at both ends");

    // ---- 흐름 제어: 뒤처진 수신자는 송신자를 멈춘다 -------------------------------------------
    static proven_byte_t big[100000];
    server.hold = true;                                    /* 서버의 처리기가 바쁘다: 아무것도 소비하지 않는다 */
    server.body_bytes = 0;
    EXAMPLE_REQUIRE(proven_h2_conn_submit_request(client.conn, request, 5, false, &stream) == PROVEN_OK && stream == 3 && carry(&client, &server), "a second request");
    sent = proven_h2_conn_send_data(client.conn, stream, (proven_mem_view_t){ big, sizeof big }, true);
    const proven_size_t taken = sent.value;
    printf("client: offered %u bytes, %u were taken\n", (unsigned)sizeof big, (unsigned)taken);
    /* 연결의 창은 기본 65,535바이트이고, 첫 요청의 12바이트는 아직 돌려받지 못했다: 창은
     * 바이트마다가 아니라 묶어서 다시 열린다. */
    EXAMPLE_REQUIRE(sent.err == PROVEN_OK && taken == 65535 - 12, "what the windows allow and no more: taking less than was offered is not an error");
    EXAMPLE_REQUIRE(proven_h2_conn_send_room(client.conn, stream) == 0 && carry(&client, &server) && carry(&server, &client) && client.writable == 0, "nothing more may go until the server says so");
    proven_h2_conn_consumed(server.conn, stream, taken);   /* 처리기가 따라잡았다 */
    server.hold = false;
    EXAMPLE_REQUIRE(carry(&server, &client) && client.writable == 1, "and now the client is told: PROVEN_H2_EVENT_STREAM_WRITABLE");
    sent = proven_h2_conn_send_data(client.conn, stream, (proven_mem_view_t){ big + taken, sizeof big - taken }, true);
    EXAMPLE_REQUIRE(sent.err == PROVEN_OK && sent.value == sizeof big - taken && carry(&client, &server) && server.body_bytes == sizeof big, "the rest follows");

    /* 서버는 이 요청에 답하는 대신 포기한다. */
    EXAMPLE_REQUIRE(proven_h2_conn_reset(server.conn, stream, PROVEN_H2_CANCEL) == PROVEN_OK && carry(&server, &client), "a reset");
    EXAMPLE_REQUIRE(client.resets == 1 && client.reset_code == PROVEN_H2_CANCEL && proven_h2_conn_stream_count(client.conn) == 0, "which the client hears of, with the code");

    // ---- 거기 누구 있는가? ------------------------------------------------------------------
    static const proven_byte_t opaque[8] = { 'p', 'r', 'o', 'v', 'e', 'n', '!', '!' };
    EXAMPLE_REQUIRE(proven_h2_conn_ping(client.conn, opaque) == PROVEN_OK && carry(&client, &server) && carry(&server, &client) && client.pongs == 1, "a ping is answered by the connection itself");

    // ---- 규칙을 어기는 상대 ------------------------------------------------------------------
    /* 아무것도 더하지 않는 WINDOW_UPDATE는 프로토콜 오류다. 연결은 상대에게 그렇게 말하고
     * (GOAWAY), 끝났다고 알리고, 더는 아무것도 받지 않는다. */
    end_t strict = { .name = "strict" };
    EXAMPLE_REQUIRE(proven_h2_conn_create(&scfg, &strict.conn) == PROVEN_OK, "another server");
    proven_byte_t bad[128];
    proven_size_t len = PROVEN_H2_PREFACE_SIZE;
    memcpy(bad, PROVEN_H2_PREFACE, PROVEN_H2_PREFACE_SIZE);
    EXAMPLE_REQUIRE(proven_h2_write_settings((proven_mem_mut_t){ bad, sizeof bad }, &len, NULL, 0) == PROVEN_OK, "a client that opens properly");
    static const proven_byte_t zero_increment[13] = { 0, 0, 4, PROVEN_H2_WINDOW_UPDATE, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    memcpy(bad + len, zero_increment, sizeof zero_increment);
    len += sizeof zero_increment;
    EXAMPLE_REQUIRE(take_events(&strict, (proven_mem_view_t){ bad, len }), "the feed itself succeeds: a peer's fault is an event");
    printf("strict: closed with code %u\n", (unsigned)strict.close_code);
    EXAMPLE_REQUIRE(strict.closed == 1 && strict.close_code == PROVEN_H2_PROTOCOL_ERROR && proven_h2_conn_is_closed(strict.conn), "PROTOCOL_ERROR, and the connection is over");
    EXAMPLE_REQUIRE(proven_h2_conn_pending_output(strict.conn).size > 0, "with a GOAWAY still to be sent before the socket is closed");
    proven_h2_conn_destroy(strict.conn);

    /* 그리고 HTTP/2가 전혀 아닌 바이트는 그 첫 바이트에서 호출의 오류가 된다. */
    EXAMPLE_REQUIRE(proven_h2_conn_create(&scfg, &strict.conn) == PROVEN_OK, "one more");
    proven_size_t used = 0;
    proven_h2_event_t ev;
    EXAMPLE_REQUIRE(proven_h2_conn_feed(strict.conn, proven_mem_view_from_u8(PROVEN_LIT("GET / HTTP/1.1\r\n\r\n")), &used, &ev) == PROVEN_ERR_INVALID_FORMAT && used == 0,
                    "HTTP/1.1 is PROVEN_ERR_INVALID_FORMAT with nothing taken: the bytes are yours to give to something else");
    proven_h2_conn_destroy(strict.conn);

    // ---- 작별 ------------------------------------------------------------------------------
    proven_h2_conn_shutdown(server.conn, PROVEN_H2_NO_ERROR);          /* 새 스트림은 없다. 열려 있던 것은 먼저 끝난다 */
    EXAMPLE_REQUIRE(carry(&server, &client) && carry(&client, &server), "the goodbye crosses");
    EXAMPLE_REQUIRE(client.goaways == 1 && client.closed == 1 && server.closed == 1 && client.close_code == PROVEN_H2_NO_ERROR, "with no stream open, both ends report the end at once");
    EXAMPLE_REQUIRE(proven_h2_conn_submit_request(client.conn, request, 5, true, &stream) == PROVEN_ERR_INVALID_STATE, "and nothing more is begun");

    proven_h2_conn_destroy(client.conn);
    proven_h2_conn_destroy(server.conn);
    return EXAMPLE_OK();
}
```

### 참조

| API | 의도 | 반환 |
|---|---|---|
| `proven_h2_conn_create(&config, &conn)` | 연결을 만든다. 여는 말이 곧바로 쌓인다. | `proven_err_t`: `INVALID_ARG`(범위를 벗어난 설정), `NOMEM`. |
| `proven_h2_conn_destroy(conn)` | 해제한다. | `void`. |
| `proven_h2_conn_feed(conn, in, &consumed, &event)` | 상대의 바이트를 받아 사건 하나를 알린다. | `proven_err_t`: `INVALID_FORMAT`(서버: HTTP/2가 아님), 끝을 알린 뒤에는 `INVALID_STATE`, `NOMEM`. |
| `proven_h2_conn_pending_output(conn)` | 보낼 바이트. | `proven_mem_view_t`. |
| `proven_h2_conn_output_sent(conn, n)` | 몇 바이트가 나갔는지 알린다. | `void`. |
| `proven_h2_conn_submit_request(conn, headers, count, end_stream, &stream_id)` | 클라이언트: 요청을 시작한다. | `proven_err_t`: `INVALID_ARG`, `BUSY`(스트림 한도, 또는 기다리는 출력), `INVALID_STATE`(작별 뒤), `OUT_OF_BOUNDS`(상대가 그만큼 큰 목록을 받지 않음), `NOMEM`. |
| `proven_h2_conn_submit_response(conn, stream_id, headers, count, end_stream)` | 서버: 답한다. | `proven_err_t`: `INVALID_ARG`, `NOT_FOUND`, `INVALID_STATE`(이미 답함), `BUSY`, `OUT_OF_BOUNDS`, `NOMEM`. |
| `proven_h2_conn_send_data(conn, stream_id, data, end_stream)` | 본문 바이트를 보낸다: 갈 수 있는 만큼. | `proven_result_size_t`: 가져간 바이트 수. `NOT_FOUND`, `INVALID_STATE`. |
| `proven_h2_conn_send_trailers(conn, stream_id, headers, count)` | 트레일러 필드로 이쪽을 끝낸다. | `proven_err_t`, 응답과 같다. |
| `proven_h2_conn_reset(conn, stream_id, code)` | 스트림을 포기한다. | `proven_err_t`: `NOT_FOUND`. |
| `proven_h2_conn_consumed(conn, stream_id, n)` | DATA를 처리했다고 알린다. | `void`. |
| `proven_h2_conn_ping(conn, opaque)` | 상대가 있는지 묻는다. | `proven_err_t`: 끝난 뒤에는 `INVALID_STATE`. |
| `proven_h2_conn_shutdown(conn, code)` | 작별한다. | `void`. |
| `proven_h2_conn_is_closed(conn)`, `_stream_count(conn)`, `_send_room(conn, stream_id)` | 끝났는가, 열린 스트림 수, 지금 갈 수 있는 본문 바이트 수. | `bool`, `proven_size_t`. |
| `proven_h2_conn_stream_set_user(conn, stream_id, user)`, `_stream_user(conn, stream_id)` | 열린 스트림에 붙이는 여러분의 포인터. | `void`, `void *`. |

### 주의사항, 그리고 무엇이 잘못되는가

- **호출할 때마다, 쌓인 것을 보낸다.** 연결은 스스로 답한다 - 승인, 윈도 갱신, 리셋, 마지막 GOAWAY -
  그리고 그 답은 여러분이 무엇을 요청했든 아니든 `proven_h2_conn_pending_output`에 있다.
- **보낸 뒤에는, 빈 입력을 넣는다.** 빈 뷰로 부른 `proven_h2_conn_feed`가 전송 덕분에 생긴 사건을
  거둔다. 그러지 않고 소켓을 기다리는 드라이버는 자기를 기다리고 있는 상대를 기다리게 된다.
- **`proven_h2_conn_send_data`가 0을 돌려주는 것은 세상의 끝도, 스트림의 끝도 아니다.** "지금은 아니다"다.
  예외는 `end_stream`과 함께 준 빈 `data`다: 그것은 언제나 나가고, 그때의 0은 "끝났다"는 뜻이다.
- **뷰는 다음 호출에서 죽는다.** 사건의 필드와 쌓인 출력은 연결을 받는 다음 호출까지만 유효하다. 간직할
  것은 복사한다.
- **포인터는 사건과 함께 온다.** 스트림이 끝나면 `proven_h2_conn_stream_user`는 더는 그것을 찾지 못한다:
  그 스트림의 마지막 사건이 `user`에 포인터를 싣고 오며, 가리키는 것을 해제할 곳은 거기다.
- **`PROVEN_H2_EVENT_CLOSED`는 모든 스트림을 하나하나 말하지 않고 끝낸다.** 열려 있던 것의 목록은
  여러분의 것을 따라간다.
- **요청보다 먼저 끝난 응답은 클라이언트에게 그만 보내라고 말한다.** 서버 쪽은 RFC 9113이 정한 대로
  `NO_ERROR`를 담은 `RST_STREAM`을 보내고 스트림은 끝난다. 클라이언트는 응답 뒤에
  `PROVEN_H2_NO_ERROR`가 담긴 `PROVEN_H2_EVENT_STREAM_RESET`을 본다: 실패가 아니다.
- **연결 하나는 한 번에 한 스레드의 것이다.**
- **시계가 없으니 타임아웃도 없다.** 연결을 열고 아무 말도 하지 않는 상대, 윈도를 끝내 열지 않는 상대를
  포기하는 것은 호출자의 일이다.

## 6. 무엇을 받아들이고, 무엇이 없으며, 어떻게 시험했는가

**디코더가 받아들이는 것은 RFC 7541이 정의한 HPACK이다.** 블록 맨 앞의 테이블 크기 갱신은 몇 개든
(RFC는 많아야 둘이라고 약속한다), 정수는 연속 바이트 다섯 개까지, 리터럴의 허프만 패딩은 1 비트 일곱
개까지이고 그 밖의 것은 안 된다.

**인코더가 쓰는 것**은 흔한 선택을 하는 유효한 HPACK이다: 테이블에 있으면 참조로, 허프만 코딩은 더 짧을
때만(길이가 같으면 하지 않는다), 새 필드는 비밀이거나 테이블보다 크지 않으면 테이블에 넣는다.

**연결이 받아들이는 것은 RFC 9113이다.** RFC가 선택을 남겨 둔 곳에서는 이렇게 한다: 이쪽이 리셋한
스트림으로 온 프레임은, 그 리셋이 기억하고 있는 마지막 `max_concurrent_streams`개의 종료 안에 있는 동안
말없이 버린다. 한도를 넘는 헤더 목록은 연결이 아니라 그 스트림의 오류다. 본문과 맞지 않는
`content-length`는 스트림의 오류다. `HEAD`에 대한 응답과 상태 204, 304인 응답은 싣지 않은 길이를
알릴 수 있다.

**여기에 없는 것:**

- **소켓.** 5절의 연결은 바이트를 받고 바이트를 돌려준다. 이 라이브러리에는 아직 그것을 대신 실어
  나르는 것이 없다. 11장, 15장, 16장의 HTTP 서버와 클라이언트는 HTTP/1.1만 말한다.
- **타임아웃.** 같은 이유에서다: 연결은 시계를 읽지 않는다.
- **확장 CONNECT**(RFC 8441, HTTP/2 위의 WebSocket)와 거기에 필요한 `SETTINGS`.
- **서버 푸시**: `PUSH_PROMISE`는 거절할 수 있도록 알아보기만 한다.
- **우선순위**(RFC 9113이 폐기한 RFC 7540의 방식): 건너뛴다.
- RFC 9113이 없앤 **`Upgrade: h2c`**, **HTTP/3**.
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

연결에는 따로 등록된 시험이 있다. 연결 둘을 메모리에서 맞물려 아홉 가지 크기의 본문을 세 가지 윈도
크기로, 한 바이트씩 그리고 한꺼번에 실어 나르고, 스트림 쉰 개를 동시에 나르고, 프레임 셋으로 나가는
40,000바이트짜리 머리를 나른다. 대본 아흔여덟 개가
저마다 RFC 9113의 규칙 하나를 어기거나 바로 그 안쪽에 서고, 연결은 RFC가 정한 판정 - 무시, 코드가 붙은
스트림의 오류, 코드가 붙은 연결의 오류 - 을 지켜야 한다. 한 번은 바이트를 한 덩어리로, 한 번은 한
바이트씩 준다. 5절의 한계는 저마다 양쪽에서 그 경계까지 몬다. 녹음한 대화를 각 역할의 새 연결에
12,214번 다시 들려준다 - 앞쪽에서는 모든 지점에서, 그 뒤로는 간격을 두고 끊거나, 한 비트를 바꿔서.
그리고 대화의 모든 할당을 한 번씩 실패시킨다. 등록된 시험 밖에서는, 소켓을 연결에 통과시키는 비공개
프로그램을 통해: Node의 HTTP/2 클라이언트와 서버(nghttp2), 그리고 curl이 양쪽 역할로 요청, 5 MB까지의
본문, 20,000바이트짜리 머리, 트레일러, 중간 응답, 취소, 설정 변경을 열두 가지 설정 조합 아래에서
주고받았다 - 검사 7,896개, 실패 없음. 그리고 무작위로 행동하는 애플리케이션과 함께 연결 140,108개에
걸친 무작위 프레임 5,000,230개가 같은 도구 아래에서 연결의 불변식을 하나도 깨지 못했다.

**하지 않은 것:** 커버리지 기반 퍼징 없음. h2spec 적합성 시험 모음 실행 없음. 속도 측정 없음(허프만
디코더는 한 번에 한 비트를 읽고, 연결은 테이블을 훑어 스트림을 찾는다). 외부 검토 없음.
