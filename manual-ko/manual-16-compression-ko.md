# 16장: 압축

**3부 — 자료 구조. 선행 조건: 압축기를 만들 때 쓰는 할당자(allocator) - 예제에서는 힙(heap) 할당자 - 는
[2장](manual-02-allocation-ko.md), 체크섬은 [4장](manual-04-containers-algorithms-ko.md).**
**이 장을 마치면** 어떤 길이의 버퍼나 스트림도 압축하고 풀 수 있고, `gzip`과 `zlib` 포장을 읽고 쓸 수
있으며, 낯선 이의 데이터가 얼마까지 부풀면 거부할지 정할 수 있다 - 그리고 무엇을 시험했고 얼마나
빠른지 말할 수 있다.

이 장은 `deflate.h`를 다룬다. 순수한 계산이다 - 압축기나 해제기를 만들 때 할당 한 번, 운영체제에서는
아무것도 받지 않는다 - 그래서 [프리스탠딩(freestanding)](manual-freestanding-ko.md) 빌드에서 쓸 수 있다.
입력과 출력은 여러분의 메모리를 가리키는 뷰(view)로 주고받는다. 6절은 예외다: 11장과 15장의 서버와
클라이언트에서 다루는 압축된 HTTP 응답 이야기다.

## 목차

1. [DEFLATE가 하는 일, 한 쪽으로](#1-deflate가-하는-일-한-쪽으로)
2. [버퍼 전체](#2-버퍼-전체)
3. [스트림](#3-스트림)
4. [레벨, 윈도, 그리고 그 비용](#4-레벨-윈도-그리고-그-비용)
5. [남이 보낸 데이터](#5-남이-보낸-데이터)
6. [HTTP 위에서](#6-http-위에서)
7. [무엇을 받아들이고, 무엇이 없으며, 어떻게 시험했는가](#7-무엇을-받아들이고-무엇이-없으며-어떻게-시험했는가)

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
void proven_inflate_destroy(proven_inflate_t *z);
```

데이터가 메모리보다 큰 파일이거나, 아직 만들어지고 있는 응답이거나, 연결이라면, 두 방향 모두 같은
방식으로 동작한다: 상태를 쥔 객체 하나와, **입력을 조금 받아 출력을 조금 채우는 단계 함수 하나**.

단계의 계약은 짧고, 한 번은 읽어 둘 만하다:

- `in`에서 할 수 있는 만큼 소비하고 `out`에 할 수 있는 만큼 생산하며, 각각 얼마인지 알려 준다.
  할당하지 않고, `out`을 넘어 쓰지 않는다.
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
`permessage-deflate`는 메시지마다 그 네 바이트를 뗀 sync flush로 보내고, 받는 쪽이 그것을 다시 붙인다.
raw 스트림이 끝나지 않아도 되는 까닭도 그것이다: 해제기는 조각조각 받고, `*done`은 참이 되지 않으며,
앞선 조각에 대해 기억하는 것이 뒤의 조각을 작게 만든다.

`proven_deflate_reset`과 `proven_inflate_reset`은 같은 객체에서 새 스트림을 시작한다. 하나 더 만드는
것보다 싸다. `*done` 뒤에, 첫 gzip 멤버에 이어지는 둘째를 읽어야 하는 해제기는 reset하고 첫째가 끝난
곳부터 받는다.

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
| 1 | 30.2% | 33 MiB/s | 66 MiB/s |
| 6(기본값) | 25.9% | 16 MiB/s | 84 MiB/s |
| 9 | 25.7% | 9 MiB/s | 83 MiB/s |

1에서 6으로 가면 속도의 절반으로 크기의 칠분의 일을 산다. 6에서 9로 가면 또 속도의 절반 가까이로 거의
아무것도 사지 못한다. 풀기는 어떤 레벨이 쓰였는지 상관하지 않는다.

모두가 쓰는 구현인 zlib는 같은 데이터를 같은 크기로 압축하고 - 레벨 6과 9에서 사백분의 일 이내, 레벨
1에서는 zlib 쪽이 조금 더 크다 - 압축은 두 배쯤, 풀기는 세 배쯤 빠르다. 이 라이브러리의 코드는 읽히도록
쓰였다. 이 수치가 어떤 프로그램에 너무 느리다면, 그 프로그램에 필요한 것은 zlib다.

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
proven_http_coding_t proven_http_content_coding(const proven_http_header_t *headers, proven_size_t count);
```

HTTP는 이것을 *콘텐츠 코딩*이라고 부른다: 서버가 본문을 압축하고 `Content-Encoding`으로 그 사실을
알리는데, `Accept-Encoding`으로 허락한 클라이언트에게만 그렇게 할 수 있다.
[11장](manual-11-http-client-server-ko.md)과 [15장](manual-15-event-loop-ko.md)의 HTTP 드라이버 넷은 이
두 가지를 모두 한다 - 그리고 **요청하기 전에는 어느 것도 하지 않는다**. 아무것도 설정하지 않은
프로그램은 이전과 같은 바이트를 보낸다.

이 장의 나머지와 달리 이 부분은 소켓이 필요하고 프리스탠딩 빌드에는 없다 - 헬퍼 함수 둘은 예외로,
`http.h`에 있는 헤더 계산일 뿐이다.

**서버는 핸들러가 그 응답에 대해 요청할 때 응답을 압축한다.** 응답을 시작하기 전에 호출 하나:

| | 일어나는 일 |
|---|---|
| 요청이 gzip을 허용하고, 응답에 자기 본문이 있다 | `Content-Encoding: gzip`으로 보낸다 |
| 요청이 gzip을 허용하지 않는다(`Accept-Encoding` 없음, `gzip;q=0`, 다른 코딩만) | 그대로 보낸다 |
| 상태 204, 206, 304. 또는 여러분의 헤더에 이미 `Content-Encoding`이나 `Content-Range`가 있다 | 그대로 보낸다 |
| 한 번에 보내는 본문이 256바이트 미만이거나, 작아지지 않는다 | 그대로 보낸다 |
| 압축기에 줄 메모리가 없다 | 그대로 보낸다 |
| 이 모든 경우에 | `Vary: Accept-Encoding`이 붙는다. 여러분의 `Vary`가 이미 그것을 말하면 붙지 않는다 |

`Vary`가 붙는 까닭은 응답이 이제 요청 헤더에 따라 달라지기 때문이다. 중간의 캐시가 gzip을 읽지 못하는
클라이언트에게 gzip을 건네서는 안 된다.

한 번에 보내는 본문은 먼저 압축한 다음 새 `Content-Length`와 함께 보낸다. 조각조각 쓰는 본문은 쓰는
대로 압축해서, 알려 준 길이와 상관없이 청크로 보낸다 - 그 길이는 여전히 여러분이 써야 할 길이다.

**클라이언트는 설정에 `decompress`가 있을 때 요청하고 푼다.** 그러면 `Accept-Encoding: gzip`을 보내고,
`gzip`이나 `deflate`로 돌아온 본문을 여러분에게 가는 길에 푼다: `proven_http_client_read`는 여러분의
버퍼를 풀린 바이트로 채우고, 이벤트 구동 클라이언트의 `on_body`는 풀린 조각을 최대 16 KiB씩 받는다.

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
} app_t;

/* 보고서는 쓰면서 만들어진다: 전체를 담은 버퍼는 없다. */
static proven_byte_t report_byte(proven_size_t i) { return (proven_byte_t)("measured value \n"[i % 16]); }

// ---- 서버 ------------------------------------------------------------------

/* 서버가 더 받지 않을 때까지 쓴다. 자리가 나면 on_writable이 이것을 다시 부른다. */
static void pump(app_t *app) {
    proven_byte_t piece[4096];
    while (app->written < REPORT_BYTES) {
        proven_size_t n = REPORT_BYTES - app->written < sizeof piece ? REPORT_BYTES - app->written : sizeof piece;
        for (proven_size_t i = 0; i < n; ++i) piece[i] = report_byte(app->written + i);
        proven_result_size_t took = proven_http_stream_write(app->stream, (proven_mem_view_t){ piece, n });
        if (took.err != PROVEN_OK) return;
        for (proven_size_t i = 0; i < took.value; ++i) app->sum_sent += piece[i];
        app->written += took.value;
        if (took.value < n) return;        /* 압축된 출력이 한도에 닿았다: 기다린다 */
    }
    (void)proven_http_stream_end(app->stream);
}

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    (void)head;
    app_t *app = ctx;
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
    app_t *app = ctx;
    app->gzip = proven_http_content_coding(head->headers, head->header_count) == PROVEN_HTTP_CODING_GZIP;
}

/* 풀린 조각들. 조각은 이 함수가 돌아갈 때까지만 유효한 뷰다. */
static void on_body(void *ctx, proven_http_event_request_t *request, proven_mem_view_t piece, bool last) {
    (void)request; (void)last;
    app_t *app = ctx;
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
    app_t *app = ctx;
    app->why = why;
    app->done = true;
    proven_loop_stop(app->loop);
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    app_t app = { .intact = true };
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
| `compress_level` (두 서버 설정) | 1(가장 빠름)부터 9(가장 작음). -1은 압축 없이 저장만 한다. 0은 6. 측정값은 4절에 있다. | 범위를 벗어나면 `create`가 `INVALID_ARG`. |
| `compress_window_bits` (둘 다) | 9부터 15: 스트림으로 쓰는 응답이 쓰이는 동안 쥐는 메모리(4절의 표). 0은 15. | 위와 같다. |
| `decompress` (두 클라이언트 설정) | `Accept-Encoding: gzip`을 보내고 `gzip`과 `deflate` 본문을 푼다. | - |
| `proven_http_accepts_coding(headers, count, coding)` | 요청의 `Accept-Encoding`이 어떤 코딩을 허용하는가: 0보다 큰 가중치로 적혀 있거나, `*`에 포함된다. | `bool`. 필드가 없으면 identity만 허용된다. |
| `proven_http_content_coding(headers, count)` | `Content-Encoding`이 말하는 코딩. | `PROVEN_HTTP_CODING_IDENTITY`, `_GZIP`(`x-gzip`도), `_DEFLATE`, 그 밖의 것과 둘 이상에는 `_OTHER`. |

### 주의할 점과, 잘못되는 경우

- **비밀과 클라이언트가 고른 글자가 섞인 응답에는 요청하지 말 것.** 5절의 누출이 가장 널리 알려진
  모습으로 나타난 것이다(BREACH): 세션 토큰을 싣고 검색어도 그대로 되돌려 주는 페이지를 압축하면,
  암호화된 연결을 지켜보는 이에게 - 길이만으로 - 검색어가 토큰의 일부와 맞았는지를 알려 준다. TLS는
  돕지 못한다. TLS는 바이트를 가리지 크기를 가리지 않는다. 서버에 "전부 압축" 설정이 없는 까닭이
  이것이다: 판단이 응답마다 달라서 호출도 응답마다 한다.
- **스트림으로 보내는 압축 응답은 조각조각 도착하지 않는다.** 쓴 것은 더 쓰거나 응답이 끝날 때까지
  압축기 안에 머물 수 있다. 이벤트 스트림([10장](manual-10-http-ko.md) 13절)처럼 클라이언트가 일어나는
  대로 보아야 하는 것에는 압축을 요청하지 말 것.
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
  스트림, 체크섬이 틀린 스트림, 또 하나의 gzip 멤버가 아닌 바이트가 뒤따르는 스트림. 잇달아 오는
  gzip 멤버 여럿은 본문 하나로 읽는다.
- **`deflate`는 실제로 두 가지를 뜻한다.** 표준은 zlib 스트림이라고 하는데 raw DEFLATE를 보내는 서버가
  있다. 클라이언트는 첫 두 바이트를 보고 어느 쪽이든 읽는다. `deflate`를 요청하는 일은 없고 - `gzip`만
  요청한다 - 서버가 그것을 보내는 일도 없다.
- **풀지 않는 것:** `206 Partial Content`(압축된 본문의 일부 범위는 압축된 본문이 아니다 - 그리고
  `decompress`를 켰을 때 `Range`가 실린 요청은 `Accept-Encoding` 없이 나간다). 이 둘이 아닌 코딩(`br`,
  `zstd`). 겹쳐 적용한 코딩 둘. 그런 본문은 헤더와 함께, 보낸 그대로 도착한다.
- **메모리.** 풀리는 중인 응답은 열려 있는 동안 약 35 KiB(이벤트 구동 클라이언트에서는 52 KiB)를
  쓴다. 스트림으로 압축되는 응답은 끝날 때까지 4절의 표가 윈도에 대해 말하는 만큼 - 기본값으로
  325 KiB - 을 쓴다. 한 번에 보내는 응답은 그 호출 동안만, 본문보다 크지 않은 윈도로 쓴다. 기본 윈도로
  스트림 응답 만 개면 3 GiB다: `compress_window_bits`를 낮추거나, 덜 압축한다.

## 7. 무엇을 받아들이고, 무엇이 없으며, 어떻게 시험했는가

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
- **Deflate64**, 그리고 다른 모든 압축 형식: Brotli, Zstandard, LZ4, bzip2, xz.
- **gzip 헤더의 필드** - 이름, 시간, 주석 - 는 두거나 읽을 수 없다.
- **여러 gzip 멤버를 하나의 스트림으로** 자동으로 읽기: 멤버마다 reset한다(3절).
  (6절의 HTTP 클라이언트는 응답 본문에 대해 이것을 해 준다.)
- **HTTP 위에서:** 어느 방향이든 압축된 *요청* 본문. `br`과 `zstd`. 스트림 응답이 지금까지 쓴 것을
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

**하지 않은 것:** 커버리지 기반 퍼징 없음. zlib 말고 두 번째 압축기의 출력과의 비교 없음. 외부 검토 없음.
