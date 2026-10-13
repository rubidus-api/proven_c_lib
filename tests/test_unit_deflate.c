#include "proven.h"
#include "proven_test.h"
#include "test_unit_deflate_vectors.h"
#include "../src/proven/deflate_tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * DEFLATE both ways, bare and framed as zlib and gzip.
 *
 * test_unit_deflate_vectors.h was made outside this library, with zlib:
 *
 *   Z format corpus crc32 length hex   a stream zlib made from corpus item `corpus`
 *   O format crc32 length hex          a hand-built stream that is odd, and that zlib accepts
 *   X format error hex                 a hand-built stream that zlib refuses (I: invalid;
 *                                      U: it wants a preset dictionary, which is unsupported)
 *
 * The corpus is not in the header. corpus() below builds the same bytes from the same rules
 * as the generator, and each Z line carries their CRC-32 and length to prove it.
 */

static proven_allocator_t g_heap;
static proven_byte_t g_a[80000], g_b[80000], g_c[90000], g_plain[80000];

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
            static proven_byte_t words[64][10];
            static proven_size_t lens[64];
            g_x = 5;
            for (int w = 0; w < 64; ++w) {
                lens[w] = 2 + (xs() & 7);
                for (proven_size_t k = 0; k < lens[w]; ++k) words[w][k] = (proven_byte_t)(97 + (xs() & 15));
            }
            while (n < 5000) {
                const int w = (int)(xs() & 63);
                for (proven_size_t k = 0; k < lens[w]; ++k) out[n++] = words[w][k];
                out[n++] = ' ';
            }
            return 5000; }
        case 6: memset(out, 'x', 517); out[258] = 'y'; return 517;
        case 7: for (n = 0; n < 1536; ++n) out[n] = (proven_byte_t)n; return n;
        case 8: memset(out, 0, 70000); return 70000;
        case 9: g_x = 9; for (n = 0; n < 6000; ++n) out[n] = (proven_byte_t)(65 + (xs() & 1)); return n;
        case 10: {
            static const char run[] = "0123456789abcdef";
            g_x = 10;
            for (n = 0; n < 320; ++n) out[n] = (proven_byte_t)run[n % 16];
            for (int k = 0; k < 700; ++k) out[n++] = (proven_byte_t)xs();
            for (int k = 0; k < 320; ++k) out[n++] = (proven_byte_t)run[k % 16];
            return n; }
        default: return 0;
    }
}
#define NCORPUS 11

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

/* Inflate `in` with at most `in_step` bytes of input and `out_step` of output space per call.
 * Returns the error; *made and *used say how far it got; *ended whether the stream said so. */
static proven_err_t inflate_steps(proven_deflate_format_t format, const proven_byte_t *in, proven_size_t in_len, proven_size_t in_step, proven_size_t out_step,
                                  proven_byte_t *out, proven_size_t out_cap, proven_size_t *made, proven_size_t *used, bool *ended) {
    proven_inflate_t *z = NULL;
    *made = 0; *used = 0; *ended = false;
    if (proven_inflate_create(g_heap, format, &z) != PROVEN_OK) return PROVEN_ERR_NOMEM;
    proven_err_t e = PROVEN_OK;
    for (long guard = 0; guard < 4000000 && !*ended; ++guard) {
        const proven_size_t give = in_len - *used < in_step ? in_len - *used : in_step, room = out_cap - *made < out_step ? out_cap - *made : out_step;
        proven_size_t c = 0, p = 0;
        e = proven_inflate(z, (proven_mem_view_t){ in + *used, give }, &c, (proven_mem_mut_t){ out + *made, room }, &p, ended);
        *used += c; *made += p;
        if (e != PROVEN_OK) break;
        if (c == 0 && p == 0 && !*ended && (*used == in_len || *made == out_cap)) break;       /* it wants what there is no more of */
    }
    proven_inflate_destroy(z);
    return e;
}

/* A tiny bit writer, for the two streams this test builds by hand. */
typedef struct { proven_byte_t *p; proven_size_t n; unsigned acc, cnt; } bw_t;
static void bw_bits(bw_t *w, unsigned v, unsigned n) { for (unsigned i = 0; i < n; ++i) { w->acc |= ((v >> i) & 1u) << w->cnt; if (++w->cnt == 8) { w->p[w->n++] = (proven_byte_t)w->acc; w->acc = 0; w->cnt = 0; } } }
static void bw_code(bw_t *w, unsigned v, unsigned n) { for (unsigned i = n; i-- > 0;) bw_bits(w, (v >> i) & 1u, 1); }
static void bw_end(bw_t *w) { if (w->cnt) { w->p[w->n++] = (proven_byte_t)w->acc; w->acc = 0; w->cnt = 0; } }

int main(void) {
    PROVEN_TEST_SUITE("DEFLATE, zlib and gzip",
        "Decompress what zlib made and refuse what zlib refuses; compress and get the input back at every level and window; behave on input cut short, changed, or fed a byte at a time.",
        "Inspect src/proven/deflate.c. For a vector, the letter and line number printed name it; tests/test_unit_deflate_vectors.h is written by a private generator that checks zlib's verdict on every hand-built stream.");
    g_heap = proven_heap_allocator();

    PROVEN_TEST_SECTION("the corpus", "The test's own bytes are the generator's.", "Compare corpus() here with the generator's; a Z line's CRC-32 and length say which item differs.");
    PROVEN_TEST_SECTION("streams zlib made", "Every Z line: decompressed in one call, and again a byte in and a byte out at a time.", "Check proven_inflate's state machine: the second form stops and resumes in every state.");
    int counts[128] = { 0 }, failed = 0;
    proven_size_t n1, n2, n3, n4, n5;
    for (proven_size_t i = 0; i < sizeof DEFLATEV / sizeof DEFLATEV[0]; ++i) {
        const char *at = DEFLATEV[i];
        const char kind = at[0];
        bool ok = false;
        at += 1;
        const char *f1 = field(&at, &n1);
        const proven_deflate_format_t format = (proven_deflate_format_t)(f1[0] - '0');
        if (kind == 'Z' || kind == 'O') {
            proven_size_t plain_len = 0;
            if (kind == 'Z') { const char *fc = field(&at, &n2); plain_len = corpus(atoi(fc), g_plain); }
            const char *f3 = field(&at, &n3), *f4 = field(&at, &n4), *f5 = field(&at, &n5);
            const proven_u32 want_crc = (proven_u32)strtoul(f3, NULL, 16);
            const proven_size_t want_len = (proven_size_t)strtoul(f4, NULL, 10), comp_len = unhex(f5, n5, g_a);
            proven_size_t made = 0, used = 0;
            bool ended = false;
            ok = proven_inflate_all(g_heap, format, (proven_mem_view_t){ g_a, comp_len }, (proven_mem_mut_t){ g_b, sizeof g_b }, &made, &used) == PROVEN_OK &&
                 made == want_len && used == comp_len && proven_crc32((proven_mem_view_t){ g_b, made }) == want_crc;
            if (kind == 'Z') ok = ok && plain_len == want_len && memcmp(g_b, g_plain, made) == 0;
            /* One byte at a time each way; then odd sizes that do not line up with anything. */
            ok = ok && inflate_steps(format, g_a, comp_len, 1, 1, g_c, sizeof g_c, &made, &used, &ended) == PROVEN_OK && ended && made == want_len && used == comp_len && memcmp(g_c, g_b, made) == 0;
            ok = ok && inflate_steps(format, g_a, comp_len, 7, 13, g_c, sizeof g_c, &made, &used, &ended) == PROVEN_OK && ended && made == want_len && memcmp(g_c, g_b, made) == 0;
            /* A stream fed in steps into room that is exactly the data's size still says it has ended. */
            ok = ok && inflate_steps(format, g_a, comp_len, 5, want_len ? want_len : 1, g_c, want_len, &made, &used, &ended) == PROVEN_OK && ended && made == want_len && used == comp_len;
            /* Exactly enough room is enough; one byte less is PROVEN_ERR_OUT_OF_BOUNDS. */
            ok = ok && proven_inflate_all(g_heap, format, (proven_mem_view_t){ g_a, comp_len }, (proven_mem_mut_t){ g_c, want_len }, &made, NULL) == PROVEN_OK && made == want_len;
            if (want_len > 0) ok = ok && proven_inflate_all(g_heap, format, (proven_mem_view_t){ g_a, comp_len }, (proven_mem_mut_t){ g_c, want_len - 1 }, &made, NULL) == PROVEN_ERR_OUT_OF_BOUNDS;
            /* Bytes after the end are not taken. */
            g_a[comp_len] = 0x55; g_a[comp_len + 1] = 0xaa;
            ok = ok && proven_inflate_all(g_heap, format, (proven_mem_view_t){ g_a, comp_len + 2 }, (proven_mem_mut_t){ g_c, sizeof g_c }, &made, &used) == PROVEN_OK && used == comp_len;
            /* Cut short by a byte: not a whole stream. */
            if (comp_len > 0) ok = ok && proven_inflate_all(g_heap, format, (proven_mem_view_t){ g_a, comp_len - 1 }, (proven_mem_mut_t){ g_c, sizeof g_c }, &made, NULL) == PROVEN_ERR_INVALID_FORMAT;
            /* And when the room is exactly the data's size, a missing end is still a missing end, not a full buffer. */
            if (format != PROVEN_DEFLATE_RAW) ok = ok && proven_inflate_all(g_heap, format, (proven_mem_view_t){ g_a, comp_len - 1 }, (proven_mem_mut_t){ g_c, want_len }, &made, NULL) == PROVEN_ERR_INVALID_FORMAT;
        } else if (kind == 'X') {
            const char *f2 = field(&at, &n2), *f3 = field(&at, &n3);
            const proven_err_t want = f2[0] == 'U' ? PROVEN_ERR_UNSUPPORTED : PROVEN_ERR_INVALID_FORMAT;
            const proven_size_t comp_len = unhex(f3, n3, g_a);
            proven_size_t made = 0, used = 0;
            bool ended = false;
            ok = proven_inflate_all(g_heap, format, (proven_mem_view_t){ g_a, comp_len }, (proven_mem_mut_t){ g_b, sizeof g_b }, &made, NULL) == want &&
                 inflate_steps(format, g_a, comp_len, 1, 1, g_c, sizeof g_c, &made, &used, &ended) == want && !ended;
        }
        counts[(int)kind]++;
        if (!ok) { failed = 1; fprintf(stderr, "line %u (%c) differs\n", (unsigned)i, kind); }
    }
    PROVEN_TEST_ASSERT(!failed, "every vector gives what zlib gave: the data, in one call and a byte at a time, with trailing bytes left alone and a short buffer or a short stream reported - or the refusal", "");
    PROVEN_TEST_ASSERT(counts['Z'] == 93 && counts['O'] == 10 && counts['X'] == 30, "ninety-three streams made by zlib, ten odd ones it accepts and thirty it refuses", "");

    PROVEN_TEST_SECTION("streams built here", "Corners that are cheaper to build than to store.", "Check the stored-block path and the copy in S_COPY.");
    {
        /* A stored block of the largest size, then one byte more in a second block. */
        proven_size_t n = 0, made = 0;
        g_x = 77;
        for (proven_size_t i = 0; i < 65536; ++i) g_plain[i] = (proven_byte_t)xs();
        g_a[n++] = 0; g_a[n++] = 0xff; g_a[n++] = 0xff; g_a[n++] = 0; g_a[n++] = 0;
        memcpy(g_a + n, g_plain, 65535); n += 65535;
        g_a[n++] = 1; g_a[n++] = 1; g_a[n++] = 0; g_a[n++] = 0xfe; g_a[n++] = 0xff; g_a[n++] = g_plain[65535];
        PROVEN_TEST_ASSERT(proven_inflate_all(g_heap, PROVEN_DEFLATE_RAW, (proven_mem_view_t){ g_a, n }, (proven_mem_mut_t){ g_b, sizeof g_b }, &made, NULL) == PROVEN_OK &&
                           made == 65536 && memcmp(g_b, g_plain, 65536) == 0, "a stored block of 65,535 bytes and one more byte after it", "");
        /* 32,768 bytes, then a match of 258 at distance 32,768 - the furthest and the longest. */
        bw_t w = { g_a, 0, 0, 0 };
        g_a[w.n++] = 0; g_a[w.n++] = 0x00; g_a[w.n++] = 0x80; g_a[w.n++] = 0xff; g_a[w.n++] = 0x7f;
        memcpy(g_a + w.n, g_plain, 32768); w.n += 32768;
        bw_bits(&w, 1, 1); bw_bits(&w, 1, 2);
        bw_code(&w, 0xc0 + 285 - 280, 8);                           /* length 258 */
        bw_code(&w, 29, 5); bw_bits(&w, 8191, 13);                   /* distance 24577 + 8191 */
        bw_code(&w, 0, 7);                                           /* end of block */
        bw_end(&w);
        PROVEN_TEST_ASSERT(proven_inflate_all(g_heap, PROVEN_DEFLATE_RAW, (proven_mem_view_t){ g_a, w.n }, (proven_mem_mut_t){ g_b, sizeof g_b }, &made, NULL) == PROVEN_OK &&
                           made == 32768 + 258 && memcmp(g_b + 32768, g_plain, 258) == 0, "a match of 258 bytes at a distance of 32,768", "");
        /* One byte further back than that exists. */
        w = (bw_t){ g_a, 0, 0, 0 };
        g_a[w.n++] = 0; g_a[w.n++] = 0xff; g_a[w.n++] = 0x7f; g_a[w.n++] = 0x00; g_a[w.n++] = 0x80;
        memcpy(g_a + w.n, g_plain, 32767); w.n += 32767;
        bw_bits(&w, 1, 1); bw_bits(&w, 1, 2);
        bw_code(&w, 0xc0 + 285 - 280, 8); bw_code(&w, 29, 5); bw_bits(&w, 8191, 13); bw_code(&w, 0, 7);
        bw_end(&w);
        PROVEN_TEST_ASSERT(proven_inflate_all(g_heap, PROVEN_DEFLATE_RAW, (proven_mem_view_t){ g_a, w.n }, (proven_mem_mut_t){ g_b, sizeof g_b }, &made, NULL) == PROVEN_ERR_INVALID_FORMAT,
            "the same distance with one byte less before it is refused", "");
        /* Forty thousand stored bytes and then that match again, read in two calls: the first
         * leaves more than a window behind it, and the second finds its match there. */
        {
            proven_size_t used = 0;
            bool ended = false;
            w = (bw_t){ g_a, 0, 0, 0 };
            g_a[w.n++] = 0; g_a[w.n++] = 0x40; g_a[w.n++] = 0x9c; g_a[w.n++] = 0xbf; g_a[w.n++] = 0x63;
            memcpy(g_a + w.n, g_plain, 40000); w.n += 40000;
            bw_bits(&w, 1, 1); bw_bits(&w, 1, 2);
            bw_code(&w, 0xc0 + 285 - 280, 8); bw_code(&w, 29, 5); bw_bits(&w, 8191, 13); bw_code(&w, 0, 7);
            bw_end(&w);
            PROVEN_TEST_ASSERT(inflate_steps(PROVEN_DEFLATE_RAW, g_a, w.n, 70000, 40000, g_b, sizeof g_b, &made, &used, &ended) == PROVEN_OK && ended && used == w.n &&
                               made == 40000 + 258 && memcmp(g_b, g_plain, 40000) == 0 && memcmp(g_b + 40000, g_plain + 40000 - 32768, 258) == 0,
                "a match at 32,768 into what an earlier call produced, when that call produced more than a window", "Check keep(): the last 32,768 bytes, with the next one to go at the window's start.");
        }
        /* A dynamic block whose code lengths are two long runs of zeros, a one for the end of
         * block, and a one for the single distance code: nothing but an end of block, and
         * valid. The same with a run of eleven zeros in place of the last length says more
         * lengths than the block has, and is refused - by whichever code reads it. */
        for (int over = 0; over < 2; ++over) {
            w = (bw_t){ g_a, 0, 0, 0 };
            bw_bits(&w, 1, 1); bw_bits(&w, 2, 2);
            bw_bits(&w, 0, 5); bw_bits(&w, 0, 5); bw_bits(&w, 14, 4);       /* 257 literal/length codes, 1 distance code, 18 lengths for the code that follows */
            for (int i = 0; i < 18; ++i) bw_bits(&w, i == 2 || i == 17 ? 1 : 0, 3);     /* symbols 18 and 1, a bit each: 1 is "0", 18 is "1" */
            bw_bits(&w, 1, 1); bw_bits(&w, 127, 7);                         /* 138 zeros */
            bw_bits(&w, 1, 1); bw_bits(&w, 107, 7);                         /* 118 more: 256 */
            bw_bits(&w, 0, 1);                                              /* the end of block: one bit */
            if (over) { bw_bits(&w, 1, 1); bw_bits(&w, 0, 7); }             /* eleven zeros where one length is left */
            else bw_bits(&w, 0, 1);                                         /* the distance code: one bit */
            bw_bits(&w, 0, 1);                                              /* the end of block */
            bw_end(&w);
            const proven_err_t e = proven_inflate_all(g_heap, PROVEN_DEFLATE_RAW, (proven_mem_view_t){ g_a, w.n }, (proven_mem_mut_t){ g_b, sizeof g_b }, &made, NULL);
            proven_size_t used = 0, made1 = 0;
            bool ended = false;
            const proven_err_t e1 = inflate_steps(PROVEN_DEFLATE_RAW, g_a, w.n, 1, 1, g_b, sizeof g_b, &made1, &used, &ended);
            if (!over) PROVEN_TEST_ASSERT(e == PROVEN_OK && made == 0 && e1 == PROVEN_OK && ended && made1 == 0, "a dynamic block that holds only its end is read, whole and a byte at a time", "");
            else PROVEN_TEST_ASSERT(e == PROVEN_ERR_INVALID_FORMAT && e1 == PROVEN_ERR_INVALID_FORMAT, "a run of code lengths that passes the number the block announced is refused, whole and a byte at a time", "Check fast_lengths and S_LENS_REPEAT: both count before they write.");
        }
        /* A run: distance 1, length 258, again and again - each copy reads what it has just written. */
        w = (bw_t){ g_a, 0, 0, 0 };
        bw_bits(&w, 1, 1); bw_bits(&w, 1, 2);
        bw_code(&w, 0x30 + 'q', 8);
        for (int k = 0; k < 100; ++k) { bw_code(&w, 0xc0 + 285 - 280, 8); bw_code(&w, 0, 5); }
        bw_code(&w, 0, 7);
        bw_end(&w);
        bool all_q = proven_inflate_all(g_heap, PROVEN_DEFLATE_RAW, (proven_mem_view_t){ g_a, w.n }, (proven_mem_mut_t){ g_b, sizeof g_b }, &made, NULL) == PROVEN_OK && made == 25801;
        for (proven_size_t i = 0; all_q && i < made; ++i) all_q = g_b[i] == 'q';
        PROVEN_TEST_ASSERT(all_q, "a hundred matches at distance 1 repeat one byte 25,801 times", "");
        /* The same, into a buffer a thousand times too small: refused, nothing written past it. */
        g_b[25] = 0x7e;
        PROVEN_TEST_ASSERT(proven_inflate_all(g_heap, PROVEN_DEFLATE_RAW, (proven_mem_view_t){ g_a, w.n }, (proven_mem_mut_t){ g_b, 25 }, &made, NULL) == PROVEN_ERR_OUT_OF_BOUNDS && g_b[25] == 0x7e,
            "117 bytes that expand to 25,801 do not fit in 25: PROVEN_ERR_OUT_OF_BOUNDS, and the byte after the buffer is untouched", "");
    }

    PROVEN_TEST_SECTION("input that stops, and input that was changed", "Every proper prefix of a stream is a request for more; every single changed bit gives an answer of some kind.", "A crash or a sanitizer report here is a read or write outside a buffer: check need(), emit() and the bounds in each state.");
    {
        int prefixes = 0, prefix_bad = 0, flips = 0, refused = 0, same = 0, other = 0;
        for (proven_size_t i = 0; i < sizeof DEFLATEV / sizeof DEFLATEV[0]; ++i) {
            const char *at = DEFLATEV[i];
            if (at[0] != 'Z') continue;
            at += 1;
            const char *f1 = field(&at, &n1), *fc = field(&at, &n2), *f3 = field(&at, &n3), *f4 = field(&at, &n4), *f5 = field(&at, &n5);
            (void)f3; (void)f4;
            if (n5 / 2 > 260) continue;
            const proven_deflate_format_t format = (proven_deflate_format_t)(f1[0] - '0');
            const proven_size_t plain_len = corpus(atoi(fc), g_plain), comp_len = unhex(f5, n5, g_a);
            for (proven_size_t cut = 0; cut < comp_len; ++cut) {
                proven_size_t made = 0, used = 0;
                bool ended = false;
                const proven_err_t e = inflate_steps(format, g_a, cut, cut + 1, sizeof g_c, g_c, sizeof g_c, &made, &used, &ended);
                prefixes++;
                if (e != PROVEN_OK || ended || made > plain_len || memcmp(g_c, g_plain, made) != 0) prefix_bad++;
            }
            for (proven_size_t bit = 0; bit < comp_len * 8; ++bit) {
                proven_size_t made = 0;
                g_a[bit / 8] ^= (proven_byte_t)(1u << (bit % 8));
                g_c[4000] = 0x5c;
                const proven_err_t e = proven_inflate_all(g_heap, format, (proven_mem_view_t){ g_a, comp_len }, (proven_mem_mut_t){ g_c, 4000 }, &made, NULL);
                g_a[bit / 8] ^= (proven_byte_t)(1u << (bit % 8));
                flips++;
                if (g_c[4000] != 0x5c) prefix_bad++;                 /* wrote past the space it was given */
                if (e != PROVEN_OK) refused++;
                else if (made == plain_len && memcmp(g_c, g_plain, made) == 0) same++;
                else other++;
            }
        }
        fprintf(stderr, "[PROVEN][TEST][INFO] %d prefixes; %d single-bit changes: %d refused, %d gave the same data, %d gave other data\n", prefixes, flips, refused, same, other);
        PROVEN_TEST_ASSERT(prefixes > 1000 && prefix_bad == 0, "every proper prefix of the short streams is PROVEN_OK, not done, and what it produced is the start of the data", "");
        PROVEN_TEST_ASSERT(flips > 8000 && refused + same + other == flips && refused > flips / 2, "every single-bit change of them gives an error or data, inside the space it was given", "");
    }

    PROVEN_TEST_SECTION("compress, then decompress", "Every corpus item at every level and three window sizes, in the three framings.", "Check emit_block's choice of block, make_code, and tokenise. zlib reads these streams too: that is a private check.");
    {
        int trips = 0, bad = 0;
        proven_size_t total_in = 0, total_out = 0;
        for (int i = 0; i < NCORPUS; ++i) {
            const proven_size_t plain_len = corpus(i, g_plain);
            for (int level = -1; level <= 9; ++level) {
                static const int windows[3] = { 9, 12, 15 };
                for (int w = 0; w < 3; ++w) {
                    proven_deflate_options_t o = { .alloc = g_heap, .format = (proven_deflate_format_t)((i + level + 1 + w) % 3), .level = level, .window_bits = windows[w] };
                    proven_size_t made = 0, back = 0, used = 0;
                    const proven_size_t bound = proven_deflate_bound(o.format, plain_len);
                    bool ok = bound <= sizeof g_c && proven_deflate_all(&o, (proven_mem_view_t){ g_plain, plain_len }, (proven_mem_mut_t){ g_c, bound }, &made) == PROVEN_OK && made <= bound;
                    ok = ok && proven_inflate_all(g_heap, o.format, (proven_mem_view_t){ g_c, made }, (proven_mem_mut_t){ g_b, sizeof g_b }, &back, &used) == PROVEN_OK &&
                         back == plain_len && used == made && memcmp(g_b, g_plain, back) == 0;
                    trips++;
                    if (!ok) { bad++; fprintf(stderr, "round trip: corpus %d level %d window %d format %d failed\n", i, level, windows[w], (int)o.format); }
                    if (level == 6 && w == 2) { total_in += plain_len; total_out += made; }
                }
            }
        }
        fprintf(stderr, "[PROVEN][TEST][INFO] at the default level the corpus of %u bytes becomes %u\n", (unsigned)total_in, (unsigned)total_out);
        PROVEN_TEST_ASSERT(trips == 363 && bad == 0, "363 round trips: the data comes back, and the compressed size is within proven_deflate_bound", "");
        PROVEN_TEST_ASSERT(total_out < total_in / 8, "the corpus shrinks to less than an eighth at the default level", "");
    }
    {
        /* Fed and drained in small pieces, with a compressor that is reset and used again. */
        proven_deflate_options_t o = { .alloc = g_heap, .format = PROVEN_DEFLATE_GZIP };
        proven_deflate_t *z = NULL;
        bool all = true;
        PROVEN_TEST_ASSERT(proven_deflate_create(&o, &z) == PROVEN_OK, "a compressor with every option left at zero but the framing", "");
        static const proven_size_t steps[4][2] = { { 1, 1 }, { 3, 7 }, { 1000, 5 }, { 70000, 70000 } };
        for (int i = 0; i < NCORPUS; ++i) {
            const proven_size_t plain_len = corpus(i, g_plain);
            for (int s = 0; s < 4; ++s) {
                proven_size_t at = 0, made = 0, back = 0;
                bool done = false;
                if (plain_len > 10000 && s == 0) continue;
                proven_deflate_reset(z);
                for (long guard = 0; guard < 4000000 && !done; ++guard) {
                    const proven_size_t give = plain_len - at < steps[s][0] ? plain_len - at : steps[s][0];
                    proven_size_t c = 0, p = 0;
                    const proven_deflate_flush_t f = at + give == plain_len ? PROVEN_DEFLATE_FLUSH_FINISH : PROVEN_DEFLATE_FLUSH_NONE;
                    if (proven_deflate(z, (proven_mem_view_t){ g_plain + at, give }, &c, (proven_mem_mut_t){ g_c + made, steps[s][1] }, &p, f, &done) != PROVEN_OK) { all = false; break; }
                    at += c; made += p;
                }
                all = all && done && at == plain_len && proven_inflate_all(g_heap, PROVEN_DEFLATE_GZIP, (proven_mem_view_t){ g_c, made }, (proven_mem_mut_t){ g_b, sizeof g_b }, &back, NULL) == PROVEN_OK &&
                      back == plain_len && memcmp(g_b, g_plain, back) == 0;
            }
        }
        PROVEN_TEST_ASSERT(all, "a byte in and a byte out at a time, and three other step sizes, through one compressor reset between streams", "");
        proven_size_t c = 0, p = 0;
        bool done = false;
        PROVEN_TEST_ASSERT(proven_deflate(z, (proven_mem_view_t){ g_plain, 1 }, &c, (proven_mem_mut_t){ g_c, 100 }, &p, PROVEN_DEFLATE_FLUSH_NONE, &done) == PROVEN_ERR_INVALID_STATE,
            "input after the stream was finished is PROVEN_ERR_INVALID_STATE", "");
        PROVEN_TEST_ASSERT(proven_deflate(z, (proven_mem_view_t){ NULL, 0 }, &c, (proven_mem_mut_t){ g_c, 100 }, &p, PROVEN_DEFLATE_FLUSH_FINISH, &done) == PROVEN_OK && done && p == 0,
            "asking a finished stream to finish again produces nothing and says done", "");
        proven_deflate_destroy(z);
    }

    PROVEN_TEST_SECTION("the fixed code's tables", "The decompressor builds nothing for a fixed block: its code is in src/proven/deflate_tables.h, generated. Here it is made again from the lengths RFC 1951 gives and compared.", "Regenerate src/proven/deflate_tables.h with the private generator; do not edit it.");
    {
        bool same = true;
        for (int which = 0; which < 2; ++which) {
            unsigned lengths[288], n = which == 0 ? 288 : 32, fbits = which == 0 ? 9 : 5;
            for (unsigned sym = 0; sym < n; ++sym) lengths[sym] = which == 1 ? 5 : sym < 144 ? 8 : sym < 256 ? 9 : sym < 280 ? 7 : 8;
            const proven_u16 *count = which == 0 ? deflate_fixed_lcount : deflate_fixed_dcount, *symbol = which == 0 ? deflate_fixed_lsymbol : deflate_fixed_dsymbol,
                             *fast = which == 0 ? deflate_fixed_lfast : deflate_fixed_dfast;
            unsigned counted[16] = { 0 }, next[16] = { 0 }, code = 0, order = 0, filled = 0;
            for (unsigned sym = 0; sym < n; ++sym) counted[lengths[sym]]++;
            for (unsigned len = 0; len < 16; ++len) same = same && count[len] == counted[len];
            for (unsigned len = 1; len < 16; ++len) { code = (code + counted[len - 1]) << 1; next[len] = code; }
            /* The symbols in the order of their codes: by length, then by number. */
            for (unsigned len = 1; len < 16; ++len) for (unsigned sym = 0; sym < n; ++sym) if (lengths[sym] == len) same = same && symbol[order++] == sym;
            /* Every index of the table: the symbol whose code, sent highest bit first and read lowest first, is its low bits. */
            for (unsigned sym = 0; sym < n; ++sym) {
                unsigned c = next[lengths[sym]]++, image = 0;
                for (unsigned b = 0; b < lengths[sym]; ++b) { image = (image << 1) | (c & 1u); c >>= 1; }
                for (unsigned at = image; at < (1u << fbits); at += 1u << lengths[sym]) { same = same && fast[at] == ((sym << 4) | lengths[sym]); filled++; }
            }
            same = same && order == n && filled == (1u << fbits);
        }
        PROVEN_TEST_ASSERT(same, "the counts, the order of the symbols and all 512 and 32 entries of the two tables are what the RFC's lengths make", "");
    }

    PROVEN_TEST_SECTION("pieces of every size", "The decoder finds whole symbols by table and leaves what the end of its input or of its output cuts to the code that works a bit at a time: where the two ends fall must not matter.",
        "Check fast() and fast_lengths() in src/proven/deflate.c - what they leave untaken, and the bytes they give back - and back() and keep() for a distance that reaches past the beginning of a call's output.");
    {
        static const proven_size_t ins[] = { 1, 2, 7, 8, 9, 64, 4096, 70000 }, outs[] = { 1, 2, 3, 8, 9, 257, 258, 259, 266, 300, 16384, 40000, 70000 };      /* 40,000: a call that leaves more than a window behind it, and then another */
        bool all = true, far_found = true;
        long runs = 0;
        for (int i = 0; i <= NCORPUS; ++i) {
            proven_size_t plain_len;
            if (i < NCORPUS) plain_len = corpus(i, g_plain);
            else {
                /* 32,000 bytes of noise, twice: the second half is one match after another at
                 * nearly the greatest distance there is, from history that a small output
                 * piece has long since left behind. */
                proven_u32 x = 88172645u;
                for (proven_size_t k = 0; k < 32000; ++k) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; g_plain[k] = g_plain[32000 + k] = (proven_byte_t)(x >> 7); }
                plain_len = 64000;
            }
            for (int level = 1; level <= 9; level += 4) {
                proven_deflate_options_t o = { .alloc = g_heap, .format = PROVEN_DEFLATE_RAW, .level = level };
                proven_size_t packed = 0;
                if (proven_deflate_all(&o, (proven_mem_view_t){ g_plain, plain_len }, (proven_mem_mut_t){ g_c, sizeof g_c }, &packed) != PROVEN_OK) { all = false; continue; }
                if (i == NCORPUS) far_found = far_found && packed < 40000;      /* the second half was found in the first: the distances are there */
                for (proven_size_t a = 0; a < sizeof ins / sizeof ins[0]; ++a) for (proven_size_t b = 0; b < sizeof outs / sizeof outs[0]; ++b) {
                    if (plain_len > 10000 && ins[a] * outs[b] < 16) continue;      /* a byte of each at a time, for the small ones only */
                    proven_size_t made = 0, used = 0;
                    bool ended = false;
                    memset(g_b, 0x5a, plain_len);
                    all = all && inflate_steps(PROVEN_DEFLATE_RAW, g_c, packed, ins[a], outs[b], g_b, plain_len, &made, &used, &ended) == PROVEN_OK && ended &&
                          made == plain_len && used == packed && memcmp(g_b, g_plain, plain_len) == 0;
                    runs++;
                }
            }
        }
        /* What a call writes and does not report: at most seven bytes after what it produced,
         * never more, and never past the space it was given. All the room there is and a
         * little input at a time: a call then ends where its input does, which in data full of
         * repeats is often just behind a copy. */
        {
            bool tidy = true;
            long beyond = 0, calls = 0;
            for (int item = 0; item < NCORPUS; ++item) {
                const proven_size_t plain_len = corpus(item, g_plain);
                proven_inflate_t *z = NULL;
                proven_size_t packed = 0, at = 0, made = 0;
                proven_deflate_options_t o = { .alloc = g_heap, .format = PROVEN_DEFLATE_RAW, .level = 6 };
                bool ended = false;
                tidy = tidy && proven_deflate_all(&o, (proven_mem_view_t){ g_plain, plain_len }, (proven_mem_mut_t){ g_c, sizeof g_c }, &packed) == PROVEN_OK &&
                       proven_inflate_create(g_heap, PROVEN_DEFLATE_RAW, &z) == PROVEN_OK;
                memset(g_b, 0x5a, sizeof g_b);
                for (long guard = 0; tidy && guard < 100000 && !ended; ++guard) {
                    proven_size_t c = 0, p = 0;
                    tidy = proven_inflate(z, (proven_mem_view_t){ g_c + at, packed - at < 5 ? packed - at : 5 }, &c, (proven_mem_mut_t){ g_b + made, sizeof g_b - made }, &p, &ended) == PROVEN_OK;
                    at += c; made += p; calls++;
                    for (proven_size_t k = made; k < made + 7 && k < sizeof g_b; ++k) if (g_b[k] != 0x5a) { beyond++; g_b[k] = 0x5a; }
                    for (proven_size_t k = made + 7; k < made + 300 && k < sizeof g_b; ++k) tidy = tidy && g_b[k] == 0x5a;
                }
                proven_inflate_destroy(z);
                tidy = tidy && ended && made == plain_len && memcmp(g_b, g_plain, plain_len) == 0;
            }
            PROVEN_TEST_ASSERT(tidy, "after each call nothing is written further than seven bytes past what it reports, and the data is whole", "Check the copy in fast(): eight at a time only with room for eight more.");
            PROVEN_TEST_ASSERT(beyond > 0, "and within those seven, some were: the case is one this test reaches", "If nothing is written beyond any more, the header and the manual should stop saying it may be.");
            PROVEN_TEST_INFO("{} bytes were written beyond what a call reported, in {} calls given five bytes of input each", PROVEN_ARG((int)beyond), PROVEN_ARG((int)calls));
        }
        PROVEN_TEST_ASSERT(far_found, "32,000 bytes of noise written twice compress to little more than once: the matches at that distance are in the stream", "");
        PROVEN_TEST_ASSERT(all && runs > 3000, "the corpus and that block, at three levels, come back whole through 104 combinations of input and output piece", "");
    }

    PROVEN_TEST_SECTION("flushing", "A sync flush makes everything so far decodable, ends in 00 00 ff ff, and the stream goes on.", "Check the sync branch of proven_deflate, and that proven_inflate keeps its history when a stream is fed in pieces.");
    {
        proven_deflate_options_t o = { .alloc = g_heap, .format = PROVEN_DEFLATE_RAW };
        proven_deflate_t *z = NULL;
        proven_inflate_t *u = NULL;
        static const char *const messages[4] = { "the first message, with some words in it", "the first message again, with some words in it", "", "and a third: words, words, words" };
        proven_size_t sizes[4];
        bool tails = true, each = true, shrinks = false, never_done = true;
        PROVEN_TEST_ASSERT(proven_deflate_create(&o, &z) == PROVEN_OK && proven_inflate_create(g_heap, PROVEN_DEFLATE_RAW, &u) == PROVEN_OK, "a compressor and a decompressor for a raw stream that is never finished", "");
        for (int m = 0; m < 4; ++m) {
            proven_size_t c = 0, p = 0, uc = 0, up = 0;
            bool done = false, udone = false;
            const proven_size_t len = strlen(messages[m]);
            each = each && proven_deflate(z, (proven_mem_view_t){ (const proven_byte_t *)messages[m], len }, &c, (proven_mem_mut_t){ g_c, sizeof g_c }, &p, PROVEN_DEFLATE_FLUSH_SYNC, &done) == PROVEN_OK && c == len && !done;
            sizes[m] = p;
            if (m == 2) { each = each && p == 0; continue; }           /* nothing new since the last flush: nothing to say */
            tails = tails && p >= 4 && g_c[p - 4] == 0 && g_c[p - 3] == 0 && g_c[p - 2] == 0xff && g_c[p - 1] == 0xff;
            /* As permessage-deflate carries it: without the four bytes, which the receiver puts back. */
            static const proven_byte_t tail[4] = { 0, 0, 0xff, 0xff };
            each = each && proven_inflate(u, (proven_mem_view_t){ g_c, p - 4 }, &uc, (proven_mem_mut_t){ g_b, sizeof g_b }, &up, &udone) == PROVEN_OK && uc == p - 4;
            proven_size_t more = 0;
            each = each && proven_inflate(u, (proven_mem_view_t){ tail, 4 }, &uc, (proven_mem_mut_t){ g_b + up, sizeof g_b - up }, &more, &udone) == PROVEN_OK && uc == 4;
            never_done = never_done && !udone;
            each = each && up + more == len && memcmp(g_b, messages[m], len) == 0;
        }
        shrinks = sizes[1] < sizes[0] / 2;
        PROVEN_TEST_ASSERT(each, "each message is taken whole, and its output - cut at the flush and with the four bytes put back by the receiver - decompresses to it", "");
        PROVEN_TEST_ASSERT(tails, "every flush with something to say ends in 00 00 ff ff", "");
        PROVEN_TEST_ASSERT(shrinks, "the second message, nearly the first again, takes less than half the bytes: the history is kept across flushes", "");
        PROVEN_TEST_ASSERT(never_done, "a stream that is only ever flushed is never done", "");
        proven_deflate_destroy(z); proven_inflate_destroy(u);
    }
    {
        /* gzip members one after another: done at the end of each, reset, go on. */
        proven_deflate_options_t o = { .alloc = g_heap, .format = PROVEN_DEFLATE_GZIP, .level = 1 };
        proven_size_t a = 0, b = 0, used = 0, made = 0;
        proven_inflate_t *u = NULL;
        bool done = false;
        PROVEN_TEST_ASSERT(proven_deflate_all(&o, (proven_mem_view_t){ (const proven_byte_t *)"first", 5 }, (proven_mem_mut_t){ g_c, 100 }, &a) == PROVEN_OK &&
                           proven_deflate_all(&o, (proven_mem_view_t){ (const proven_byte_t *)"second", 6 }, (proven_mem_mut_t){ g_c + a, 100 }, &b) == PROVEN_OK, "two gzip members, one after the other", "");
        PROVEN_TEST_ASSERT(proven_inflate_create(g_heap, PROVEN_DEFLATE_GZIP, &u) == PROVEN_OK &&
                           proven_inflate(u, (proven_mem_view_t){ g_c, a + b }, &used, (proven_mem_mut_t){ g_b, 100 }, &made, &done) == PROVEN_OK && done && used == a && made == 5 && memcmp(g_b, "first", 5) == 0,
            "the decompressor stops at the end of the first and says where", "");
        PROVEN_TEST_ASSERT(proven_inflate(u, (proven_mem_view_t){ g_c + a, b }, &used, (proven_mem_mut_t){ g_b, 100 }, &made, &done) == PROVEN_OK && done && used == 0 && made == 0, "asked again without a reset it stays done and takes nothing", "");
        proven_inflate_reset(u);
        PROVEN_TEST_ASSERT(proven_inflate(u, (proven_mem_view_t){ g_c + a, b }, &used, (proven_mem_mut_t){ g_b, 100 }, &made, &done) == PROVEN_OK && done && used == b && made == 6 && memcmp(g_b, "second", 6) == 0,
            "reset, it reads the second", "");
        /* An error stays an error until reset. */
        static const proven_byte_t junk[3] = { 7, 0, 0 };
        proven_inflate_t *r = NULL;
        PROVEN_TEST_ASSERT(proven_inflate_create(g_heap, PROVEN_DEFLATE_RAW, &r) == PROVEN_OK &&
                           proven_inflate(r, (proven_mem_view_t){ junk, 3 }, &used, (proven_mem_mut_t){ g_b, 100 }, &made, &done) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_inflate(r, (proven_mem_view_t){ g_c, 0 }, &used, (proven_mem_mut_t){ g_b, 100 }, &made, &done) == PROVEN_ERR_INVALID_FORMAT, "after an error every call gives that error", "");
        proven_inflate_reset(r);
        static const proven_byte_t empty_fixed[2] = { 3, 0 };
        PROVEN_TEST_ASSERT(proven_inflate(r, (proven_mem_view_t){ empty_fixed, 2 }, &used, (proven_mem_mut_t){ g_b, 100 }, &made, &done) == PROVEN_OK && done && made == 0, "until it is reset", "");
        proven_inflate_destroy(u); proven_inflate_destroy(r);
    }

    PROVEN_TEST_SECTION("arguments", "What the functions refuse before doing anything.", "Check the first lines of each public function.");
    {
        proven_inflate_t *u = (proven_inflate_t *)1;
        proven_deflate_t *z = (proven_deflate_t *)1;
        proven_deflate_options_t o = { .alloc = g_heap };
        proven_allocator_t none = { 0 };
        proven_size_t c = 0, p = 0;
        bool done = false;
        PROVEN_TEST_ASSERT(proven_inflate_create(g_heap, (proven_deflate_format_t)3, &u) == PROVEN_ERR_INVALID_ARG && u == NULL && proven_inflate_create(none, PROVEN_DEFLATE_RAW, &u) == PROVEN_ERR_INVALID_ARG &&
                           proven_inflate_create(g_heap, PROVEN_DEFLATE_RAW, NULL) == PROVEN_ERR_INVALID_ARG, "a decompressor needs a framing that exists, an allocator and somewhere to go", "");
        o.level = 10;
        PROVEN_TEST_ASSERT(proven_deflate_create(&o, &z) == PROVEN_ERR_INVALID_ARG && z == NULL, "a level above 9 is refused", "");
        o.level = -2;
        PROVEN_TEST_ASSERT(proven_deflate_create(&o, &z) == PROVEN_ERR_INVALID_ARG, "and one below -1", "");
        o.level = 0; o.window_bits = 8;
        PROVEN_TEST_ASSERT(proven_deflate_create(&o, &z) == PROVEN_ERR_INVALID_ARG, "a window of 8 bits is refused", "");
        o.window_bits = 16;
        PROVEN_TEST_ASSERT(proven_deflate_create(&o, &z) == PROVEN_ERR_INVALID_ARG, "and one of 16", "");
        o.window_bits = 0; o.format = (proven_deflate_format_t)3;
        PROVEN_TEST_ASSERT(proven_deflate_create(&o, &z) == PROVEN_ERR_INVALID_ARG && proven_deflate_create(NULL, &z) == PROVEN_ERR_INVALID_ARG, "and a framing that does not exist, and no options", "");
        o.format = PROVEN_DEFLATE_RAW;
        PROVEN_TEST_ASSERT(proven_deflate_create(&o, &z) == PROVEN_OK && proven_inflate_create(g_heap, PROVEN_DEFLATE_RAW, &u) == PROVEN_OK, "defaults are accepted", "");
        PROVEN_TEST_ASSERT(proven_deflate(z, (proven_mem_view_t){ NULL, 5 }, &c, (proven_mem_mut_t){ g_c, 10 }, &p, PROVEN_DEFLATE_FLUSH_NONE, &done) == PROVEN_ERR_INVALID_ARG &&
                           proven_deflate(z, (proven_mem_view_t){ g_a, 5 }, NULL, (proven_mem_mut_t){ g_c, 10 }, &p, PROVEN_DEFLATE_FLUSH_NONE, &done) == PROVEN_ERR_INVALID_ARG &&
                           proven_deflate(z, (proven_mem_view_t){ g_a, 5 }, &c, (proven_mem_mut_t){ g_c, 10 }, &p, (proven_deflate_flush_t)3, &done) == PROVEN_ERR_INVALID_ARG &&
                           proven_inflate(u, (proven_mem_view_t){ g_a, 5 }, &c, (proven_mem_mut_t){ NULL, 10 }, &p, &done) == PROVEN_ERR_INVALID_ARG &&
                           proven_inflate(NULL, (proven_mem_view_t){ g_a, 5 }, &c, (proven_mem_mut_t){ g_c, 10 }, &p, &done) == PROVEN_ERR_INVALID_ARG,
            "null buffers with a length, missing out-parameters, an unknown flush and no state are PROVEN_ERR_INVALID_ARG", "");
        PROVEN_TEST_ASSERT(proven_deflate(z, (proven_mem_view_t){ NULL, 0 }, &c, (proven_mem_mut_t){ NULL, 0 }, &p, PROVEN_DEFLATE_FLUSH_NONE, &done) == PROVEN_OK && c == 0 && p == 0 && !done &&
                           proven_inflate(u, (proven_mem_view_t){ NULL, 0 }, &c, (proven_mem_mut_t){ NULL, 0 }, &p, &done) == PROVEN_OK && c == 0 && p == 0 && !done,
            "nothing in and nowhere to put anything is PROVEN_OK with no progress", "");
        proven_deflate_destroy(z); proven_inflate_destroy(u);
        proven_deflate_destroy(NULL); proven_inflate_destroy(NULL); proven_deflate_reset(NULL); proven_inflate_reset(NULL);
        o.format = PROVEN_DEFLATE_GZIP;
        PROVEN_TEST_ASSERT(proven_deflate_all(&o, (proven_mem_view_t){ g_plain, 1000 }, (proven_mem_mut_t){ g_c, 10 }, &p) == PROVEN_ERR_OUT_OF_BOUNDS && p == 0, "compressing into a buffer that is too small is PROVEN_ERR_OUT_OF_BOUNDS", "");
        PROVEN_TEST_ASSERT(proven_deflate_all(&o, (proven_mem_view_t){ NULL, 0 }, (proven_mem_mut_t){ g_c, 100 }, &p) == PROVEN_OK && p == 20 && proven_deflate_bound(PROVEN_DEFLATE_GZIP, 0) >= 20,
            "no data at all compresses to a 20-byte gzip member", "");
        PROVEN_TEST_ASSERT(proven_deflate_bound(PROVEN_DEFLATE_RAW, 1000) >= 1005 && proven_deflate_bound(PROVEN_DEFLATE_RAW, 1000) < 1100, "the bound for a kilobyte is a little over a kilobyte", "");
    }

    PROVEN_TEST_PASS("DEFLATE reads what zlib writes, refuses what zlib refuses, and gets its own output back.");
    return 0;
}
