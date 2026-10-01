#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * Written from the contract in include/proven/utf.h (TESTING.md section 5.1).
 *
 * UTF-8 validity is judged against a SECOND formulation of the standard, not against the
 * implementation's own table: `ref_utf8_valid` decodes naively - any lead pattern, any
 * continuation byte - and only then rejects by code point (overlong for its length, surrogate,
 * above U+10FFFF). The implementation instead narrows the second byte's range per lead byte
 * (Unicode table 3-7). Two routes to the same answer, compared over every 1-, 2- and 3-byte
 * input and a sweep of 4-byte ones, is the check that the table has no hole in it.
 *
 * Every scalar value U+0000..U+10FFFF is then round-tripped through both directions and compared
 * with a reference UTF-16 encoding, which pins the surrogate arithmetic at both ends.
 */

static proven_u8str_view_t bv(const void *p, proven_size_t n) {
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)p, .size = n };
}

/* Naive decode, then judge the code point. Returns the length of one valid sequence at s, 0 if
 * the input ends inside a sequence whose seen bytes are all well-formed, -1 if malformed. */
static int ref_utf8_one(const unsigned char *s, size_t n) {
    unsigned b0 = s[0];
    int len;
    unsigned long cp;
    if (b0 < 0x80) return 1;
    else if ((b0 & 0xE0) == 0xC0) { len = 2; cp = b0 & 0x1F; }
    else if ((b0 & 0xF0) == 0xE0) { len = 3; cp = b0 & 0x0F; }
    else if ((b0 & 0xF8) == 0xF0) { len = 4; cp = b0 & 0x07; }
    else return -1;
    int avail = (int)(n < (size_t)len ? n : (size_t)len);
    for (int i = 1; i < avail; ++i) {
        if ((s[i] & 0xC0) != 0x80) return -1;
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    if (avail < len) {
        /* Incomplete. Still malformed if no completion could be valid: pad with the smallest
         * and the largest continuation and see whether either lands on a scalar value. */
        unsigned long lo = cp, hi = cp;
        for (int i = avail; i < len; ++i) { lo = (lo << 6); hi = (hi << 6) | 0x3F; }
        unsigned long min_for_len = (len == 2) ? 0x80 : (len == 3) ? 0x800 : 0x10000;
        if (hi < min_for_len || lo > 0x10FFFF) return -1;
        if (lo >= 0xD800 && hi <= 0xDFFF) return -1;
        return 0;
    }
    unsigned long min_for_len = (len == 2) ? 0x80 : (len == 3) ? 0x800 : 0x10000;
    if (cp < min_for_len) return -1;
    if (cp >= 0xD800 && cp <= 0xDFFF) return -1;
    if (cp > 0x10FFFF) return -1;
    return len;
}

/* Whole-string reference: 1 valid, 0 ends mid-character, -1 malformed. */
static int ref_utf8_valid(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        int r = ref_utf8_one(s + i, n - i);
        if (r <= 0) return r;
        i += (size_t)r;
    }
    return 1;
}

/* Reference for proven_utf8_decode_next at the start of s[0..n): the step the standard defines,
 * built from ref_utf8_one alone. A malformed step covers the maximal subpart - the longest
 * prefix that is still an incomplete-but-valid start - or one byte when there is none. */
typedef struct { int kind; size_t len; } ref_step_t;   /* kind: 1 ok, 0 need more, -1 malformed */
static ref_step_t ref_decode_step(const unsigned char *s, size_t n) {
    int r = ref_utf8_one(s, n);
    if (r > 0) return (ref_step_t){ 1, (size_t)r };
    if (r == 0) return (ref_step_t){ 0, n };
    size_t span = 1;
    for (size_t k = 1; k < n && k < 4; ++k) {
        if (ref_utf8_one(s, k) == 0) span = k;
    }
    return (ref_step_t){ -1, span };
}

static size_t ref_utf8_encode(unsigned long cp, unsigned char *o) {
    if (cp < 0x80) { o[0] = (unsigned char)cp; return 1; }
    if (cp < 0x800) { o[0] = (unsigned char)(0xC0 | (cp >> 6)); o[1] = (unsigned char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (unsigned char)(0xE0 | (cp >> 12)); o[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (unsigned char)(0x80 | (cp & 0x3F)); return 3;
    }
    o[0] = (unsigned char)(0xF0 | (cp >> 18)); o[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
}

/* An allocator that refuses after a budget of successful calls, to prove the grow forms undo. */
typedef struct { int budget; } budget_ctx_t;
static proven_allocator_t g_heap;
static proven_result_mem_mut_t b_alloc(void *ctx, proven_size_t size, proven_size_t align) {
    budget_ctx_t *b = ctx;
    if (b->budget-- <= 0) return (proven_result_mem_mut_t){ .err = PROVEN_ERR_NOMEM };
    return g_heap.alloc_fn(g_heap.ctx, size, align);
}
static proven_result_mem_mut_t b_realloc(void *ctx, void *p, proven_size_t o, proven_size_t n, proven_size_t align) {
    budget_ctx_t *b = ctx;
    if (b->budget-- <= 0) return (proven_result_mem_mut_t){ .err = PROVEN_ERR_NOMEM };
    return g_heap.realloc_fn(g_heap.ctx, p, o, n, align);
}
static void b_free(void *ctx, void *p) { (void)ctx; g_heap.free_fn(g_heap.ctx, p); }

int main(void) {
    PROVEN_TEST_SUITE("UTF-8 and UTF-16 transcoding",
        "Every scalar value round-trips; malformed input is refused, input cut mid-character is NEED_MORE in the partial forms and malformed in the whole forms, and nothing is written on a refusal.",
        "Inspect src/proven/utf.c. A validity mismatch names the input bytes; compare the lead byte's second-byte range with Unicode table 3-7.");

    g_heap = proven_heap_allocator();
    proven_u16 u16[64];
    proven_byte_t u8[64];
    proven_size_t w = 0;

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("known text, both directions",
        "ASCII, a two-byte Latin letter, three-byte Hangul and a four-byte emoji, which becomes a surrogate pair.",
        "");
    // ---------------------------------------------------------------
    {
        const char *s = "A\xC3\xA9\xED\x95\x9C\xF0\x9F\x98\x80";   /* A, e-acute, a Hangul syllable, an emoji */
        const proven_u16 want[] = { 0x0041, 0x00E9, 0xD55C, 0xD83D, 0xDE00 };
        proven_err_t e = proven_utf8_to_utf16(bv(s, strlen(s)), u16, 64, &w);
        PROVEN_TEST_ASSERT(proven_is_ok(e) && w == 5 && memcmp(u16, want, sizeof want) == 0,
            "UTF-8 must decode to the expected code units, the emoji as D83D DE00", "");
        PROVEN_TEST_ASSERT(proven_utf8_to_utf16_size(bv(s, strlen(s))).value == 5, "size must agree", "");
        PROVEN_TEST_ASSERT(proven_utf16_to_utf8_size(want, 5).value == strlen(s), "reverse size must agree", "");
        e = proven_utf16_to_utf8(want, 5, u8, 64, &w);
        PROVEN_TEST_ASSERT(proven_is_ok(e) && w == strlen(s) && memcmp(u8, s, w) == 0,
            "UTF-16 must encode back to the same bytes", "");
        e = proven_utf8_to_utf16(bv("", 0), u16, 0, &w);
        PROVEN_TEST_ASSERT(proven_is_ok(e) && w == 0, "empty in, empty out, even with no output buffer", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("every scalar value round-trips",
        "U+0000..U+10FFFF minus the surrogates: UTF-8 -> UTF-16 matches the reference encoding, and back again is the identity.",
        "A failure at U+10000 or U+10FFFF is the surrogate arithmetic; at U+0800 or U+FFFF, a three-byte boundary.");
    // ---------------------------------------------------------------
    for (unsigned long cp = 0; cp <= 0x10FFFF; ++cp) {
        if (cp >= 0xD800 && cp <= 0xDFFF) continue;
        unsigned char enc[4];
        size_t n = ref_utf8_encode(cp, enc);
        proven_u16 want[2];
        size_t wn;
        if (cp < 0x10000) { want[0] = (proven_u16)cp; wn = 1; }
        else { want[0] = (proven_u16)(0xD800 + ((cp - 0x10000) >> 10)); want[1] = (proven_u16)(0xDC00 + ((cp - 0x10000) & 0x3FF)); wn = 2; }

        proven_err_t e = proven_utf8_to_utf16(bv(enc, n), u16, 2, &w);
        if (!proven_is_ok(e) || w != wn || memcmp(u16, want, wn * sizeof(proven_u16)) != 0) {
            PROVEN_TEST_INFO("code point U+{:X} failed UTF-8 -> UTF-16", PROVEN_ARG((unsigned long long)cp));
            PROVEN_TEST_ASSERT(false, "every scalar value must decode to its reference UTF-16", "");
        }
        e = proven_utf16_to_utf8(want, wn, u8, 4, &w);
        if (!proven_is_ok(e) || w != n || memcmp(u8, enc, n) != 0) {
            PROVEN_TEST_INFO("code point U+{:X} failed UTF-16 -> UTF-8", PROVEN_ARG((unsigned long long)cp));
            PROVEN_TEST_ASSERT(false, "every scalar value must encode back to its reference UTF-8", "");
        }
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("UTF-8 validity agrees with a second formulation over every short input",
        "All 256 one-byte, 65,536 two-byte and 16,777,216 three-byte inputs, plus four-byte inputs over every lead and second byte: the size function's verdict and the partial form's stop must both match the reference.",
        "The reference decodes naively and rejects by code point; the implementation narrows byte ranges by lead byte. A mismatch is a hole in one of the two.");
    // ---------------------------------------------------------------
    {
        unsigned char s[4];
        unsigned long mismatches = 0;
        for (size_t len = 1; len <= 3; ++len) {
            unsigned long total = 1UL << (8 * len);
            for (unsigned long v = 0; v < total; ++v) {
                for (size_t i = 0; i < len; ++i) s[i] = (unsigned char)(v >> (8 * i));
                int ref = ref_utf8_valid(s, len);
                proven_result_size_t sz = proven_utf8_to_utf16_size(bv(s, len));
                bool whole_ok = proven_is_ok(sz.err);
                proven_utf_step_t st = proven_utf8_to_utf16_partial(bv(s, len), u16, 64);
                bool partial_ok = (ref == 1  && st.err == PROVEN_OK) ||
                                  (ref == 0  && st.err == PROVEN_ERR_NEED_MORE) ||
                                  (ref == -1 && st.err == PROVEN_ERR_INVALID_ENCODING);
                if (whole_ok != (ref == 1) || !partial_ok) {
                    if (mismatches++ < 5) {
                        PROVEN_TEST_INFO("len {} bytes {:x} {:x} {:x} ref {} whole {} partial {}",
                            PROVEN_ARG((int)len), PROVEN_ARG((unsigned)s[0]), PROVEN_ARG((unsigned)s[1]),
                            PROVEN_ARG((unsigned)s[2]), PROVEN_ARG(ref), PROVEN_ARG((int)sz.err), PROVEN_ARG((int)st.err));
                    }
                }
            }
        }
        for (unsigned b0 = 0xF0; b0 <= 0xFF; ++b0) {
            for (unsigned b1 = 0; b1 < 256; ++b1) {
                for (unsigned b2 = 0x70; b2 < 0xD0; b2 += 0x0F) {
                    for (unsigned b3 = 0x70; b3 < 0xD0; b3 += 0x0B) {
                        s[0] = (unsigned char)b0; s[1] = (unsigned char)b1; s[2] = (unsigned char)b2; s[3] = (unsigned char)b3;
                        int ref = ref_utf8_valid(s, 4);
                        proven_result_size_t sz = proven_utf8_to_utf16_size(bv(s, 4));
                        if (proven_is_ok(sz.err) != (ref == 1)) {
                            if (mismatches++ < 5) {
                                PROVEN_TEST_INFO("4-byte {:x} {:x} {:x} {:x} ref {}", PROVEN_ARG(b0), PROVEN_ARG(b1),
                                                 PROVEN_ARG(b2), PROVEN_ARG(b3), PROVEN_ARG(ref));
                            }
                        }
                    }
                }
            }
        }
        PROVEN_TEST_ASSERT(mismatches == 0, "the implementation and the reference must agree on every input", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("decoding one character at a time agrees with the reference over every short input",
        "proven_utf8_decode_next on all 1-, 2- and 3-byte inputs and the same four-byte sweep: its verdict, its step length - the maximal subpart for malformed input - and, for a character, a code point whose reference encoding is exactly the bytes it consumed.",
        "A wrong length for malformed input resynchronises in the wrong place: compare the step with the longest prefix the reference still calls a valid start.");
    // ---------------------------------------------------------------
    {
        unsigned char s[4];
        unsigned long mismatches = 0;
        for (size_t len = 1; len <= 4; ++len) {
            unsigned long total = (len <= 3) ? (1UL << (8 * len)) : 16UL * 256UL * 7UL * 9UL;
            for (unsigned long v = 0; v < total; ++v) {
                if (len <= 3) {
                    for (size_t i = 0; i < len; ++i) s[i] = (unsigned char)(v >> (8 * i));
                } else {
                    unsigned long w = v;
                    s[3] = (unsigned char)(0x70 + (w % 9) * 0x0B); w /= 9;
                    s[2] = (unsigned char)(0x70 + (w % 7) * 0x0F); w /= 7;
                    s[1] = (unsigned char)(w % 256); w /= 256;
                    s[0] = (unsigned char)(0xF0 + w);
                }
                ref_step_t ref = ref_decode_step(s, len);
                proven_utf8_char_t c = proven_utf8_decode_next(bv(s, len), 0);
                bool ok = c.len == ref.len &&
                          ((ref.kind == 1 && c.err == PROVEN_OK) ||
                           (ref.kind == 0 && c.err == PROVEN_ERR_NEED_MORE) ||
                           (ref.kind == -1 && c.err == PROVEN_ERR_INVALID_ENCODING && c.cp == 0));
                if (ok && ref.kind == 1) {
                    unsigned char e[4];
                    ok = ref_utf8_encode(c.cp, e) == c.len && memcmp(e, s, c.len) == 0;
                }
                if (!ok && mismatches++ < 5) {
                    PROVEN_TEST_INFO("len {} bytes {:x} {:x} {:x} {:x}: ref kind {} len {}, got err {} len {} cp {:x}",
                        PROVEN_ARG((int)len), PROVEN_ARG((unsigned)s[0]), PROVEN_ARG((unsigned)s[1]), PROVEN_ARG((unsigned)s[2]),
                        PROVEN_ARG((unsigned)s[3]), PROVEN_ARG(ref.kind), PROVEN_ARG((int)ref.len), PROVEN_ARG((int)c.err),
                        PROVEN_ARG((int)c.len), PROVEN_ARG((unsigned)c.cp));
                }
            }
        }
        PROVEN_TEST_ASSERT(mismatches == 0, "every one-character step must match the reference", "");

        /* Walking text by `len` visits every byte once and stops exactly at the end, through
         * characters, malformed bytes and a cut-off tail alike. */
        static const unsigned char text[] = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\xC0\x80\xED\xA0\x80\xF4\x90z\xE2\x82";
        proven_u8str_view_t tv = bv(text, sizeof text - 1);
        proven_size_t pos = 0, steps = 0, chars = 0, bad = 0, more = 0;
        while (pos < tv.size) {
            proven_utf8_char_t c = proven_utf8_decode_next(tv, pos);
            PROVEN_TEST_ASSERT(c.len >= 1, "a step inside the text always advances", "");
            if (c.err == PROVEN_OK) ++chars;
            else if (c.err == PROVEN_ERR_INVALID_ENCODING) ++bad;
            else if (c.err == PROVEN_ERR_NEED_MORE) ++more;
            pos += c.len;
            ++steps;
        }
        PROVEN_TEST_ASSERT(pos == tv.size, "the walk ends exactly at the end", "");
        /* a, e-acute, euro, emoji, z = 5 characters; C0 / 80 / ED / A0 / 80 / F4 / 90 = 7 maximal
         * subparts of one byte each; E2 82 = one cut-off character. */
        PROVEN_TEST_ASSERT(chars == 5 && bad == 7 && more == 1, "the walk sees five characters, seven malformed steps and one cut-off tail", "");
        PROVEN_TEST_ASSERT(proven_utf8_decode_next(tv, tv.size).err == PROVEN_ERR_EOF, "at the end: EOF", "");
        PROVEN_TEST_ASSERT(proven_utf8_decode_next(tv, tv.size + 1).err == PROVEN_ERR_OUT_OF_BOUNDS, "past the end: OUT_OF_BOUNDS", "");
        PROVEN_TEST_ASSERT(proven_utf8_decode_next((proven_u8str_view_t){ NULL, 3 }, 0).err == PROVEN_ERR_INVALID_ARG,
            "a NULL view with a size: INVALID_ARG", "");
        (void)steps;
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("UTF-16 validity over every unit, alone and before every kind of neighbour",
        "Each of the 65,536 units alone, and each surrogate followed by the units either side of every surrogate boundary: valid exactly when a high surrogate (D800-DBFF) is followed by a low one (DC00-DFFF), and a lone unit is valid exactly when it is not a surrogate.",
        "A mismatch inside DC00-DFFF means a lone low surrogate was taken for a high one.");
    // ---------------------------------------------------------------
    {
        static const proven_u16 next[] = { 0x0000, 0x0041, 0xD7FF, 0xD800, 0xDBFF, 0xDC00, 0xDFFF, 0xE000, 0xFFFF };
        unsigned long mismatches = 0;
        for (unsigned long u = 0; u <= 0xFFFF; ++u) {
            proven_u16 one[1] = { (proven_u16)u };
            bool is_sur = u >= 0xD800 && u <= 0xDFFF;
            bool is_high = u >= 0xD800 && u <= 0xDBFF;
            proven_utf_step_t st = proven_utf16_to_utf8_partial(one, 1, u8, 64);
            proven_err_t want = !is_sur ? PROVEN_OK : is_high ? PROVEN_ERR_NEED_MORE : PROVEN_ERR_INVALID_ENCODING;
            if (st.err != want) mismatches++;
            if (proven_is_ok(proven_utf16_to_utf8_size(one, 1).err) == is_sur) mismatches++;
            if (!is_sur) continue;
            for (size_t k = 0; k < sizeof next / sizeof next[0]; ++k) {
                proven_u16 two[2] = { (proven_u16)u, next[k] };
                bool valid = is_high && next[k] >= 0xDC00 && next[k] <= 0xDFFF;
                if (proven_is_ok(proven_utf16_to_utf8_size(two, 2).err) != valid) {
                    if (mismatches++ < 5) PROVEN_TEST_INFO("pair {:x} {:x}", PROVEN_ARG(u), PROVEN_ARG((unsigned)next[k]));
                }
            }
        }
        PROVEN_TEST_ASSERT(mismatches == 0, "UTF-16 validity must match the surrogate rules exactly", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the malformed forms the standard names are refused",
        "Overlong, encoded surrogate, above U+10FFFF, stray continuation, a lead byte that can never start a character, and a bad continuation - each INVALID_ENCODING, and `consumed` points at it.",
        "");
    // ---------------------------------------------------------------
    {
        static const struct { const char *bytes; size_t n; } bad[] = {
            { "\xC0\x80", 2 }, { "\xC1\xBF", 2 }, { "\xE0\x80\x80", 3 }, { "\xE0\x9F\xBF", 3 },
            { "\xED\xA0\x80", 3 }, { "\xED\xBF\xBF", 3 }, { "\xF0\x8F\xBF\xBF", 4 },
            { "\xF4\x90\x80\x80", 4 }, { "\xF5\x80\x80\x80", 4 }, { "\x80", 1 }, { "\xBF", 1 },
            { "\xFE", 1 }, { "\xFF", 1 }, { "\xE0\xA0\x41", 3 }, { "\xC3\xC3", 2 },
        };
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
            char buf[8] = { 'o', 'k' };
            memcpy(buf + 2, bad[i].bytes, bad[i].n);
            proven_utf_step_t st = proven_utf8_to_utf16_partial(bv(buf, bad[i].n + 2), u16, 64);
            PROVEN_TEST_ASSERT(st.err == PROVEN_ERR_INVALID_ENCODING && st.consumed == 2 && st.written == 2,
                "a malformed sequence must stop the conversion exactly where it starts", "");
            PROVEN_TEST_ASSERT(proven_utf8_to_utf16_size(bv(buf, bad[i].n + 2)).err == PROVEN_ERR_INVALID_ENCODING,
                "and the size function must refuse it", "");
        }
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("cut mid-character is NEED_MORE in pieces, malformed as a whole",
        "A three-byte Hangul syllable split after one and after two bytes, a four-byte emoji split after three, and a high surrogate at the end of UTF-16.",
        "If a partial conversion reports INVALID_ENCODING here, text read in pieces cannot be converted.");
    // ---------------------------------------------------------------
    {
        const char *s = "A\xED\x95\x9C";
        for (size_t cut = 2; cut <= 3; ++cut) {
            proven_utf_step_t st = proven_utf8_to_utf16_partial(bv(s, cut), u16, 64);
            PROVEN_TEST_ASSERT(st.err == PROVEN_ERR_NEED_MORE && st.consumed == 1 && st.written == 1,
                "a partial must convert the 'A' and stop before the incomplete syllable", "");
            PROVEN_TEST_ASSERT(proven_utf8_to_utf16_size(bv(s, cut)).err == PROVEN_ERR_INVALID_ENCODING,
                "a whole text that ends mid-character is malformed", "");
            proven_err_t e = proven_utf8_to_utf16(bv(s, cut), u16, 64, &w);
            PROVEN_TEST_ASSERT(e == PROVEN_ERR_INVALID_ENCODING && w == 0, "the atomic form refuses it too", "");
        }
        proven_utf_step_t st = proven_utf8_to_utf16_partial(bv("\xF0\x9F\x98", 3), u16, 64);
        PROVEN_TEST_ASSERT(st.err == PROVEN_ERR_NEED_MORE && st.consumed == 0, "three of four emoji bytes is NEED_MORE", "");

        const proven_u16 hi_end[] = { 0x0041, 0xD83D };
        st = proven_utf16_to_utf8_partial(hi_end, 2, u8, 64);
        PROVEN_TEST_ASSERT(st.err == PROVEN_ERR_NEED_MORE && st.consumed == 1 && st.written == 1,
            "a trailing high surrogate is NEED_MORE: its low half may be in the next piece", "");
        PROVEN_TEST_ASSERT(proven_utf16_to_utf8_size(hi_end, 2).err == PROVEN_ERR_INVALID_ENCODING,
            "and malformed when there is no next piece", "");

        const proven_u16 lone_lo[] = { 0x0041, 0xDE00 };
        st = proven_utf16_to_utf8_partial(lone_lo, 2, u8, 64);
        PROVEN_TEST_ASSERT(st.err == PROVEN_ERR_INVALID_ENCODING && st.consumed == 1, "a lone low surrogate is malformed", "");
        const proven_u16 hi_then_a[] = { 0xD83D, 0x0041 };
        st = proven_utf16_to_utf8_partial(hi_then_a, 2, u8, 64);
        PROVEN_TEST_ASSERT(st.err == PROVEN_ERR_INVALID_ENCODING && st.consumed == 0,
            "a high surrogate followed by anything but a low one is malformed", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a full output never splits a character, and the atomic forms write nothing",
        "One free unit cannot take a surrogate pair; two free bytes cannot take Hangul. A refused atomic call leaves the output as it was.",
        "");
    // ---------------------------------------------------------------
    {
        const char *emoji = "\xF0\x9F\x98\x80";
        proven_utf_step_t st = proven_utf8_to_utf16_partial(bv(emoji, 4), u16, 1);
        PROVEN_TEST_ASSERT(st.err == PROVEN_ERR_OUT_OF_BOUNDS && st.consumed == 0 && st.written == 0,
            "half a surrogate pair must never be written", "");
        const proven_u16 han[] = { 0x0041, 0xD55C };
        st = proven_utf16_to_utf8_partial(han, 2, u8, 3);
        PROVEN_TEST_ASSERT(st.err == PROVEN_ERR_OUT_OF_BOUNDS && st.consumed == 1 && st.written == 1,
            "a three-byte character must not be cut to fit", "");

        memset(u16, 0x5A, sizeof u16);
        proven_err_t e = proven_utf8_to_utf16(bv("abc", 3), u16, 2, &w);
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_OUT_OF_BOUNDS && w == 0 && u16[0] == 0x5A5A,
            "an undersized atomic conversion writes nothing", "");
        memset(u8, 0x5A, sizeof u8);
        e = proven_utf16_to_utf8(han, 2, u8, 3, &w);
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_OUT_OF_BOUNDS && w == 0 && u8[0] == 0x5A, "in both directions", "");
        e = proven_utf8_to_utf16(bv(NULL, 3), u16, 64, &w);
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_INVALID_ARG, "a null input with a size is refused", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the grow forms append, and undo themselves on failure",
        "Appending to a string that already holds text keeps that text; malformed input, aliasing input and an allocator that runs dry all leave the string exactly as it was, terminator included.",
        "A string left one unit longer, or unterminated, after a failure is the rollback missing.");
    // ---------------------------------------------------------------
    {
        proven_result_u16str_t r16 = proven_u16str_create(g_heap, 4);
        PROVEN_TEST_ASSERT(proven_is_ok(r16.err), "create", "");
        proven_u16str_t s16 = r16.value;
        PROVEN_TEST_ASSERT(proven_is_ok(proven_utf8_append_to_u16str(g_heap, &s16, bv("x", 1))), "first append", "");
        PROVEN_TEST_ASSERT(proven_is_ok(proven_utf8_append_to_u16str(g_heap, &s16, bv("\xED\x95\x9C\xEA\xB8\x80", 6))),
            "Hangul appends", "");
        const proven_u16 want[] = { 'x', 0xD55C, 0xAE00, 0 };
        PROVEN_TEST_ASSERT(proven_u16str_len(&s16) == 3 && memcmp(proven_u16str_as_ptr(&s16), want, sizeof want) == 0,
            "the string holds both appends, terminated", "");
        PROVEN_TEST_ASSERT(proven_utf8_append_to_u16str(g_heap, &s16, bv("ok\xFF", 3)) == PROVEN_ERR_INVALID_ENCODING &&
                           proven_u16str_len(&s16) == 3, "malformed input changes nothing", "");

        /* A long text through an allocator that refuses: the string must come back exactly as
         * it was, length and terminator. (The grow now happens once, before any conversion, so
         * there is no half-appended state to undo - this pins that no state is left either.) */
        static char big[3 * 600];
        for (size_t i = 0; i < 600; ++i) memcpy(big + 3 * i, "\xED\x95\x9C", 3);
        budget_ctx_t budget = { 0 };
        proven_allocator_t starved = { &budget, b_alloc, b_realloc, b_free };
        proven_err_t e = proven_utf8_append_to_u16str(starved, &s16, bv(big, sizeof big));
        const proven_u16 *p = proven_u16str_as_ptr(&s16);
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_NOMEM && proven_u16str_len(&s16) == 3 && p[3] == 0,
            "a refused allocation leaves the old length and terminator", "");
        budget.budget = 1;
        e = proven_utf8_append_to_u16str(starved, &s16, bv(big, sizeof big));
        PROVEN_TEST_ASSERT(proven_is_ok(e) && proven_u16str_len(&s16) == 603 && proven_u16str_as_ptr(&s16)[603] == 0 &&
                           proven_u16str_as_ptr(&s16)[602] == 0xD55C,
            "and one allocation is all a 600-syllable append needs", "");
        proven_u16str_destroy(g_heap, &s16);

        proven_result_u8str_t r8 = proven_u8str_create(g_heap, 4);
        PROVEN_TEST_ASSERT(proven_is_ok(r8.err), "create", "");
        proven_u8str_t s8 = r8.value;
        PROVEN_TEST_ASSERT(proven_is_ok(proven_u8str_append_grow(g_heap, &s8, bv("x", 1))), "seed", "");
        const proven_u16 han2[] = { 0xD55C, 0xAE00, 0xD83D, 0xDE00 };
        PROVEN_TEST_ASSERT(proven_is_ok(proven_utf16_append_to_u8str(g_heap, &s8, han2, 4)), "UTF-16 appends", "");
        PROVEN_TEST_ASSERT(s8.internal.len == 11 && memcmp(proven_u8str_as_cstr(&s8), "x\xED\x95\x9C\xEA\xB8\x80\xF0\x9F\x98\x80", 12) == 0,
            "the bytes are the UTF-8 of the units, terminated", "");
        PROVEN_TEST_ASSERT(proven_utf16_append_to_u8str(g_heap, &s8, han2, 3) == PROVEN_ERR_INVALID_ENCODING &&
                           s8.internal.len == 11, "a trailing high surrogate is malformed as a whole and changes nothing", "");
        PROVEN_TEST_ASSERT(proven_utf16_append_to_u8str(g_heap, &s8, (const proven_u16 *)(void *)s8.internal.ptr, 2) == PROVEN_ERR_INVALID_ARG,
            "input from inside the destination is refused before it can move", "");
        proven_u8str_destroy(g_heap, &s8);
    }

    PROVEN_TEST_PASS("UTF-8 and UTF-16 transcoding");
    return 0;
}
