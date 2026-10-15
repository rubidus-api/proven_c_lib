#include "proven.h"
#include "proven_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PROVEN_NO_BROTLI
int main(void) {
    PROVEN_TEST_SUITE("Brotli decompression", "Built with PROVEN_NO_BROTLI: there is no decoder to test.", "");
    PROVEN_TEST_PASS("Skipped: PROVEN_NO_BROTLI leaves the decoder out.");
    return 0;
}
#else
#include "test_unit_brotli_vectors.h"

/*
 * Brotli decompression.
 *
 * test_unit_brotli_vectors.h was made outside this library, with libbrotli (Python's brotli):
 *
 *   B crc32 length hex name   a stream libbrotli made, with the CRC-32 and length of its input
 *   O crc32 length hex name   a hand-built stream libbrotli decodes, with the CRC-32 and length of
 *                             what it makes: metadata, uncompressed meta-blocks, every window,
 *                             dictionary words under all 121 transforms, every context mode,
 *                             context maps with runs and move-to-front, every short distance
 *                             code, block switches in each category, odd prefix codes
 *   X verdict hex name        a hand-built stream libbrotli refuses (I: invalid)
 */

static proven_allocator_t g_heap;
static proven_byte_t g_in[40000], g_out[70000], g_out2[70000], g_cat[40000];

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

/* A vector by name: its kind, and its stream into `out`. */
static proven_size_t vector(const char *name, proven_byte_t *out, proven_u32 *crc, proven_size_t *len) {
    for (proven_size_t i = 0; i < sizeof BROTLIV / sizeof BROTLIV[0]; ++i) {
        const char *at = BROTLIV[i] + 1;
        proven_size_t n1, n2, n3, n4;
        if (BROTLIV[i][0] == 'X') continue;
        const char *f1 = field(&at, &n1), *f2 = field(&at, &n2), *f3 = field(&at, &n3), *f4 = field(&at, &n4);
        if (n4 == strlen(name) && strncmp(f4, name, n4) == 0) {
            if (crc) *crc = (proven_u32)strtoul(f1, NULL, 16);
            if (len) *len = (proven_size_t)strtoul(f2, NULL, 10);
            return unhex(f3, n3, out);
        }
    }
    return 0;
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
 * until the stream ends or nothing more can happen. `*used`: the input consumed. */
static proven_err_t decode_steps(proven_allocator_t alloc, proven_u64 window_max, const proven_byte_t *in, proven_size_t in_len, proven_size_t in_step,
                                 proven_size_t out_step, proven_byte_t *out, proven_size_t out_cap, proven_size_t *made, bool *ended, proven_size_t *used) {
    proven_brotli_decoder_t *b = NULL;
    *made = 0;
    *ended = false;
    *used = 0;
    proven_err_t e = proven_brotli_decoder_create(alloc, window_max, &b);
    if (e != PROVEN_OK) return e;
    for (long guard = 0; guard < 4000000; ++guard) {
        const proven_size_t give = in_len - *used < in_step ? in_len - *used : in_step, room = out_cap - *made < out_step ? out_cap - *made : out_step;
        proven_size_t c = 0, p = 0;
        e = proven_brotli_decode(b, (proven_mem_view_t){ in + *used, give }, &c, (proven_mem_mut_t){ out + *made, room }, &p, ended);
        *used += c;
        *made += p;
        if (e != PROVEN_OK || *ended) break;
        if (c == 0 && p == 0 && (*used == in_len || *made == out_cap)) break;       /* it wants what there is no more of */
    }
    proven_brotli_decoder_destroy(b);
    return e;
}

int main(void) {
    PROVEN_TEST_SUITE("Brotli decompression",
        "Decompress what libbrotli made and refuse what libbrotli refuses; stay inside the window limit and the output given; behave on input cut short, changed, or fed a byte at a time.",
        "Inspect src/proven/brotli.c. For a vector, the name printed says what it is; tests/test_unit_brotli_vectors.h is written by a private generator that checks libbrotli's verdict on every line.");
    g_heap = proven_heap_allocator();

    PROVEN_TEST_SECTION("streams libbrotli made, and hand-built ones", "Every line: decompressed in one call, a byte in and a byte out at a time, and in odd steps.", "Check proven_brotli_decode's state machine: the stepped forms stop and resume at every item.");
    {
        int failed = 0, counts[3] = { 0 };
        for (proven_size_t i = 0; i < sizeof BROTLIV / sizeof BROTLIV[0]; ++i) {
            const char *at = BROTLIV[i];
            const char kind = at[0];
            proven_size_t n1, n2, n3, n4;
            bool ok = false;
            at += 1;
            if (kind == 'B' || kind == 'O') {
                const char *f1 = field(&at, &n1), *f2 = field(&at, &n2), *f3 = field(&at, &n3);
                (void)field(&at, &n4);
                const proven_u32 want_crc = (proven_u32)strtoul(f1, NULL, 16);
                const proven_size_t want_len = (proven_size_t)strtoul(f2, NULL, 10), comp_len = unhex(f3, n3, g_in);
                proven_size_t made = 0, used = 0;
                ok = proven_brotli_decode_all(g_heap, (proven_mem_view_t){ g_in, comp_len }, (proven_mem_mut_t){ g_out, sizeof g_out }, &made) == PROVEN_OK &&
                     made == want_len && proven_crc32((proven_mem_view_t){ g_out, made }) == want_crc;
                static const proven_size_t steps[][2] = { { 1, 1 }, { 7, 13 }, { 4096, 1000 }, { 70000, 65536 } };
                for (int s = 0; ok && s < 4; ++s) {
                    if (s == 0 && comp_len > 5000) continue;            /* a byte at a time: the short streams */
                    bool ended = false;
                    ok = decode_steps(g_heap, 0, g_in, comp_len, steps[s][0], steps[s][1], g_out2, sizeof g_out2, &made, &ended, &used) == PROVEN_OK &&
                         ended && used == comp_len && made == want_len && memcmp(g_out2, g_out, made) == 0;
                }
                counts[kind == 'B' ? 0 : 1]++;
            } else {
                const char *f1 = field(&at, &n1), *f2 = field(&at, &n2);
                const proven_size_t comp_len = unhex(f2, n2, g_in);
                const proven_err_t want = f1[0] == 'B' ? PROVEN_ERR_OUT_OF_BOUNDS : PROVEN_ERR_INVALID_FORMAT;
                proven_size_t made = 0, used = 0;
                bool ended = false;
                ok = proven_brotli_decode_all(g_heap, (proven_mem_view_t){ g_in, comp_len }, (proven_mem_mut_t){ g_out, sizeof g_out }, &made) == want && made == 0;
                /* In steps: the same error - or, for bytes after a stream's end, the end with
                 * those bytes not consumed. */
                const proven_err_t e = decode_steps(g_heap, 0, g_in, comp_len, 1, 1, g_out2, sizeof g_out2, &made, &ended, &used);
                ok = ok && (e == want || (e == PROVEN_OK && ended && used < comp_len));
                counts[2]++;
            }
            if (!ok) { failed++; printf("vector %u differs: %s\n", (unsigned)i, strrchr(BROTLIV[i], ' ') + 1); }
        }
        PROVEN_TEST_ASSERT(failed == 0, "every stream libbrotli made decodes to its input, whole and in steps; every hand-built stream gets libbrotli's verdict", "The vector's index and name are printed above.");
        PROVEN_TEST_ASSERT(counts[0] == 110 && counts[1] == 66 && counts[2] == 25, "110 streams libbrotli made, 66 hand-built ones it decodes, 25 it refuses", "");
    }

    PROVEN_TEST_SECTION("damage", "Every prefix and every single-bit change of the short streams: refused, or decoded within the output given - and the same whole as in steps.", "A sanitizer report here is an out-of-bounds read or write in src/proven/brotli.c.");
    {
        int streams = 0, prefixes_refused = 0, prefixes = 0, mismatches = 0;
        long changed = 0, refused = 0;
        for (proven_size_t i = 0; i < sizeof BROTLIV / sizeof BROTLIV[0]; ++i) {
            const char *at = BROTLIV[i];
            if (at[0] == 'X') continue;
            proven_size_t n;
            const char *h = NULL;
            at += 1;
            for (int k = 0; k < 3; ++k) h = field(&at, &n);
            const proven_size_t len = unhex(h, n, g_in);
            if (len > 400) continue;
            streams++;
            for (proven_size_t cut = 0; cut < len; ++cut) {
                proven_size_t made = 0;
                prefixes++;
                if (proven_brotli_decode_all(g_heap, (proven_mem_view_t){ g_in, cut }, (proven_mem_mut_t){ g_out, sizeof g_out }, &made) == PROVEN_ERR_INVALID_FORMAT) prefixes_refused++;
            }
            for (proven_size_t bit = 0; bit < 8 * len; ++bit) {
                memcpy(g_cat, g_in, len);
                g_cat[bit / 8] ^= (proven_byte_t)(1u << (bit % 8));
                proven_size_t made = 0, made2 = 0, used = 0;
                bool ended = false;
                const proven_err_t e1 = proven_brotli_decode_all(g_heap, (proven_mem_view_t){ g_cat, len }, (proven_mem_mut_t){ g_out, 60000 }, &made);
                const proven_err_t e2 = decode_steps(g_heap, 0, g_cat, len, 3, 5, g_out2, 60000, &made2, &ended, &used);
                changed++;
                if (e1 != PROVEN_OK) refused++;
                if ((e1 == PROVEN_OK) != (e2 == PROVEN_OK && ended && used == len) || (e1 == PROVEN_OK && (made != made2 || memcmp(g_out, g_out2, made) != 0))) mismatches++;
            }
        }
        printf("  %d streams: %d prefixes, %ld changed bits of which %ld refused, %d differ in steps\n", streams, prefixes, changed, refused, mismatches);
        PROVEN_TEST_ASSERT(streams > 50 && prefixes_refused == prefixes, "every proper prefix of a stream is PROVEN_ERR_INVALID_FORMAT", "Check where proven_brotli_decode_all tells input that ended inside a stream from output that has no room.");
        PROVEN_TEST_ASSERT(mismatches == 0 && changed > 20000 && refused > 0, "every single-bit change is refused or decoded within the output given, and the same whole as in steps", "");
    }

    PROVEN_TEST_SECTION("the end of a stream", "A step stops where the stream ends, takes nothing after it, and a reset starts the next.", "Check S_END and S_DONE in proven_brotli_decode.");
    {
        proven_u32 crc = 0;
        proven_size_t want = 0;
        const proven_size_t a = vector("html-q11-w22", g_cat, &crc, &want);
        memcpy(g_cat + a, "after", 5);
        proven_brotli_decoder_t *b = NULL;
        PROVEN_TEST_ASSERT(a > 0 && proven_brotli_decoder_create(g_heap, 0, &b) == PROVEN_OK, "a decoder and a stream with five bytes after it", "");
        proven_size_t c = 0, p = 0;
        bool done = false;
        PROVEN_TEST_ASSERT(proven_brotli_decode(b, (proven_mem_view_t){ g_cat, a + 5 }, &c, (proven_mem_mut_t){ g_out, sizeof g_out }, &p, &done) == PROVEN_OK && done && c == a && p == want &&
                           proven_crc32((proven_mem_view_t){ g_out, p }) == crc,
            "one call decodes the stream and stops at its end: done, and what it consumed is the stream", "");
        PROVEN_TEST_ASSERT(proven_brotli_decode(b, (proven_mem_view_t){ g_cat + a, 5 }, &c, (proven_mem_mut_t){ g_out, sizeof g_out }, &p, &done) == PROVEN_OK && done && c == 0 && p == 0,
            "after the end it takes nothing more and stays done", "");
        proven_brotli_decoder_reset(b);
        PROVEN_TEST_ASSERT(proven_brotli_decode(b, (proven_mem_view_t){ g_cat, a }, &c, (proven_mem_mut_t){ g_out, sizeof g_out }, &p, &done) == PROVEN_OK && done && p == want,
            "a reset reads the next stream with the buffers kept", "");
        /* A stream with a smaller window than the history kept: the history is given back
         * and made again for it. */
        proven_u32 crc10 = 0;
        proven_size_t want10 = 0;
        const proven_size_t s10 = vector("html-q5-w10", g_in, &crc10, &want10);
        proven_brotli_decoder_reset(b);
        PROVEN_TEST_ASSERT(proven_brotli_decode(b, (proven_mem_view_t){ g_in, s10 }, &c, (proven_mem_mut_t){ g_out, sizeof g_out }, &p, &done) == PROVEN_OK && done && p == want10 &&
                           proven_crc32((proven_mem_view_t){ g_out, p }) == crc10,
            "after a reset, a stream with a 1 KiB window decodes with the same decoder", "Check stream_header: a history larger than the new window is freed.");
        proven_brotli_decoder_reset(b);
        g_cat[0] = (proven_byte_t)((g_cat[0] & 0x80u) | 0x11u);    /* the window field 0010001, which RFC 7932 does not allow */
        PROVEN_TEST_ASSERT(proven_brotli_decode(b, (proven_mem_view_t){ g_cat, a }, &c, (proven_mem_mut_t){ g_out, sizeof g_out }, &p, &done) != PROVEN_OK &&
                           proven_brotli_decode(b, (proven_mem_view_t){ g_cat, a }, &c, (proven_mem_mut_t){ g_out, sizeof g_out }, &p, &done) != PROVEN_OK,
            "an error stays until a reset", "");
        proven_brotli_decoder_destroy(b);
        proven_size_t w = 0;
        PROVEN_TEST_ASSERT(proven_brotli_decode_all(g_heap, (proven_mem_view_t){ NULL, 0 }, (proven_mem_mut_t){ g_out, 10 }, &w) == PROVEN_ERR_INVALID_FORMAT,
            "no bytes at all are not a stream", "");
    }

    PROVEN_TEST_SECTION("bounds", "The window limit, the output given, and the memory a decoder holds.", "Check stream_header (the limit, before anything is allocated), room and tab_room in src/proven/brotli.c.");
    {
        proven_u32 crc = 0;
        proven_size_t want = 0, made = 0, used = 0;
        bool ended = false;
        proven_size_t n = vector("html-q11-w24", g_in, &crc, &want);
        proven_err_t e = decode_steps(counting(0), 64 * 1024, g_in, n, n, sizeof g_out, g_out, sizeof g_out, &made, &ended, &used);
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_OUT_OF_BOUNDS && made == 0 && g_count.calls == 1, "a stream whose window (16 MiB) is above the decoder's limit is PROVEN_ERR_OUT_OF_BOUNDS, with nothing allocated past the decoder itself", "");
        e = decode_steps(counting(0), 0, g_in, n, 100, 100, g_out, sizeof g_out, &made, &ended, &used);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && ended && made == want && g_count.peak < 64 * 1024 && g_count.live == 0,
            "with the default limit it decodes, holding its output's worth of history (not the 16 MiB window), and nothing after", "");
        printf("  5,000 bytes with a 16 MiB window: at most %u bytes held\n", (unsigned)g_count.peak);
        n = vector("html-q5-w10", g_in, &crc, &want);
        e = decode_steps(counting(0), 0, g_in, n, 50, 70, g_out, sizeof g_out, &made, &ended, &used);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && ended && made == want && want > 4 * 1024 && proven_crc32((proven_mem_view_t){ g_out, made }) == crc && g_count.peak < 40 * 1024,
            "a stream five times longer than its 1 KiB window decodes, the history going round", "Check room: once the history is full size, a byte goes only where one already taken was.");
        printf("  the same with a 1 KiB window: at most %u bytes held\n", (unsigned)g_count.peak);
        n = vector("one-q5-w16", g_in, &crc, &want);
        e = decode_steps(counting(0), 0, g_in, n, n, 10, g_out, sizeof g_out, &made, &ended, &used);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && made == 1 && g_count.peak < 40 * 1024, "a stream of one byte costs the decoder and little more", "");
        printf("  one byte: %u bytes held\n", (unsigned)g_count.peak);

        proven_size_t w = 0;
        n = vector("c-q9-w24", g_in, &crc, &want);
        PROVEN_TEST_ASSERT(proven_brotli_decode_all(g_heap, (proven_mem_view_t){ g_in, n }, (proven_mem_mut_t){ g_out, want - 1 }, &w) == PROVEN_ERR_OUT_OF_BOUNDS && w == 0 &&
                           proven_brotli_decode_all(g_heap, (proven_mem_view_t){ g_in, n }, (proven_mem_mut_t){ g_out, want }, &w) == PROVEN_OK && w == want,
            "decode_all into one byte too few is PROVEN_ERR_OUT_OF_BOUNDS; into exactly enough, PROVEN_OK", "");
        g_out[100] = 0x5a;
        PROVEN_TEST_ASSERT(decode_steps(g_heap, 0, g_in, n, n, 100, g_out, 100, &made, &ended, &used) == PROVEN_OK && made == 100 && !ended && g_out[100] == 0x5a,
            "a step writes only the bytes it reports produced", "");

        int nomem = 0, other = 0;
        n = vector("context-many-trees", g_in, &crc, &want);
        for (proven_size_t k = 1; k <= 6; ++k) {
            e = decode_steps(counting(k), 0, g_in, n, 100, 1000, g_out, sizeof g_out, &made, &ended, &used);
            if (e == PROVEN_ERR_NOMEM) nomem++;
            else if (e != PROVEN_OK) other++;
            PROVEN_TEST_ASSERT(g_count.live == 0, "nothing is left held after an allocation fails", "");
        }
        PROVEN_TEST_ASSERT(nomem >= 3 && other == 0, "each allocation that fails is PROVEN_ERR_NOMEM", "");
    }

    PROVEN_TEST_SECTION("what every function refuses", "Arguments that cannot be right.", "");
    {
        proven_brotli_decoder_t *b = NULL;
        proven_size_t c = 0, p = 0, w = 0;
        bool done = false;
        PROVEN_TEST_ASSERT(proven_brotli_decoder_create(g_heap, 0, NULL) == PROVEN_ERR_INVALID_ARG &&
                           proven_brotli_decoder_create(g_heap, 1023, &b) == PROVEN_ERR_INVALID_ARG && b == NULL &&
                           proven_brotli_decoder_create(g_heap, ((proven_u64)1 << 31) + 1, &b) == PROVEN_ERR_INVALID_ARG && b == NULL &&
                           proven_brotli_decoder_create((proven_allocator_t){ 0 }, 0, &b) == PROVEN_ERR_INVALID_ARG,
            "no out pointer, a limit under 1 KiB or over 2 GiB, and no allocator are PROVEN_ERR_INVALID_ARG", "");
        PROVEN_TEST_ASSERT(proven_brotli_decoder_create(g_heap, 1024, &b) == PROVEN_OK, "1 KiB is a limit", "");
        PROVEN_TEST_ASSERT(proven_brotli_decode(b, (proven_mem_view_t){ NULL, 5 }, &c, (proven_mem_mut_t){ g_out, 10 }, &p, &done) == PROVEN_ERR_INVALID_ARG &&
                           proven_brotli_decode(b, (proven_mem_view_t){ g_in, 5 }, NULL, (proven_mem_mut_t){ g_out, 10 }, &p, &done) == PROVEN_ERR_INVALID_ARG &&
                           proven_brotli_decode(b, (proven_mem_view_t){ g_in, 5 }, &c, (proven_mem_mut_t){ NULL, 10 }, &p, &done) == PROVEN_ERR_INVALID_ARG &&
                           proven_brotli_decode(b, (proven_mem_view_t){ g_in, 5 }, &c, (proven_mem_mut_t){ g_out, 10 }, NULL, &done) == PROVEN_ERR_INVALID_ARG &&
                           proven_brotli_decode(b, (proven_mem_view_t){ g_in, 5 }, &c, (proven_mem_mut_t){ g_out, 10 }, &p, NULL) == PROVEN_ERR_INVALID_ARG &&
                           proven_brotli_decode(NULL, (proven_mem_view_t){ g_in, 5 }, &c, (proven_mem_mut_t){ g_out, 10 }, &p, &done) == PROVEN_ERR_INVALID_ARG,
            "null buffers with a length, missing out-parameters and no decoder are PROVEN_ERR_INVALID_ARG", "");
        PROVEN_TEST_ASSERT(proven_brotli_decode(b, (proven_mem_view_t){ NULL, 0 }, &c, (proven_mem_mut_t){ NULL, 0 }, &p, &done) == PROVEN_OK && c == 0 && p == 0 && !done,
            "nothing in and nowhere to put anything is PROVEN_OK with no progress, and not done", "");
        PROVEN_TEST_ASSERT(proven_brotli_decode_all(g_heap, (proven_mem_view_t){ g_in, 5 }, (proven_mem_mut_t){ g_out, 10 }, NULL) == PROVEN_ERR_INVALID_ARG &&
                           proven_brotli_decode_all(g_heap, (proven_mem_view_t){ NULL, 5 }, (proven_mem_mut_t){ g_out, 10 }, &w) == PROVEN_ERR_INVALID_ARG,
            "and decode_all without its out-parameter, or with a null buffer that has a length", "");
        proven_brotli_decoder_destroy(b);
        proven_brotli_decoder_destroy(NULL);
        proven_brotli_decoder_reset(NULL);
    }

    PROVEN_TEST_PASS("Brotli: reads what libbrotli writes, refuses what libbrotli refuses, and stays inside its bounds.");
    return 0;
}
#endif
