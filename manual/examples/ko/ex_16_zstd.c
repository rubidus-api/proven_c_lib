#include "example.h"
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
