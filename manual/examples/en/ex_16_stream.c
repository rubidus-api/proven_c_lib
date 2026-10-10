#include "example.h"
#include <string.h>

/*
 * Compression as a stream: data that arrives in pieces and leaves in pieces, through buffers
 * of whatever size you have. The same loop works for a file of any length, a response that
 * is still being produced, or a connection.
 */

/* Where the compressed bytes go. A real program writes them to a file or a socket. */
static proven_byte_t g_wire[8192];
static proven_size_t g_wire_len;

/* Give the compressor some input and take whatever output it has, 64 bytes at a time, until
 * it has taken all of the input - and, when `flush` asks for more than that, until it has
 * produced all of that too. */
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
        /* Nothing left to give, and it had less than a bufferful to say: it is up to date. */
        if (in.size == 0 && produced < sizeof out) return true;
    }
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    /* A compressor is made once. Zero in the options means the defaults: level 6, a 32 KiB
     * window. Level 1 is faster and compresses a little less; 9 is slower and a little more. */
    proven_deflate_options_t options = { .alloc = heap, .format = PROVEN_DEFLATE_ZLIB, .level = 6 };
    proven_deflate_t *z = NULL;
    EXAMPLE_REQUIRE(proven_deflate_create(&options, &z) == PROVEN_OK, "a compressor");

    /* Pieces go in as they come. With FLUSH_NONE the compressor keeps what it is given until
     * it has a block's worth - nothing may come out for a long time, and that is how it
     * compresses well. */
    bool done = false;
    EXAMPLE_REQUIRE(feed(z, "first piece of the stream; ", PROVEN_DEFLATE_FLUSH_NONE, &done), "a piece goes in");
    const proven_size_t after_first = g_wire_len;
    EXAMPLE_REQUIRE(after_first <= 2 && !done, "and nothing but the two-byte header has come out yet");

    /* A sync flush says: the other side must be able to read everything up to here, now. It
     * costs a few bytes each time - use it when somebody is waiting, not after every write. */
    EXAMPLE_REQUIRE(feed(z, "second piece of the stream; ", PROVEN_DEFLATE_FLUSH_SYNC, &done) && g_wire_len > after_first + 20 && !done,
                    "after a sync flush both pieces are on the wire");
    const proven_size_t at_flush = g_wire_len;

    /* FINISH ends the stream: the last block, and for zlib and gzip the checksum. */
    EXAMPLE_REQUIRE(feed(z, "third and last piece.", PROVEN_DEFLATE_FLUSH_FINISH, &done) && done, "the last piece, and the stream is finished");

    /* The receiving side: a decompressor, fed 10 bytes at a time into a 16-byte buffer. */
    proven_inflate_t *u = NULL;
    EXAMPLE_REQUIRE(proven_inflate_create(heap, PROVEN_DEFLATE_ZLIB, &u) == PROVEN_OK, "a decompressor");
    char text[200];
    proven_size_t text_len = 0, at = 0, seen_at_flush = 0;
    bool ended = false;
    /* First only what had been written when the sync flush returned; then the rest. */
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
            /* PROVEN_OK with nothing consumed and nothing produced: it has used everything
             * it was offered and wants more. That is not an error - it is how a stream waits. */
            if (ended || (consumed == 0 && produced == 0)) break;
        }
        if (limit == at_flush) seen_at_flush = text_len;
        else break;
    }
    static const char whole[] = "first piece of the stream; second piece of the stream; third and last piece.";
    EXAMPLE_REQUIRE(ended && text_len == sizeof whole - 1 && memcmp(text, whole, text_len) == 0, "the three pieces come out as one text");
    EXAMPLE_REQUIRE(at == g_wire_len, "and the stream ended exactly where the compressor stopped writing");
    EXAMPLE_REQUIRE(seen_at_flush >= 55, "at the flush point the receiver already had both of the first two pieces");

    /* Both objects are reused for the next stream rather than made again. */
    proven_deflate_reset(z);
    proven_inflate_reset(u);
    g_wire_len = 0;
    EXAMPLE_REQUIRE(feed(z, "another stream", PROVEN_DEFLATE_FLUSH_FINISH, &done) && done, "the same compressor, a new stream");
    proven_byte_t again[32];
    proven_size_t consumed = 0, produced = 0;
    EXAMPLE_REQUIRE(proven_inflate(u, (proven_mem_view_t){ g_wire, g_wire_len }, &consumed, (proven_mem_mut_t){ again, sizeof again }, &produced, &ended) == PROVEN_OK &&
                    ended && produced == 14 && memcmp(again, "another stream", 14) == 0, "and the same decompressor reads it");

    proven_deflate_destroy(z);
    proven_inflate_destroy(u);
    return EXAMPLE_OK();
}
