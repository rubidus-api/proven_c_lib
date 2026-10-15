#include "example.h"
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

    /* 스트림: 64 KiB 창, 체크섬 포함으로 1,000바이트씩. 처음 5,000바이트 뒤의 블록 flush는 그때까지를
     * 바로 풀 수 있게 한다 - 서버가 더 보낼 것을 기다리기 전에 하는 일이다. FINISH가 프레임을 끝낸다. */
    proven_zstd_encoder_t *z = NULL;
    o = (proven_zstd_encoder_options_t){ .alloc = heap, .level = 3, .window_log = 16, .checksum = true };
    EXAMPLE_REQUIRE(proven_zstd_encoder_create(&o, &z) == PROVEN_OK, "레벨 3, 64 KiB 창의 인코더");
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
