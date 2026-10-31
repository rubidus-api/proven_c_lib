# 16장: 압축

**3부 — 자료 구조. 선행 조건: 압축기를 만들 때 쓰는 할당자(allocator) - 예제에서는 힙(heap) 할당자 - 는
[2장](manual-02-allocation-ko.md), 체크섬은 [4장](manual-04-containers-algorithms-ko.md).**
**이 장을 마치면** 어떤 길이의 버퍼나 스트림도 압축하고 풀 수 있고, `gzip`과 `zlib` 포장을 읽고 쓸 수
있으며, Zstandard를 읽고 쓸 수 있고, Brotli를 읽을 수 있고, 낯선 이의 데이터가 얼마까지 부풀면 - 그리고 메모리를 얼마나 쓰게
하면 - 거부할지 정할 수 있다. 그리고 무엇을 시험했고 얼마나 빠른지 말할 수 있다.

이 장은 `deflate.h`를, 7절에서는 `zstd.h`를, 8절에서는 `brotli.h`를 다룬다. 모두 순수한 계산이다 - 할당은 여러분이 준 할당자로만
하고, 운영체제에서는 아무것도 받지 않는다 - 그래서 [프리스탠딩(freestanding)](manual-freestanding-ko.md)
빌드에서 쓸 수 있다. 입력과 출력은 여러분의 메모리를 가리키는 뷰(view)로 주고받는다. 6절은 예외다: 11장과
15장의 서버와 클라이언트에서 다루는 압축된 HTTP 응답 이야기다.

## 목차

1. [DEFLATE가 하는 일, 한 쪽으로](#1-deflate가-하는-일-한-쪽으로)
2. [버퍼 전체](#2-버퍼-전체)
3. [스트림](#3-스트림)
4. [레벨, 윈도, 그리고 그 비용](#4-레벨-윈도-그리고-그-비용)
5. [남이 보낸 데이터](#5-남이-보낸-데이터)
6. [HTTP 위에서](#6-http-위에서)
7. [Zstandard](#7-zstandard)
8. [Brotli](#8-brotli)
9. [무엇을 받아들이고, 무엇이 없으며, 어떻게 시험했는가](#9-무엇을-받아들이고-무엇이-없으며-어떻게-시험했는가)

## 1. DEFLATE가 하는 일, 한 쪽으로

DEFLATE(RFC 1951)는 `.gz` 파일, HTTP의 `Content-Encoding: gzip`, PNG 이미지, ZIP 압축 파일, WebSocket의
`permessage-deflate` 안에 있는 압축이다. 서른 살이고, 가장 좋은 것은 아니며, 모두가 읽는다.

차례로 두 가지를 한다:

- **되풀이를 참조로 바꾼다.** 다음 몇 바이트가 지난 32 KiB 안에 나온 적이 있으면, 그 바이트 대신
  "*거리* 바이트 전과 같은 것을 *길이* 바이트만큼"이라고 쓴다. 로그 파일, 웹 페이지, 소스 코드는 대부분
  되풀이다.
- **남은 것을 길이가 다른 코드로 쓴다**: 흔한 것은 짧게, 드문 것은 길게.

그래서 텍스트는 삼분의 일이나 사분의 일로 줄고, 숫자 표는 흔히 더 줄며, **이미 압축되었거나 암호화된
데이터는 전혀 줄지 않는다** - 몇 바이트 늘어난다.

압축된 데이터가 파일이나 메시지가 되려면 둘레에 무언가가 필요하다. 선택은 셋이고, **양 끝이 같은 것을
골라야 한다**:

| 형식 | 데이터의 둘레 | 쓰이는 곳 |
|---|---|---|
| `PROVEN_DEFLATE_GZIP` | 열 바이트 헤더, 그리고 끝에 데이터의 CRC-32와 길이(RFC 1952) | `.gz` 파일. HTTP의 `gzip` |
| `PROVEN_DEFLATE_ZLIB` | 두 바이트 헤더와 데이터의 Adler-32(RFC 1950) | PNG. HTTP의 `deflate` |
| `PROVEN_DEFLATE_RAW` | 없음 | 자기 포장이 있는 다른 형식의 안: ZIP 항목, `permessage-deflate` |

체크섬이 있는 둘은 손상을 알아챈다. raw는 그렇지 않다: 바뀐 비트는 다른 데이터나 오류를 내고, 어느
쪽인지 아무것도 말해 주지 않는다.

## 2. 버퍼 전체

```c
proven_size_t proven_deflate_bound(proven_deflate_format_t format, proven_size_t input_size);
proven_err_t proven_deflate_all(const proven_deflate_options_t *options, proven_mem_view_t in,
                                proven_mem_mut_t out, proven_size_t *written);
proven_err_t proven_inflate_all(proven_allocator_t alloc, proven_deflate_format_t format, proven_mem_view_t in,
                                proven_mem_mut_t out, proven_size_t *written, proven_size_t *consumed);
```

데이터가 메모리에 있고 결과를 둘 자리도 그렇다면 이 두 호출이 전부다. 둘 다 여러분의 버퍼에 쓴다.

- **압축**은 확실히 들어가려면 `proven_deflate_bound` 바이트의 버퍼가 필요하다 - 입력의 크기에 조금
  더. 압축되지 않는 데이터는 약간 커지기 때문이다. 더 작으면 `PROVEN_ERR_OUT_OF_BOUNDS`로 실패할 수
  있다.
- **풀기**는 결과가 얼마나 커도 되는지를 여러분이 정해야 한다. 압축된 데이터가 정하게 두지 않기
  때문이다. **`out`의 크기가 한계다**: 더 많이 낼 스트림은, 그 자체가 아무리 작아도,
  `PROVEN_ERR_OUT_OF_BOUNDS`로 거부된다. 버퍼를 대신 키워 주는 판이 왜 없는지는 5절에 있다.

테스트 스위트가 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_16_gzip.c -->
```c
#include <string.h>

/*
 * 버퍼를 압축하고, 되찾고, 약속한 것보다 많이 건네받기를 거부한다: 버퍼 전체를 다루는 두 호출과,
 * 남이 보낸 것을 풀 때의 한 가지 규칙.
 */

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    /* 압축되는 것: 텍스트는 스스로를 되풀이한다. */
    static proven_byte_t text[4000];
    static const char line[] = "GET /index.html HTTP/1.1 - 200 - text/html; charset=utf-8\n";
    for (proven_size_t i = 0; i < sizeof text; ++i) text[i] = (proven_byte_t)line[i % (sizeof line - 1)];

    /* 버퍼가 한계값이 말하는 만큼 크면 압축은 자리가 없어 실패하지 않는다. */
    static proven_byte_t packed[4200];
    proven_size_t packed_len = 0;
    proven_deflate_options_t options = { .alloc = heap, .format = PROVEN_DEFLATE_GZIP };
    EXAMPLE_REQUIRE(proven_deflate_bound(PROVEN_DEFLATE_GZIP, sizeof text) <= sizeof packed, "4,000바이트의 한계값은 4,000을 조금 넘는다");
    EXAMPLE_REQUIRE(proven_deflate_all(&options, (proven_mem_view_t){ text, sizeof text }, (proven_mem_mut_t){ packed, sizeof packed }, &packed_len) == PROVEN_OK,
                    "로그 4,000바이트가 압축된다");
    EXAMPLE_REQUIRE(packed_len < 200, "200 미만으로: 같은 줄이 되풀이되므로");
    EXAMPLE_REQUIRE(packed[0] == 0x1f && packed[1] == 0x8b, "나온 것은 gzip 멤버다 - .gz 파일, 또는 Content-Encoding: gzip인 HTTP 본문");

    /* 풀기: 출력 버퍼가 곧 한계다. 여기서는 데이터와 정확히 같은 크기다. */
    static proven_byte_t back[4000];
    proven_size_t back_len = 0, used = 0;
    EXAMPLE_REQUIRE(proven_inflate_all(heap, PROVEN_DEFLATE_GZIP, (proven_mem_view_t){ packed, packed_len }, (proven_mem_mut_t){ back, sizeof back }, &back_len, &used) == PROVEN_OK,
                    "풀린다");
    EXAMPLE_REQUIRE(back_len == sizeof text && memcmp(back, text, sizeof text) == 0 && used == packed_len, "넣은 그대로, 멤버의 모든 바이트를 써서");

    /* 같은 멤버를 더 작은 버퍼에 풀면 거부된다. 이것이 "압축 폭탄"에 대한 방어의 전부다: 얼마나
     * 받을지를 당신이 말하고, 자리는 그만큼뿐이다. 몇백 바이트가 이 프로그램에게 기가바이트를
     * 할당하게 만들 수 없다. */
    static proven_byte_t small[1000];
    EXAMPLE_REQUIRE(proven_inflate_all(heap, PROVEN_DEFLATE_GZIP, (proven_mem_view_t){ packed, packed_len }, (proven_mem_mut_t){ small, sizeof small }, &back_len, NULL) == PROVEN_ERR_OUT_OF_BOUNDS,
                    "4,000바이트는 1,000에 들어가지 않는다: PROVEN_ERR_OUT_OF_BOUNDS");

    /* 손상은 알아챈다. gzip은 데이터의 CRC-32를 싣는다. 압축된 스트림 어디서든 비트 하나가
     * 바뀌면 데이터가 바뀌거나 형식이 깨진다. */
    packed[packed_len / 2] ^= 0x04;
    EXAMPLE_REQUIRE(proven_inflate_all(heap, PROVEN_DEFLATE_GZIP, (proven_mem_view_t){ packed, packed_len }, (proven_mem_mut_t){ back, sizeof back }, &back_len, NULL) == PROVEN_ERR_INVALID_FORMAT,
                    "바뀐 비트 하나: PROVEN_ERR_INVALID_FORMAT");
    packed[packed_len / 2] ^= 0x04;
    EXAMPLE_REQUIRE(proven_inflate_all(heap, PROVEN_DEFLATE_GZIP, (proven_mem_view_t){ packed, packed_len - 1 }, (proven_mem_mut_t){ back, sizeof back }, &back_len, NULL) == PROVEN_ERR_INVALID_FORMAT,
                    "잘린 멤버는 멤버가 아니다");

    /* 포장은 양 끝에서 함께 정하는 선택이다. 같은 데이터가 zlib로는 열두 바이트 작고(더 짧은
     * 헤더, 더 짧은 체크섬), raw는 더 작다 - 그리고 체크섬이 아예 없어서 손상이 눈에 띄지 않을 수
     * 있다. */
    proven_size_t zlib_len = 0, raw_len = 0;
    static proven_byte_t other[4200];
    options.format = PROVEN_DEFLATE_ZLIB;
    EXAMPLE_REQUIRE(proven_deflate_all(&options, (proven_mem_view_t){ text, sizeof text }, (proven_mem_mut_t){ other, sizeof other }, &zlib_len) == PROVEN_OK && zlib_len == packed_len - 12,
                    "zlib 포장: gzip보다 12바이트 적다");
    EXAMPLE_REQUIRE(proven_inflate_all(heap, PROVEN_DEFLATE_GZIP, (proven_mem_view_t){ other, zlib_len }, (proven_mem_mut_t){ back, sizeof back }, &back_len, NULL) == PROVEN_ERR_INVALID_FORMAT,
                    "그리고 gzip이 아니다: 푸는 쪽은 무엇을 읽는지 들어야 한다");
    options.format = PROVEN_DEFLATE_RAW;
    EXAMPLE_REQUIRE(proven_deflate_all(&options, (proven_mem_view_t){ text, sizeof text }, (proven_mem_mut_t){ other, sizeof other }, &raw_len) == PROVEN_OK && raw_len == zlib_len - 6,
                    "raw: 헤더도 체크섬도 없이 다시 6바이트 적다");

    /* 압축되지 않는 것은 조금 커진다 - 최악의 경우 한계값까지. */
    static proven_byte_t noise[4000], noisy[4200];
    proven_u32 x = 2463534242u;
    for (proven_size_t i = 0; i < sizeof noise; ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; noise[i] = (proven_byte_t)x; }
    proven_size_t noisy_len = 0;
    options.format = PROVEN_DEFLATE_GZIP;
    EXAMPLE_REQUIRE(proven_deflate_all(&options, (proven_mem_view_t){ noise, sizeof noise }, (proven_mem_mut_t){ noisy, sizeof noisy }, &noisy_len) == PROVEN_OK &&
                    noisy_len > sizeof noise && noisy_len <= proven_deflate_bound(PROVEN_DEFLATE_GZIP, sizeof noise),
                    "무작위 바이트는 조금 더 크게 나오고, 한계값을 넘지는 않는다");

    return EXAMPLE_OK();
}
```

## 3. 스트림

```c
proven_err_t proven_deflate_create(const proven_deflate_options_t *options, proven_deflate_t **out);
proven_err_t proven_deflate(proven_deflate_t *z, proven_mem_view_t in, proven_size_t *consumed,
                            proven_mem_mut_t out, proven_size_t *produced, proven_deflate_flush_t flush, bool *done);
void proven_deflate_reset(proven_deflate_t *z);
void proven_deflate_destroy(proven_deflate_t *z);

proven_err_t proven_inflate_create(proven_allocator_t alloc, proven_deflate_format_t format, proven_inflate_t **out);
proven_err_t proven_inflate(proven_inflate_t *z, proven_mem_view_t in, proven_size_t *consumed,
                            proven_mem_mut_t out, proven_size_t *produced, bool *done);
void proven_inflate_reset(proven_inflate_t *z);
void proven_inflate_restart(proven_inflate_t *z);
void proven_inflate_destroy(proven_inflate_t *z);
```

데이터가 메모리보다 큰 파일이거나, 아직 만들어지고 있는 응답이거나, 연결이라면, 두 방향 모두 같은
방식으로 동작한다: 상태를 쥔 객체 하나와, **입력을 조금 받아 출력을 조금 채우는 단계 함수 하나**.

단계의 계약은 짧고, 한 번은 읽어 둘 만하다:

- `in`에서 할 수 있는 만큼 소비하고 `out`에 할 수 있는 만큼 생산하며, 각각 얼마인지 알려 준다.
  할당하지 않고, `out`을 넘어 쓰지 않는다. (`out` 안에서는 해제기가 보고한 바이트 뒤로 일곱 바이트까지
  쓸 수 있다: 비어 있는 자리를 넘길 것.)
- **아무것도 소비하지 않고 아무것도 생산하지 않은 `PROVEN_OK`는 오류가 아니다.** 단계가 둘 중 하나를
  더 원한다는 뜻이다: 입력을 더, 또는 자리를 더. 생기면 다시 부른다. 그냥 도착하기를 멈춘 스트림도 꼭
  이렇게 보이므로, 더 올 입력이 없음을 아는 호출자는 이것을 잘린 스트림으로 다뤄야 한다.
- `*done`은 스트림이 끝났고 모든 것이 생산되었을 때 참이 된다 - 데이터가 준 자리를 정확히 채웠을
  때에도. 끝 뒤의 입력은 소비되지 않는다: `*consumed`가 스트림이 어디서 끝났는지 알려 준다.

**압축할 때 `flush`는 출력이 얼마나 급한지를 말한다.**

| `flush` | 뜻 | 쓸 때 |
|---|---|---|
| `PROVEN_DEFLATE_FLUSH_NONE` | 더 온다. 출력은 압축기가 블록 하나만큼 모았을 때 나온다 - 오래 걸릴 수 있다 | 아래 두 경우가 아닌 모든 때 |
| `PROVEN_DEFLATE_FLUSH_SYNC` | 지금까지 준 모든 것을 지금 풀 수 있어야 한다. 스트림은 이어진다 | 쓴 것을 누군가 기다릴 때: 연결 위의 메시지, 실시간 로그의 한 줄. 매번 몇 바이트와 압축률 얼마를 치른다 |
| `PROVEN_DEFLATE_FLUSH_FINISH` | 그것이 마지막 입력이었다: 스트림을 끝낸다 | 끝에서 한 번. `*done`이 될 때까지 이것과 함께, 자리를 더 주며 계속 부른다 |

sync flush는 바이트 경계에서 끝나고, 그것이 내는 마지막 네 바이트는 언제나 `00 00 ff ff`다. WebSocket의
`permessage-deflate`는 메시지마다 그 네 바이트를 뗀 sync flush로 보내고, 받는 쪽이 그것을 다시 붙인다([12장](manual-12-websocket-ko.md) 9절: WebSocket 드라이버는 그 네 바이트를 떼고 다시 붙이며, context takeover에서는 연결 내내 압축기 하나와 해제기 하나를 쓴다).
raw 스트림이 끝나지 않아도 되는 까닭도 그것이다: 해제기는 조각조각 받고, `*done`은 참이 되지 않으며,
앞선 조각에 대해 기억하는 것이 뒤의 조각을 작게 만든다.

`proven_deflate_reset`과 `proven_inflate_reset`은 같은 객체에서 새 스트림을 시작한다. 하나 더 만드는
것보다 싸다. `*done` 뒤에, 첫 gzip 멤버에 이어지는 둘째를 읽어야 하는 해제기는 reset하고 첫째가 끝난
곳부터 받는다. `proven_inflate_restart`도 새 스트림을 시작하지만 마지막 32 KiB를 지킨다. 그래서 새
스트림의 일치가 옛 스트림 안을 가리킬 수 있다. context takeover를 쓰는 `permessage-deflate`를 위한
것이다: 보내는 쪽이 메시지를 마지막 블록으로 끝내도, 다음 메시지는 여전히 앞의 메시지들을 가리킨다.

테스트 스위트가 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_16_stream.c -->
```c
#include <string.h>

/*
 * 스트림으로서의 압축: 조각으로 들어와 조각으로 나가는 데이터를, 가진 크기 그대로의 버퍼로.
 * 같은 루프가 어떤 길이의 파일에도, 아직 만들어지고 있는 응답에도, 연결에도 통한다.
 */

/* 압축된 바이트가 가는 곳. 실제 프로그램이라면 파일이나 소켓에 쓴다. */
static proven_byte_t g_wire[8192];
static proven_size_t g_wire_len;

/* 압축기에 입력을 주고 가진 출력을 64바이트씩 받는다. 입력을 모두 가져갈 때까지 - 그리고
 * `flush`가 그 이상을 청하면 그것까지 모두 내놓을 때까지. */
static bool feed(proven_deflate_t *z, const char *text, proven_deflate_flush_t flush, bool *done) {
    proven_mem_view_t in = { (const proven_byte_t *)text, strlen(text) };
    for (;;) {
        proven_byte_t out[64];
        proven_size_t consumed = 0, produced = 0;
        if (proven_deflate(z, in, &consumed, (proven_mem_mut_t){ out, sizeof out }, &produced, flush, done) != PROVEN_OK) return false;
        if (g_wire_len + produced > sizeof g_wire) return false;
        memcpy(g_wire + g_wire_len, out, produced);
        g_wire_len += produced;
        in.ptr += consumed; in.size -= consumed;
        /* 더 줄 것이 없고, 내놓은 것이 버퍼 하나에 못 미쳤다: 밀린 것이 없다. */
        if (in.size == 0 && produced < sizeof out) return true;
    }
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    /* 압축기는 한 번 만든다. 옵션의 0은 기본값을 뜻한다: 레벨 6, 32 KiB 윈도. 레벨 1은 더
     * 빠르고 조금 덜 압축하며, 9는 더 느리고 조금 더 압축한다. */
    proven_deflate_options_t options = { .alloc = heap, .format = PROVEN_DEFLATE_ZLIB, .level = 6 };
    proven_deflate_t *z = NULL;
    EXAMPLE_REQUIRE(proven_deflate_create(&options, &z) == PROVEN_OK, "압축기");

    /* 조각은 오는 대로 들어간다. FLUSH_NONE이면 압축기는 블록 하나만큼 모일 때까지 받은 것을
     * 쥐고 있다 - 오랫동안 아무것도 나오지 않을 수 있고, 그래서 잘 압축한다. */
    bool done = false;
    EXAMPLE_REQUIRE(feed(z, "first piece of the stream; ", PROVEN_DEFLATE_FLUSH_NONE, &done), "조각 하나가 들어간다");
    const proven_size_t after_first = g_wire_len;
    EXAMPLE_REQUIRE(after_first <= 2 && !done, "아직 두 바이트 헤더 말고는 나온 것이 없다");

    /* sync flush는 말한다: 상대가 여기까지를 지금 읽을 수 있어야 한다. 매번 몇 바이트가 든다 -
     * 누군가 기다릴 때 쓰고, 쓸 때마다 쓰지는 말 것. */
    EXAMPLE_REQUIRE(feed(z, "second piece of the stream; ", PROVEN_DEFLATE_FLUSH_SYNC, &done) && g_wire_len > after_first + 20 && !done,
                    "sync flush 뒤에는 두 조각 모두 선 위에 있다");
    const proven_size_t at_flush = g_wire_len;

    /* FINISH는 스트림을 끝낸다: 마지막 블록, 그리고 zlib와 gzip이라면 체크섬. */
    EXAMPLE_REQUIRE(feed(z, "third and last piece.", PROVEN_DEFLATE_FLUSH_FINISH, &done) && done, "마지막 조각, 그리고 스트림이 끝났다");

    /* 받는 쪽: 10바이트씩 받아 16바이트 버퍼에 푸는 해제기. */
    proven_inflate_t *u = NULL;
    EXAMPLE_REQUIRE(proven_inflate_create(heap, PROVEN_DEFLATE_ZLIB, &u) == PROVEN_OK, "해제기");
    char text[200];
    proven_size_t text_len = 0, at = 0, seen_at_flush = 0;
    bool ended = false;
    /* 먼저 sync flush가 돌아왔을 때 쓰여 있던 것만, 그다음 나머지. */
    for (proven_size_t limit = at_flush; !ended; limit = g_wire_len) {
        for (;;) {
            proven_byte_t out[16];
            proven_size_t consumed = 0, produced = 0;
            const proven_size_t give = limit - at < 10 ? limit - at : 10;
            if (proven_inflate(u, (proven_mem_view_t){ g_wire + at, give }, &consumed, (proven_mem_mut_t){ out, sizeof out }, &produced, &ended) != PROVEN_OK) break;
            if (text_len + produced > sizeof text) break;
            memcpy(text + text_len, out, produced);
            text_len += produced;
            at += consumed;
            /* 소비도 생산도 없는 PROVEN_OK: 받은 것을 다 쓰고 더 원한다. 오류가 아니다 -
             * 스트림은 그렇게 기다린다. */
            if (ended || (consumed == 0 && produced == 0)) break;
        }
        if (limit == at_flush) seen_at_flush = text_len;
        else break;
    }
    static const char whole[] = "first piece of the stream; second piece of the stream; third and last piece.";
    EXAMPLE_REQUIRE(ended && text_len == sizeof whole - 1 && memcmp(text, whole, text_len) == 0, "세 조각이 하나의 텍스트로 나온다");
    EXAMPLE_REQUIRE(at == g_wire_len, "스트림은 압축기가 쓰기를 멈춘 바로 그곳에서 끝났다");
    EXAMPLE_REQUIRE(seen_at_flush >= 55, "flush 지점에서 받는 쪽은 이미 앞의 두 조각을 모두 갖고 있었다");

    /* 두 객체는 다시 만들지 않고 다음 스트림에 다시 쓴다. */
    proven_deflate_reset(z);
    proven_inflate_reset(u);
    g_wire_len = 0;
    EXAMPLE_REQUIRE(feed(z, "another stream", PROVEN_DEFLATE_FLUSH_FINISH, &done) && done, "같은 압축기, 새 스트림");
    proven_byte_t again[32];
    proven_size_t consumed = 0, produced = 0;
    EXAMPLE_REQUIRE(proven_inflate(u, (proven_mem_view_t){ g_wire, g_wire_len }, &consumed, (proven_mem_mut_t){ again, sizeof again }, &produced, &ended) == PROVEN_OK &&
                    ended && produced == 14 && memcmp(again, "another stream", 14) == 0, "같은 해제기가 그것을 읽는다");

    proven_deflate_destroy(z);
    proven_inflate_destroy(u);
    return EXAMPLE_OK();
}
```

## 4. 레벨, 윈도, 그리고 그 비용

압축기는 `proven_deflate_options_t`로 만든다. 0으로 초기화하고, `alloc`을 두고, 바꾸고 싶은 것을 둔다:

| 필드 | 뜻 |
|---|---|
| `alloc` | 필수. 압축기는 할당 하나이며 여기서 만들고 `proven_deflate_destroy`가 해제한다 |
| `format` | 1절의 포장. 0은 `PROVEN_DEFLATE_RAW` |
| `level` | 1~9: 되풀이를 얼마나 열심히 찾는가. **0은 기본값 6이다.** -1은 전혀 압축하지 않는다 - 데이터를 감싸기만 한다 |
| `window_bits` | 되풀이가 얼마나 멀리 있어도 되는가, 2의 거듭제곱으로: 9(512바이트)~15(32 KiB). **0은 기본값 15다** |

**레벨은 시간과 크기를 맞바꾸고, 그 교환은 한쪽으로 기운다.** 개발 기계에서 이 라이브러리 자신의
소스와 매뉴얼(텍스트 3.2 MiB)을 압축하면:

| 레벨 | 압축 결과 | 압축 | 풀기 |
|---|---|---|---|
| 1 | 29.4% | 34 MiB/s | 330 MiB/s |
| 6(기본값) | 25.2% | 16 MiB/s | 375 MiB/s |
| 9 | 25.0% | 9 MiB/s | 380 MiB/s |

1에서 6으로 가면 속도의 절반으로 크기의 칠분의 일을 산다. 6에서 9로 가면 또 속도의 절반 가까이로 거의
아무것도 사지 못한다. 풀기는 어떤 레벨이 쓰였는지 상관하지 않는다.

모두가 쓰는 구현인 zlib는 같은 데이터를 같은 크기로 압축하고 - 레벨 6과 9에서 사백분의 일 이내, 레벨
1에서는 zlib 쪽이 조금 더 크다 - 압축은 두 배쯤, 레벨 1에서는 세 배 가까이 빠르다. 풀기는 둘이 비슷하다:
같은 데이터를 Python에서 부른 zlib 1.3.1은 기계의 부하에 따라 190에서 310 MiB/s를 냈다. (v0.42.0 전에는
이 라이브러리의 해제기가 한 비트씩 읽었고 75에서 90 MiB/s였다.) 압축기는 읽히도록 쓰였다. 그 수치가 어떤
프로그램에 너무 느리다면, 그 프로그램에 필요한 것은 zlib다.

**윈도는 대체로 메모리의 문제다.** 압축기는 윈도의 두 배와 그 옆의 표들을 쥔다. 해제기는 언제나
32 KiB를 쥔다. 상대가 어떤 윈도를 썼는지 알 수 없기 때문이다:

| | 메모리 |
|---|---|
| 해제기 | 약 35 KiB |
| 압축기, `window_bits` 15(기본값) | 약 325 KiB |
| 압축기, `window_bits` 12 | 약 53 KiB |
| 압축기, `window_bits` 9 | 약 11 KiB |

윈도가 작으면 덜 압축된다 - 윈도보다 멀리 떨어진 되풀이는 찾지 못한다 - 그리고 속도에는 계획할 만한
차이가 없다. 압축기가 한꺼번에 많을 때(연결마다 하나)나 메모리가 적을 때 쓴다.

## 5. 남이 보낸 데이터

프로그램 밖에서 들어오는 압축 데이터가 위험한 경우이고, 까닭은 하나다:

**보기보다 훨씬 클 수 있다.** DEFLATE는 "마지막 바이트를 다시, 258번"을 두 바이트로, 거듭거듭 말할 수
있다. 이 라이브러리의 테스트에 있는 117바이트는 25,801바이트로 부푼다. 메가바이트 하나가 기가바이트가
될 수 있다. 받은 것을 무엇이든 필요한 만큼 자라는 버퍼에 푸는 프로그램은, 보내는 모든 이에게 자기
메모리를 바닥낼 능력을 준 것이다.

그래서 여기서는 아무것도 자라지 않는다. `proven_inflate`는 넘겨받은 공간에만 쓰고, 그것이 차면 멈추고
그렇다고 말한다. `proven_inflate_all`은 한계를 출력의 크기로 받는다. 읽고 있는 것 - 설정 파일, 요청
본문, 메시지 - 에 말이 되는 가장 큰 결과를 정하고, 그것을 넘는 것은 거부하라. 해제기 자신의 메모리는
무엇을 받든 약 35 KiB로 고정이다.

**그리고 그냥 틀렸을 수 있다**, 사고로든 일부러든:

| 반환 | 언제 |
|---|---|
| `PROVEN_ERR_INVALID_FORMAT` | 바이트가 해제기를 만들 때 정한 형식의 유효한 스트림이 아니다. 또는 - gzip과 zlib에서 - 끝의 체크섬이 데이터와 맞지 않는다. 또는 `proven_inflate_all`에서, 스트림이 끝나기 전에 입력이 끝났다 |
| `PROVEN_ERR_UNSUPPORTED` | 미리 정한 사전이 필요한 zlib 스트림 |
| `PROVEN_ERR_OUT_OF_BOUNDS` | `proven_inflate_all`에서: 데이터가 준 버퍼에 들어가지 않는다 |

오류 뒤에 해제기는 reset할 때까지 모든 호출에서 그 오류를 돌려준다. 오류 전에 생산한 데이터를 완전한
것으로 믿어서는 안 된다: gzip과 zlib에서 체크섬이 맞는지는 맨 끝에서야 알 수 있다.

**알아 둘 만한 것 둘 더.**

- **압축은 압축하는 것을 새게 할 수 있다.** 비밀과 공격자가 고른 텍스트를 함께 압축하고 공격자가 결과의
  크기를 볼 수 있으면, 그 크기는 고른 텍스트가 비밀의 일부를 되풀이했는지를 말해 주고, 비밀은 한 글자씩
  추측될 수 있다. 이것은 실제로 HTTPS를 깼다(CRIME, BREACH). 지킬 규칙은 애플리케이션의 몫이다: 비밀을
  낯선 이가 통제하는 것과 함께 압축하지 말 것.
- **raw DEFLATE에는 체크섬이 없다.** 바이트가 오는 길에 손상되었을 수 있고 둘레의 형식이 검사하지
  않는다면 zlib나 gzip을 쓴다.

## 6. HTTP 위에서

```c
void proven_http_exchange_compress(proven_http_exchange_t *exchange);     /* 11장의 서버 */
void proven_http_stream_compress(proven_http_stream_t *stream);           /* 15장의 서버 */

bool proven_http_accepts_coding(const proven_http_header_t *headers, proven_size_t count, proven_http_coding_t coding);
unsigned proven_http_accept_weight(const proven_http_header_t *headers, proven_size_t count, proven_http_coding_t coding);
proven_http_coding_t proven_http_content_coding(const proven_http_header_t *headers, proven_size_t count);
```

HTTP는 이것을 *콘텐츠 코딩*이라고 부른다: 서버가 본문을 압축하고 `Content-Encoding`으로 그 사실을
알리는데, `Accept-Encoding`으로 허락한 클라이언트에게만 그렇게 할 수 있다.
[11장](manual-11-http-client-server-ko.md)과 [15장](manual-15-event-loop-ko.md)의 HTTP 드라이버 넷은 이
두 가지를 모두 한다 - 그리고 **요청하기 전에는 어느 것도 하지 않는다**. 아무것도 설정하지 않은
프로그램은 이전과 같은 바이트를 보낸다.

이 장의 나머지와 달리 이 부분은 소켓이 필요하고 프리스탠딩 빌드에는 없다 - 헬퍼 함수 셋은 예외로,
`http.h`에 있는 헤더 계산일 뿐이다.

**서버는 핸들러가 그 응답에 대해 요청할 때 응답을 압축한다.** 응답을 시작하기 전에 호출 하나:

| | 일어나는 일 |
|---|---|
| 요청이 gzip을 허용하고, 응답에 자기 본문이 있다 | `Content-Encoding: gzip`으로 보낸다 - 또는 `zstd`로, 아래 |
| 요청이 어느 쪽도 허용하지 않는다(`Accept-Encoding` 없음, `gzip;q=0`, 다른 코딩만) | 그대로 보낸다 |
| 상태 204, 206, 304. 또는 여러분의 헤더에 이미 `Content-Encoding`이나 `Content-Range`가 있다 | 그대로 보낸다 |
| 한 번에 보내는 본문이 256바이트 미만이거나, 작아지지 않는다 | 그대로 보낸다 |
| 압축기에 줄 메모리가 없다 | 그대로 보낸다 |
| 이 모든 경우에 | `Vary: Accept-Encoding`이 붙는다. 여러분의 `Vary`가 이미 그것을 말하면 붙지 않는다 |

`Vary`가 붙는 까닭은 응답이 이제 요청 헤더에 따라 달라지기 때문이다. 중간의 캐시가 gzip을 읽지 못하는
클라이언트에게 gzip을 건네서는 안 된다.

**서버 설정에 `compress_zstd`가 있으면 Zstandard로도 압축한다.** 압축을 요청한 응답은 요청의
`Accept-Encoding`이 zstd에 gzip 이상의 가중치를 줄 때 `zstd`로, 그 밖에는 이전처럼 gzip으로 나간다.
curl과, zstd를 받는 브라우저는 둘을 같은 가중치로 보낸다 - `gzip, deflate, br, zstd` - 그래서 zstd를 받는다.
`gzip;q=1, zstd;q=0.5`라고 쓰는 클라이언트는 gzip을 받고, zstd를 적지 않는 클라이언트는 전에 받던 것을
받는다. 같을 때 zstd로 가는 까닭은 둘 가운데 만드는 비용이 적기 때문이다(7절: 레벨 1에서 gzip의 여섯 배
속도, 크기는 gzip 1 이하). 레벨과 창은 `compress_zstd_level`(1부터 9, 0은 1)과
`compress_zstd_window_log`(10부터 23, 0은 17, 128 KiB)다. 한 번에 보내는 본문은 창과 상관없이 본문에 맞춘
표로 압축하고, 스트림으로 보내는 본문은 이 기본값에서 끝날 때까지 약 0.8 MiB를 쥔다. 레벨 6은 gzip 6의
크기 이하를 그 속도의 약 세 배로 낸다 - gzip 6의 크기를 원하는 서버를 위한 것이다. 그 표 때문에 스트림 응답
하나가 약 1.8 MiB를 쥔다.
`proven_http_accept_weight`는 같은 가중치를 돌려준다. 직접 고를 때 쓴다.

한 번에 보내는 본문은 먼저 압축한 다음 새 `Content-Length`와 함께 보낸다. 조각조각 쓰는 본문은 쓰는
대로 압축해서, 알려 준 길이와 상관없이 청크로 보낸다 - 그 길이는 여전히 여러분이 써야 할 길이다.

**클라이언트는 설정에 `decompress`가 있을 때 요청하고 푼다.** 그러면 `Accept-Encoding: gzip`을 보내고,
`gzip`이나 `deflate`로 돌아온 본문을 여러분에게 가는 길에 푼다: `proven_http_client_read`는 여러분의
버퍼를 풀린 바이트로 채우고, 이벤트 구동 클라이언트의 `on_body`는 풀린 조각을 최대 16 KiB씩 받는다.

**`decompress_zstd`도 켜면 Zstandard도 요청한다** - `Accept-Encoding: zstd, gzip` - 그리고 `zstd` 본문을
7절의 디코더와, RFC 9659가 HTTP에 정한 8 MiB 창 상한으로 같은 방식으로 푼다. 따로 켜는 까닭은 그 창
때문이다: gzip은 35 KiB면 되는데, 서버는 응답 하나에 클라이언트가 최대 8 MiB를 들고 있게 할 수 있다.
`decompress` 없이 `decompress_zstd`만 켜면 `PROVEN_ERR_INVALID_ARG`다.

**`decompress_brotli`를 켜면 Brotli도 요청한다** - 목록에 `br`이 들어가고, 두 스위치를 다 켜면
`Accept-Encoding: zstd, br, gzip` - 그리고 `br` 본문을 8절의 디코더로 푼다. 이 디코더는 RFC 7932가
허용하는 모든 창, 최대 16 MiB를 받아들인다. RFC 9659가 Zstandard에 대해 하듯 더 낮은 창을 정한 HTTP
명세는 없다. 따로 켜는 까닭은 Zstandard와 같다: 응답 하나에 그중 얼마를 쓰게 할지 서버가 정한다.
`decompress` 없이 켜면 `PROVEN_ERR_INVALID_ARG`, `PROVEN_NO_BROTLI`로 만든 빌드(8절)에서는
`PROVEN_ERR_UNSUPPORTED`다. 서버는 `br`을 보내지 않는다: 이 라이브러리에는 Brotli 인코더가 없다.

### 예제: 페이지는 압축하고, 계정 페이지는 하지 않는다

<!-- example: manual/examples/ko/ex_16_http_gzip.c -->
```c
#include <stdio.h>
#include <string.h>

/*
 * HTTP로 보내는 압축된 응답: 핸들러가 요청하고 클라이언트가 받아들이면 압축하는 서버와,
 * 압축을 요청하고 풀어 읽는 클라이언트.
 *
 * 프로그램이 말하기 전에는 둘 다 꺼져 있다. 서버 쪽은 핸들러에서 응답마다 부르는 호출 하나이고,
 * 클라이언트 쪽은 설정의 플래그 하나다.
 */

static proven_byte_t g_page[20000];        /* 제공할 페이지: 텍스트라서 압축이 잘 된다 */

static void fill_page(void) {
    proven_size_t n = 0;
    for (unsigned k = 0; n < sizeof g_page; ++k) {
        char line[48];
        int len = snprintf(line, sizeof line, "row %u of a table that says much the same\n", k);
        for (int i = 0; i < len && n < sizeof g_page; ++i) g_page[n++] = (proven_byte_t)line[i];
    }
}

static void handle(void *ctx, proven_http_exchange_t *x) {
    proven_http_server_t **server = ctx;
    const proven_http_request_t *req = proven_http_exchange_request(x);
    proven_mem_view_t page = { g_page, sizeof g_page };

    if (proven_u8str_view_eq(req->target, PROVEN_LIT("/page"))) {
        /* 응답을 시작하기 전에 호출 하나. 나머지는 서버가 한다: 요청의 Accept-Encoding을
         * 읽고, gzip이 허용되면 압축하고, Content-Encoding과 Vary와 새 Content-Length를
         * 쓴다. */
        proven_http_exchange_compress(x);
        proven_http_header_t type = { PROVEN_LIT("Content-Type"), PROVEN_LIT("text/plain") };
        (void)proven_http_exchange_respond(x, 200, &type, 1, page);

    } else if (proven_u8str_view_eq(req->target, PROVEN_LIT("/account"))) {
        /* 여기서는 일부러 요청하지 않는다: 이 응답은 비밀을 클라이언트가 고른 글자 옆에
         * 싣게 되는데, 압축된 본문의 크기는 그 비밀을 한 바이트씩 새어 나가게 한다.
         * 압축을 응답마다 요청하게 한 것은 핸들러만이 이를 알기 때문이다. */
        (void)proven_http_exchange_respond(x, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("token=... you searched for: ...\n")));

    } else if (proven_u8str_view_eq(req->target, PROVEN_LIT("/accepts"))) {
        /* 서버가 묻는 것과 같은 질문을, 스스로 고르고 싶은 핸들러가 묻는다 -
         * 예컨대 파일과 미리 압축해 둔 그 짝 가운데 하나를. */
        bool gzip = proven_http_accepts_coding(req->headers, req->header_count, PROVEN_HTTP_CODING_GZIP);
        (void)proven_http_exchange_respond(x, 200, NULL, 0, proven_mem_view_from_u8(gzip ? PROVEN_LIT("gzip") : PROVEN_LIT("identity")));

    } else if (proven_u8str_view_eq(req->target, PROVEN_LIT("/log"))) {
        /* 조각으로 쓰는 압축 응답은 받은 것을 붙들고 있다. flush가 그것을 내보낸다:
         * 클라이언트는 일이 끝났을 때가 아니라 지금 "started"를 읽는다. */
        proven_http_exchange_compress(x);
        if (proven_http_exchange_begin(x, 200, NULL, 0, PROVEN_HTTP_LENGTH_UNKNOWN) == PROVEN_OK &&
            proven_http_exchange_write(x, proven_mem_view_from_u8(PROVEN_LIT("started\n"))) == PROVEN_OK &&
            proven_http_exchange_flush(x) == PROVEN_OK) {
            (void)proven_http_exchange_write(x, proven_mem_view_from_u8(PROVEN_LIT("finished\n")));
        }

    } else {
        (void)proven_http_exchange_respond(x, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("bye\n")));
        proven_http_server_stop(*server);
    }
}

static void serve(void *arg) { (void)proven_http_server_run(*(proven_http_server_t **)arg); }

/* `path`를 GET한다. 읽은 본문의 크기, Content-Length, 헤더가 말하는 코딩을 알려 준다. */
static bool get(proven_http_client_t *client, proven_u16 port, const char *path,
                proven_size_t *read_size, proven_u64 *content_length, proven_http_coding_t *coding, proven_u8str_t *body) {
    char url[96];
    int n = snprintf(url, sizeof url, "http://127.0.0.1:%u%s", (unsigned)port, path);
    proven_http_client_response_t resp;
    bool ok = proven_http_client_get(client, (proven_u8str_view_t){ (const proven_byte_t *)url, (proven_size_t)n }, &resp) == PROVEN_OK && resp.status == 200;
    if (ok) {
        /* 헤더는 서버가 보낸 그대로다: 무엇이 전송됐는지는 이렇게 본다. */
        *coding = proven_http_content_coding(resp.headers, resp.header_count);
        proven_u8str_view_t length = { 0 };
        *content_length = 0;
        if (proven_http_header_find(resp.headers, resp.header_count, PROVEN_LIT("Content-Length"), &length)) {
            for (proven_size_t i = 0; i < length.size; ++i) *content_length = *content_length * 10 + (proven_u64)(length.ptr[i] - '0');
        }
        /* 한도는 여기에 도착하는 것에 걸린다 - 클라이언트가 푼다면 풀린 바이트에. */
        (void)proven_u8str_reset(body);
        ok = proven_http_client_read_all(&resp, proven_heap_allocator(), body, 1024 * 1024) == PROVEN_OK;
        *read_size = proven_u8str_as_view(body).size;
    }
    proven_http_client_finish(&resp);
    return ok;
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    fill_page();

    static proven_http_server_t *server;
    proven_http_server_config_t config = { .alloc = heap, .handler = handle, .handler_ctx = &server };
    config.compress_level = 6;              /* 1이 가장 빠르고 9가 가장 작다. 0은 이 값을 뜻한다 */
    EXAMPLE_REQUIRE(proven_http_server_create(&config, &server) == PROVEN_OK, "a server");
    proven_net_addr_t at;
    proven_err_t err = proven_http_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        proven_http_server_destroy(server);
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "it listens");
    proven_job_sys_t *threads = NULL;
    proven_job_group_t running;
    proven_job_group_init(&running);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &threads) == PROVEN_OK &&
                    proven_job_group_submit(threads, &running, serve, &server) == PROVEN_OK, "the server loop is started");

    /* 클라이언트 둘: 하나는 늘 하던 대로, 하나는 압축된 응답을 요청한다. */
    proven_http_client_config_t plain_config = { .alloc = heap, .max_idle_connections = 2 };
    proven_http_client_config_t decoding_config = { .alloc = heap, .max_idle_connections = 2, .decompress = true };
    proven_http_client_t *plain = NULL, *decoding = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&plain_config, &plain) == PROVEN_OK &&
                    proven_http_client_create(&decoding_config, &decoding) == PROVEN_OK, "two clients");

    proven_u8str_t body = { 0 };
    proven_size_t read_size = 0;
    proven_u64 sent = 0;
    proven_http_coding_t coding = PROVEN_HTTP_CODING_IDENTITY;

    /* 아무것도 설정하지 않은 클라이언트는 Accept-Encoding을 보내지 않고, 달라지는 것이 없다. */
    EXAMPLE_REQUIRE(get(plain, at.port, "/page", &read_size, &sent, &coding, &body), "the page, to a client that did not ask");
    EXAMPLE_REQUIRE(coding == PROVEN_HTTP_CODING_IDENTITY && sent == sizeof g_page && read_size == sizeof g_page, "arrives as it is");

    /* 요청한 쪽은 같은 바이트를 그 일부만으로 받는다. */
    EXAMPLE_REQUIRE(get(decoding, at.port, "/page", &read_size, &sent, &coding, &body), "the page, to a client that asked");
    EXAMPLE_REQUIRE(coding == PROVEN_HTTP_CODING_GZIP && read_size == sizeof g_page &&
                    memcmp(proven_u8str_as_view(&body).ptr, g_page, sizeof g_page) == 0, "was sent as gzip and read as the page");
    EXAMPLE_REQUIRE(sent < sizeof g_page / 5, "in less than a fifth of the bytes");
    printf("the page: %u bytes, sent as %u\n", (unsigned)sizeof g_page, (unsigned)sent);

    /* 핸들러가 요청하지 않은 응답은, 누가 달라고 하든 압축되지 않는다. */
    EXAMPLE_REQUIRE(get(decoding, at.port, "/account", &read_size, &sent, &coding, &body) && coding == PROVEN_HTTP_CODING_IDENTITY, "the account page is sent as it is");

    EXAMPLE_REQUIRE(get(decoding, at.port, "/accepts", &read_size, &sent, &coding, &body) &&
                    proven_u8str_view_eq(proven_u8str_as_view(&body), PROVEN_LIT("gzip")), "the handler can see that this client accepts gzip");
    EXAMPLE_REQUIRE(get(plain, at.port, "/accepts", &read_size, &sent, &coding, &body) &&
                    proven_u8str_view_eq(proven_u8str_as_view(&body), PROVEN_LIT("identity")), "and that the other does not");

    /* 중간에 flush한 응답도 여전히 gzip 스트림 하나다. */
    EXAMPLE_REQUIRE(get(decoding, at.port, "/log", &read_size, &sent, &coding, &body) && coding == PROVEN_HTTP_CODING_GZIP &&
                    proven_u8str_view_eq(proven_u8str_as_view(&body), PROVEN_LIT("started\nfinished\n")), "a log sent in two parts, the first flushed, reads as one");

    EXAMPLE_REQUIRE(get(plain, at.port, "/quit", &read_size, &sent, &coding, &body), "stop");
    proven_job_group_wait(threads, &running);
    proven_u8str_destroy(heap, &body);
    proven_http_client_destroy(plain);
    proven_http_client_destroy(decoding);
    proven_http_server_destroy(server);
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    return EXAMPLE_OK();
}
```

### 예제: 같은 일을 루프 위에서

<!-- example: manual/examples/ko/ex_16_http_event_gzip.c -->
```c
#include <stdio.h>
#include <string.h>

/*
 * 같은 일을 루프 위에서: 조각조각 쓰는 응답을 압축하는 이벤트 구동 서버와, 도착하는 대로
 * 풀어 읽는 이벤트 구동 클라이언트.
 *
 * 어느 쪽도 본문을 쥐고 있지 않는다. 서버는 받은 것을 압축해 출력 큐에 넣되 큐의 한도를
 * 지키고, 클라이언트는 풀린 조각을 최대 16 KiB씩 넘겨준다.
 */

#define REPORT_BYTES ((proven_size_t)300000)

typedef struct {
    proven_loop_t *loop;
    proven_http_stream_t *stream;      /* 쓰고 있는 응답 하나 */
    proven_size_t written;
    proven_size_t received, pieces, largest;
    unsigned long sum_sent, sum_received;
    bool gzip, intact, done;
    proven_err_t why;
} ex_app_t;

/* 보고서는 쓰면서 만들어진다: 전체를 담은 버퍼는 없다. */
static proven_byte_t report_byte(proven_size_t i) { return (proven_byte_t)("measured value \n"[i % 16]); }

// ---- 서버 ------------------------------------------------------------------

/* 서버가 더 받지 않을 때까지 쓴다. 자리가 나면 on_writable이 이것을 다시 부른다. */
static void pump(ex_app_t *app) {
    proven_byte_t piece[4096];
    while (app->written < REPORT_BYTES) {
        proven_size_t n = REPORT_BYTES - app->written < sizeof piece ? REPORT_BYTES - app->written : sizeof piece;
        for (proven_size_t i = 0; i < n; ++i) piece[i] = report_byte(app->written + i);
        proven_result_size_t took = proven_http_stream_write(app->stream, (proven_mem_view_t){ piece, n });
        if (took.err != PROVEN_OK) return;
        for (proven_size_t i = 0; i < took.value; ++i) app->sum_sent += piece[i];
        app->written += took.value;
        /* 첫 조각은 곧바로 내보낸다: 나머지를 아직 만드는 동안 읽는 쪽은 보고서가 시작되는 것을
         * 본다. 이것이 없으면 압축기 안에서 기다렸을 것이다. */
        if (app->written == sizeof piece) (void)proven_http_stream_flush(app->stream);
        if (took.value < n) return;        /* 압축된 출력이 한도에 닿았다: 기다린다 */
    }
    (void)proven_http_stream_end(app->stream);
}

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    (void)head;
    ex_app_t *app = ctx;
    app->stream = stream;
    /* 응답을 시작하기 전에. 여기서부터 본문은 이것이 없을 때와 똑같이 쓴다:
     * 클라이언트에게는 gzip 청크가 가지만, 알려 준 길이는 여전히 써야 할 길이다.
     * 쓴 것은 뒤따르는 것이 올 때까지 압축기 안에서 기다릴 수 있다. */
    proven_http_stream_compress(stream);
    if (proven_http_stream_begin(stream, 200, NULL, 0, REPORT_BYTES) == PROVEN_OK) pump(app);
}
static void on_writable(void *ctx, proven_http_stream_t *stream) { (void)stream; pump(ctx); }

// ---- 클라이언트 ------------------------------------------------------------

static void on_response(void *ctx, proven_http_event_request_t *request, const proven_http_response_t *head) {
    (void)request;
    ex_app_t *app = ctx;
    app->gzip = proven_http_content_coding(head->headers, head->header_count) == PROVEN_HTTP_CODING_GZIP;
}

/* 풀린 조각들. 조각은 이 함수가 돌아갈 때까지만 유효한 뷰다. */
static void on_body(void *ctx, proven_http_event_request_t *request, proven_mem_view_t piece, bool last) {
    (void)request; (void)last;
    ex_app_t *app = ctx;
    for (proven_size_t i = 0; i < piece.size; ++i) {
        if (piece.ptr[i] != report_byte(app->received + i)) app->intact = false;
        app->sum_received += piece.ptr[i];
    }
    app->received += piece.size;
    app->pieces++;
    if (piece.size > app->largest) app->largest = piece.size;
}

static void on_done(void *ctx, proven_http_event_request_t *request, proven_err_t why) {
    (void)request;
    ex_app_t *app = ctx;
    app->why = why;
    app->done = true;
    proven_loop_stop(app->loop);
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    ex_app_t app = { .intact = true };
    EXAMPLE_REQUIRE(proven_loop_create(heap, &app.loop) == PROVEN_OK, "a loop");

    /* 창 크기가 압축 응답 하나가 쓰이는 동안 쥐는 메모리를 정한다: 12비트에서 약 53 KiB,
     * 최대인 15비트에서 325 KiB. 한꺼번에 많이 다룬다면 골라야 할 숫자가 이것이다. */
    proven_http_event_server_config_t server_config = { .on = { .on_request = on_request, .on_writable = on_writable }, .ctx = &app, .compress_window_bits = 12 };
    proven_http_event_server_t *server = NULL;
    proven_net_addr_t at;
    EXAMPLE_REQUIRE(proven_http_event_server_create(app.loop, &server_config, &server) == PROVEN_OK, "a server");
    proven_err_t err = proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        proven_http_event_server_destroy(server);
        proven_loop_destroy(app.loop);
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "it listens");

    /* `decompress`는 gzip을 요청하고 풀어 준다. 그러면 `max_body_bytes`는 풀린 크기에도
     * 걸리므로, 작은 본문이 거대하게 펼쳐지더라도 한도에서 끊긴다. */
    proven_http_event_client_config_t client_config = { .decompress = true, .max_body_bytes = 1024 * 1024 };
    proven_http_event_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_event_client_create(app.loop, &client_config, &client) == PROVEN_OK, "a client that decodes");

    char url[64];
    int n = snprintf(url, sizeof url, "http://127.0.0.1:%u/report", (unsigned)at.port);
    proven_http_event_request_options_t options = {
        .url = { (const proven_byte_t *)url, (proven_size_t)n },
        .on = { on_response, on_body, NULL, on_done },
        .ctx = &app,
    };
    EXAMPLE_REQUIRE(proven_http_event_client_start(client, &options, NULL) == PROVEN_OK, "the request is started");
    EXAMPLE_REQUIRE(proven_loop_run(app.loop) == PROVEN_OK && app.done, "the loop ran until it was done");

    EXAMPLE_REQUIRE(app.why == PROVEN_OK && app.gzip, "the response came as gzip");
    EXAMPLE_REQUIRE(app.received == REPORT_BYTES && app.intact && app.sum_received == app.sum_sent, "and was delivered as the report that was written");
    EXAMPLE_REQUIRE(app.largest <= 16384 && app.pieces >= REPORT_BYTES / 16384, "in pieces of at most 16 KiB");
    printf("%u bytes written, compressed, sent, decoded and received\n", (unsigned)app.received);

    proven_http_event_client_destroy(client);
    proven_http_event_server_destroy(server);
    proven_loop_destroy(app.loop);
    return EXAMPLE_OK();
}
```

### 레퍼런스

| API | 의도 | 반환 |
|---|---|---|
| `proven_http_exchange_compress(x)` | 이 응답을 압축해 달라고 요청한다. `respond`나 `begin` 전에. 그 뒤에는 아무 일도 하지 않는다. | 없음. |
| `proven_http_stream_compress(stream)` | 이벤트 구동 서버에서 같은 일: `on_request` 안에서나 그 뒤에, 응답을 시작하기 전에. | 없음. |
| `proven_http_exchange_flush(x)`, `proven_http_stream_flush(stream)` | 압축 중인 응답이 붙들고 있는 것을 내보낸다. 응답은 계속된다. 압축하지 않는 응답에는 아무 일도 하지 않고 성공한다. | `proven_err_t`: 응답이 시작되기 전이나 끝난 뒤면 `INVALID_STATE`. 그 밖에는 쓰기와 같다. |
| `compress_level` (두 서버 설정) | 1(가장 빠름)부터 9(가장 작음). -1은 압축 없이 저장만 한다. 0은 6. 측정값은 4절에 있다. | 범위를 벗어나면 `create`가 `INVALID_ARG`. |
| `compress_window_bits` (둘 다) | 9부터 15: 스트림으로 쓰는 응답이 쓰이는 동안 쥐는 메모리(4절의 표). 0은 15. | 위와 같다. |
| `compress_zstd` (두 서버 설정) | zstd에 gzip 이상의 가중치를 주는 요청에는 `zstd`로, 그 밖에는 gzip으로 압축한다. false: gzip만. | - |
| `compress_zstd_level`, `compress_zstd_window_log` (둘 다) | 7절 인코더의 레벨(1부터 9 - 미리 압축하는 파일용인 10부터 19는 아니다 - 0은 1)과 창(10부터 23, 0은 17). | 범위를 벗어나면 `create`가 `INVALID_ARG`. |
| `decompress` (두 클라이언트 설정) | `Accept-Encoding: gzip`을 보내고 `gzip`과 `deflate` 본문을 푼다. | - |
| `decompress_zstd` (두 클라이언트 설정) | `decompress`와 함께: `Accept-Encoding: zstd, gzip`을 보내고 `zstd` 본문도 푼다. | `decompress` 없이 켜면 `create`가 `INVALID_ARG`. |
| `decompress_brotli` (두 클라이언트 설정) | `decompress`와 함께: `br`도 요청하고 `br` 본문도 푼다(v0.60.0부터). | `decompress` 없이 켜면 `create`가 `INVALID_ARG`. `PROVEN_NO_BROTLI` 빌드에서는 `UNSUPPORTED`. |
| `proven_http_accepts_coding(headers, count, coding)` | 요청의 `Accept-Encoding`이 어떤 코딩을 허용하는가: 0보다 큰 가중치로 적혀 있거나, `*`에 포함된다. | `bool`. 필드가 없으면 identity만 허용된다. |
| `proven_http_accept_weight(headers, count, coding)` | 얼마나: 그 코딩의 가중치, 없으면 `*`의 가중치, 천분율로. | 0부터 1000. `accepts_coding`이 아니라고 할 때 꼭 0. 필드가 없으면 identity는 1000, 나머지는 0. |
| `proven_http_content_coding(headers, count)` | `Content-Encoding`이 말하는 코딩. | `PROVEN_HTTP_CODING_IDENTITY`, `_GZIP`(`x-gzip`도), `_DEFLATE`, `_ZSTD`(v0.57.0부터. 그 전에는 `_OTHER`), `br`에는 `_BROTLI`(v0.60.0부터. 그 전에는 `_OTHER`), 그 밖의 것과 둘 이상에는 `_OTHER`. |

### 주의할 점과, 잘못되는 경우

- **비밀과 클라이언트가 고른 글자가 섞인 응답에는 요청하지 말 것.** 5절의 누출이 가장 널리 알려진
  모습으로 나타난 것이다(BREACH): 세션 토큰을 싣고 검색어도 그대로 되돌려 주는 페이지를 압축하면,
  암호화된 연결을 지켜보는 이에게 - 길이만으로 - 검색어가 토큰의 일부와 맞았는지를 알려 준다. TLS는
  돕지 못한다. TLS는 바이트를 가리지 크기를 가리지 않는다. 서버에 "전부 압축" 설정이 없는 까닭이
  이것이다: 판단이 응답마다 달라서 호출도 응답마다 한다.
- **스트림으로 보내는 압축 응답은 flush하지 않으면 조각조각 도착하지 않는다.** 쓴 것은 더 쓰거나
  응답이 끝날 때까지 압축기 안에 머물 수 있다. 이벤트 스트림([10장](manual-10-http-ko.md) 13절)처럼
  클라이언트가 일어나는 대로 보아야 하는 것이라면 조각마다 `proven_http_exchange_flush`나
  `proven_http_stream_flush`를 부를 것: 그때까지 쓴 것이 클라이언트가 곧바로 읽을 수 있는 형태로
  나간다. flush 한 번에는 몇 바이트와 압축률 일부가 든다. 읽는 쪽이 기다리는 자리에서만 부르고 쓸
  때마다 부르지는 말 것 - 아니면 그런 응답은 아예 압축하지 말 것.
- **크기가 다른 압축 응답에는 다른 검증자가 필요하다.** `ETag`를 보낸다면 압축된 응답과 그대로인
  응답은 서로 다른 표현이다. 다른 태그를 줄 것. 그러지 않으면 캐시가 한쪽을 다른 쪽과 맞출 수 있다.
  서버는 여러분의 태그를 고쳐 쓰지 않는다.
- **클라이언트가 보는 헤더는 서버가 보낸 것이다.** `decompress`를 켜도 `Content-Encoding: gzip`은 그대로
  있고 `Content-Length`는 여전히 압축된 바이트를 센다 - 여러분이 읽는 바이트가 아니다. 읽은 것을
  세거나, 어느 쪽이었는지는 `proven_http_content_coding`으로 본다.
- **서버가 보낼 수 있는 양의 한도는 풀린 바이트로 센다.** `read_all`의 `max_bytes`와 이벤트 구동
  클라이언트의 `max_body_bytes`는 해제기에서 나오는 것을 센다: 2메가바이트로 펼쳐지는 2킬로바이트는
  상대의 한도가 아니라 여러분의 한도에서 끊긴다. 그냥 `proven_http_client_read`는 늘 그렇듯 여러분이
  준 버퍼로 묶인다.
- **손상된 압축 본문은 `PROVEN_ERR_INVALID_FORMAT`이다.** 짧은 성공이 되는 일은 없다: 중간에 끊긴
  스트림, 체크섬이 틀린 스트림, 또 하나의 gzip 멤버(또는 Zstandard 프레임)가 아닌 바이트가 뒤따르는
  스트림. 잇달아 오는 gzip 멤버나 Zstandard 프레임 여럿은 본문 하나로 읽는다. Brotli 스트림 뒤에는 아무것도 없다.
- **`deflate`는 실제로 두 가지를 뜻한다.** 표준은 zlib 스트림이라고 하는데 raw DEFLATE를 보내는 서버가
  있다. 클라이언트는 첫 두 바이트를 보고 어느 쪽이든 읽는다. `deflate`를 요청하는 일은 없고 - `gzip`과,
  `decompress_zstd`를 켰다면 `zstd`, `decompress_brotli`를 켰다면 `br`만 요청한다 - 서버가 그것을 보내는 일도 없다.
- **풀지 않는 것:** `206 Partial Content`(압축된 본문의 일부 범위는 압축된 본문이 아니다 - 그리고
  `decompress`를 켰을 때 `Range`가 실린 요청은 `Accept-Encoding` 없이 나간다). `decompress_zstd` 없이 온
  `zstd`. `decompress_brotli` 없이 온 `br`. 그 밖의 코딩(`compress`). 겹쳐 적용한 코딩 둘. 그런 본문은 헤더와 함께, 보낸 그대로 도착한다.
- **메모리.** 풀리는 중인 응답은 열려 있는 동안 약 35 KiB(이벤트 구동 클라이언트에서는 52 KiB)를
  쓴다 - `zstd` 응답은 그 프레임의 창만큼(최대 8 MiB)과 약 400 KiB를 더, `br` 응답은 그 스트림의 창만큼(최대 16 MiB)과 그 코드들(8절)을 더, 각각 출력이 자라는 대로 할당해 쓴다. 스트림으로 압축되는 응답은 끝날 때까지 4절의 표가 윈도에 대해 말하는 만큼 - 기본값으로
  325 KiB - 을 쓴다. 한 번에 보내는 응답은 그 호출 동안만, 본문보다 크지 않은 윈도로 쓴다. 기본 윈도로
  스트림 응답 만 개면 3 GiB다: `compress_window_bits`를 낮추거나, 덜 압축한다. `zstd`로 스트림을 보내는
  응답은 기본값(레벨 1, 128 KiB 창)에서 약 0.8 MiB - gzip의 325 KiB보다 많다 - 를, 레벨 3에 512 KiB 창이면
  2 MiB까지 쥔다. 한 번에 보내는 응답에는 본문에 맞춘 표가 필요할 뿐이고, 짧은 본문이면 몇 킬로바이트다.

## 7. Zstandard

```c
#include "proven/zstd.h"

proven_err_t proven_zstd_decoder_create(proven_allocator_t alloc, proven_u64 window_max, proven_zstd_decoder_t **out);
void proven_zstd_decoder_destroy(proven_zstd_decoder_t *z);
void proven_zstd_decoder_reset(proven_zstd_decoder_t *z);
proven_err_t proven_zstd_decode(proven_zstd_decoder_t *z, proven_mem_view_t in, proven_size_t *consumed,
                                proven_mem_mut_t out, proven_size_t *produced, bool *done);
proven_err_t proven_zstd_decode_all(proven_allocator_t alloc, proven_mem_view_t in, proven_mem_mut_t out, proven_size_t *written);
```

Zstandard(RFC 8878)는 DEFLATE보다 새 형식이다: `.zst` 파일, HTTP의 `Content-Encoding: zstd`(RFC 9659),
일부 패키지 형식. 압축기는 gzip의 가장 빠른 것보다 빠른 데서부터 gzip의 가장 작은 것보다 작은 데까지
고를 수 있고, 무엇으로 압축했든 빨리 풀린다. 이 라이브러리는 이것을 읽고, v0.58.0부터는 쓴다 - v0.59.0부터는 레벨 아홉, v0.101.0부터는 열둘, v0.102.0부터는 열아홉으로(아래 "Zstandard 쓰기").

디코더는 3절의 해제기처럼 쓴다 - 상태 하나와, 있는 입력을 가져가고 있는 출력 자리를 채우는 한 걸음 -
다만 형식 때문에 두 가지가 다르다:

- **한 걸음이 할당할 수 있다.** Zstandard는 블록의 일부를 끝에서부터 읽으므로, 블록 하나(압축된 상태로
  최대 128 KiB)를 통째로 쥔 다음에 풀고, 뒤 블록들이 가리키는 기록 옆에 통째로 푼다. 이 버퍼들은
  프레임의 출력이 자라는 대로 자라고 - 300바이트라고 밝힌 프레임은 8 MiB가 아니라 300바이트의 기록만
  쓴다 - 다음 프레임을 위해 남겨 둔다. 그래서 `proven_zstd_decode`가 `PROVEN_ERR_NOMEM`을 돌려줄 수 있다.
- **출력이 디코더 안에서 기다릴 수 있다.** 여러분이 준 자리가 모자라는 동안, 최대 블록 하나만큼.

**창이 곧 메모리이고, 상한은 여러분이 정한다.** 모든 프레임은 얼마나 뒤까지 참조할 수 있는지 - 창 - 를
밝히고, 디코더는 자기가 낸 것을 그만큼 들고 있어야 한다. 프레임은 테라바이트를 주장할 수도 있다.
`window_max`는 여러분이 받아들이는 최대치다: 0이면 RFC 9659가 HTTP에 정한 8 MiB. 그보다 많이 필요한
프레임은 그것을 위해 아무것도 할당하기 전에 `PROVEN_ERR_OUT_OF_BOUNDS`다.

| | 메모리 |
|---|---|
| 만든 디코더 | 표에 약 20 KiB |
| 프레임을 푸는 동안 | 그 창(최대 여러분의 상한)과, 풀고 있는 블록에 최대 384 KiB. `proven_zstd_decode_all`로 풀 때 크기를 밝히고 들어가는 프레임은 창을 잡지 않는다 - 그 자리에서 풀린다 |

**스트림은 프레임의 연속이다.** `*done`은 프레임이 끝나는 곳에서 참이 되고, 그 호출은 거기서 멈춘다:
가져간 것이 그 프레임이다. 나머지로 다시 부르면 다음 프레임이 시작된다 - reset은 필요 없다. 건너뛸 수
있는 프레임(매직 넘버 `0x184D2A50`부터 `0x184D2A5F`)은 아무것도 내지 않고 읽고 지나간다. 사전이 필요한
프레임은 `PROVEN_ERR_UNSUPPORTED`다. HTTP로 보내는 콘텐츠는 사전을 쓰지 않는다.

### 예제: 버퍼 하나, 프레임 둘, 너무 큰 창

<!-- example: manual/examples/ko/ex_16_zstd.c -->
```c
#include <string.h>

/*
 * Zstandard: 버퍼 전체, 이어서 두 프레임으로 된 스트림을 조각조각 읽기, 그리고 남의 프레임이
 * 이 프로그램이 쓸 메모리를 정하지 못하게 하는 창 상한.
 */

/* 로그 줄 4,000바이트를 libzstd가 레벨 19, 체크섬 포함으로 쓴 것: 77바이트. */
static const proven_byte_t FRAME[] = {
    0x28, 0xb5, 0x2f, 0xfd, 0x64, 0xa0, 0x0e, 0xfd, 0x01, 0x00, 0x62, 0xc3,
    0x0c, 0x11, 0x90, 0x7d, 0x00, 0x04, 0x1c, 0xf9, 0xc4, 0x4b, 0x1e, 0x4d,
    0x80, 0xa0, 0x8f, 0x99, 0x3a, 0xe6, 0x0c, 0xe0, 0x84, 0xb9, 0x79, 0xda,
    0x52, 0xe7, 0x47, 0xe1, 0xa3, 0xb1, 0xbb, 0xdc, 0x1f, 0x9e, 0x10, 0xf3,
    0xe1, 0x8b, 0x28, 0xa3, 0x39, 0xc7, 0xde, 0xde, 0x4d, 0x45, 0x97, 0x11,
    0xd7, 0xf1, 0x4e, 0xad, 0x02, 0x00, 0xc6, 0xde, 0x95, 0x2f, 0x3b, 0x3b,
    0x9f, 0xe8, 0x22, 0xa1, 0x2a,
};

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    static const char line[] = "GET /index.html HTTP/1.1 - 200 - text/html; charset=utf-8\n";
    static proven_byte_t text[4000];
    for (proven_size_t i = 0; i < sizeof text; ++i) text[i] = (proven_byte_t)line[i % (sizeof line - 1)];

    /* 버퍼 전체: gzip과 같이 출력 버퍼가 한계다. */
    static proven_byte_t out[8000];
    proven_size_t n = 0;
    EXAMPLE_REQUIRE(proven_zstd_decode_all(heap, (proven_mem_view_t){ FRAME, sizeof FRAME }, (proven_mem_mut_t){ out, sizeof out }, &n) == PROVEN_OK &&
                    n == sizeof text && memcmp(out, text, n) == 0, "Zstandard 77바이트가 libzstd에 주어진 4,000바이트로 풀린다");
    EXAMPLE_REQUIRE(proven_zstd_decode_all(heap, (proven_mem_view_t){ FRAME, sizeof FRAME }, (proven_mem_mut_t){ out, 3999 }, &n) == PROVEN_ERR_OUT_OF_BOUNDS,
                    "그리고 3,999바이트에는 들어가지 않는다: PROVEN_ERR_OUT_OF_BOUNDS");

    /* 스트림: 같은 프레임 두 개를 10바이트씩, 자리는 500바이트씩 준다. 각 호출은 할 수 있는
     * 만큼 가져가고, `done`은 프레임 하나가 막 끝났다는 뜻이며, 다음 호출은 그대로 뒤따르는
     * 프레임을 이어 읽는다. */
    static proven_byte_t two[2 * sizeof FRAME];
    memcpy(two, FRAME, sizeof FRAME);
    memcpy(two + sizeof FRAME, FRAME, sizeof FRAME);
    proven_zstd_decoder_t *z = NULL;
    EXAMPLE_REQUIRE(proven_zstd_decoder_create(heap, 0, &z) == PROVEN_OK, "기본 창 상한(8 MiB)의 디코더");
    proven_size_t in_at = 0, out_at = 0;
    int frames = 0;
    while (in_at < sizeof two || out_at < sizeof out) {
        proven_size_t give = sizeof two - in_at < 10 ? sizeof two - in_at : 10, room = sizeof out - out_at < 500 ? sizeof out - out_at : 500;
        proven_size_t used = 0, made = 0;
        bool done = false;
        EXAMPLE_REQUIRE(proven_zstd_decode(z, (proven_mem_view_t){ two + in_at, give }, &used, (proven_mem_mut_t){ out + out_at, room }, &made, &done) == PROVEN_OK, "한 걸음");
        in_at += used;
        out_at += made;
        if (done && used + made > 0) frames++;
        if (used == 0 && made == 0) break;              /* 줄 것이 더 없거나, 둘 곳이 없다 */
    }
    EXAMPLE_REQUIRE(frames == 2 && out_at == 2 * sizeof text && memcmp(out + sizeof text, text, sizeof text) == 0, "프레임 둘이 차례로, 8,000바이트");

    /* 창 상한. 모든 프레임은 얼마나 뒤까지 참조할 수 있는지 밝히고, 디코더는 그만큼을 들고
     * 있어야 한다. 손으로 만든 이 헤더는 1 MiB를 요구한다. 64 KiB로 제한한 디코더는 그것을 위해
     * 아무것도 할당하기 전에 거부한다. reset이 디코더를 다시 쓸 수 있게 한다. */
    static const proven_byte_t wide[] = { 0x28, 0xb5, 0x2f, 0xfd, 0x00, (20 - 10) << 3, 0x19, 0x00, 0x00, 'a', 'b', 'c' };
    proven_zstd_decoder_t *small = NULL;
    proven_size_t used = 0, made = 0;
    bool done = false;
    EXAMPLE_REQUIRE(proven_zstd_decoder_create(heap, 64 * 1024, &small) == PROVEN_OK, "64 KiB로 제한한 디코더");
    EXAMPLE_REQUIRE(proven_zstd_decode(small, (proven_mem_view_t){ wide, sizeof wide }, &used, (proven_mem_mut_t){ out, sizeof out }, &made, &done) == PROVEN_ERR_OUT_OF_BOUNDS,
                    "1 MiB 창이 필요한 프레임: PROVEN_ERR_OUT_OF_BOUNDS");
    proven_zstd_decoder_reset(small);
    EXAMPLE_REQUIRE(proven_zstd_decode(small, (proven_mem_view_t){ FRAME, sizeof FRAME }, &used, (proven_mem_mut_t){ out, sizeof out }, &made, &done) == PROVEN_OK &&
                    done && made == sizeof text, "reset 뒤에는 상한 안의 프레임이 풀린다");
    EXAMPLE_REQUIRE(proven_zstd_decode(z, (proven_mem_view_t){ wide, sizeof wide }, &used, (proven_mem_mut_t){ out, sizeof out }, &made, &done) == PROVEN_OK &&
                    done && made == 3, "기본 상한은 그것을 받아들인다");

    proven_zstd_decoder_destroy(small);
    proven_zstd_decoder_destroy(z);
    return EXAMPLE_OK();
}
```

### 레퍼런스

| API | 의도 | 반환 |
|---|---|---|
| `proven_zstd_decoder_create(alloc, window_max, &z)` | 디코더. `window_max`는 1 KiB부터 2 GiB. 0은 8 MiB. | `INVALID_ARG`, `NOMEM`. |
| `proven_zstd_decode(z, in, &consumed, out, &produced, &done)` | `in`의 일부를 가져가고 `out`의 일부를 채운다. 프레임 끝에서 멈춘다. 낸다고 알린 바이트만 쓴다. | `INVALID_FORMAT`(손상, 맞지 않는 체크섬이나 밝힌 크기), `OUT_OF_BOUNDS`(상한을 넘는 창), `UNSUPPORTED`(사전), `NOMEM`, `INVALID_ARG`. 오류 뒤에는 reset까지 같은 오류. |
| `proven_zstd_decoder_reset(z)` | 진행 중인 스트림을 잊는다. 버퍼는 남긴다. | 없음. |
| `proven_zstd_decoder_destroy(z)` | 해제한다. NULL도 된다. | 없음. |
| `proven_zstd_decode_all(alloc, in, out, &written)` | `in`의 모든 프레임을 `out`으로, 기본 상한으로. `out`의 크기가 출력의 한도다. 크기를 밝히고 들어가는 프레임은 `out`에 바로 풀며 자기 창을 따로 잡지 않는다(v0.86.0부터). 그때 `out`의 `*written` 뒤 바이트가 쓰일 수 있다. | 들어가지 않으면 `OUT_OF_BOUNDS`. `in`이 비었거나 프레임 중간에서 끝나도 `INVALID_FORMAT`. |

### 주의할 점과, 잘못되는 경우

- **메모리는 데이터의 크기가 아니라 창이 정한다.** 서버가 - 또는 여러분이 프레임을 풀어 주는 누구든 -
  여러분의 상한 안에서 그것을 고른다. 응답 천 개를 동시에, 각각 8 MiB 창으로 풀면 8 GiB다. 할 수 있는
  곳에서는 `window_max`를 낮추고(자기 `.zst` 파일을 푸는 디코더는 그 파일이 쓰는 창을 안다), 디코더 수를
  연결 수처럼 셀 것.
- **Zstandard에서 체크섬은 선택이다.** 체크섬 없이 쓴 프레임은 손상되고도 틀린 바이트로 풀릴 수 있다.
  디코더는 해석되지 않는 것만 거부할 수 있다. 중요한 곳에서는 푼 것을 직접 확인할 것.
- **gzip 스트림이 아니다.** `proven_zstd_decode`는 Zstandard만 읽는다. HTTP에서는 클라이언트가
  `Content-Encoding`을 보고 디코더를 고른다. 파일이라면 첫 네 바이트(`28 b5 2f fd`)를 본다.

**libzstd가 거부하는 것은 하나도 받아들이지 않고, libzstd가 읽어 넘기는 손상 일부는 거부한다.** RFC 8878이
디코더에 맡긴 경우는 libzstd 1.5.7이 정하는 대로 - 12비트까지의 Huffman 코드를 읽는다 - 또는 더 엄격하게
정했다: 모든 Huffman 스트림과 시퀀스 스트림은 첫 비트까지 정확히 읽혀야 한다(libzstd의 빠른 Huffman
디코더는 스트림 넷에 대해 이것을 확인하지 않는다). 바이트가 없는 압축 블록은 RFC 8878대로 거부한다(libzstd의
스트림 디코더는 빈 블록으로 받고, 한 번 호출 디코더는 거부한다). 등록된 테스트 밖에서, libzstd가 만든 스트림
92개를 자르고 바꾼 사본 80,232개를 양쪽에 주었다: libzstd가 거부한 것을 여기서 받아들인 것은 없었고, 둘 다
받아들인 것이 다르게 나온 것도 없었으며, libzstd가 받아들인 1,985개를 여기서는 거부했다: 1,948개는 네
Huffman 스트림 중 하나가 정확히 읽히지 않아서, 25개는 프레임 뒤의 바이트 때문에(libzstd를 부른 방식인
Node는 그것을 무시한다), 8개는 바이트가 없는 압축 블록이어서, 4개는 리터럴을 담기에 너무 짧은 블록이어서다.

**속도.** 개발 기계에서, 이 라이브러리의 C 소스(2.9 MiB)를 libzstd가 레벨 1, 3, 19로 압축한 것은
`proven_zstd_decode_all`로 각각 약 650, 650, 750 MiB/s, 디코더에 한 번에 다 주면 약 590, 570, 730 MiB/s로
풀린다. 같은 프로세스의 libzstd는 약 1,020, 940, 1,030 MiB/s - 이 디코더는 libzstd 속도의 0.6-0.7이다
(v0.86.0. 그 전에는 절반쯤). v0.86.0 전에, 같은 소스 2.2 MiB를 gzip으로 해서 3절의 해제기로 풀면 약
335 MiB/s였다.

### Zstandard 쓰기

```c
proven_err_t proven_zstd_encoder_create(const proven_zstd_encoder_options_t *options, proven_zstd_encoder_t **out);
void proven_zstd_encoder_destroy(proven_zstd_encoder_t *z);
void proven_zstd_encoder_reset(proven_zstd_encoder_t *z);
proven_err_t proven_zstd_encode(proven_zstd_encoder_t *z, proven_mem_view_t in, proven_size_t *consumed,
                                proven_mem_mut_t out, proven_size_t *produced, proven_zstd_flush_t flush, bool *done);
proven_size_t proven_zstd_encode_bound(proven_size_t input_size);
proven_err_t proven_zstd_encode_all(const proven_zstd_encoder_options_t *options, proven_mem_view_t in, proven_mem_mut_t out, proven_size_t *written);
```

인코더는 3절의 압축기처럼 쓴다: 옵션(`alloc`, `level`, `window_log`, `checksum`), 상태, 그리고 flush가 있는
한 걸음 - 더 올 때는 `PROVEN_ZSTD_FLUSH_NONE`, 프레임은 이어 가면서 지금까지 준 것을 모두 풀 수 있게
하려면 `_BLOCK`, 프레임을 끝내려면 `_FINISH`. 만드는 프레임은 어떤 Zstandard 디코더든 읽는다. 같은 입력에
대해 libzstd가 썼을 바이트와 같지는 않다.

**libzstd와 같은 레벨.** 레벨 1은 해시 표 하나로, 레벨 2와 3은 둘로 일치를 찾는다(libzstd의
"fast"와 "double fast"). 레벨 4부터 9는 모든 위치를 해시 체인에 두고 그 앞쪽 4에서 32개를 보며, 찾은 가장 좋은
일치를 고르거나(레벨 4와 5), 그 전에 한 바이트(6) 또는 두 바이트(7부터 9) 뒤에 더 좋은 것이 있는지 본다.
블록마다 리터럴에는 Huffman 코드를, 시퀀스에는 FSE 표를 붙이는데, 표마다 그 블록에 드는 비용을 보고 고른다.
4절의 세 파일을 개발 머신의 코어 하나에서, 릴리스 플래그로 잰 값:

| | HTML 페이지 | Markdown | C 소스 |
|---|---|---|---|
| 이 라이브러리 gzip 레벨 1 | 26.25 %, 45 MiB/s | 36.77 %, 33 | 27.24 %, 43 |
| 이 라이브러리 gzip 레벨 6 | 22.62 %, 25 | 32.89 %, 16 | 22.88 %, 21 |
| 이 라이브러리 Zstandard 레벨 1 | 24.93 %, 263 | 36.19 %, 191 | 26.71 %, 234 |
| 이 라이브러리 Zstandard 레벨 3 | 23.83 %, 193 | 34.58 %, 132 | 25.50 %, 172 |
| 이 라이브러리 Zstandard 레벨 6 | 21.66 %, 67 | 32.68 %, 49 | 22.79 %, 64 |
| 이 라이브러리 Zstandard 레벨 9 | 21.22 %, 37 | 32.11 %, 28 | 22.21 %, 36 |
| libzstd 1.5.7 레벨 1 | 25.27 %, 312 | 36.68 %, 231 | 26.91 %, 273 |
| libzstd 1.5.7 레벨 3 | 23.76 %, 232 | 34.42 %, 161 | 25.42 %, 213 |
| libzstd 1.5.7 레벨 6 | 21.93 %, 74 | 32.65 %, 54 | 22.97 %, 67 |
| libzstd 1.5.7 레벨 9 | 21.25 %, 48 | 32.12 %, 36 | 22.22 %, 43 |

레벨 1은 이 라이브러리의 gzip 레벨 1보다 다섯에서 여섯 배 빠르고 더 작다. 레벨 6은 gzip 레벨 6보다 크지 않고
2.6에서 3.2배 빠르다. libzstd에 견주면 레벨 1~3과 5~8은 속도 0.8에서 1.0배, 레벨 9는 약 0.8배, 크기는 0.3포인트
안이다. 레벨 6은 libzstd의 레벨 6이 보는 8개 대신 12개의 후보를 보는데, 그래서 세 파일 모두에서 gzip 6 이하에
머문다(libzstd 6은 C 소스에서 gzip 6보다 조금 크다). 레벨 4는 libzstd가 그 레벨에서 쓰는 방법이 아니다: 약 1포인트
더 작고, 속도는 절반쯤이다.

v0.103.0부터 256 KiB가 넘는 입력을 proven_zstd_encode_all로 압축하면 레벨 6~9도 그런 입력에 libzstd가 쓰는 더 큰
표를 쓴다: 2^19~2^21 항목의 해시와 2^18~2^20의 체인, 각각 창에 맞춰 줄인다(기본 창에서 레벨 6은 3 MiB, 7과 8은
6 MiB, 9는 8 MiB, 가장 큰 창에서는 12 MiB). 스트림(서버가 쓰는 쪽)은 위의 1 MiB를 그대로 쓴다. 같은 레벨, 같은 창의
libzstd와 견주면(3.9 MB 실행 파일, 2.7 MB C 소스, 3.7 MB JSON, 395 KB 텍스트):

| 이 라이브러리 / libzstd | 실행 파일 | C 소스 | JSON | 텍스트 |
|---|---|---|---|---|
| 레벨 6, 기본 창, 이전 / v0.103.0부터 | 1.017 / 1.009 | 1.002 / 0.996 | 0.984 / 0.986 | 0.998 / 0.995 |
| 레벨 9, 기본 창 | 0.999 / 0.987 | 1.006 / 0.993 | 1.014 / 1.012 | 0.999 / 0.996 |
| 레벨 9, 창 23 | 1.009 / 0.989 | 1.021 / 0.998 | 0.983 / 0.988 | 0.999 / 0.996 |

메가바이트 단위 입력에서 약 1 %, 가장 큰 창에서는 2 % 작아진다.

**레벨 10에서 12**(v0.101.0부터)는 다시 lazy2이고, libzstd가 그 레벨에 쓰는 더 큰 표 - 해시 2^22, 2^22,
2^23 항목과 체인 2^21, 2^21, 2^22, 각각 창에 맞춰 줄인다 - 와 후보 48, 96, 128개를 쓴다: 한 번 압축해 여러 번
보내는 파일용이다. 같은 레벨의 libzstd와, 다른 입력 넷에서 가장 큰 창(`window_log` 23)으로 비교했다(각 한 번,
부하가 있는 머신이라 속도는 대략이다):

| | C 소스 3.0 MB | HTML 페이지 330 KB | JSON 191 KB | 실행 파일 2.0 MB |
|---|---|---|---|---|
| 레벨 9, 이 라이브러리 / libzstd | 크기의 1.020 | 0.990 | 1.003 | 0.987 |
| 레벨 10 | 1.001 | 0.991 | 1.002 | 0.974 |
| 레벨 11 | 1.001 | 0.991 | 1.007 | 0.973 |
| 레벨 12 | 0.997 | 0.988 | 1.013 | 0.973 |
| 레벨 12 속도, 이 라이브러리 / libzstd | 4.7 / 23 MB/s | 21 / 50 | 41 / 23 | 4.6 / 19 |

레벨 12는 레벨 9보다 2~5 % 작고, 속도는 그 5분의 1에서 절반이다. libzstd는 후보를 더 빨리 찾고(행에 둔다),
256 KiB 아래에서는 레벨 11부터 이진 트리로 바꾼다 - JSON 줄의 차이다. 기본 창(19)에서는 표가 각각 2^20이고 C
소스가 libzstd보다 2포인트 크다. libzstd는 이 레벨들에서 4 MiB를 쓴다.

**레벨 13에서 19**(v0.102.0부터). 13에서 15는 같은 파싱을 이진 트리 위에서 한다: 모든 위치를 같은 해시의
앞선 위치들로 된 트리에, 뒤따르는 바이트 순서로 두므로, 가장 긴 일치를 체인을 따라가지 않고 트리를 내려가며
찾는다(트리 2^21에서 2^22 위치, 비교하는 노드 32, 64, 128). 16에서 19는 같은 트리 위에서 **최적으로** 파싱한다:
한 위치에서 찾은 모든 일치와 세 반복 오프셋에, 쓰는 데 드는 값 - 오프셋, 길이, 그 시퀀스의 리터럴 길이, 리터럴
바이트마다, 프레임이 지금까지 쓴 것에서 - 을 매기고, 4,096바이트까지의 구간마다 가장 싼 길을 고른다; 18과 19는
3바이트 일치도 쓴다. 256 KiB 이하의 버퍼를 통째로 주면 libzstd처럼 레벨 13부터 최적으로 파싱한다. 같은 입력
넷과 창:

| 이 라이브러리 / libzstd | C 소스 | HTML 페이지 | JSON(191 KB) | 실행 파일 |
|---|---|---|---|---|
| 레벨 13 | 0.991 | 0.973 | 0.959 | 0.972 |
| 레벨 15 | 1.010 | 0.995 | 1.008 | 0.973 |
| 레벨 16 | 0.993 | 0.996 | 1.008 | 0.966 |
| 레벨 17 | 0.998 | 1.001 | 1.007 | 0.997 |
| 레벨 18 | 1.005 | 1.000 | 1.006 | 1.009 |
| 레벨 19 | 1.005 | 1.003 | 1.006 | 1.010 |
| 레벨 19 속도, 이 라이브러리 / libzstd | 3.0 / 2.7 MB/s | 3.8 / 2.1 | 4.0 / 1.8 | 3.7 / 3.9 |

레벨 19는 이 네 입력에서 레벨 9보다 7~13 % 작다(번호 붙은 줄에서는 4분의 1). libzstd의 레벨 19와 알려진 차이가
하나 있다: libzstd는 첫 블록을 한 번 먼저 훑어 통계를 배운다; 여기서는 블록 자신의 바이트와 형식의 기본 분포에서
시작한다.

**장거리 일치**(`long_distance`, v0.103.0부터)는 libzstd의 `--long`과 같은 방식이다: 마지막 64바이트의 굴림
해시가 대략 128개 위치 가운데 하나를 고르고, 그 위치들을 창 전체에 걸쳐 버킷에 담아 두며(창 128바이트마다 8바이트:
`window_log` 23에서 512 KiB), 그것으로 찾은 64바이트 이상의 일치를 쓰고 그 사이는 레벨 자신의 매처가 채운다. 빠른
레벨의 표가 닿는 거리보다 더 먼 곳에서 반복되는 큰 입력 - 아카이브, 로그, 한 파일의 여러 판 - 을 위한 것이다.
레벨 1~5에서만 동작한다: 6부터는 레벨 자신의 매처가 거의 같은 만큼을 찾고, 9부터는 파싱에 억지로 넣은 긴 일치가
아끼는 것보다 비싸서(레벨 19에서 3 %) 아무것도 하지 않는다. 가장 큰 창에서, 켰을 때 / 껐을 때의 크기와 속도:

| 레벨 | C 소스, 실행 파일, C 소스 다시, 9.3 MB | 실행 파일, 3.9 MB | C 소스, 2.7 MB | JSON, 3.7 MB |
|---|---|---|---|---|
| 1 | 0.710, 1.26배 빠름 | 0.901, 0.96 | 0.992, 0.84 | 1.000, 0.83 |
| 3 | 0.754, 1.16 | 0.979, 0.89 | 0.994, 0.88 | 1.000, 0.84 |
| 5 | 0.894, 1.02 | 0.988, 0.92 | 0.996, 0.91 | 1.000, 0.83 |

첫 입력에서 레벨 3에 이것을 켜면 같은 창에서 `--long`을 켠 libzstd 레벨 3과 1.5 % 안쪽이다. 그런 반복이 없는
입력에서는 속도가 4~22 % 준다: 굴림 해시가 모든 바이트를 읽는다.

표는 v0.89.0 전에 잰 것이다. 그 뒤로 BMI2(플래그 없이 레지스터만큼 시프트)가 있는 x86 프로세서에서는 인코더가
그것에 맞춰 컴파일한 블록 코더의 두 번째 빌드를 쓴다 - 같은 C, 같은 출력이고, 프로세서에 처음 물을 때 고른다.
0.4~3.9 MB의 다른 입력 넷에서 레벨 1~3은 libzstd 속도의 0.76~0.88에서 0.82~0.96이 되었다.

**메모리.** `proven_zstd_encode_all`은 호출자의 버퍼를 그 자리에서 압축하고, 표와 블록 버퍼를 그 크기에
맞춘다: 짧은 입력이면 몇 킬로바이트다. 스트림은 창을 자기 버퍼에 두고, 많아야 다음을 쥔다:

| | 메모리 |
|---|---|
| 해시 표(인코더를 만들거나 reset할 때 0으로 채운다) | 레벨 1에서 64 KiB, 2에서 96 KiB, 3과 4에서 768 KiB, 5부터 9에서 1 MiB; 10부터 19에서 기본 창이면 8 MiB, 가장 큰 창이면 10과 11에서 24 MiB, 13과 16에서 32 MiB, 12, 14, 17, 18에서 48 MiB, 15에서 64 MiB, 19에서 80 MiB(통째로 주는 버퍼가 짧으면 더 적다) |
| 최적 파싱의 경로와 3바이트 표(레벨 16부터 19, 그리고 256 KiB 이하의 통째 버퍼에서 13부터 15) | 약 380 KiB(짧은 통째 버퍼면 더 적다) |
| 블록 하나의 일: 시퀀스, 리터럴, 출력 | 약 500 KiB |
| 스트림의 입력 버퍼 | 창과 창의 절반, 또는 그보다 많으면 창과 128 KiB: 기본 512 KiB에서 768 KiB |

창(`window_log`, 10부터 23, 0은 19)은 디코더가 그 프레임을 읽으려면 들고 있어야 하는 양이라서, 읽는 쪽의
메모리이기도 하다: 23(8 MiB)이 RFC 9659가 HTTP 클라이언트에게 받아들이라고 하는 최대다. 창보다 크지 않은
버퍼를 통째로 주면 그 크기를 창으로 밝힌다.

### 예제: 통째로, flush가 있는 스트림으로, 그리고 요청의 가중치

<!-- example: manual/examples/ko/ex_16_zstd_encode.c -->
```c
#include <string.h>

/*
 * Zstandard 쓰기: 버퍼 전체, flush에서 가진 것을 넘기는 스트림, 그리고 요청의 Accept-Encoding을
 * 보고 서버가 zstd와 gzip 가운데 고르는 일.
 */

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    static const char line[] = "GET /index.html HTTP/1.1 - 200 - text/html; charset=utf-8\n";
    static proven_byte_t text[40000];
    for (proven_size_t i = 0; i < sizeof text; ++i) text[i] = (proven_byte_t)line[i % (sizeof line - 1)];
    static proven_byte_t frame[48000], back[40000];

    /* 버퍼 전체. encode_bound면 언제나 충분하다. 프레임이 입력의 크기를 밝히므로 디코더는 그보다
     * 큰 창을 둘 필요가 없다. */
    proven_zstd_encoder_options_t o = { .alloc = heap, .level = 1 };
    proven_size_t n = 0, got = 0;
    EXAMPLE_REQUIRE(proven_zstd_encode_bound(sizeof text) <= sizeof frame, "40,000바이트의 상한이 버퍼에 들어간다");
    EXAMPLE_REQUIRE(proven_zstd_encode_all(&o, (proven_mem_view_t){ text, sizeof text }, (proven_mem_mut_t){ frame, sizeof frame }, &n) == PROVEN_OK && n < 200,
                    "되풀이되는 로그 줄 40,000바이트가 200바이트 미만으로 압축된다");
    EXAMPLE_REQUIRE(proven_zstd_decode_all(heap, (proven_mem_view_t){ frame, n }, (proven_mem_mut_t){ back, sizeof back }, &got) == PROVEN_OK &&
                    got == sizeof text && memcmp(back, text, got) == 0, "그리고 한 바이트도 빠짐없이 풀린다");

    /* 스트림: 레벨 6, 64 KiB 창, 체크섬 포함으로 1,000바이트씩. 처음 5,000바이트 뒤의 블록 flush는 그때까지를
     * 바로 풀 수 있게 한다 - 서버가 더 보낼 것을 기다리기 전에 하는 일이다. FINISH가 프레임을 끝낸다. */
    proven_zstd_encoder_t *z = NULL;
    o = (proven_zstd_encoder_options_t){ .alloc = heap, .level = 6, .window_log = 16, .checksum = true };
    EXAMPLE_REQUIRE(proven_zstd_encoder_create(&o, &z) == PROVEN_OK, "레벨 6 - gzip 6 정도의 크기 - , 64 KiB 창의 인코더");
    proven_size_t in_at = 0, out_at = 0;
    bool done = false;
    while (!done) {
        const proven_size_t give = sizeof text - in_at < 1000 ? sizeof text - in_at : 1000;
        const proven_zstd_flush_t flush = in_at + give == sizeof text ? PROVEN_ZSTD_FLUSH_FINISH
                                        : in_at + give == 5000 ? PROVEN_ZSTD_FLUSH_BLOCK : PROVEN_ZSTD_FLUSH_NONE;
        proven_size_t used = 0, made = 0;
        EXAMPLE_REQUIRE(proven_zstd_encode(z, (proven_mem_view_t){ text + in_at, give }, &used, (proven_mem_mut_t){ frame + out_at, sizeof frame - out_at }, &made, flush, &done) == PROVEN_OK,
                        "한 걸음");
        in_at += used;
        out_at += made;
        if (flush == PROVEN_ZSTD_FLUSH_BLOCK) {
            /* 지금까지 나온 것만으로 지금까지의 전부가 풀린다. 프레임은 아직 끝나지 않았다. */
            proven_zstd_decoder_t *d = NULL;
            proven_size_t dused = 0, dmade = 0;
            bool dend = true;
            EXAMPLE_REQUIRE(proven_zstd_decoder_create(heap, 0, &d) == PROVEN_OK &&
                            proven_zstd_decode(d, (proven_mem_view_t){ frame, out_at }, &dused, (proven_mem_mut_t){ back, sizeof back }, &dmade, &dend) == PROVEN_OK &&
                            dmade == 5000 && !dend, "flush 뒤 디코더가 5,000바이트를 모두 돌려주고 나머지를 기다린다");
            proven_zstd_decoder_destroy(d);
        }
    }
    EXAMPLE_REQUIRE(proven_zstd_decode_all(heap, (proven_mem_view_t){ frame, out_at }, (proven_mem_mut_t){ back, sizeof back }, &got) == PROVEN_OK &&
                    got == sizeof text && memcmp(back, text, got) == 0, "끝난 프레임은 체크섬까지 맞고 40,000바이트로 풀린다");
    proven_zstd_encoder_reset(z);                         /* 같은 인코더, 새 프레임 */
    proven_size_t used = 0, made = 0;
    EXAMPLE_REQUIRE(proven_zstd_encode(z, (proven_mem_view_t){ text, 300 }, &used, (proven_mem_mut_t){ frame, sizeof frame }, &made, PROVEN_ZSTD_FLUSH_FINISH, &done) == PROVEN_OK &&
                    done && used == 300, "reset 뒤 두 번째 프레임");
    proven_zstd_encoder_destroy(z);

    /* 요청이 어느 코딩을 가장 원하는가: 천분율의 가중치. curl과 브라우저는 zstd와 gzip을 같은
     * 가중치로 보낸다. zstd를 제공하는 서버(compress_zstd)는 그때 zstd를 고른다. */
    const proven_http_header_t asks[] = { { PROVEN_LIT("Accept-Encoding"), PROVEN_LIT("gzip, deflate, br, zstd") } };
    const proven_http_header_t prefers_gzip[] = { { PROVEN_LIT("Accept-Encoding"), PROVEN_LIT("gzip;q=1.0, zstd;q=0.5") } };
    EXAMPLE_REQUIRE(proven_http_accept_weight(asks, 1, PROVEN_HTTP_CODING_ZSTD) == 1000 && proven_http_accept_weight(asks, 1, PROVEN_HTTP_CODING_GZIP) == 1000,
                    "브라우저의 목록: zstd도 gzip도 1000");
    EXAMPLE_REQUIRE(proven_http_accept_weight(prefers_gzip, 1, PROVEN_HTTP_CODING_ZSTD) == 500 && proven_http_accept_weight(prefers_gzip, 1, PROVEN_HTTP_CODING_GZIP) == 1000,
                    "gzip을 더 원하는 클라이언트: zstd 500, gzip 1000");
    return EXAMPLE_OK();
}
```

### 레퍼런스: 인코더

| API | 의도 | 반환 |
|---|---|---|
| `proven_zstd_encoder_create(&options, &z)` | 인코더: `level` 1부터 19(0은 libzstd의 기본값인 3), `window_log` 10부터 23(0은 19), `checksum`. 해시 표를 할당하고, 버퍼는 입력이 오는 대로. | `INVALID_ARG`, `NOMEM`. |
| `proven_zstd_encode(z, in, &consumed, out, &produced, flush, &done)` | `in`의 일부를 받고 `out`의 일부를 채운다. flush가 있으면 입력을 다 받고 더 나오는 것이 없을 때까지(FINISH면 `done`까지) 다시 부른다. | FINISH 뒤의 입력은 `INVALID_STATE`(먼저 reset). `NOMEM` - 그 뒤로는 reset할 때까지 모든 호출이 그렇게 말한다. `INVALID_ARG`. |
| `proven_zstd_encoder_reset(z)` | 같은 옵션으로 새 프레임을 시작한다. 버퍼는 둔다. | 없음. |
| `proven_zstd_encoder_destroy(z)` | 해제한다. NULL 허용. | 없음. |
| `proven_zstd_encode_bound(n)` | `n`바이트의 FINISH나 `encode_all`이 쓸 수 있는 최대. | 크기. 상한이 크기에 들어가지 않으면 가장 큰 크기. |
| `proven_zstd_encode_all(&options, in, out, &written)` | `in`을 자기 크기를 밝힌 프레임 하나로 `out`에. | `out`이 작으면 `OUT_OF_BOUNDS`. |

### 쓸 때 주의할 점

스트림은 자기 크기를 밝히지 않으므로 디코더는 창 전체를 준비해야 한다: `window_log`는 압축률만이
아니라 읽는 쪽을 보고 고를 것. 체크섬은 요청하지 않으면 없다: 체크섬이 없으면 손상된 프레임이 틀린
바이트로 풀릴 수 있다. 블록 flush 한 번에는 블록 헤더와 짧은 블록의 표가 든다 - 읽는 쪽이 기다리는
자리에서 flush하고, 쓸 때마다 하지는 말 것. 압축은 gzip보다 더 비밀스럽지 않다: 클라이언트가 고른 글자
옆의 비밀에 대한 6절의 주의는 gzip에서와 같이 `zstd`에도 해당한다.

## 8. Brotli

```c
#include "proven/brotli.h"

proven_err_t proven_brotli_decoder_create(proven_allocator_t alloc, proven_u64 window_max, proven_brotli_decoder_t **out);
void proven_brotli_decoder_destroy(proven_brotli_decoder_t *b);
void proven_brotli_decoder_reset(proven_brotli_decoder_t *b);
proven_err_t proven_brotli_decode(proven_brotli_decoder_t *b, proven_mem_view_t in, proven_size_t *consumed,
                                  proven_mem_mut_t out, proven_size_t *produced, bool *done);
proven_err_t proven_brotli_decode_all(proven_allocator_t alloc, proven_mem_view_t in, proven_mem_mut_t out, proven_size_t *written);
```

Brotli(RFC 7932)는 HTTP의 `Content-Encoding: br`이다. 서버는 대개 미리 한 번, 가장 높은 품질로 압축해 두는
파일에 쓴다. 그 품질에서는 gzip과 Zstandard보다 작게 나온다. 그 품질로 압축하는 일은 느리고, 푸는 일은
품질과 상관없이 빠르다. 이 라이브러리는 v0.60.0부터 이것을 읽고, 쓰지는 않는다: 여기의 서버는 gzip이나
Zstandard를 보낸다.

디코더는 7절의 것처럼 쓴다 - 상태 하나와, 있는 만큼의 입력을 받아 있는 만큼의 출력 공간을 채우는 한
걸음. 그리고 어디서든 멈출 수 있다: 입력이 거기서 끝나면 코드의 두 비트 사이에서, 공간이 거기서 끝나면
복사 한가운데서. 두 가지가 7절과 같다:

- **한 걸음이 할당할 수 있다.** 뒤의 바이트가 참조하는 히스토리는 스트림의 출력과 함께 자라며, 스트림이
  밝힌 창을 넘지 않는다. 그리고 메타블록(Brotli의 단위, 출력 16 MiB까지)마다 접두 부호를 따로 가져오고,
  그것들은 개수에 따라 자라는 표 하나에 만들어진다. 둘 다 다음 스트림을 위해 남겨 둔다. 그래서
  `proven_brotli_decode`는 `PROVEN_ERR_NOMEM`을 돌려줄 수 있다.
- **출력이 디코더 안에서 기다릴 수 있다.** 여러분이 준 공간이 모자라는 동안 그 히스토리 안에서.

**창이 곧 메모리이고, 상한은 여러분의 것이다.** 스트림의 처음 몇 비트가 얼마나 멀리 거슬러 참조할지를
말한다: 2^WBITS - 16바이트, WBITS는 10부터 24, 그러니 최대 16 MiB. `window_max`는 여러분이 허락하는
최대다: 0은 16 MiB로, RFC 7932가 허용하는 모든 스트림을 받아들인다. 그보다 많이 필요한 스트림은 그것을
위해 아무것도 할당하기 전에 `PROVEN_ERR_OUT_OF_BOUNDS`다. RFC 7932가 아닌 libbrotli의 "큰 창" 변형은
잘못된 형식으로 거절한다.

| | 메모리 |
|---|---|
| 만든 디코더 | 약 24 KiB |
| 스트림을 푸는 동안 | 히스토리 - 출력이 닿은 만큼의 창, 최대 2^WBITS바이트와 32바이트 - 와 코드들: 인코더가 쓰는 것은 수십 KiB, 메타블록이 가질 수 있는 최대(세 종류마다 256개, 각각 코드 표가 될 수 있는 가장 큰 크기)는 1,343,024바이트 |

**스트림 하나, 그 뒤에는 아무것도 없다.** Brotli 스트림은 gzip 멤버나 Zstandard 프레임처럼 잇달아 오지
않는다. `*done`은 스트림이 끝나는 곳에서 참이 되고, 그 호출은 거기서 멈춘다 - 소비한 것이 곧 그
스트림이다 - 그 뒤의 호출은 아무것도 더 소비하지 않는다. 다른 스트림을 읽으려면 reset한다.
`proven_brotli_decode_all`은 끝 뒤의 바이트를 거절하고, HTTP 클라이언트도 그렇다.

**정적 사전.** Brotli 스트림은 모든 디코더가 지니는 사전에서 단어를 복사할 수 있다: 여러 언어의 단어
122,784바이트와, 단어마다 바꾸는 방식 121가지(앞에 공백, 첫 글자를 대문자로, `ing ` 같은 접미사).
이것이 이 디코더가 프로그램에 더하는 것의 대부분이다: 이것이 있으면 라이브러리가 162,351바이트 커진다
(x86-64에서 `-O2`로, 코드와 데이터 998,985바이트 대 836,634바이트). `-DPROVEN_NO_BROTLI`로 만든 빌드는 디코더와
사전을 뺀다. 그때 `proven.h`는 `brotli.h`를 포함하지 않고, Brotli를 풀라는 HTTP 클라이언트는
`PROVEN_ERR_UNSUPPORTED`다.

### 예제: 버퍼, 뒤에 무언가 붙은 스트림, 그리고 너무 큰 창

<!-- example: manual/examples/ko/ex_16_brotli.c -->
```c
#include <string.h>

/*
 * Brotli: 버퍼 전체, 이어서 뒤에 다른 바이트가 붙은 같은 스트림을 조각조각 읽기, 그리고 남의
 * 스트림이 이 프로그램이 쓸 메모리를 정하지 못하게 하는 창 상한.
 */

/* 로그 줄 4,000바이트를 libbrotli가 품질 11, 64 KiB 창으로 쓴 것: 58바이트. */
static const proven_byte_t STREAM[] = {
    0xe2, 0xf3, 0x01, 0xb1, 0xe3, 0x30, 0x76, 0xb3, 0x54, 0x22, 0x7f, 0x6e,
    0x31, 0x35, 0x23, 0x54, 0x7a, 0x83, 0xbc, 0xb8, 0x1c, 0x8d, 0x96, 0x8c,
    0x29, 0xa2, 0x82, 0x0d, 0x38, 0x60, 0x0f, 0x30, 0xc2, 0x45, 0x17, 0x5a,
    0x8c, 0xce, 0x18, 0x20, 0xa3, 0x8c, 0xc1, 0x41, 0x9a, 0xb7, 0x0a, 0x2d,
    0x23, 0xe3, 0x31, 0x41, 0x6b, 0xd0, 0x07, 0x39, 0x00, 0x40,
};

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    static const char line[] = "GET /index.html HTTP/1.1 - 200 - text/html; charset=utf-8\n";
    static proven_byte_t text[4000];
    for (proven_size_t i = 0; i < sizeof text; ++i) text[i] = (proven_byte_t)line[i % (sizeof line - 1)];

    /* 버퍼 전체: gzip처럼 출력 버퍼가 상한이다. */
    static proven_byte_t out[8000];
    proven_size_t n = 0;
    EXAMPLE_REQUIRE(proven_brotli_decode_all(heap, (proven_mem_view_t){ STREAM, sizeof STREAM }, (proven_mem_mut_t){ out, sizeof out }, &n) == PROVEN_OK &&
                    n == sizeof text && memcmp(out, text, n) == 0, "Brotli 58바이트가 libbrotli가 받은 4,000바이트로 풀린다");
    EXAMPLE_REQUIRE(proven_brotli_decode_all(heap, (proven_mem_view_t){ STREAM, sizeof STREAM }, (proven_mem_mut_t){ out, 3999 }, &n) == PROVEN_ERR_OUT_OF_BOUNDS,
                    "3,999바이트에는 들어가지 않는다: PROVEN_ERR_OUT_OF_BOUNDS");

    /* 뒤에 다른 5바이트가 붙은 스트림을 7바이트씩, 500바이트의 자리로 준다. `done`은 스트림이
     * 끝나는 곳에서 오고, 그 뒤의 바이트는 소비되지 않는다. Brotli 스트림 뒤에는 이어질
     * 프레임이 없으니, 그 바이트가 무엇인지는 호출한 쪽이 정할 일이다. */
    static proven_byte_t more[sizeof STREAM + 5];
    memcpy(more, STREAM, sizeof STREAM);
    memcpy(more + sizeof STREAM, "after", 5);
    proven_brotli_decoder_t *b = NULL;
    EXAMPLE_REQUIRE(proven_brotli_decoder_create(heap, 0, &b) == PROVEN_OK, "기본 창 상한(16 MiB)의 디코더");
    proven_size_t in_at = 0, out_at = 0;
    bool done = false;
    while (!done) {
        proven_size_t give = sizeof more - in_at < 7 ? sizeof more - in_at : 7, room = sizeof out - out_at < 500 ? sizeof out - out_at : 500;
        proven_size_t used = 0, made = 0;
        EXAMPLE_REQUIRE(proven_brotli_decode(b, (proven_mem_view_t){ more + in_at, give }, &used, (proven_mem_mut_t){ out + out_at, room }, &made, &done) == PROVEN_OK, "한 걸음");
        in_at += used;
        out_at += made;
        if (used == 0 && made == 0 && !done) break;     /* 줄 것이 없거나 둘 자리가 없다 */
    }
    EXAMPLE_REQUIRE(done && in_at == sizeof STREAM && out_at == sizeof text && memcmp(out, text, out_at) == 0,
                    "스트림의 58바이트 뒤에서 done, 그 뒤의 5바이트는 그대로");

    /* 창 상한. 스트림의 처음 몇 비트가 얼마나 멀리 거슬러 참조할지를 말하고, 디코더는 그만큼을
     * 쥐고 있어야 한다. libbrotli의 가장 큰 창으로 만든 이 스트림은 16 MiB를 요구한다. 64 KiB로
     * 제한한 디코더는 아무것도 할당하기 전에 거절하고, reset하면 다시 쓸 수 있다. */
    static const proven_byte_t wide[] = { 0x0f, 0x01, 0x80, 0x61, 0x62, 0x63, 0x03 };
    proven_brotli_decoder_t *small = NULL;
    proven_size_t used = 0, made = 0;
    EXAMPLE_REQUIRE(proven_brotli_decoder_create(heap, 64 * 1024, &small) == PROVEN_OK, "64 KiB로 제한한 디코더");
    EXAMPLE_REQUIRE(proven_brotli_decode(small, (proven_mem_view_t){ wide, sizeof wide }, &used, (proven_mem_mut_t){ out, sizeof out }, &made, &done) == PROVEN_ERR_OUT_OF_BOUNDS,
                    "16 MiB 창의 스트림: PROVEN_ERR_OUT_OF_BOUNDS");
    proven_brotli_decoder_reset(small);
    EXAMPLE_REQUIRE(proven_brotli_decode(small, (proven_mem_view_t){ STREAM, sizeof STREAM }, &used, (proven_mem_mut_t){ out, sizeof out }, &made, &done) == PROVEN_OK &&
                    done && made == sizeof text, "reset 뒤에는 상한 안의 스트림이 풀린다");
    proven_brotli_decoder_reset(b);
    EXAMPLE_REQUIRE(proven_brotli_decode(b, (proven_mem_view_t){ wide, sizeof wide }, &used, (proven_mem_mut_t){ out, sizeof out }, &made, &done) == PROVEN_OK &&
                    done && made == 3 && memcmp(out, "abc", 3) == 0, "기본 상한은 받아들인다 - 히스토리는 16 MiB가 아니라 몇 KiB");

    proven_brotli_decoder_destroy(small);
    proven_brotli_decoder_destroy(b);
    return EXAMPLE_OK();
}
```

### 레퍼런스

| API | 의도 | 반환 |
|---|---|---|
| `proven_brotli_decoder_create(alloc, window_max, &b)` | 디코더. `window_max`는 1 KiB부터 2 GiB. 0은 16 MiB. | `INVALID_ARG`, `NOMEM`. |
| `proven_brotli_decode(b, in, &consumed, out, &produced, &done)` | `in`의 일부를 받아 `out`의 일부를 채운다. 스트림의 끝에서 멈춘다. 만들었다고 보고한 바이트만 쓴다. | `INVALID_FORMAT`(Brotli가 아니거나 손상됨), `OUT_OF_BOUNDS`(창이 상한 위), `NOMEM`, `INVALID_ARG`. 오류 뒤에는 reset까지 같은 오류. |
| `proven_brotli_decoder_reset(b)` | 진행 중인 스트림을 잊는다. 버퍼는 남긴다. | 없음. |
| `proven_brotli_decoder_destroy(b)` | 해제한다. NULL도 된다. | 없음. |
| `proven_brotli_decode_all(alloc, in, out, &written)` | `in`의 스트림 하나를 기본 상한으로 `out`에. `out`의 크기가 출력의 상한이다. | 들어가지 않으면 `OUT_OF_BOUNDS`. `in`이 비었거나, 스트림 안에서 끝나거나, 스트림 뒤에 더 있으면 `INVALID_FORMAT`. |

### 주의할 점과, 잘못되는 경우

- **창이 메모리를 정한다.** 7절과 같다: 스트림을 쓴 쪽이 여러분의 상한 안에서 고른다. 16 MiB 창을 쓰고 그
  창을 채울 만큼 긴 응답 천 개를 한꺼번에 받으면 16 GiB다. 스트림을 아는 곳(알려진 창으로 직접 만든
  파일)에서는 `window_max`를 낮추고, 디코더 수를 연결 수처럼 셀 것.
- **체크섬이 없다.** 여전히 유효한 스트림을 이루는 손상은 다른 바이트로 풀린다. 디코더는 해석되지 않는
  것만 거부할 수 있다. 중요한 곳에서는 푼 것을 직접 확인할 것.
- **첫 바이트로 알아볼 수 없다.** Brotli에는 매직 넘버가 없다: gzip이나 텍스트를 받은 디코더는 곧바로가
  아니라 안쪽 어딘가에서 실패하고, 다른 종류의 아주 짧은 입력이 우연히 유효한 스트림일 수도 있다. HTTP에서는
  `Content-Encoding`이 본문이 무엇인지 말한다. 파일이라면 이름(`.br`)이나 자기 헤더를 붙여 둘 것.

**libbrotli가 거부하는 것은 하나도 받아들이지 않는다.** 등록된 테스트 밖에서, 스트림 176개를 자르거나 바꾼
사본 1,437,490개를 이 디코더와 libbrotli 1.1.0의 한 번 호출 디코더에 함께 주었다: libbrotli가 거부한 것을
여기서 받아들인 것은 없고, 둘 다 받아들인 것 가운데 다르게 나온 것도 없으며, libbrotli가 받아들이고 여기서
거부한 것이 11,242개다 - 모두 입력보다 먼저 끝나 같은 출력을 내는 스트림으로, 남은 바이트를 libbrotli의 한 번
호출 디코더는 무시하고 `proven_brotli_decode_all`은 (libbrotli의 Python 바인딩처럼) 무시하지 않는다.

**속도.** 개발 기계에서, 이 라이브러리의 C 소스(2.9 MiB)를 libbrotli가 품질 1, 5, 11로 압축한 것은 각각 약
195, 265, 290 MiB/s로 풀린다 - 같은 프로세스에서 잰 libbrotli 자신의 디코더(약 270, 410, 435 MiB/s)의
0.65~0.73배(v0.87.0. 그 전에는 0.57~0.65). 비압축 메타블록이나 긴 반복은 libbrotli의 속도로 복사한다.

## 9. 무엇을 받아들이고, 무엇이 없으며, 어떻게 시험했는가

**해제기가 받아들이는 것은 zlib가 받아들이는 것이다.** RFC 1951이 모퉁이를 열어 둔 곳마다, 경우 하나하나를
먼저 zlib에 시험해 보고 그 판정을 따랐다. 실제 생산자가 zlib가 봐주는 것을 낼 수 있기 때문이다. 그래서:
코드 하나만 쓰는 블록은 받아들이고, 마지막 앞 코드의 가장 긴 형태로 쓴 길이도 그렇다. 존재할 수 있는
것보다 많은 기호를 기술하는 코드, 형식 밖의 길이와 거리, 첫 바이트보다 앞을 가리키는 거리, 코드 표의
합이 맞지 않는 스트림은 모두 거부한다. gzip 헤더는 추가 필드, 파일 이름, 주석, 자기 체크섬을 실을 수
있고, 모두 읽고 지나간다(체크섬은 검사한다). 이름과 시간은 알려 주지 않는다.

**압축기가 쓰는 것은 어떤 해제기든 읽는 유효한 DEFLATE다.** 같은 입력에 대해 zlib가 썼을 것과 바이트
하나까지 같지는 않고, 그럴 필요도 없다.

**여기에 없는 것:**

- **미리 정한 사전**(zlib의 `FDICT`), 어느 방향이든.
- **ZIP 압축 파일, tar 파일**: 그것들은 그릇이다. 이것은 그 안의 코덱이다.
- **Deflate64**, 그리고 다른 모든 압축 형식: LZ4, bzip2, xz. Zstandard의 느린 레벨(libzstd의 10부터 22:
  이진 트리와 optimal 파싱), 사전, long-distance 모드. Brotli 압축. Brotli의 큰 창 변형과 공유 사전.
- **gzip 헤더의 필드** - 이름, 시간, 주석 - 는 두거나 읽을 수 없다.
- **여러 gzip 멤버를 하나의 스트림으로** 자동으로 읽기: 멤버마다 reset한다(3절).
  (6절의 HTTP 클라이언트는 응답 본문에 대해 이것을 해 준다.)
- **HTTP 위에서:** 어느 방향이든 압축된 *요청* 본문. `br` 보내기. 스트림 응답이 지금까지 쓴 것을
  밀어내는 호출. 미리 압축해 둔 파일 제공(핸들러가 직접 할 수 있다: `proven_http_accepts_coding`으로
  물은 다음 자기 `Content-Encoding`을 쓴다).

**어떻게 시험했는가.** 등록된 테스트는 zlib 1.3.1이 만든 스트림 93개 - 모든 레벨과 전략, 작은 윈도, 중간의
flush, 세 형식 모두 - 를 한 바이트씩, 그리고 한 번의 호출로 풀어 zlib가 받았던 데이터를 얻는다. 드물지만
zlib가 받아들이는, 손으로 만든 스트림 열 개를 받아들인다. zlib가 거부하는, 손으로 만든 스트림 서른 개 -
스트림이 틀릴 수 있는 방식마다 하나 - 를 거부한다. 짧은 스트림들의 모든 진부분 접두에 대해 "입력을 더"를
답하고, 그것들의 바뀐 비트 하나하나에 대해 어떤 식으로든 답을 낸다 - 버퍼 밖에 쓰는 일 없이. 그리고 열한
종류의 데이터를 모든 레벨과 세 윈도 크기로 압축해 되찾는다. 등록된 테스트 밖에서는: 이 압축기가 더 큰
말뭉치에 대해 낸 모든 것 - 모든 레벨, 윈도, 형식, flush와 작은 조각으로 - 을 zlib가 풀었다(스트림 2,856개).
이 해제기를 5,838개에 대해 더 zlib와 비교했다. 그리고 무작위로 손상시킨 스트림 육백만 개를, 버퍼 밖의
읽기나 쓰기를 보고하는 도구 아래에서 풀었고, 보고된 것은 없었으며 해제기가 진전 없이 일하게 만든
스트림도 없었다.

**6절은** zlib이 만든 본문 - gzip, zlib, raw DEFLATE, 멤버 둘, 선택 헤더 필드 전부, 그리고 끊기거나
손상되는 각 방식 - 을 두 클라이언트에 통째로 그리고 한 바이트짜리 청크로 통과시켜 시험했고, 시험의
표를 만들 때 각각을 zlib 자신의 판정과 대조했다. 두 서버는 일곱 가지 크기를, 한 번에 그리고 스트림으로, 평문과 TLS
위에서 압축했다. 등록된 시험 밖에서는 curl과 Python이 두 서버가 보내는 것을 읽었고, 두 클라이언트가
Python 서버가 보내는 것을 읽었다.

**7절은** Node를 거쳐 libzstd 1.5.7에 대고 시험했다: 등록된 테스트는 libzstd가 일곱 종류의 데이터로
만든 스트림 91개 - 레벨 1, 3, 9, 19, 체크섬과 콘텐츠 크기가 있고 없는 것, 블록을 넘어 가리키는 일치, 창보다
긴 프레임 - 를 한 번의 호출로, 한 바이트씩, 그리고 엇갈린 걸음으로 풀어 libzstd가 받았던 데이터를 얻는다.
드물지만 libzstd가 받아들이는, 손으로 만든 프레임 열세 개(그중에는 모든 종류의 반복 오프셋이 있다)를
받아들이고, 스무 개를 그 이유(손상, 사전, 8 MiB를 넘는 창, 위의 빈 압축 블록)대로 거부한다. 짧은 스트림들의 모든 진부분 접두를 거부하고, 그것들의 바뀐 비트
하나하나에 대해 통째로든 걸음으로든 같은 판정을 낸다. 그리고 창 상한이 어떤 할당보다 먼저 거부하는 것,
들고 있는 메모리, 실패한 할당마다 `PROVEN_ERR_NOMEM`으로 답하는 것을 보인다. 등록된 테스트 밖에서는:
libzstd가 만든 스트림 130개를 더 - 2.2 MiB까지, 프레임 여럿, flush한 스트림 - 1바이트부터 70,000바이트까지의
걸음으로 바이트 하나까지 같게 풀었다. 그중 일곱의 잘림과 바뀐 비트 약 7만 개를 버퍼 밖의 읽기나 쓰기를
보고하는 도구 아래에서 풀었고, 보고된 것은 없었다. HTTP에서는 블로킹, 이벤트 구동, HTTP/2 클라이언트가
Node 서버가 `zstd`로 보낸 것(레벨 1, 3, 19, 통째로, flush하며, 프레임 여럿으로, 3 MB까지)을 풀었고, 끊기거나
뒤에 쓰레기가 붙은 것은 거부했으며, 요청하지 않은 클라이언트에게는 gzip이 왔다. curl은 이 라이브러리
서버에서 핸들러가 직접 보낸 Zstandard를 읽었다.

**8절은** Python 바인딩(1.1.0)을 거쳐 libbrotli에 대고 시험했다: 등록된 테스트는 libbrotli가 열 종류의
데이터로 만든 스트림 110개 - 품질 0, 1, 5, 9, 11, 1 KiB부터 16 MiB까지의 창, 텍스트와 일반 모드 - 를 한 번의
호출로, 한 바이트씩, 그리고 고르지 않은 걸음으로 풀어 libbrotli가 받았던 데이터를 얻는다. libbrotli가 푸는,
손으로 만든 스트림 66개를 같은 결과로 푼다 - 메타데이터, 비압축 메타블록, 모든 창 크기, 121가지 변환 모두를
거친 사전 단어(2바이트와 3바이트 글자의 대문자 변환 포함), 모든 문맥 모드, 반복과 move-to-front를 쓴 문맥
맵, 모든 짧은 거리 코드, 모든 종류의 블록 전환, 접두 부호를 쓰는 모든 방식. libbrotli가 거부하는 25개를, 각기 다른 방식으로 틀린
것들을 거부한다. 짧은 스트림들의 모든 진부분 접두를 거부하고, 그 모든 1비트 변경(72,376개)에 통째와 걸음
단위에서 같은 판정을 내린다. 창 상한이 할당 전에 거부하는 것, 쥐는 메모리, 실패한 할당마다
`PROVEN_ERR_NOMEM`이 오는 것도 보인다. 사전과 변환은 RFC 7932 본문에서 가져와, RFC가 밝힌 CRC-32와
libbrotli 자신의 사본 모두와 대조했다. 등록된 테스트 밖에서는: Python의 libbrotli가 만든 스트림 358개와
Node의 libbrotli(따로 들어 있는 1.2.0)가 만든 144개 - 이 저장소의 파일들, 한국어 매뉴얼, 소스 2.2 MB - 를
1바이트부터 70,000바이트까지의 걸음으로, 버퍼 밖의 읽기나 쓰기를 보고하는 도구 아래에서 바이트 하나까지
같게 풀었다. 위의 손상된 사본 1,437,490개. 그리고 알파벳마다 만들 수 있는 가장 큰 코드 표를 계산한 다음 실제로
만들어 보았다. HTTP에서는 블로킹, 이벤트 구동, HTTP/2 클라이언트가 Node 서버가 `br`로 보낸 것(품질 1, 5, 11,
1 KiB부터 16 MiB까지의 창, 통째로와 flush하며, 3 MB까지)을 풀었고, 끊기거나 뒤에 쓰레기가 붙거나 두 번
이어진 것은 거부했으며, 요청하지 않은 클라이언트에게는 gzip이 왔다.

**7절의 인코더는** 자기 디코더와 libzstd에 대고 시험했다. 등록된 테스트는 열한 종류의 데이터(빈 것, 한
바이트, 같은 바이트의 연속, 번호 붙은 줄, 먼 뒤를 복사하는 잡음, 무작위 바이트, 128개가 넘는 바이트 값 등)를
열아홉 레벨 모두와 세 창으로 통째로, 그리고 방식마다 한 레벨씩 스트림으로 - 조각마다 블록 flush, 출력은 한
바이트씩 - 압축하고, 모든 프레임을 이 라이브러리의 디코더로 푼다. 이 디코더는 오프셋마다 프레임이 밝힌 창
안인지 검사한다. 모든 레벨에서 스트림의 블록이 같은 입력을 통째로 압축한 블록과 바이트까지 같은지도 본다. 프레임
헤더, 입력 8,000개에 대한 상한, flush, 끝과 reset, 압축 블록 안의 한 값 리터럴, 쥐는 메모리, 실패한 할당을
검사하고, libzstd가 푼 프레임 열셋 - 그중 다섯은 reset 뒤에 버퍼를 밀며 스트림으로 만든 것 - 의 크기와 CRC를
고정해 둔다. 등록된 테스트 밖에서는 인코더가 말뭉치 - 이 저장소의 파일들, 한국어
매뉴얼, 바이너리, 소스 4 MB, 4,096바이트까지의 모든 크기와 블록 및 헤더 경계의 입력, 한 값·두 값·네 값의
리터럴, 블록의 시퀀스를 채우는 주기 - 로 만든 모든 프레임을 모든 레벨과 1 KiB부터 8 MiB까지의 창으로,
통째로 그리고 한 바이트부터의 조각으로, 버퍼 밖의 읽기나 쓰기를 보고하는 도구 아래에서 libzstd가 풀었다:
프레임 4,105개, 모두 바이트 하나까지 같다. HTTP에서는 Node의 libzstd와 curl이 두 서버가 `zstd`로 보낸 것을,
통째로와 스트림으로, 레벨 1, 2, 3, 6, 9와 1 KiB부터 8 MiB까지의 창으로 풀었고, gzip을 더 원하는 클라이언트에게는
가중치가 gzip을 골랐다.

**하지 않은 것:** 커버리지 기반 퍼징 없음. zlib 말고 두 번째 압축기의 출력과의 비교 없음. 외부 검토 없음.
Zstandard에 대해서는: libzstd 말고 다른 인코더의 프레임 없음. 브라우저 없음. 인코더의 프레임을 푼 디코더는 libzstd와 이 라이브러리의 것뿐.
Brotli에 대해서는: libbrotli의 두 사본 말고 다른 인코더의 스트림 없음. 브라우저 없음.
