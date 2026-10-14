#include "example.h"
#include <string.h>

/*
 * Zstandard: a whole buffer, then a stream of two frames read piece by piece, and the window
 * limit that keeps a stranger's frame from deciding how much memory this program spends.
 */

/* 4,000 bytes of log lines, as libzstd wrote them at level 19 with a checksum: 77 bytes. */
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

    /* A whole buffer: as with gzip, the output buffer is the limit. */
    static proven_byte_t out[8000];
    proven_size_t n = 0;
    EXAMPLE_REQUIRE(proven_zstd_decode_all(heap, (proven_mem_view_t){ FRAME, sizeof FRAME }, (proven_mem_mut_t){ out, sizeof out }, &n) == PROVEN_OK &&
                    n == sizeof text && memcmp(out, text, n) == 0, "77 bytes of Zstandard decode to the 4,000 bytes libzstd was given");
    EXAMPLE_REQUIRE(proven_zstd_decode_all(heap, (proven_mem_view_t){ FRAME, sizeof FRAME }, (proven_mem_mut_t){ out, 3999 }, &n) == PROVEN_ERR_OUT_OF_BOUNDS,
                    "and do not fit in 3,999: PROVEN_ERR_OUT_OF_BOUNDS");

    /* A stream: the same frame twice, given 10 bytes at a time with 500 bytes of room. Each
     * call takes what it can; `done` says a frame has just ended, and the next call simply
     * goes on with the frame that follows. */
    static proven_byte_t two[2 * sizeof FRAME];
    memcpy(two, FRAME, sizeof FRAME);
    memcpy(two + sizeof FRAME, FRAME, sizeof FRAME);
    proven_zstd_decoder_t *z = NULL;
    EXAMPLE_REQUIRE(proven_zstd_decoder_create(heap, 0, &z) == PROVEN_OK, "a decoder with the default window limit, 8 MiB");
    proven_size_t in_at = 0, out_at = 0;
    int frames = 0;
    while (in_at < sizeof two || out_at < sizeof out) {
        proven_size_t give = sizeof two - in_at < 10 ? sizeof two - in_at : 10, room = sizeof out - out_at < 500 ? sizeof out - out_at : 500;
        proven_size_t used = 0, made = 0;
        bool done = false;
        EXAMPLE_REQUIRE(proven_zstd_decode(z, (proven_mem_view_t){ two + in_at, give }, &used, (proven_mem_mut_t){ out + out_at, room }, &made, &done) == PROVEN_OK, "a step");
        in_at += used;
        out_at += made;
        if (done && used + made > 0) frames++;
        if (used == 0 && made == 0) break;              /* nothing more to give, or nowhere to put it */
    }
    EXAMPLE_REQUIRE(frames == 2 && out_at == 2 * sizeof text && memcmp(out + sizeof text, text, sizeof text) == 0, "two frames, one after the other, 8,000 bytes");

    /* The window limit. Every frame says how far back it may refer, and a decoder must keep
     * that much; this hand-made header asks for 1 MiB. A decoder limited to 64 KiB refuses it
     * before allocating anything for it. reset makes the decoder usable again. */
    static const proven_byte_t wide[] = { 0x28, 0xb5, 0x2f, 0xfd, 0x00, (20 - 10) << 3, 0x19, 0x00, 0x00, 'a', 'b', 'c' };
    proven_zstd_decoder_t *small = NULL;
    proven_size_t used = 0, made = 0;
    bool done = false;
    EXAMPLE_REQUIRE(proven_zstd_decoder_create(heap, 64 * 1024, &small) == PROVEN_OK, "a decoder limited to 64 KiB");
    EXAMPLE_REQUIRE(proven_zstd_decode(small, (proven_mem_view_t){ wide, sizeof wide }, &used, (proven_mem_mut_t){ out, sizeof out }, &made, &done) == PROVEN_ERR_OUT_OF_BOUNDS,
                    "a frame that needs 1 MiB of window: PROVEN_ERR_OUT_OF_BOUNDS");
    proven_zstd_decoder_reset(small);
    EXAMPLE_REQUIRE(proven_zstd_decode(small, (proven_mem_view_t){ FRAME, sizeof FRAME }, &used, (proven_mem_mut_t){ out, sizeof out }, &made, &done) == PROVEN_OK &&
                    done && made == sizeof text, "after a reset, a frame within the limit decodes");
    EXAMPLE_REQUIRE(proven_zstd_decode(z, (proven_mem_view_t){ wide, sizeof wide }, &used, (proven_mem_mut_t){ out, sizeof out }, &made, &done) == PROVEN_OK &&
                    done && made == 3, "while the default limit takes it");

    proven_zstd_decoder_destroy(small);
    proven_zstd_decoder_destroy(z);
    return EXAMPLE_OK();
}
