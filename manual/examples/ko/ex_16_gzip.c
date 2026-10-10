#include "example.h"
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
