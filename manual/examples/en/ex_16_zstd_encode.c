#include "example.h"
#include <string.h>

/*
 * Zstandard, written: a whole buffer, a stream that hands over what it has at a flush, and
 * the choice a server makes between zstd and gzip from a request's Accept-Encoding.
 */

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    static const char line[] = "GET /index.html HTTP/1.1 - 200 - text/html; charset=utf-8\n";
    static proven_byte_t text[40000];
    for (proven_size_t i = 0; i < sizeof text; ++i) text[i] = (proven_byte_t)line[i % (sizeof line - 1)];
    static proven_byte_t frame[48000], back[40000];

    /* A whole buffer. encode_bound is always enough; the frame states the input's size, so a
     * decoder needs no more window than that. */
    proven_zstd_encoder_options_t o = { .alloc = heap, .level = 1 };
    proven_size_t n = 0, got = 0;
    EXAMPLE_REQUIRE(proven_zstd_encode_bound(sizeof text) <= sizeof frame, "the bound for 40,000 bytes fits the buffer");
    EXAMPLE_REQUIRE(proven_zstd_encode_all(&o, (proven_mem_view_t){ text, sizeof text }, (proven_mem_mut_t){ frame, sizeof frame }, &n) == PROVEN_OK && n < 200,
                    "40,000 bytes of repeated log lines compress to fewer than 200");
    EXAMPLE_REQUIRE(proven_zstd_decode_all(heap, (proven_mem_view_t){ frame, n }, (proven_mem_mut_t){ back, sizeof back }, &got) == PROVEN_OK &&
                    got == sizeof text && memcmp(back, text, got) == 0, "and decode to every byte");

    /* A stream: 1,000 bytes at a time into a 64 KiB window, with the checksum. A block flush
     * after the first 5,000 makes them decodable at once - what a server does before it
     * waits for more to send; FINISH ends the frame. */
    proven_zstd_encoder_t *z = NULL;
    o = (proven_zstd_encoder_options_t){ .alloc = heap, .level = 3, .window_log = 16, .checksum = true };
    EXAMPLE_REQUIRE(proven_zstd_encoder_create(&o, &z) == PROVEN_OK, "an encoder at level 3 with a 64 KiB window");
    proven_size_t in_at = 0, out_at = 0;
    bool done = false;
    while (!done) {
        const proven_size_t give = sizeof text - in_at < 1000 ? sizeof text - in_at : 1000;
        const proven_zstd_flush_t flush = in_at + give == sizeof text ? PROVEN_ZSTD_FLUSH_FINISH
                                        : in_at + give == 5000 ? PROVEN_ZSTD_FLUSH_BLOCK : PROVEN_ZSTD_FLUSH_NONE;
        proven_size_t used = 0, made = 0;
        EXAMPLE_REQUIRE(proven_zstd_encode(z, (proven_mem_view_t){ text + in_at, give }, &used, (proven_mem_mut_t){ frame + out_at, sizeof frame - out_at }, &made, flush, &done) == PROVEN_OK,
                        "a step");
        in_at += used;
        out_at += made;
        if (flush == PROVEN_ZSTD_FLUSH_BLOCK) {
            /* Everything so far, decoded from what has come out so far: the frame is not over. */
            proven_zstd_decoder_t *d = NULL;
            proven_size_t dused = 0, dmade = 0;
            bool dend = true;
            EXAMPLE_REQUIRE(proven_zstd_decoder_create(heap, 0, &d) == PROVEN_OK &&
                            proven_zstd_decode(d, (proven_mem_view_t){ frame, out_at }, &dused, (proven_mem_mut_t){ back, sizeof back }, &dmade, &dend) == PROVEN_OK &&
                            dmade == 5000 && !dend, "after the flush, a decoder gives back all 5,000 bytes and waits for the rest");
            proven_zstd_decoder_destroy(d);
        }
    }
    EXAMPLE_REQUIRE(proven_zstd_decode_all(heap, (proven_mem_view_t){ frame, out_at }, (proven_mem_mut_t){ back, sizeof back }, &got) == PROVEN_OK &&
                    got == sizeof text && memcmp(back, text, got) == 0, "the finished frame decodes to the 40,000 bytes, checksum checked");
    proven_zstd_encoder_reset(z);                         /* the same encoder, a new frame */
    proven_size_t used = 0, made = 0;
    EXAMPLE_REQUIRE(proven_zstd_encode(z, (proven_mem_view_t){ text, 300 }, &used, (proven_mem_mut_t){ frame, sizeof frame }, &made, PROVEN_ZSTD_FLUSH_FINISH, &done) == PROVEN_OK &&
                    done && used == 300, "after a reset, a second frame");
    proven_zstd_encoder_destroy(z);

    /* Which coding a request wants most: weights in thousandths. curl and browsers send zstd
     * and gzip at one weight; a server that offers zstd takes it then (compress_zstd). */
    const proven_http_header_t asks[] = { { PROVEN_LIT("Accept-Encoding"), PROVEN_LIT("gzip, deflate, br, zstd") } };
    const proven_http_header_t prefers_gzip[] = { { PROVEN_LIT("Accept-Encoding"), PROVEN_LIT("gzip;q=1.0, zstd;q=0.5") } };
    EXAMPLE_REQUIRE(proven_http_accept_weight(asks, 1, PROVEN_HTTP_CODING_ZSTD) == 1000 && proven_http_accept_weight(asks, 1, PROVEN_HTTP_CODING_GZIP) == 1000,
                    "a browser's list: zstd and gzip both 1000");
    EXAMPLE_REQUIRE(proven_http_accept_weight(prefers_gzip, 1, PROVEN_HTTP_CODING_ZSTD) == 500 && proven_http_accept_weight(prefers_gzip, 1, PROVEN_HTTP_CODING_GZIP) == 1000,
                    "a client that prefers gzip: zstd 500, gzip 1000");
    return EXAMPLE_OK();
}
