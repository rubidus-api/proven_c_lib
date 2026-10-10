#include "example.h"
#include <string.h>

/*
 * Compress a buffer, get it back, and refuse to be handed more than was agreed: the two
 * whole-buffer calls, and the one rule about decompressing what somebody else sent.
 */

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    /* Something that compresses: text repeats itself. */
    static proven_byte_t text[4000];
    static const char line[] = "GET /index.html HTTP/1.1 - 200 - text/html; charset=utf-8\n";
    for (proven_size_t i = 0; i < sizeof text; ++i) text[i] = (proven_byte_t)line[i % (sizeof line - 1)];

    /* Compressing never fails for want of room if the buffer is as large as the bound says. */
    static proven_byte_t packed[4200];
    proven_size_t packed_len = 0;
    proven_deflate_options_t options = { .alloc = heap, .format = PROVEN_DEFLATE_GZIP };
    EXAMPLE_REQUIRE(proven_deflate_bound(PROVEN_DEFLATE_GZIP, sizeof text) <= sizeof packed, "the bound for 4,000 bytes is a little more than 4,000");
    EXAMPLE_REQUIRE(proven_deflate_all(&options, (proven_mem_view_t){ text, sizeof text }, (proven_mem_mut_t){ packed, sizeof packed }, &packed_len) == PROVEN_OK,
                    "4,000 bytes of log lines compress");
    EXAMPLE_REQUIRE(packed_len < 200, "to less than 200: the same line, over and over");
    EXAMPLE_REQUIRE(packed[0] == 0x1f && packed[1] == 0x8b, "and what came out is a gzip member - a .gz file, or an HTTP body with Content-Encoding: gzip");

    /* Decompressing: the output buffer is the limit. Here it is exactly as large as the data. */
    static proven_byte_t back[4000];
    proven_size_t back_len = 0, used = 0;
    EXAMPLE_REQUIRE(proven_inflate_all(heap, PROVEN_DEFLATE_GZIP, (proven_mem_view_t){ packed, packed_len }, (proven_mem_mut_t){ back, sizeof back }, &back_len, &used) == PROVEN_OK,
                    "it decompresses");
    EXAMPLE_REQUIRE(back_len == sizeof text && memcmp(back, text, sizeof text) == 0 && used == packed_len, "to exactly what went in, using every byte of the member");

    /* The same member into a smaller buffer is refused. This is the whole defence against a
     * "decompression bomb": you say how much you are willing to receive, and that is all the
     * room there is. A few hundred bytes cannot make this program allocate a gigabyte. */
    static proven_byte_t small[1000];
    EXAMPLE_REQUIRE(proven_inflate_all(heap, PROVEN_DEFLATE_GZIP, (proven_mem_view_t){ packed, packed_len }, (proven_mem_mut_t){ small, sizeof small }, &back_len, NULL) == PROVEN_ERR_OUT_OF_BOUNDS,
                    "4,000 bytes do not fit in 1,000: PROVEN_ERR_OUT_OF_BOUNDS");

    /* Damage is noticed. gzip carries a CRC-32 of the data; one changed bit anywhere in the
     * compressed stream changes the data or breaks the format. */
    packed[packed_len / 2] ^= 0x04;
    EXAMPLE_REQUIRE(proven_inflate_all(heap, PROVEN_DEFLATE_GZIP, (proven_mem_view_t){ packed, packed_len }, (proven_mem_mut_t){ back, sizeof back }, &back_len, NULL) == PROVEN_ERR_INVALID_FORMAT,
                    "a changed bit: PROVEN_ERR_INVALID_FORMAT");
    packed[packed_len / 2] ^= 0x04;
    EXAMPLE_REQUIRE(proven_inflate_all(heap, PROVEN_DEFLATE_GZIP, (proven_mem_view_t){ packed, packed_len - 1 }, (proven_mem_mut_t){ back, sizeof back }, &back_len, NULL) == PROVEN_ERR_INVALID_FORMAT,
                    "a member cut short is not a member");

    /* The framing is a choice made at both ends. The same data as zlib is twelve bytes
     * smaller (a shorter header, a shorter checksum), and raw is smaller again - and has no
     * checksum at all, so damage to it can go unseen. */
    proven_size_t zlib_len = 0, raw_len = 0;
    static proven_byte_t other[4200];
    options.format = PROVEN_DEFLATE_ZLIB;
    EXAMPLE_REQUIRE(proven_deflate_all(&options, (proven_mem_view_t){ text, sizeof text }, (proven_mem_mut_t){ other, sizeof other }, &zlib_len) == PROVEN_OK && zlib_len == packed_len - 12,
                    "zlib framing: 12 bytes less than gzip");
    EXAMPLE_REQUIRE(proven_inflate_all(heap, PROVEN_DEFLATE_GZIP, (proven_mem_view_t){ other, zlib_len }, (proven_mem_mut_t){ back, sizeof back }, &back_len, NULL) == PROVEN_ERR_INVALID_FORMAT,
                    "and not gzip: a decompressor must be told which it is reading");
    options.format = PROVEN_DEFLATE_RAW;
    EXAMPLE_REQUIRE(proven_deflate_all(&options, (proven_mem_view_t){ text, sizeof text }, (proven_mem_mut_t){ other, sizeof other }, &raw_len) == PROVEN_OK && raw_len == zlib_len - 6,
                    "raw: no header and no checksum, 6 bytes less again");

    /* What does not compress grows a little - by the bound, at worst. */
    static proven_byte_t noise[4000], noisy[4200];
    proven_u32 x = 2463534242u;
    for (proven_size_t i = 0; i < sizeof noise; ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; noise[i] = (proven_byte_t)x; }
    proven_size_t noisy_len = 0;
    options.format = PROVEN_DEFLATE_GZIP;
    EXAMPLE_REQUIRE(proven_deflate_all(&options, (proven_mem_view_t){ noise, sizeof noise }, (proven_mem_mut_t){ noisy, sizeof noisy }, &noisy_len) == PROVEN_OK &&
                    noisy_len > sizeof noise && noisy_len <= proven_deflate_bound(PROVEN_DEFLATE_GZIP, sizeof noise),
                    "random bytes come out slightly larger, and never larger than the bound");

    return EXAMPLE_OK();
}
