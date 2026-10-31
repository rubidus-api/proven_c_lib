#include "proven.h"
#include "proven_test.h"
#include "../src/proven/proven_internal_zstd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Zstandard compression.
 *
 * Everything the encoder makes is decoded here by this library's decoder (zstd.c), which
 * checks every offset against the window the frame states and every stream for being read
 * exactly. That libzstd decodes the same frames is checked by a private program over the same
 * kinds of input and many more (named in TEST.md); the pinned sizes and CRCs below are of
 * frames it decoded, and catch an unintended change of output - they are not a contract.
 */

static proven_allocator_t g_heap;
static proven_byte_t g_in[1100000], g_enc[1200000], g_dec[1100000];

static proven_u32 g_x;
static proven_u32 xs(void) { g_x ^= g_x << 13; g_x ^= g_x >> 17; g_x ^= g_x << 5; return g_x; }

/* The inputs: nothing, one byte, a run, numbered lines, noise with copies from far back,
 * random bytes, bytes of more than 128 values; matches at three offsets in turn (each one a
 * repeat offset of the third kind); two byte values, and four equally common ones (Huffman
 * weights written four bits each); and 129 copies of a piece, a random byte before each:
 * exactly 128 sequences, the most a count of one byte cannot hold. */
static proven_size_t corpus(int i, proven_byte_t *out) {
    proven_size_t n = 0;
    switch (i) {
        case 0: return 0;
        case 1: out[0] = 'a'; return 1;
        case 2: memset(out, 'a', 300000); return 300000;
        case 3: {
            g_x = 3;
            char line[96];
            for (unsigned k = 0; n < 400000; ++k) {
                /* Two statements: the order of a call's arguments is the compiler's to choose. */
                const unsigned a = xs() % 1000, b = xs() % 50;
                const int len = snprintf(line, sizeof line, "line %u says %u and %u\n", k, a, b);
                for (int j = 0; j < len && n < 400000; ++j) out[n++] = (proven_byte_t)line[j];
            }
            return n; }
        case 4: {
            g_x = 4;
            while (n < 1000000) {
                if (n < 1000 || (xs() & 7) == 0) {
                    proven_u32 k = 1 + xs() % 8;
                    while (k-- > 0 && n < 1000000) out[n++] = (proven_byte_t)xs();
                } else {
                    const proven_u32 len = 4 + xs() % 300, lim = n < 900000 ? (proven_u32)n : 900000u, d = 1 + xs() % lim;
                    for (proven_u32 k = 0; k < len && n < 1000000; ++k, ++n) out[n] = out[n - d];
                }
            }
            return n; }
        case 5: g_x = 5; for (n = 0; n < 70000; ++n) out[n] = (proven_byte_t)xs(); return n;
        case 6: g_x = 6; for (n = 0; n < 50000; ++n) out[n] = (proven_byte_t)(xs() % 200 < 150 ? 32 + xs() % 200 : 'e'); return n;
        case 7: {
            static const proven_u32 OFF[3] = { 300, 500, 700 };
            g_x = 7;
            for (n = 0; n < 1000; ++n) out[n] = (proven_byte_t)xs();
            for (unsigned k = 0; n < 300000; ++k) {
                out[n++] = (proven_byte_t)xs();
                const proven_u32 len = 20 + xs() % 180;
                for (proven_u32 j = 0; j < len && n < 300000; ++j, ++n) out[n] = out[n - OFF[k % 3]];
            }
            return n; }
        case 8: g_x = 8; for (n = 0; n < 20000; ++n) out[n] = (proven_byte_t)(xs() & 1); return n;
        case 9: g_x = 9; for (n = 0; n < 20000; ++n) out[n] = (proven_byte_t)(xs() & 3); return n;
        case 10: {
            g_x = 10;
            proven_byte_t piece[24];
            for (int j = 0; j < 24; ++j) piece[j] = (proven_byte_t)xs();
            for (int k = 0; k < 129; ++k) { out[n++] = (proven_byte_t)xs(); memcpy(out + n, piece, 24); n += 24; }
            return n; }
        default: return 0;
    }
}

/* An allocator that counts: the bytes held now and at most, and a failure on demand. */
typedef struct {
    proven_size_t live, peak, calls, fail_at;
    void *ptr[16];
    proven_size_t size[16];
} counter_t;
static counter_t g_count;
static void count_set(void *p, proven_size_t n) {
    for (int i = 0; i < 16; ++i) if (!g_count.ptr[i]) { g_count.ptr[i] = p; g_count.size[i] = n; break; }
    g_count.live += n;
    if (g_count.live > g_count.peak) g_count.peak = g_count.live;
}
static void count_drop(void *p) {
    for (int i = 0; i < 16; ++i) if (g_count.ptr[i] == p) { g_count.live -= g_count.size[i]; g_count.ptr[i] = NULL; break; }
}
static proven_result_mem_mut_t c_alloc(void *ctx, proven_size_t n, proven_size_t align) {
    (void)ctx;
    if (++g_count.calls == g_count.fail_at) return (proven_result_mem_mut_t){ .err = PROVEN_ERR_NOMEM };
    proven_result_mem_mut_t m = g_heap.alloc_fn(g_heap.ctx, n, align);
    if (m.err == PROVEN_OK) count_set(m.value.ptr, n);
    return m;
}
static proven_result_mem_mut_t c_realloc(void *ctx, void *old, proven_size_t old_n, proven_size_t n, proven_size_t align) {
    (void)ctx;
    if (++g_count.calls == g_count.fail_at) return (proven_result_mem_mut_t){ .err = PROVEN_ERR_NOMEM };
    proven_result_mem_mut_t m = g_heap.realloc_fn(g_heap.ctx, old, old_n, n, align);
    if (m.err == PROVEN_OK) { count_drop(old); count_set(m.value.ptr, n); }
    return m;
}
static void c_free(void *ctx, void *p) { (void)ctx; count_drop(p); g_heap.free_fn(g_heap.ctx, p); }
static proven_allocator_t counting(proven_size_t fail_at) {
    memset(&g_count, 0, sizeof g_count);
    g_count.fail_at = fail_at;
    return (proven_allocator_t){ .ctx = NULL, .alloc_fn = c_alloc, .realloc_fn = c_realloc, .free_fn = c_free };
}

/* Compress through one encoder with `piece` bytes in and `room` bytes out per call; a block
 * flush after each piece if asked; FINISH with the last. */
static proven_err_t encode_steps(proven_zstd_encoder_t *z, const proven_byte_t *in, proven_size_t n, proven_size_t piece, proven_size_t room,
                                 bool flush, proven_byte_t *out, proven_size_t cap, proven_size_t *made) {
    proven_size_t ia = 0;
    *made = 0;
    bool done = false;
    for (long guard = 0; !done && guard < 40000000; ++guard) {
        const proven_size_t give = n - ia < piece ? n - ia : piece, r = cap - *made < room ? cap - *made : room;
        const proven_zstd_flush_t f = ia + give == n ? PROVEN_ZSTD_FLUSH_FINISH : flush ? PROVEN_ZSTD_FLUSH_BLOCK : PROVEN_ZSTD_FLUSH_NONE;
        proven_size_t c = 0, p = 0;
        proven_err_t e = proven_zstd_encode(z, (proven_mem_view_t){ in + ia, give }, &c, (proven_mem_mut_t){ out + *made, r }, &p, f, &done);
        if (e != PROVEN_OK) return e;
        ia += c;
        *made += p;
        if (c == 0 && p == 0 && !done && *made == cap) return PROVEN_ERR_OUT_OF_BOUNDS;
    }
    return done && ia == n ? PROVEN_OK : PROVEN_ERR_INVALID_STATE;
}

static bool decodes_to(const proven_byte_t *f, proven_size_t fn, const proven_byte_t *want, proven_size_t n, proven_u64 window_max) {
    proven_zstd_decoder_t *d = NULL;
    if (proven_zstd_decoder_create(g_heap, window_max, &d) != PROVEN_OK) return false;
    proven_size_t c = 0, p = 0;
    bool done = false;
    proven_err_t e = proven_zstd_decode(d, (proven_mem_view_t){ f, fn }, &c, (proven_mem_mut_t){ g_dec, sizeof g_dec }, &p, &done);
    proven_zstd_decoder_destroy(d);
    return e == PROVEN_OK && done && c == fn && p == n && memcmp(g_dec, want, n) == 0;
}

int main(void) {
    PROVEN_TEST_SUITE("Zstandard compression",
        "Compress so that the decoder gives back every byte, whole and in steps, at every level and window; keep to the bound, the window and the memory stated; flush and finish as documented.",
        "Inspect src/proven/zstd_encode.c. A failing input is printed by its corpus number, level and window.");
    g_heap = proven_heap_allocator();

    PROVEN_TEST_SECTION("round trips", "Eleven inputs at levels 1 to 19 (13 to 19 on the large ones with the 1 KiB window only) and windows of 1 KiB, 64 KiB and the default, with and without the checksum, in one call, and in steps at levels 1 to 4, 6, 8 and 13 (each matcher).", "Check compress_block and write_block; a frame the decoder refuses is printed with what made it.");
    {
        static const int WL[3] = { 0, 10, 16 };
        int failed = 0, frames = 0;
        for (int i = 0; i <= 10; ++i) {
            const proven_size_t n = corpus(i, g_in);
            for (int level = 1; level <= 19; ++level) {
                for (int w = 0; w < 3; ++w) {
                    /* The binary trees (13 to 19) are slow under the sanitizers: their large inputs
                     * with the 1 KiB window only - still many blocks and slides. */
                    if (level >= 13 && n > 100000 && WL[w] != 10) continue;
                    proven_zstd_encoder_options_t o = { .alloc = g_heap, .level = level, .window_log = WL[w], .checksum = (i + level) % 2 == 0 };
                    proven_size_t made = 0;
                    proven_err_t e = proven_zstd_encode_all(&o, (proven_mem_view_t){ g_in, n }, (proven_mem_mut_t){ g_enc, sizeof g_enc }, &made);
                    const proven_u64 wmax = WL[w] ? (proven_u64)1 << WL[w] : 0;
                    frames++;
                    if (e != PROVEN_OK || made > proven_zstd_encode_bound(n) || !decodes_to(g_enc, made, g_in, n, wmax)) { failed++; printf("  whole: input %d level %d window %d: %d\n", i, level, WL[w], (int)e); }
                    if (n > 400000 && WL[w] == 0) continue;
                    if (level > 4 && level != 6 && level != 8 && level != 13) continue;     /* steps: one level for each matcher and depth */
                    proven_zstd_encoder_t *z = NULL;
                    if (proven_zstd_encoder_create(&o, &z) != PROVEN_OK) { failed++; continue; }
                    static const proven_size_t PIECE[3] = { 7, 1000, 100000 }, ROOM[3] = { 64, 1, (proven_size_t)-1 };
                    for (int s = 0; s < 3; ++s) {
                        if (PIECE[s] < 1000 && n > 80000) continue;
                        const proven_size_t room = n > 80000 && s == 1 ? 4096 : ROOM[s];
                        e = encode_steps(z, g_in, n, PIECE[s], room, s == 1, g_enc, sizeof g_enc, &made);
                        frames++;
                        if (e != PROVEN_OK || !decodes_to(g_enc, made, g_in, n, wmax)) { failed++; printf("  steps: input %d level %d window %d piece %zu: %d\n", i, level, WL[w], PIECE[s], (int)e); }
                        proven_zstd_encoder_reset(z);
                    }
                    proven_zstd_encoder_destroy(z);
                }
            }
        }
        printf("  %d frames\n", frames);
        PROVEN_TEST_ASSERT(failed == 0 && frames > 600, "every frame decodes to its input, with every offset inside the window it states", "");
    }

    PROVEN_TEST_SECTION("blocks full of sequences", "Short matches a few bytes apart, more sequences than a block holds, at the optimal levels: each block ends where its sequences run out, whole and streamed, and every frame decodes; and a long run of one byte through the trees.", "Check the stretch's limit (`span`, the sequences left) in match_opt in src/proven/zstd_encode.c.");
    {
        /* Four-byte tokens from a set of 64, a random byte after each: a match of four (or a little
         * more) every five bytes - about 26,000 sequences in a block of 128 KiB, which holds 16,384. */
        static proven_byte_t tok[64][4];
        g_x = 77;
        for (int t = 0; t < 64; ++t) for (int b = 0; b < 4; ++b) tok[t][b] = (proven_byte_t)xs();
        proven_size_t n = 0;
        while (n + 5 <= 300000) { memcpy(g_in + n, tok[xs() % 64], 4); n += 4; g_in[n++] = (proven_byte_t)xs(); }
        int bad = 0;
        for (int level = 16; level <= 19; level += 3) {
            proven_size_t made = 0;
            proven_zstd_encoder_options_t o = { .alloc = g_heap, .level = level };
            if (proven_zstd_encode_all(&o, (proven_mem_view_t){ g_in, n }, (proven_mem_mut_t){ g_enc, sizeof g_enc }, &made) != PROVEN_OK || !decodes_to(g_enc, made, g_in, n, 0)) bad++;
            o.window_log = 16;
            proven_zstd_encoder_t *z = NULL;
            if (proven_zstd_encoder_create(&o, &z) != PROVEN_OK || encode_steps(z, g_in, n, 100000, (proven_size_t)-1, false, g_enc, sizeof g_enc, &made) != PROVEN_OK ||
                !decodes_to(g_enc, made, g_in, n, (proven_u64)1 << 16)) bad++;
            proven_zstd_encoder_destroy(z);
        }
        PROVEN_TEST_ASSERT(bad == 0, "at levels 16 and 19, whole and streamed: every frame decodes to its input", "");
        /* Three-byte tokens from a set of 64, one after another: a three-byte match every three
         * bytes at 18 and 19, which take them - more matches in a stretch than four-byte ones fit. */
        static proven_byte_t tok3[64][3];
        g_x = 78;
        for (int t = 0; t < 64; ++t) for (int b = 0; b < 3; ++b) tok3[t][b] = (proven_byte_t)xs();
        n = 0;
        while (n + 3 <= 300000) { memcpy(g_in + n, tok3[xs() % 64], 3); n += 3; }
        bad = 0;
        for (int level = 18; level <= 19; ++level) {
            proven_size_t made = 0;
            proven_zstd_encoder_options_t o = { .alloc = g_heap, .level = level };
            if (proven_zstd_encode_all(&o, (proven_mem_view_t){ g_in, n }, (proven_mem_mut_t){ g_enc, sizeof g_enc }, &made) != PROVEN_OK || !decodes_to(g_enc, made, g_in, n, 0)) bad++;
        }
        PROVEN_TEST_ASSERT(bad == 0, "three-byte matches every three bytes, at 18 and 19: every frame decodes to its input", "");
        /* A million bytes of one value, at the largest window: every position a match to the end of
         * its block. Putting each into the trees would walk the block each time - minutes for this;
         * a position whose match is over 384 bytes lets the next 192 go without (bt_update). */
        memset(g_in, 'z', 1000000);
        bad = 0;
        for (int level = 13; level <= 19; level += 3) {
            proven_size_t made = 0;
            proven_zstd_encoder_options_t o = { .alloc = g_heap, .level = level, .window_log = 23 };
            if (proven_zstd_encode_all(&o, (proven_mem_view_t){ g_in, 1000000 }, (proven_mem_mut_t){ g_enc, sizeof g_enc }, &made) != PROVEN_OK || made > 200 ||
                !decodes_to(g_enc, made, g_in, 1000000, 0)) bad++;
        }
        PROVEN_TEST_ASSERT(bad == 0, "a million bytes of one value at 13, 16 and 19: under 200 bytes, and quickly", "Check bt_update: without its skip this takes minutes.");
    }

    PROVEN_TEST_SECTION("what the frame states", "A whole buffer states its size, and its window is that size; a stream states its window.", "Check frame_header in src/proven/zstd_encode.c.");
    {
        proven_zstd_encoder_options_t o = { .alloc = g_heap, .level = 1 };
        proven_size_t made = 0;
        const proven_size_t n = corpus(3, g_in);
        PROVEN_TEST_ASSERT(proven_zstd_encode_all(&o, (proven_mem_view_t){ g_in, 200 }, (proven_mem_mut_t){ g_enc, sizeof g_enc }, &made) == PROVEN_OK &&
                           g_enc[4] == 0x20 && g_enc[5] == 200, "200 bytes: single segment, the size in one byte, no checksum", "");
        o.checksum = true;
        PROVEN_TEST_ASSERT(proven_zstd_encode_all(&o, (proven_mem_view_t){ g_in, 300 }, (proven_mem_mut_t){ g_enc, sizeof g_enc }, &made) == PROVEN_OK &&
                           g_enc[4] == 0x64 && g_enc[5] + 256 * g_enc[6] + 256 == 300, "300 bytes with the checksum: the size in two bytes, less 256", "");
        PROVEN_TEST_ASSERT(proven_zstd_encode_all(&o, (proven_mem_view_t){ g_in, 0 }, (proven_mem_mut_t){ g_enc, sizeof g_enc }, &made) == PROVEN_OK && made == 13 &&
                           g_enc[4] == 0x24 && g_enc[5] == 0 && g_enc[6] == 1 && g_enc[7] == 0 && g_enc[8] == 0 && decodes_to(g_enc, made, g_in, 0, 0),
                           "nothing: a frame of size 0, one empty last block and the checksum, 13 bytes", "");
        o.window_log = 16;
        o.checksum = false;
        PROVEN_TEST_ASSERT(proven_zstd_encode_all(&o, (proven_mem_view_t){ g_in, n }, (proven_mem_mut_t){ g_enc, sizeof g_enc }, &made) == PROVEN_OK &&
                           g_enc[4] == 0x80 && g_enc[5] == (16 - 10) << 3 && decodes_to(g_enc, made, g_in, n, 1 << 16),
                           "400,000 bytes with a 64 KiB window: the size in four bytes, the window stated, and decodable by a decoder limited to 64 KiB", "");
        proven_zstd_encoder_t *z = NULL;
        PROVEN_TEST_ASSERT(proven_zstd_encoder_create(&o, &z) == PROVEN_OK && encode_steps(z, g_in, 5000, 5000, 1 << 20, false, g_enc, sizeof g_enc, &made) == PROVEN_OK &&
                           g_enc[4] == 0x00 && g_enc[5] == (16 - 10) << 3, "a stream: no size, the window", "");
        proven_zstd_encoder_destroy(z);
    }

    PROVEN_TEST_SECTION("the bound", "Random and repeated bytes of every size to 4,096 and around block sizes: never more than proven_zstd_encode_bound; one byte less room than needed is PROVEN_ERR_OUT_OF_BOUNDS.", "Check proven_zstd_encode_bound and the stored-block fallback in write_block.");
    {
        int over = 0, short_ok = 0, tried = 0;
        g_x = 9;
        for (proven_size_t n = 0; n <= 140000; n += n < 4096 ? 1 : 9973) {
            for (int kind = 0; kind < 2; ++kind) {
                for (proven_size_t i = 0; i < n; ++i) g_in[i] = kind ? 'q' : (proven_byte_t)xs();
                proven_zstd_encoder_options_t o = { .alloc = g_heap, .level = 1 + (int)(n % 9), .checksum = n % 2 == 1 };
                proven_size_t made = 0, m2 = 0;
                tried++;
                if (proven_zstd_encode_all(&o, (proven_mem_view_t){ g_in, n }, (proven_mem_mut_t){ g_enc, proven_zstd_encode_bound(n) }, &made) != PROVEN_OK ||
                    made > proven_zstd_encode_bound(n) || !decodes_to(g_enc, made, g_in, n, 0)) over++;
                if (made > 0 && proven_zstd_encode_all(&o, (proven_mem_view_t){ g_in, n }, (proven_mem_mut_t){ g_enc, made - 1 }, &m2) != PROVEN_ERR_OUT_OF_BOUNDS) short_ok++;
            }
        }
        printf("  %d inputs\n", tried);
        PROVEN_TEST_ASSERT(over == 0 && tried > 8000, "the bound always holds and is always enough", "");
        PROVEN_TEST_ASSERT(short_ok == 0, "with less room than the frame needs, PROVEN_ERR_OUT_OF_BOUNDS", "");
        PROVEN_TEST_ASSERT(proven_zstd_encode_bound((proven_size_t)-1) == (proven_size_t)-1, "and a size whose bound does not fit is answered with the largest size", "");
    }

    PROVEN_TEST_SECTION("flushes, the end, and starting over", "What a block flush hands over decodes on its own; after FINISH input is refused until a reset; a reset begins a new frame.", "Check proven_zstd_encode's loop.");
    {
        const proven_size_t n = corpus(3, g_in);
        proven_zstd_encoder_options_t o = { .alloc = g_heap, .level = 2, .checksum = true };
        proven_zstd_encoder_t *z = NULL;
        PROVEN_TEST_ASSERT(proven_zstd_encoder_create(&o, &z) == PROVEN_OK, "an encoder", "");
        proven_size_t c = 0, p = 0, made = 0;
        bool done = false;
        PROVEN_TEST_ASSERT(proven_zstd_encode(z, (proven_mem_view_t){ g_in, 1000 }, &c, (proven_mem_mut_t){ g_enc, sizeof g_enc }, &p, PROVEN_ZSTD_FLUSH_NONE, &done) == PROVEN_OK &&
                           c == 1000 && p == 6 && !done, "1,000 bytes without a flush: taken, and only the frame header comes out", "");
        made = p;
        PROVEN_TEST_ASSERT(proven_zstd_encode(z, (proven_mem_view_t){ g_in + 1000, 500 }, &c, (proven_mem_mut_t){ g_enc + made, sizeof g_enc - made }, &p, PROVEN_ZSTD_FLUSH_BLOCK, &done) == PROVEN_OK &&
                           c == 500 && p > 0 && !done, "500 more with a block flush: blocks come out, and the frame is not done", "");
        made += p;
        proven_zstd_decoder_t *d = NULL;
        proven_size_t dc = 0, dp = 0;
        bool dd = true;
        PROVEN_TEST_ASSERT(proven_zstd_decoder_create(g_heap, 0, &d) == PROVEN_OK &&
                           proven_zstd_decode(d, (proven_mem_view_t){ g_enc, made }, &dc, (proven_mem_mut_t){ g_dec, sizeof g_dec }, &dp, &dd) == PROVEN_OK &&
                           dc == made && dp == 1500 && !dd && memcmp(g_dec, g_in, 1500) == 0, "a decoder given what came out so far gives back all 1,500 bytes, and waits for more", "");
        proven_zstd_decoder_destroy(d);
        PROVEN_TEST_ASSERT(proven_zstd_encode(z, (proven_mem_view_t){ NULL, 0 }, &c, (proven_mem_mut_t){ g_enc + made, sizeof g_enc - made }, &p, PROVEN_ZSTD_FLUSH_BLOCK, &done) == PROVEN_OK &&
                           c == 0 && p == 0, "a block flush with nothing new gives nothing", "");
        PROVEN_TEST_ASSERT(proven_zstd_encode(z, (proven_mem_view_t){ NULL, 0 }, &c, (proven_mem_mut_t){ g_enc + made, 2 }, &p, PROVEN_ZSTD_FLUSH_FINISH, &done) == PROVEN_OK &&
                           p == 2 && !done, "FINISH into 2 bytes: 2 bytes, not done", "");
        made += p;
        PROVEN_TEST_ASSERT(proven_zstd_encode(z, (proven_mem_view_t){ NULL, 0 }, &c, (proven_mem_mut_t){ g_enc + made, sizeof g_enc - made }, &p, PROVEN_ZSTD_FLUSH_FINISH, &done) == PROVEN_OK &&
                           p == 5 && done, "and the rest: an empty last block and the checksum, done", "");
        made += p;
        PROVEN_TEST_ASSERT(decodes_to(g_enc, made, g_in, 1500, 0), "the frame decodes to the 1,500 bytes", "");
        PROVEN_TEST_ASSERT(proven_zstd_encode(z, (proven_mem_view_t){ g_in, 1 }, &c, (proven_mem_mut_t){ g_enc, 10 }, &p, PROVEN_ZSTD_FLUSH_FINISH, &done) == PROVEN_ERR_INVALID_STATE,
                           "input after the end is PROVEN_ERR_INVALID_STATE", "");
        PROVEN_TEST_ASSERT(proven_zstd_encode(z, (proven_mem_view_t){ NULL, 0 }, &c, (proven_mem_mut_t){ g_enc, 10 }, &p, PROVEN_ZSTD_FLUSH_FINISH, &done) == PROVEN_OK && done && p == 0,
                           "and nothing after the end is done, with nothing more", "");
        proven_zstd_encoder_reset(z);
        PROVEN_TEST_ASSERT(encode_steps(z, g_in, n, 70000, (proven_size_t)-1, false, g_enc, sizeof g_enc, &made) == PROVEN_OK && decodes_to(g_enc, made, g_in, n, 0),
                           "after a reset, a new frame of 400,000 bytes", "");
        /* Literals of one byte value inside a compressed block: 1,000 random bytes in a block
         * of their own, then a copy from 1,000 back with a new x at every 41st byte - each x a
         * literal, each followed by a match at the first repeat offset. */
        g_x = 11;
        for (proven_size_t i = 0; i < 1000; ++i) g_in[i] = (proven_byte_t)xs();
        for (proven_size_t i = 1000; i < 91000; ++i) g_in[i] = i % 41 == 0 && g_in[i - 1000] != 'x' ? 'x' : g_in[i - 1000];
        proven_zstd_encoder_reset(z);
        PROVEN_TEST_ASSERT(proven_zstd_encode(z, (proven_mem_view_t){ g_in, 1000 }, &c, (proven_mem_mut_t){ g_enc, sizeof g_enc }, &p, PROVEN_ZSTD_FLUSH_BLOCK, &done) == PROVEN_OK && c == 1000,
                           "1,000 random bytes, and a block flush", "");
        made = p;
        PROVEN_TEST_ASSERT(proven_zstd_encode(z, (proven_mem_view_t){ g_in + 1000, 90000 }, &c, (proven_mem_mut_t){ g_enc + made, sizeof g_enc - made }, &p, PROVEN_ZSTD_FLUSH_FINISH, &done) == PROVEN_OK &&
                           c == 90000 && done, "90,000 more taken and finished in one call", "");
        const proven_size_t b2 = made + 3;               /* the second block's literals section */
        made += p;
        PROVEN_TEST_ASSERT((g_enc[made - 3 - (p - 3)] & 6) == 4 && (g_enc[b2] & 3) == 1 && decodes_to(g_enc, made, g_in, 91000, 0),
                           "the second block is compressed, its literals one byte repeated, and the frame decodes", "Check the single-value branch of write_literals.");
        proven_zstd_encoder_destroy(z);
    }

    PROVEN_TEST_SECTION("a stream is the whole buffer", "300,000 bytes at every level, with windows of 1 KiB and 64 KiB: streamed in pieces of 100,000, the input buffer sliding many times, the blocks are byte for byte those of the same bytes compressed whole - only the frame headers differ.",
        "Check slide in src/proven/zstd_encode.c: the hash tables, and the chain of levels 4 to 12 and the trees of 13 to 19 (indexed by position modulo their size, so rotated), and the three-byte table of the optimal parse, must move with the buffer.");
    {
        corpus(4, g_in);
        const proven_size_t n = 300000;
        int differ = 0;
        static proven_byte_t whole[400000];
        for (int level = 1; level <= 19; ++level) {
            for (int wl = 10; wl <= 16; wl += 6) {
                proven_zstd_encoder_options_t o = { .alloc = g_heap, .level = level, .window_log = wl };
                proven_size_t w = 0, made = 0;
                proven_zstd_encoder_t *z = NULL;
                if (proven_zstd_encode_all(&o, (proven_mem_view_t){ g_in, n }, (proven_mem_mut_t){ whole, sizeof whole }, &w) != PROVEN_OK ||
                    proven_zstd_encoder_create(&o, &z) != PROVEN_OK) { differ++; continue; }
                (void)encode_steps(z, g_in, n, 100000, (proven_size_t)-1, false, g_enc, sizeof g_enc, &made);
                proven_zstd_encoder_destroy(z);
                /* Whole: magic, descriptor, window, four bytes of size. Streamed: magic, descriptor, window. */
                if (w < 10 || made < 6 || w - 10 != made - 6 || memcmp(whole + 10, g_enc + 6, made - 6) != 0) { differ++; printf("  level %d window %d: streamed %zu, whole %zu\n", level, wl, made, w); }
            }
        }
        PROVEN_TEST_ASSERT(differ == 0, "at every level and both windows, the streamed frame's blocks are the whole frame's", "");
    }

    PROVEN_TEST_SECTION("memory", "What an encoder holds, for a stream and for a whole buffer; and allocations that fail.", "Check make, block_room, emit and the input buffer's growth in src/proven/zstd_encode.c.");
    {
        const proven_size_t n = corpus(4, g_in);
        for (int level = 1; level <= 19; ++level) {
            /* At 16 to 19 the tables, and the parse's state: its path and its three-byte table. */
            static const proven_size_t HASH[20] = { 0, 64 * 1024, 96 * 1024, 768 * 1024, 768 * 1024, 1024 * 1024, 1024 * 1024, 1024 * 1024, 1024 * 1024, 1024 * 1024,
                                                    8 << 20, 8 << 20, 8 << 20, 8 << 20, 8 << 20, 8 << 20, (8 << 20) + 400 * 1024, (8 << 20) + 400 * 1024,
                                                    (8 << 20) + 400 * 1024, (8 << 20) + 400 * 1024 };
            proven_zstd_encoder_options_t o = { .alloc = counting(0), .level = level };
            proven_zstd_encoder_t *z = NULL;
            proven_size_t made = 0;
            PROVEN_TEST_ASSERT(proven_zstd_encoder_create(&o, &z) == PROVEN_OK && encode_steps(z, g_in, n, 100000, (proven_size_t)-1, false, g_enc, sizeof g_enc, &made) == PROVEN_OK,
                               "a stream of 1,000,000 bytes", "");
            proven_zstd_encoder_destroy(z);
            printf("  level %d: a stream held at most %zu bytes\n", level, g_count.peak);
            PROVEN_TEST_ASSERT(g_count.peak <= HASH[level] + 768 * 1024 + 500 * 1024 + 64 * 1024 && g_count.live == 0,
                               "at most its hash tables, the 768 KiB input buffer of the default window, about 500 KiB for a block, and the encoder itself; nothing after", "");
            (void)proven_zstd_encode_all(&(proven_zstd_encoder_options_t){ .alloc = counting(0), .level = level }, (proven_mem_view_t){ g_in, 2000 }, (proven_mem_mut_t){ g_enc, sizeof g_enc }, &made);
            printf("  level %d: 2,000 bytes in one call held at most %zu bytes\n", level, g_count.peak);
            /* From 13 on a short input is parsed optimally (as libzstd does below 256 KiB): its path and three-byte table too. */
            PROVEN_TEST_ASSERT(g_count.peak <= (level >= 13 ? 160u : 64u) * 1024 && g_count.live == 0, "a whole buffer of 2,000 bytes needs less than 64 KiB (160 KiB from level 13)", "");
        }
        int nomem = 0, other = 0;
        for (proven_size_t k = 1; k <= 9; ++k) {
            proven_zstd_encoder_options_t o = { .alloc = counting(k), .level = 3 };
            proven_zstd_encoder_t *z = NULL;
            proven_size_t made = 0;
            proven_err_t e = proven_zstd_encoder_create(&o, &z);
            if (e == PROVEN_OK) {
                e = encode_steps(z, g_in, 300000, 100000, (proven_size_t)-1, false, g_enc, sizeof g_enc, &made);
                if (e == PROVEN_ERR_NOMEM) {
                    proven_size_t c = 0, p = 0;
                    bool done = false;
                    PROVEN_TEST_ASSERT(proven_zstd_encode(z, (proven_mem_view_t){ NULL, 0 }, &c, (proven_mem_mut_t){ g_enc, 10 }, &p, PROVEN_ZSTD_FLUSH_FINISH, &done) == PROVEN_ERR_NOMEM,
                                       "after an allocation failed, every call says so", "");
                    proven_zstd_encoder_reset(z);
                    g_count.fail_at = 0;
                    PROVEN_TEST_ASSERT(encode_steps(z, g_in, 300000, 100000, (proven_size_t)-1, false, g_enc, sizeof g_enc, &made) == PROVEN_OK && decodes_to(g_enc, made, g_in, 300000, 0),
                                       "until a reset, after which the encoder works", "");
                }
                proven_zstd_encoder_destroy(z);
            }
            if (e == PROVEN_ERR_NOMEM) nomem++;
            else if (e != PROVEN_OK) other++;
            PROVEN_TEST_ASSERT(g_count.live == 0, "nothing is left held after an allocation fails", "");
            proven_size_t made2 = 0;
            e = proven_zstd_encode_all(&(proven_zstd_encoder_options_t){ .alloc = counting(k), .level = 1 }, (proven_mem_view_t){ g_in, 300000 }, (proven_mem_mut_t){ g_enc, sizeof g_enc }, &made2);
            PROVEN_TEST_ASSERT((e == PROVEN_OK || e == PROVEN_ERR_NOMEM) && g_count.live == 0, "nor after encode_all fails to allocate", "");
        }
        PROVEN_TEST_ASSERT(nomem >= 5 && other == 0, "each allocation that fails is PROVEN_ERR_NOMEM", "");
    }

    PROVEN_TEST_SECTION("pinned output", "The size and CRC-32 of twenty-six frames - nineteen whole (levels 1 to 3, 6, 9 to 19, and 13 on a short input, which it parses optimally), seven streamed after a reset with a sliding buffer, at levels 3, 4, 6, 8, 13 and 16 - which libzstd decoded when they were pinned, from the processor's build of the block coder and from the plain one: a change here is a change of output, to be looked at and re-pinned.", "If the change was meant, run the private check again and update the numbers.");
    {
        static const struct { int input, level; proven_size_t size; proven_u32 crc; } PIN[] = {
            { 3, 1, 91849, 0xd24793b3u }, { 3, 2, 97612, 0x7367ffaeu }, { 3, 3, 97699, 0xb8b0492eu },
            { 4, 1, 107904, 0x77238193u }, { 4, 3, 99452, 0xd3c79d20u }, { 6, 1, 40820, 0x86ab3620u },
            { 3, 6, 88115, 0x7fbcdd68u }, { 3, 9, 83927, 0xfdcda819u },
            { 3, 10, 83394, 0x1e7e8206u }, { 3, 11, 82273, 0x5c6b3433u }, { 3, 12, 82196, 0xce1ef842u },
            { 3, 13, 82203, 0x75d59277u }, { 3, 14, 82200, 0xcccab3e5u }, { 3, 15, 82208, 0x492a6348u },
            { 3, 16, 64346, 0x2c10a426u }, { 3, 17, 64346, 0x2c10a426u }, { 3, 18, 64528, 0x4688e29au }, { 3, 19, 64528, 0x4688e29au },
            { 6, 13, 42025, 0xce9fe149u },
        };
        int differ = 0;
        /* Twice: on the build the processor gets (with BMI2 where it has it) and on the plain one -
         * the same C compiled twice must give the same bytes. */
        for (int plain = 0; plain < 2; ++plain) {
        proven_zstd_encode_force_plain_(plain != 0);
        printf("  the %s build of the block coder:\n", plain ? "plain" : "processor's");
        for (proven_size_t i = 0; i < sizeof PIN / sizeof PIN[0]; ++i) {
            const proven_size_t n = corpus(PIN[i].input, g_in);
            proven_size_t made = 0;
            proven_zstd_encoder_options_t o = { .alloc = g_heap, .level = PIN[i].level, .checksum = true };
            (void)proven_zstd_encode_all(&o, (proven_mem_view_t){ g_in, n }, (proven_mem_mut_t){ g_enc, sizeof g_enc }, &made);
            const proven_u32 crc = proven_crc32((proven_mem_view_t){ g_enc, made });
            printf("  input %d level %d: %zu bytes, CRC-32 %08lx\n", PIN[i].input, PIN[i].level, made, (unsigned long)crc);
            if (made != PIN[i].size || crc != PIN[i].crc) differ++;
        }
        /* Streamed with a 64 KiB window, so that the input buffer slides, by an encoder that
         * made a frame of the same input before: what its reset clears and what its slides move
         * - the hash chain of levels 4 to 9 among them - are in the bytes. */
        static const struct { int input, level; proven_size_t size; proven_u32 crc; } SPIN[] = {
            { 4, 3, 144686, 0x39bbe145u }, { 3, 3, 94238, 0x14701dceu }, { 3, 4, 91302, 0x9ee82c05u }, { 3, 6, 87870, 0x4f3e8d3fu }, { 3, 8, 83613, 0x688406a3u },
            { 3, 13, 82106, 0x552de691u },
            { 3, 16, 63363, 0x4fbebaa1u },
        };
        for (proven_size_t i = 0; i < sizeof SPIN / sizeof SPIN[0]; ++i) {
            proven_zstd_encoder_t *z = NULL;
            PROVEN_TEST_ASSERT(proven_zstd_encoder_create(&(proven_zstd_encoder_options_t){ .alloc = g_heap, .level = SPIN[i].level, .window_log = 16, .checksum = true }, &z) == PROVEN_OK, "an encoder", "");
            proven_size_t made = 0, n = corpus(SPIN[i].input, g_in);
            (void)encode_steps(z, g_in, n, 100000, (proven_size_t)-1, false, g_enc, sizeof g_enc, &made);
            proven_zstd_encoder_reset(z);
            n = corpus(SPIN[i].input, g_in);
            (void)encode_steps(z, g_in, n, 100000, (proven_size_t)-1, false, g_enc, sizeof g_enc, &made);
            proven_zstd_encoder_reset(z);
            const proven_u32 crc = proven_crc32((proven_mem_view_t){ g_enc, made });
            printf("  input %d streamed, level %d, window 64 KiB, after a reset: %zu bytes, CRC-32 %08lx\n", SPIN[i].input, SPIN[i].level, made, (unsigned long)crc);
            if (made != SPIN[i].size || crc != SPIN[i].crc) differ++;
            proven_zstd_encoder_destroy(z);
        }
        }
        proven_zstd_encode_force_plain_(false);
        PROVEN_TEST_ASSERT(differ == 0, "the frames are what they were when pinned, from both builds of the block coder", "Check block_plain and block_bmi2 in src/proven/zstd_encode.c: the same C, so a difference is the compiler's.");
    }

    PROVEN_TEST_SECTION("what every function refuses", "Arguments that cannot be right.", "");
    {
        proven_zstd_encoder_t *z = NULL;
        proven_size_t c = 0, p = 0, w = 0;
        bool done = false;
        PROVEN_TEST_ASSERT(proven_zstd_encoder_create(&(proven_zstd_encoder_options_t){ .alloc = g_heap }, NULL) == PROVEN_ERR_INVALID_ARG &&
                           proven_zstd_encoder_create(NULL, &z) == PROVEN_ERR_INVALID_ARG && z == NULL &&
                           proven_zstd_encoder_create(&(proven_zstd_encoder_options_t){ 0 }, &z) == PROVEN_ERR_INVALID_ARG &&
                           proven_zstd_encoder_create(&(proven_zstd_encoder_options_t){ .alloc = g_heap, .level = 20 }, &z) == PROVEN_ERR_INVALID_ARG &&
                           proven_zstd_encoder_create(&(proven_zstd_encoder_options_t){ .alloc = g_heap, .level = -1 }, &z) == PROVEN_ERR_INVALID_ARG &&
                           proven_zstd_encoder_create(&(proven_zstd_encoder_options_t){ .alloc = g_heap, .window_log = 9 }, &z) == PROVEN_ERR_INVALID_ARG &&
                           proven_zstd_encoder_create(&(proven_zstd_encoder_options_t){ .alloc = g_heap, .window_log = 24 }, &z) == PROVEN_ERR_INVALID_ARG && z == NULL,
                           "no out pointer, no options, no allocator, a level outside 0 to 19 and a window outside 10 to 23 are PROVEN_ERR_INVALID_ARG", "");
        PROVEN_TEST_ASSERT(proven_zstd_encoder_create(&(proven_zstd_encoder_options_t){ .alloc = g_heap, .window_log = 23 }, &z) == PROVEN_OK, "8 MiB is a window", "");
        PROVEN_TEST_ASSERT(proven_zstd_encode(z, (proven_mem_view_t){ NULL, 5 }, &c, (proven_mem_mut_t){ g_enc, 10 }, &p, PROVEN_ZSTD_FLUSH_NONE, &done) == PROVEN_ERR_INVALID_ARG &&
                           proven_zstd_encode(z, (proven_mem_view_t){ g_in, 5 }, NULL, (proven_mem_mut_t){ g_enc, 10 }, &p, PROVEN_ZSTD_FLUSH_NONE, &done) == PROVEN_ERR_INVALID_ARG &&
                           proven_zstd_encode(z, (proven_mem_view_t){ g_in, 5 }, &c, (proven_mem_mut_t){ NULL, 10 }, &p, PROVEN_ZSTD_FLUSH_NONE, &done) == PROVEN_ERR_INVALID_ARG &&
                           proven_zstd_encode(z, (proven_mem_view_t){ g_in, 5 }, &c, (proven_mem_mut_t){ g_enc, 10 }, NULL, PROVEN_ZSTD_FLUSH_NONE, &done) == PROVEN_ERR_INVALID_ARG &&
                           proven_zstd_encode(z, (proven_mem_view_t){ g_in, 5 }, &c, (proven_mem_mut_t){ g_enc, 10 }, &p, PROVEN_ZSTD_FLUSH_NONE, NULL) == PROVEN_ERR_INVALID_ARG &&
                           proven_zstd_encode(z, (proven_mem_view_t){ g_in, 5 }, &c, (proven_mem_mut_t){ g_enc, 10 }, &p, (proven_zstd_flush_t)3, &done) == PROVEN_ERR_INVALID_ARG &&
                           proven_zstd_encode(NULL, (proven_mem_view_t){ g_in, 5 }, &c, (proven_mem_mut_t){ g_enc, 10 }, &p, PROVEN_ZSTD_FLUSH_NONE, &done) == PROVEN_ERR_INVALID_ARG,
                           "null buffers with a length, missing out-parameters, an unknown flush and no encoder are PROVEN_ERR_INVALID_ARG", "");
        PROVEN_TEST_ASSERT(proven_zstd_encode(z, (proven_mem_view_t){ NULL, 0 }, &c, (proven_mem_mut_t){ NULL, 0 }, &p, PROVEN_ZSTD_FLUSH_FINISH, &done) == PROVEN_OK && c == 0 && p == 0 && !done,
                           "nothing in and nowhere to put anything is PROVEN_OK with no progress, and not done", "");
        PROVEN_TEST_ASSERT(proven_zstd_encode_all(&(proven_zstd_encoder_options_t){ .alloc = g_heap }, (proven_mem_view_t){ g_in, 5 }, (proven_mem_mut_t){ g_enc, 10 }, NULL) == PROVEN_ERR_INVALID_ARG &&
                           proven_zstd_encode_all(&(proven_zstd_encoder_options_t){ .alloc = g_heap }, (proven_mem_view_t){ NULL, 5 }, (proven_mem_mut_t){ g_enc, 10 }, &w) == PROVEN_ERR_INVALID_ARG &&
                           proven_zstd_encode_all(NULL, (proven_mem_view_t){ g_in, 5 }, (proven_mem_mut_t){ g_enc, 10 }, &w) == PROVEN_ERR_INVALID_ARG,
                           "and encode_all without its out-parameter, with a null buffer that has a length, or without options", "");
        proven_zstd_encoder_destroy(z);
        proven_zstd_encoder_destroy(NULL);
        proven_zstd_encoder_reset(NULL);
    }

    PROVEN_TEST_PASS("Zstandard: compresses into frames that decode to every byte, within the bound, the window and the memory stated.");
    return 0;
}
