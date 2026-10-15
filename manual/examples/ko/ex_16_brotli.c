#include "example.h"
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
