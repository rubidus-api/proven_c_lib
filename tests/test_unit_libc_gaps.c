#include "proven.h"
#include "proven/bit.h"
#include "proven/bytes.h"
#include "proven_test.h"
#include <stdint.h>
#include <string.h>

/*
 * RFC-0058's first group: bit.h against naive definitions (every 8- and 16-bit input, sampled
 * 32- and 64-bit ones, every zero case), bytes.h's searches against naive loops on views that end
 * at an allocation's last byte (so ASan sees any read past the view), the ASCII classes on all
 * 256 bytes, proven_err_name for every code, and the u8 integer parser's edges.
 */

static unsigned n_ones(proven_u64 x) { unsigned n = 0; while (x) { n += (unsigned)(x & 1); x >>= 1; } return n; }
static unsigned n_lz(proven_u64 x, unsigned w) { unsigned n = 0; for (int i = (int)w - 1; i >= 0 && !((x >> i) & 1); i--) n++; return n; }
static unsigned n_tz(proven_u64 x, unsigned w) { unsigned n = 0; for (unsigned i = 0; i < w && !((x >> i) & 1); i++) n++; return n; }
static proven_u64 g_seed = 0x9e3779b97f4a7c15u;
static proven_u64 rnd(void) { g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17; return g_seed; }

int main(void) {
    PROVEN_TEST_SUITE("libc gaps: bit.h, bytes.h, error names, integer parsing in any base",
        "RFC-0058's first group against naive references: every small input, sampled large ones, every zero and edge case.",
        "Inspect include/proven/bit.h, include/proven/bytes.h, src/proven/bytes.c, src/proven/error.c and u8str.c's parser.");

    PROVEN_TEST_SECTION("bit.h", "Counts, widths, powers of two and rotations: every 8- and 16-bit input, 2 million 32- and 64-bit ones.", "Check the zero guards and the narrowing of the 64-bit core.");
    {
        long bad = 0;
        for (unsigned v = 0; v < 65536; v++) {
            const proven_u16 x = (proven_u16)v;
            const proven_u8 y = (proven_u8)v;
            if (proven_bit_count_ones_u16(x) != n_ones(x) || proven_bit_leading_zeros_u16(x) != n_lz(x, 16) || proven_bit_trailing_zeros_u16(x) != n_tz(x, 16)) bad++;
            if (proven_bit_count_ones_u8(y) != n_ones(y) || proven_bit_leading_zeros_u8(y) != n_lz(y, 8) || proven_bit_trailing_zeros_u8(y) != n_tz(y, 8)) bad++;
            if (proven_bit_leading_ones_u16(x) != n_lz((proven_u16)~x, 16) || proven_bit_trailing_ones_u8(y) != n_tz((proven_u8)~y, 8)) bad++;
            const unsigned w = 16 - n_lz(x, 16);
            if (proven_bit_width_u16(x) != w || proven_bit_floor_u16(x) != (x ? (proven_u16)(1u << (w - 1)) : 0)) bad++;
            unsigned c = 1;
            while (c < x) c <<= 1;
            if (proven_bit_ceil_u16(x) != (c >= 65536 ? 0 : (proven_u16)c) || proven_bit_has_single_u16(x) != (n_ones(x) == 1)) bad++;
            for (unsigned r = 0; r < 40; r += 7) {
                const proven_u16 rl = (r % 16) ? (proven_u16)((x << (r % 16)) | (x >> (16 - r % 16))) : x;
                if (proven_bit_rotl_u16(x, r) != rl || proven_bit_rotr_u16(rl, r) != x) bad++;
            }
            if (PROVEN_BIT_COUNT_ONES(x) != n_ones(x) || PROVEN_BIT_WIDTH(y) != 8 - n_lz(y, 8)) bad++;
        }
        for (long i = 0; i < 2000000; i++) {
            const proven_u64 x = rnd() >> (rnd() & 63);
            const proven_u32 z = (proven_u32)x;
            if (proven_bit_count_ones_u64(x) != n_ones(x) || proven_bit_leading_zeros_u64(x) != n_lz(x, 64) || proven_bit_trailing_zeros_u64(x) != n_tz(x, 64)) bad++;
            if (proven_bit_count_ones_u32(z) != n_ones(z) || proven_bit_leading_zeros_u32(z) != n_lz(z, 32) || proven_bit_trailing_zeros_u32(z) != n_tz(z, 32)) bad++;
            if (proven_bit_rotr_u64(proven_bit_rotl_u64(x, (unsigned)i), (unsigned)i) != x || proven_bit_byteswap_u64(proven_bit_byteswap_u64(x)) != x) bad++;
            proven_byte_t b[8];
            proven_store_u64_be(b, x);
            if (proven_load_u64_be(b) != x || b[0] != (proven_byte_t)(x >> 56)) bad++;
            proven_store_u32_le(b, z);
            if (proven_load_u32_le(b) != z || b[0] != (proven_byte_t)z || proven_load_u32_be(b) != proven_bit_byteswap_u32(z)) bad++;
        }
        PROVEN_TEST_ASSERT(bad == 0, "every function as its naive definition", "");
        PROVEN_TEST_ASSERT(proven_bit_leading_zeros_u64(0) == 64 && proven_bit_trailing_zeros_u32(0) == 32 && proven_bit_width_u64(0) == 0
            && proven_bit_floor_u32(0) == 0 && proven_bit_ceil_u64(0) == 1 && proven_bit_ceil_u64(((proven_u64)1 << 63) + 1) == 0
            && proven_bit_ceil_u8(129) == 0 && proven_bit_leading_ones_u32(UINT32_MAX) == 32 && !proven_bit_has_single_u64(0),
            "every zero and out-of-range case has its documented answer (C23 leaves some undefined)", "");
    }

    PROVEN_TEST_SECTION("bytes.h", "The ASCII classes on all 256 bytes; the searches against naive loops on views ending at the allocation's end.", "Check the word-at-a-time loop's tail and zero_bytes.");
    {
        long bad = 0;
        for (int c = 0; c < 256; c++) {
            const proven_byte_t b = (proven_byte_t)c;
            const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'), digit = c >= '0' && c <= '9';
            const bool space = c == ' ' || (c >= 9 && c <= 13), graph = c > 32 && c < 127;
            if (proven_ascii_is_alpha(b) != letter || proven_ascii_is_digit(b) != digit || proven_ascii_is_alnum(b) != (letter || digit)
                || proven_ascii_is_space(b) != space || proven_ascii_is_graph(b) != graph || proven_ascii_is_print(b) != (graph || c == ' ')
                || proven_ascii_is_punct(b) != (graph && !letter && !digit) || proven_ascii_is_cntrl(b) != (c < 32 || c == 127)
                || proven_ascii_is_xdigit(b) != (digit || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
                || proven_ascii_upper(b) != ((c >= 'a' && c <= 'z') ? c - 32 : c) || proven_ascii_lower(b) != ((c >= 'A' && c <= 'Z') ? c + 32 : c)) bad++;
            const unsigned dv = digit ? (unsigned)(c - '0') : letter ? (unsigned)((c | 32) - 'a' + 10) : 255;
            if (proven_ascii_digit_value(b) != dv) bad++;
        }
        PROVEN_TEST_ASSERT(bad == 0, "the ASCII classes, case and digit values of every byte; nothing from 0x80 is in a class", "");
        proven_allocator_t alloc = proven_heap_allocator();
        bad = 0;
        for (long it = 0; it < 200000; it++) {
            const proven_size_t n = (proven_size_t)(rnd() % 200);
            proven_byte_t *p = NULL;
            if (n) { const proven_result_mem_mut_t r = alloc.alloc_fn(alloc.ctx, n, 1); if (r.err != PROVEN_OK) { bad++; continue; } p = r.value.ptr; }
            for (proven_size_t i = 0; i < n; i++) p[i] = (proven_byte_t)(250 + rnd() % 6);
            const proven_mem_view_t v = { p, n };
            const proven_byte_t b = (proven_byte_t)(250 + rnd() % 6);
            proven_size_t first = PROVEN_INDEX_NOT_FOUND, last = PROVEN_INDEX_NOT_FOUND, span = 0, span_not = 0;
            for (proven_size_t i = 0; i < n; i++) if (p[i] == b) { if (first == PROVEN_INDEX_NOT_FOUND) first = i; last = i; }
            const proven_byte_t pair[2] = { b, (proven_byte_t)(b + 1) };
            const proven_byteset_t set = proven_byteset_of((proven_mem_view_t){ pair, 2 });
            while (span < n && (p[span] == pair[0] || p[span] == pair[1])) span++;
            while (span_not < n && p[span_not] != pair[0] && p[span_not] != pair[1]) span_not++;
            if (proven_mem_find_byte(v, b) != first || proven_mem_find_last_byte(v, b) != last) bad++;
            if (proven_mem_span(v, &set) != span || proven_mem_span_not(v, &set) != span_not
                || proven_mem_find_any(v, &set) != (span_not < n ? span_not : PROVEN_INDEX_NOT_FOUND)) bad++;
            if (p) alloc.free_fn(alloc.ctx, p);
        }
        PROVEN_TEST_ASSERT(bad == 0, "200,000 views: memchr, the last byte, strspn, strcspn and strpbrk as naive loops", "");
    }

    PROVEN_TEST_SECTION("error names", "Every code has its own name and a message.", "Add the code's row to src/proven/error.c's table.");
    {
        int bad = 0;
        for (int i = 0; i <= (int)PROVEN_ERR_LAST; i++) {
            const char *n = proven_err_name((proven_err_t)i), *m = proven_err_message((proven_err_t)i);
            if (!n || !m || !m[0] || strncmp(n, "PROVEN_", 7) != 0 || !strcmp(n, "PROVEN_ERR_UNKNOWN")) bad++;
            for (int j = 0; j < i; j++) if (!strcmp(n, proven_err_name((proven_err_t)j))) bad++;
        }
        PROVEN_TEST_ASSERT(bad == 0 && !strcmp(proven_err_name(PROVEN_OK), "PROVEN_OK") && !strcmp(proven_err_name(PROVEN_ERR_NOMEM), "PROVEN_ERR_NOMEM")
            && !strcmp(proven_err_name(PROVEN_ERR_CANCELLED), "PROVEN_ERR_CANCELLED") && !strcmp(proven_err_name((proven_err_t)0xfff), "PROVEN_ERR_UNKNOWN")
            && !strcmp(proven_err_message((proven_err_t)0xfff), "unknown error"),
            "every code from PROVEN_OK to PROVEN_ERR_LAST has a distinct name and a message; an unknown code is named as such", "");
    }

    PROVEN_TEST_SECTION("integer parsing", "Every base 2-36, the limits, signs, prefixes and refusals.", "Compare u8_parse_mag with u16str_view.c's parse_mag.");
    {
        proven_u64 u = 0; proven_i64 s = 0; proven_u32 u32 = 0; proven_i32 s32 = 0; proven_size_t used = 0;
        PROVEN_TEST_ASSERT(proven_u8str_view_parse_u64(PROVEN_LIT("18446744073709551615"), 10, NULL, &u) == PROVEN_OK && u == UINT64_MAX
            && proven_u8str_view_parse_u64(PROVEN_LIT("18446744073709551616"), 10, NULL, &u) == PROVEN_ERR_OVERFLOW
            && proven_u8str_view_parse_i64(PROVEN_LIT("-9223372036854775808"), 10, NULL, &s) == PROVEN_OK && s == INT64_MIN
            && proven_u8str_view_parse_i64(PROVEN_LIT("9223372036854775808"), 10, NULL, &s) == PROVEN_ERR_OVERFLOW
            && proven_u8str_view_parse_u64(PROVEN_LIT("zz"), 36, NULL, &u) == PROVEN_OK && u == 35 * 36 + 35
            && proven_u8str_view_parse_u64(PROVEN_LIT("ZZ"), 36, NULL, &u) == PROVEN_OK && u == 1295
            && proven_u8str_view_parse_u64(PROVEN_LIT("1011"), 2, NULL, &u) == PROVEN_OK && u == 11
            && proven_u8str_view_parse_u64(PROVEN_LIT("102"), 2, NULL, &u) == PROVEN_ERR_INVALID_ARG
            && proven_u8str_view_parse_u64(PROVEN_LIT("102"), 2, &used, &u) == PROVEN_OK && u == 2 && used == 2
            && proven_u8str_view_parse_u64(PROVEN_LIT("0xFF"), 16, NULL, &u) == PROVEN_OK && u == 255
            && proven_u8str_view_parse_u64(PROVEN_LIT("0xg"), 16, &used, &u) == PROVEN_OK && u == 0 && used == 1
            && proven_u8str_view_parse_u64(PROVEN_LIT("-1"), 10, NULL, &u) == PROVEN_ERR_INVALID_ARG
            && proven_u8str_view_parse_u64(PROVEN_LIT(" 1"), 10, NULL, &u) == PROVEN_ERR_INVALID_ARG
            && proven_u8str_view_parse_u64(PROVEN_LIT("1"), 37, NULL, &u) == PROVEN_ERR_INVALID_ARG
            && proven_u8str_view_parse_u64(PROVEN_LIT("1"), 1, NULL, &u) == PROVEN_ERR_INVALID_ARG
            && proven_u8str_view_parse_u64(PROVEN_LIT(""), 10, NULL, &u) == PROVEN_ERR_INVALID_ARG
            && proven_u8str_view_parse_u32(PROVEN_LIT("4294967296"), 10, NULL, &u32) == PROVEN_ERR_OVERFLOW
            && proven_u8str_view_parse_i32(PROVEN_LIT("-2147483648"), 10, NULL, &s32) == PROVEN_OK && s32 == INT32_MIN
            && proven_u8str_view_parse_i32(PROVEN_LIT("+7f"), 16, NULL, &s32) == PROVEN_OK && s32 == 127,
            "limits, overflow, bases 2-36 in either case, partial reads, the 0x prefix, signs and refusals", "");
        int bad = 0;
        for (unsigned base = 2; base <= 36; base++)
            for (int k = 0; k < 2000; k++) {
                const proven_u64 v = rnd() >> (rnd() & 63);
                char buf[80]; int n = 0; proven_u64 t = v;
                char rev[80]; int m = 0;
                do { const unsigned d = (unsigned)(t % base); rev[m++] = (char)(d < 10 ? '0' + d : (k & 1 ? 'A' : 'a') + d - 10); t /= base; } while (t);
                while (m) buf[n++] = rev[--m];
                proven_u64 got = 0;
                if (proven_u8str_view_parse_u64((proven_u8str_view_t){ (const proven_byte_t *)buf, (proven_size_t)n }, base, NULL, &got) != PROVEN_OK || got != v) bad++;
            }
        PROVEN_TEST_ASSERT(bad == 0, "70,000 random values written in every base and read back", "");
    }
    PROVEN_TEST_PASS("libc gaps: all checks passed");
    return 0;
}
