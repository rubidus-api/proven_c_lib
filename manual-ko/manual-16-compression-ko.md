# 16장: 압축

**3부 — 자료 구조. 선행 조건: 압축기를 만들 때 쓰는 할당자(allocator) - 예제에서는 힙(heap) 할당자 - 는
[2장](manual-02-allocation-ko.md), 체크섬은 [4장](manual-04-containers-algorithms-ko.md).**
**이 장을 마치면** 어떤 길이의 버퍼나 스트림도 압축하고 풀 수 있고, `gzip`과 `zlib` 포장을 읽고 쓸 수
있으며, 낯선 이의 데이터가 얼마까지 부풀면 거부할지 정할 수 있다 - 그리고 무엇을 시험했고 얼마나
빠른지 말할 수 있다.

이 장은 `deflate.h`를 다룬다. 순수한 계산이다 - 압축기나 해제기를 만들 때 할당 한 번, 운영체제에서는
아무것도 받지 않는다 - 그래서 [프리스탠딩(freestanding)](manual-freestanding-ko.md) 빌드에서 쓸 수 있다.
입력과 출력은 여러분의 메모리를 가리키는 뷰(view)로 주고받는다.

## 목차

1. [DEFLATE가 하는 일, 한 쪽으로](#1-deflate가-하는-일-한-쪽으로)
2. [버퍼 전체](#2-버퍼-전체)
3. [스트림](#3-스트림)
4. [레벨, 윈도, 그리고 그 비용](#4-레벨-윈도-그리고-그-비용)
5. [남이 보낸 데이터](#5-남이-보낸-데이터)
6. [무엇을 받아들이고, 무엇이 없으며, 어떻게 시험했는가](#6-무엇을-받아들이고-무엇이-없으며-어떻게-시험했는가)

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

## 6. 무엇을 받아들이고, 무엇이 없으며, 어떻게 시험했는가

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

**하지 않은 것:** 커버리지 기반 퍼징 없음. zlib 말고 두 번째 압축기의 출력과의 비교 없음. 외부 검토 없음.
