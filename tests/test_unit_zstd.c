#include "proven.h"
#include "proven_test.h"
#include "test_unit_zstd_vectors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Zstandard decompression.
 *
 * test_unit_zstd_vectors.h was made outside this library, with Node's libzstd:
 *
 *   Z item level crc32 length hex   a stream libzstd made from corpus item `item`
 *   O crc32 length hex              a hand-built stream that is odd, and that libzstd accepts,
 *                                   with the CRC-32 and length of what it decodes to
 *   X verdict hex                   a hand-built stream that libzstd refuses (I: invalid;
 *                                   U: it needs a dictionary; B: its window is above 8 MiB), or
 *                                   one RFC 8878 does not allow and that only libzstd's
 *                                   streaming decoder accepts (S); every one is refused here
 *
 * The corpus is not in the header. corpus() below builds the same bytes from the same rules
 * as the generator, and each Z line carries their CRC-32 and length to prove it.
 */

static proven_allocator_t g_heap;
static proven_byte_t g_plain[200000], g_in[120000], g_out[220000], g_out2[220000], g_cat[150000];

static proven_u32 g_x;
static proven_u32 xs(void) { g_x ^= g_x << 13; g_x ^= g_x >> 17; g_x ^= g_x << 5; return g_x; }

static proven_size_t corpus(int i, proven_byte_t *out) {
    proven_size_t n = 0;
    switch (i) {
        case 0: return 0;
        case 1: out[0] = 'a'; return 1;
        case 2: memset(out, 'a', 1000); return 1000;
        case 3: for (n = 0; n < 4000; ++n) out[n] = n % 2 ? 'b' : 'a'; return n;
        case 4: g_x = 1; for (n = 0; n < 1200; ++n) out[n] = (proven_byte_t)xs(); return n;
        case 5: {
            /* Numbered lines: Huffman-coded literals and short matches. */
            g_x = 5;
            char line[96];
            for (unsigned k = 0; n < 16000; ++k) {
                const unsigned a = xs() % 1000, b = xs() % 50;
                const int len = snprintf(line, sizeof line, "line %u says %u and %u\n", k, a, b);
                for (int j = 0; j < len && n < 16000; ++j) out[n++] = (proven_byte_t)line[j];
            }
            return n; }
        case 6: {
            /* Runs of noise and copies from up to 180,000 bytes back: matches across blocks. */
            g_x = 6;
            while (n < 200000) {
                const proven_u32 r = xs();
                if (n < 1000 || (r & 7) == 0) {
                    proven_u32 k = 1 + xs() % 8;
                    while (k-- > 0 && n < 200000) out[n++] = (proven_byte_t)xs();
                } else {
                    const proven_u32 len = 4 + xs() % 300, lim = n < 180000 ? (proven_u32)n : 180000u, d = 1 + xs() % lim;
                    for (proven_u32 k = 0; k < len && n < 200000; ++k, ++n) out[n] = out[n - d];
                }
            }
            return n; }
        default: return 0;
    }
}

static proven_size_t unhex(const char *s, proven_size_t n, proven_byte_t *out) {
    for (proven_size_t i = 0; i + 1 < n; i += 2) {
        unsigned v = 0;
        for (int k = 0; k < 2; ++k) { char c = s[i + (proven_size_t)k]; v = v * 16 + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10); }
        out[i / 2] = (proven_byte_t)v;
    }
    return n / 2;
}

static const char *field(const char **at, proven_size_t *len) {
    const char *p = *at;
    while (*p == ' ') ++p;
    const char *start = p;
    while (*p && *p != ' ') ++p;
    *len = (proven_size_t)(p - start);
    *at = p;
    return start;
}

/* An allocator that counts: the bytes held now and at most, and a failure on demand. */
typedef struct {
    proven_size_t live, peak, calls, fail_at;      /* fail_at: the call that fails, 0: none */
    void *ptr[8];
    proven_size_t size[8];
} counter_t;
static counter_t g_count;

static void count_set(void *p, proven_size_t n) {
    for (int i = 0; i < 8; ++i) if (!g_count.ptr[i]) { g_count.ptr[i] = p; g_count.size[i] = n; break; }
    g_count.live += n;
    if (g_count.live > g_count.peak) g_count.peak = g_count.live;
}
static void count_drop(void *p) {
    for (int i = 0; i < 8; ++i) if (g_count.ptr[i] == p) { g_count.live -= g_count.size[i]; g_count.ptr[i] = NULL; break; }
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

/* Decode `in` with at most `in_step` bytes of input and `out_step` of output space per call,
 * going on past each frame's end while there is input. */
static proven_err_t decode_steps(proven_allocator_t alloc, proven_u64 window_max, const proven_byte_t *in, proven_size_t in_len, proven_size_t in_step,
                                 proven_size_t out_step, proven_byte_t *out, proven_size_t out_cap, proven_size_t *made, bool *ended) {
    proven_zstd_decoder_t *z = NULL;
    *made = 0;
    *ended = false;
    proven_err_t e = proven_zstd_decoder_create(alloc, window_max, &z);
    if (e != PROVEN_OK) return e;
    proven_size_t used = 0;
    for (long guard = 0; guard < 4000000; ++guard) {
        const proven_size_t give = in_len - used < in_step ? in_len - used : in_step, room = out_cap - *made < out_step ? out_cap - *made : out_step;
        proven_size_t c = 0, p = 0;
        e = proven_zstd_decode(z, (proven_mem_view_t){ in + used, give }, &c, (proven_mem_mut_t){ out + *made, room }, &p, ended);
        used += c;
        *made += p;
        if (e != PROVEN_OK) break;
        if (*ended && used == in_len) break;
        if (c == 0 && p == 0 && (used == in_len || *made == out_cap)) break;       /* it wants what there is no more of */
    }
    proven_zstd_decoder_destroy(z);
    return e;
}

/* The Z lines' streams, by item and level, for the sections after the first. */
static const char *zline(int item, int level, int nth) {
    char want[16];
    snprintf(want, sizeof want, "Z %d %d ", item, level);
    for (proven_size_t i = 0; i < sizeof ZSTDV / sizeof ZSTDV[0]; ++i)
        if (strncmp(ZSTDV[i], want, strlen(want)) == 0 && nth-- == 0) return ZSTDV[i];
    return NULL;
}
static proven_size_t zstream(const char *line, proven_byte_t *out) {
    const char *at = line + 1;
    proven_size_t n;
    for (int k = 0; k < 4; ++k) (void)field(&at, &n);
    const char *h = field(&at, &n);
    return unhex(h, n, out);
}

int main(void) {
    PROVEN_TEST_SUITE("Zstandard decompression",
        "Decompress what libzstd made and refuse what libzstd refuses; stay inside the window limit and the output given; behave on input cut short, changed, or fed a byte at a time.",
        "Inspect src/proven/zstd.c. For a vector, the letter and line number printed name it; tests/test_unit_zstd_vectors.h is written by a private generator that checks libzstd's verdict on every line.");
    g_heap = proven_heap_allocator();

    PROVEN_TEST_SECTION("streams libzstd made, and hand-built ones", "Every line: decompressed in one call, a byte in and a byte out at a time, and in odd steps.", "Check proven_zstd_decode's state machine: the stepped forms stop and resume in every state.");
    {
        int failed = 0, counts[3] = { 0 };
        for (proven_size_t i = 0; i < sizeof ZSTDV / sizeof ZSTDV[0]; ++i) {
            const char *at = ZSTDV[i];
            const char kind = at[0];
            proven_size_t n1, n2, n3, n4, n5;
            bool ok = false;
            at += 1;
            if (kind == 'Z') {
                const char *f1 = field(&at, &n1);
                (void)field(&at, &n2);
                const char *f3 = field(&at, &n3), *f4 = field(&at, &n4), *f5 = field(&at, &n5);
                const proven_size_t plain_len = corpus(atoi(f1), g_plain);
                const proven_u32 want_crc = (proven_u32)strtoul(f3, NULL, 16);
                const proven_size_t want_len = (proven_size_t)strtoul(f4, NULL, 10), comp_len = unhex(f5, n5, g_in);
                proven_size_t made = 0;
                ok = plain_len == want_len && proven_crc32((proven_mem_view_t){ g_plain, plain_len }) == want_crc &&
                     proven_zstd_decode_all(g_heap, (proven_mem_view_t){ g_in, comp_len }, (proven_mem_mut_t){ g_out, sizeof g_out }, &made) == PROVEN_OK &&
                     made == want_len && memcmp(g_out, g_plain, made) == 0;
                static const proven_size_t steps[][2] = { { 1, 1 }, { 7, 13 }, { 4096, 1000 }, { 70000, 65536 } };
                for (int s = 0; ok && s < 4; ++s) {
                    if (s == 0 && comp_len > 5000) continue;            /* a byte at a time: the short streams */
                    bool ended = false;
                    ok = decode_steps(g_heap, 0, g_in, comp_len, steps[s][0], steps[s][1], g_out2, sizeof g_out2, &made, &ended) == PROVEN_OK &&
                         ended && made == want_len && memcmp(g_out2, g_plain, made) == 0;
                }
                counts[0]++;
            } else if (kind == 'O') {
                const char *f1 = field(&at, &n1), *f2 = field(&at, &n2), *f3 = field(&at, &n3);
                const proven_u32 want_crc = (proven_u32)strtoul(f1, NULL, 16);
                const proven_size_t want_len = (proven_size_t)strtoul(f2, NULL, 10), comp_len = unhex(f3, n3, g_in);
                proven_size_t made = 0, made2 = 0;
                bool ended = false;
                ok = proven_zstd_decode_all(g_heap, (proven_mem_view_t){ g_in, comp_len }, (proven_mem_mut_t){ g_out, sizeof g_out }, &made) == PROVEN_OK && made == want_len &&
                     proven_crc32((proven_mem_view_t){ g_out, made }) == want_crc &&
                     decode_steps(g_heap, 0, g_in, comp_len, 1, 1, g_out2, sizeof g_out2, &made2, &ended) == PROVEN_OK && ended && made2 == made && memcmp(g_out, g_out2, made) == 0;
                counts[1]++;
            } else {
                const char *f1 = field(&at, &n1), *f2 = field(&at, &n2);
                const proven_size_t comp_len = unhex(f2, n2, g_in);
                const proven_err_t want = f1[0] == 'U' ? PROVEN_ERR_UNSUPPORTED : f1[0] == 'B' ? PROVEN_ERR_OUT_OF_BOUNDS : PROVEN_ERR_INVALID_FORMAT;
                proven_size_t made = 0;
                bool ended = false;
                ok = proven_zstd_decode_all(g_heap, (proven_mem_view_t){ g_in, comp_len }, (proven_mem_mut_t){ g_out, sizeof g_out }, &made) == want && made == 0 &&
                     decode_steps(g_heap, 0, g_in, comp_len, 1, 1, g_out2, sizeof g_out2, &made, &ended) == want;
                counts[2]++;
            }
            if (!ok) { failed++; printf("vector %u (%c) differs\n", (unsigned)i, kind); }
        }
        PROVEN_TEST_ASSERT(failed == 0, "every stream libzstd made decodes to its corpus item, whole and in steps; every hand-built stream gets libzstd's verdict", "The vector's index and letter are printed above.");
        PROVEN_TEST_ASSERT(counts[0] == 91 && counts[1] == 13 && counts[2] == 20, "91 streams libzstd made, 13 odd ones it accepts, 20 it refuses (one of them only its streaming decoder accepts)", "");
    }

    PROVEN_TEST_SECTION("damage", "Every prefix and every single-bit change of the short streams: refused, or decoded within the output given - and the same whole as in steps.", "A sanitizer report here is an out-of-bounds read or write in src/proven/zstd.c.");
    {
        int streams = 0, prefixes_refused = 0, prefixes = 0, mismatches = 0;
        long changed = 0, refused = 0;
        for (proven_size_t i = 0; i < sizeof ZSTDV / sizeof ZSTDV[0]; ++i) {
            const char *at = ZSTDV[i];
            if (at[0] == 'X') continue;
            proven_size_t n;
            const char *h = NULL;
            at += 1;
            for (int k = 0; k < (ZSTDV[i][0] == 'Z' ? 5 : 3); ++k) h = field(&at, &n);
            const proven_size_t len = unhex(h, n, g_in);
            if (len > 600) continue;
            streams++;
            for (proven_size_t cut = 0; cut < len; ++cut) {
                proven_size_t made = 0;
                prefixes++;
                if (proven_zstd_decode_all(g_heap, (proven_mem_view_t){ g_in, cut }, (proven_mem_mut_t){ g_out, sizeof g_out }, &made) == PROVEN_ERR_INVALID_FORMAT) prefixes_refused++;
            }
            for (proven_size_t bit = 0; bit < 8 * len; ++bit) {
                memcpy(g_cat, g_in, len);
                g_cat[bit / 8] ^= (proven_byte_t)(1u << (bit % 8));
                proven_size_t made = 0, made2 = 0;
                bool ended = false;
                const proven_err_t e1 = proven_zstd_decode_all(g_heap, (proven_mem_view_t){ g_cat, len }, (proven_mem_mut_t){ g_out, 70000 }, &made);
                const proven_err_t e2 = decode_steps(g_heap, 0, g_cat, len, 3, 5, g_out2, 70000, &made2, &ended);
                changed++;
                if (e1 != PROVEN_OK) refused++;
                if ((e1 == PROVEN_OK) != (e2 == PROVEN_OK && ended) || (e1 == PROVEN_OK && (made != made2 || memcmp(g_out, g_out2, made) != 0))) mismatches++;
            }
        }
        printf("  %d streams: %d prefixes, %ld changed bits of which %ld refused, %d differ in steps\n", streams, prefixes, changed, refused, mismatches);
        PROVEN_TEST_ASSERT(streams > 50 && prefixes_refused == prefixes, "every proper prefix of a one-frame stream is PROVEN_ERR_INVALID_FORMAT", "Check where proven_zstd_decode_all tells input that ended inside a frame from output that has no room.");
        PROVEN_TEST_ASSERT(mismatches == 0 && changed > 10000 && refused > 0, "every single-bit change is refused or decoded within the output given, and the same whole as in steps", "");
    }

    PROVEN_TEST_SECTION("several frames", "Frames that follow each other, with a skippable one between: each call stops at a frame's end, and the next goes on.", "Check the boundary in proven_zstd_decode: Z_MAGIC with `boundary` set.");
    {
        /* The first frame has a checksum, so that it ends where the checksum is read. */
        proven_size_t n = 0, a = zstream(zline(5, 3, 1), g_cat), b;
        n = a;
        const proven_byte_t skip[] = { 0x5e, 0x2a, 0x4d, 0x18, 3, 0, 0, 0, 'a', 'b', 'c' };
        memcpy(g_cat + n, skip, sizeof skip);
        n += sizeof skip;
        b = zstream(zline(4, 19, 3), g_cat + n);
        n += b;
        proven_zstd_decoder_t *z = NULL;
        PROVEN_TEST_ASSERT(proven_zstd_decoder_create(g_heap, 0, &z) == PROVEN_OK, "a decoder", "");
        proven_size_t used = 0, made = 0, c = 0, p = 0;
        bool done = false;
        PROVEN_TEST_ASSERT(proven_zstd_decode(z, (proven_mem_view_t){ g_cat, n }, &c, (proven_mem_mut_t){ g_out, sizeof g_out }, &p, &done) == PROVEN_OK && done && c == a && p == 16000,
            "the first call stops at the end of the first frame: done, and what it consumed is that frame", "");
        used += c; made += p;
        PROVEN_TEST_ASSERT(proven_zstd_decode(z, (proven_mem_view_t){ g_cat + used, n - used }, &c, (proven_mem_mut_t){ g_out + made, sizeof g_out - made }, &p, &done) == PROVEN_OK && done &&
                           c == sizeof skip && p == 0, "the next reads past the skippable frame, producing nothing, and is done again", "");
        used += c; made += p;
        PROVEN_TEST_ASSERT(proven_zstd_decode(z, (proven_mem_view_t){ g_cat + used, n - used }, &c, (proven_mem_mut_t){ g_out + made, sizeof g_out - made }, &p, &done) == PROVEN_OK && done &&
                           c == b && p == 1200, "and the third reads the last frame, with no reset between", "");
        used += c; made += p;
        PROVEN_TEST_ASSERT(proven_zstd_decode(z, (proven_mem_view_t){ NULL, 0 }, &c, (proven_mem_mut_t){ NULL, 0 }, &p, &done) == PROVEN_OK && done && c == 0 && p == 0,
            "with nothing more, it stays done", "");
        corpus(5, g_plain);
        corpus(4, g_plain + 16000);
        PROVEN_TEST_ASSERT(made == 17200 && memcmp(g_out, g_plain, made) == 0, "what came out is the two items in order", "");
        const proven_byte_t junk[] = { 'j', 'u', 'n', 'k' };
        PROVEN_TEST_ASSERT(proven_zstd_decode(z, (proven_mem_view_t){ junk, 4 }, &c, (proven_mem_mut_t){ g_out, 10 }, &p, &done) == PROVEN_ERR_INVALID_FORMAT && !done &&
                           proven_zstd_decode(z, (proven_mem_view_t){ g_cat, n }, &c, (proven_mem_mut_t){ g_out, 10 }, &p, &done) == PROVEN_ERR_INVALID_FORMAT,
            "bytes after a frame that begin no frame are PROVEN_ERR_INVALID_FORMAT, and so is every call after it", "");
        proven_zstd_decoder_reset(z);
        PROVEN_TEST_ASSERT(proven_zstd_decode(z, (proven_mem_view_t){ g_cat, a }, &c, (proven_mem_mut_t){ g_out, sizeof g_out }, &p, &done) == PROVEN_OK && done && p == 16000,
            "until a reset", "");
        proven_zstd_decoder_destroy(z);
        proven_size_t w = 0;
        PROVEN_TEST_ASSERT(proven_zstd_decode_all(g_heap, (proven_mem_view_t){ g_cat, n }, (proven_mem_mut_t){ g_out, sizeof g_out }, &w) == PROVEN_OK && w == 17200 && memcmp(g_out, g_plain, w) == 0,
            "decode_all reads every frame", "");
        PROVEN_TEST_ASSERT(proven_zstd_decode_all(g_heap, (proven_mem_view_t){ skip, sizeof skip }, (proven_mem_mut_t){ g_out, 10 }, &w) == PROVEN_OK && w == 0 &&
                           proven_zstd_decode_all(g_heap, (proven_mem_view_t){ NULL, 0 }, (proven_mem_mut_t){ g_out, 10 }, &w) == PROVEN_ERR_INVALID_FORMAT,
            "a skippable frame alone is an empty stream; no bytes at all are not a stream", "");
    }

    PROVEN_TEST_SECTION("bounds", "The window limit, the output given, and the memory a decoder holds.", "Check frame_header (the limit, before anything is allocated), make_room and grow in src/proven/zstd.c.");
    {
        const proven_size_t big = zstream(zline(6, 1, 0), g_in);
        proven_size_t made = 0;
        bool ended = false;
        proven_err_t e = decode_steps(counting(0), 64 * 1024, g_in, big, big, sizeof g_out, g_out, sizeof g_out, &made, &ended);
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_OUT_OF_BOUNDS && made == 0 && g_count.calls == 1, "a frame whose window is above the decoder's limit is PROVEN_ERR_OUT_OF_BOUNDS, with nothing allocated past the decoder itself", "");
        e = decode_steps(counting(0), 0, g_in, big, 4096, 4096, g_out, sizeof g_out, &made, &ended);
        corpus(6, g_plain);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && ended && made == 200000 && memcmp(g_out, g_plain, made) == 0, "the same frame decodes with the default limit", "");
        PROVEN_TEST_ASSERT(g_count.peak <= 256 * 1024 + 3 * 128 * 1024 + 32 * 1024 && g_count.live == 0, "holding at most its window (here 256 KiB) and 384 KiB, plus its tables; and nothing after", "");
        printf("  a 200,000-byte frame with a 256 KiB window: at most %u bytes held\n", (unsigned)g_count.peak);
        /* The same data with a window of 64 KiB: more than the window and a block, so the oldest
         * history has to go as the frame is decoded. */
        const proven_size_t slid = zstream(zline(6, 3, 0), g_in);
        PROVEN_TEST_ASSERT(g_in[5] == (16 - 10) << 3, "the frame's window is 64 KiB", "");
        e = decode_steps(counting(0), 0, g_in, slid, 3000, 5000, g_out, sizeof g_out, &made, &ended);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && ended && made == 200000 && memcmp(g_out, g_plain, made) == 0 && g_count.peak <= 64 * 1024 + 3 * 128 * 1024 + 32 * 1024,
            "a frame longer than its window and a block decodes, holding at most that window and 384 KiB", "Check the slide in make_room: it keeps the last window's worth of output.");
        printf("  the same with a 64 KiB window: at most %u bytes held\n", (unsigned)g_count.peak);
        const proven_size_t small = zstream(zline(1, 3, 3), g_in);
        e = decode_steps(counting(0), 0, g_in, small, small, 10, g_out, sizeof g_out, &made, &ended);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && made == 1 && g_count.peak < 32 * 1024, "a frame of one byte costs the decoder and little more", "");

        proven_size_t w = 0;
        const proven_size_t text = zstream(zline(5, 9, 1), g_in);
        PROVEN_TEST_ASSERT(proven_zstd_decode_all(g_heap, (proven_mem_view_t){ g_in, text }, (proven_mem_mut_t){ g_out, 15999 }, &w) == PROVEN_ERR_OUT_OF_BOUNDS && w == 0 &&
                           proven_zstd_decode_all(g_heap, (proven_mem_view_t){ g_in, text }, (proven_mem_mut_t){ g_out, 16000 }, &w) == PROVEN_OK && w == 16000,
            "decode_all into one byte too few is PROVEN_ERR_OUT_OF_BOUNDS; into exactly enough, PROVEN_OK", "");
        g_out[100] = 0x5a;
        PROVEN_TEST_ASSERT(decode_steps(g_heap, 0, g_in, text, text, 100, g_out, 100, &made, &ended) == PROVEN_OK && made == 100 && !ended && g_out[100] == 0x5a,
            "a step writes only the bytes it reports produced", "");

        int nomem = 0, other = 0;
        for (proven_size_t k = 1; k <= 6; ++k) {
            e = decode_steps(counting(k), 0, g_in, text, 100, 1000, g_out, sizeof g_out, &made, &ended);
            if (e == PROVEN_ERR_NOMEM) nomem++;
            else if (e != PROVEN_OK) other++;
            PROVEN_TEST_ASSERT(g_count.live == 0, "nothing is left held after an allocation fails", "");
        }
        PROVEN_TEST_ASSERT(nomem >= 3 && other == 0, "each allocation that fails is PROVEN_ERR_NOMEM", "");
    }

    PROVEN_TEST_SECTION("what every function refuses", "Arguments that cannot be right.", "");
    {
        proven_zstd_decoder_t *z = NULL;
        proven_size_t c = 0, p = 0, w = 0;
        bool done = false;
        PROVEN_TEST_ASSERT(proven_zstd_decoder_create(g_heap, 0, NULL) == PROVEN_ERR_INVALID_ARG &&
                           proven_zstd_decoder_create(g_heap, 1023, &z) == PROVEN_ERR_INVALID_ARG && z == NULL &&
                           proven_zstd_decoder_create(g_heap, ((proven_u64)1 << 31) + 1, &z) == PROVEN_ERR_INVALID_ARG && z == NULL &&
                           proven_zstd_decoder_create((proven_allocator_t){ 0 }, 0, &z) == PROVEN_ERR_INVALID_ARG,
            "no out pointer, a limit under 1 KiB or over 2 GiB, and no allocator are PROVEN_ERR_INVALID_ARG", "");
        PROVEN_TEST_ASSERT(proven_zstd_decoder_create(g_heap, 1024, &z) == PROVEN_OK, "1 KiB is a limit", "");
        PROVEN_TEST_ASSERT(proven_zstd_decode(z, (proven_mem_view_t){ NULL, 5 }, &c, (proven_mem_mut_t){ g_out, 10 }, &p, &done) == PROVEN_ERR_INVALID_ARG &&
                           proven_zstd_decode(z, (proven_mem_view_t){ g_in, 5 }, NULL, (proven_mem_mut_t){ g_out, 10 }, &p, &done) == PROVEN_ERR_INVALID_ARG &&
                           proven_zstd_decode(z, (proven_mem_view_t){ g_in, 5 }, &c, (proven_mem_mut_t){ NULL, 10 }, &p, &done) == PROVEN_ERR_INVALID_ARG &&
                           proven_zstd_decode(z, (proven_mem_view_t){ g_in, 5 }, &c, (proven_mem_mut_t){ g_out, 10 }, NULL, &done) == PROVEN_ERR_INVALID_ARG &&
                           proven_zstd_decode(z, (proven_mem_view_t){ g_in, 5 }, &c, (proven_mem_mut_t){ g_out, 10 }, &p, NULL) == PROVEN_ERR_INVALID_ARG &&
                           proven_zstd_decode(NULL, (proven_mem_view_t){ g_in, 5 }, &c, (proven_mem_mut_t){ g_out, 10 }, &p, &done) == PROVEN_ERR_INVALID_ARG,
            "null buffers with a length, missing out-parameters and no decoder are PROVEN_ERR_INVALID_ARG", "");
        PROVEN_TEST_ASSERT(proven_zstd_decode(z, (proven_mem_view_t){ NULL, 0 }, &c, (proven_mem_mut_t){ NULL, 0 }, &p, &done) == PROVEN_OK && c == 0 && p == 0 && !done,
            "nothing in and nowhere to put anything is PROVEN_OK with no progress, and not done", "");
        PROVEN_TEST_ASSERT(proven_zstd_decode_all(g_heap, (proven_mem_view_t){ g_in, 5 }, (proven_mem_mut_t){ g_out, 10 }, NULL) == PROVEN_ERR_INVALID_ARG &&
                           proven_zstd_decode_all(g_heap, (proven_mem_view_t){ NULL, 5 }, (proven_mem_mut_t){ g_out, 10 }, &w) == PROVEN_ERR_INVALID_ARG,
            "and decode_all without its out-parameter, or with a null buffer that has a length", "");
        proven_zstd_decoder_destroy(z);
        proven_zstd_decoder_destroy(NULL);
        proven_zstd_decoder_reset(NULL);
    }

    PROVEN_TEST_PASS("Zstandard: reads what libzstd writes, refuses what libzstd refuses, and stays inside its bounds.");
    return 0;
}
