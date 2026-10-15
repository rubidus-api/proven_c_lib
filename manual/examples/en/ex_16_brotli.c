#include "example.h"
#include <string.h>

/*
 * Brotli: a whole buffer, then the same stream read piece by piece with other bytes after it,
 * and the window limit that keeps a stranger's stream from deciding how much memory this
 * program spends.
 */

/* 4,000 bytes of log lines, as libbrotli wrote them at quality 11 with a 64 KiB window: 58 bytes. */
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

    /* A whole buffer: as with gzip, the output buffer is the limit. */
    static proven_byte_t out[8000];
    proven_size_t n = 0;
    EXAMPLE_REQUIRE(proven_brotli_decode_all(heap, (proven_mem_view_t){ STREAM, sizeof STREAM }, (proven_mem_mut_t){ out, sizeof out }, &n) == PROVEN_OK &&
                    n == sizeof text && memcmp(out, text, n) == 0, "58 bytes of Brotli decode to the 4,000 bytes libbrotli was given");
    EXAMPLE_REQUIRE(proven_brotli_decode_all(heap, (proven_mem_view_t){ STREAM, sizeof STREAM }, (proven_mem_mut_t){ out, 3999 }, &n) == PROVEN_ERR_OUT_OF_BOUNDS,
                    "and do not fit in 3,999: PROVEN_ERR_OUT_OF_BOUNDS");

    /* A stream with five other bytes after it, given 7 bytes at a time with 500 bytes of room.
     * `done` comes where the stream ends, and the bytes after it are not consumed: a Brotli
     * stream has no frames to follow it, so what they are is the caller's business. */
    static proven_byte_t more[sizeof STREAM + 5];
    memcpy(more, STREAM, sizeof STREAM);
    memcpy(more + sizeof STREAM, "after", 5);
    proven_brotli_decoder_t *b = NULL;
    EXAMPLE_REQUIRE(proven_brotli_decoder_create(heap, 0, &b) == PROVEN_OK, "a decoder with the default window limit, 16 MiB");
    proven_size_t in_at = 0, out_at = 0;
    bool done = false;
    while (!done) {
        proven_size_t give = sizeof more - in_at < 7 ? sizeof more - in_at : 7, room = sizeof out - out_at < 500 ? sizeof out - out_at : 500;
        proven_size_t used = 0, made = 0;
        EXAMPLE_REQUIRE(proven_brotli_decode(b, (proven_mem_view_t){ more + in_at, give }, &used, (proven_mem_mut_t){ out + out_at, room }, &made, &done) == PROVEN_OK, "a step");
        in_at += used;
        out_at += made;
        if (used == 0 && made == 0 && !done) break;     /* nothing more to give, or nowhere to put it */
    }
    EXAMPLE_REQUIRE(done && in_at == sizeof STREAM && out_at == sizeof text && memcmp(out, text, out_at) == 0,
                    "done after the 58 bytes of the stream, with the five after it left alone");

    /* The window limit. The first bits of a stream say how far back it may refer, and a
     * decoder must keep that much: this one, made with libbrotli's largest window, asks for
     * 16 MiB. A decoder limited to 64 KiB refuses it before allocating anything for it; reset
     * makes the decoder usable again. */
    static const proven_byte_t wide[] = { 0x0f, 0x01, 0x80, 0x61, 0x62, 0x63, 0x03 };
    proven_brotli_decoder_t *small = NULL;
    proven_size_t used = 0, made = 0;
    EXAMPLE_REQUIRE(proven_brotli_decoder_create(heap, 64 * 1024, &small) == PROVEN_OK, "a decoder limited to 64 KiB");
    EXAMPLE_REQUIRE(proven_brotli_decode(small, (proven_mem_view_t){ wide, sizeof wide }, &used, (proven_mem_mut_t){ out, sizeof out }, &made, &done) == PROVEN_ERR_OUT_OF_BOUNDS,
                    "a stream with a 16 MiB window: PROVEN_ERR_OUT_OF_BOUNDS");
    proven_brotli_decoder_reset(small);
    EXAMPLE_REQUIRE(proven_brotli_decode(small, (proven_mem_view_t){ STREAM, sizeof STREAM }, &used, (proven_mem_mut_t){ out, sizeof out }, &made, &done) == PROVEN_OK &&
                    done && made == sizeof text, "after a reset, a stream within the limit decodes");
    proven_brotli_decoder_reset(b);
    EXAMPLE_REQUIRE(proven_brotli_decode(b, (proven_mem_view_t){ wide, sizeof wide }, &used, (proven_mem_mut_t){ out, sizeof out }, &made, &done) == PROVEN_OK &&
                    done && made == 3 && memcmp(out, "abc", 3) == 0, "while the default limit takes it - with a few KiB of history, not 16 MiB");

    proven_brotli_decoder_destroy(small);
    proven_brotli_decoder_destroy(b);
    return EXAMPLE_OK();
}
