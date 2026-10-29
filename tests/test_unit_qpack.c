#include "proven.h"
#include "proven_test.h"
#include "../src/proven/proven_internal_h3.h"
#include "test_unit_qpack_vectors.h"
#include "test_unit_hpack_vectors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * QPACK (RFC 9204) with a dynamic table of capacity 0: the static table; appendix B.1 decoded
 * byte for byte and B.2 to B.5, which need a dynamic table, refused with their codes; the
 * encoder's choices; field sections through the encoder and decoder; every refusal of the
 * decoder; the limits; the peer's encoder and decoder streams; and every truncation and
 * single-bit change of encoded sections.
 */

#ifndef PROVEN_NO_HTTP3

static proven_size_t unhex(const char *hex, proven_byte_t *out, proven_size_t cap) {
    proven_size_t n = strlen(hex) / 2;
    if (n > cap) { fprintf(stderr, "vector too long\n"); exit(1); }
    for (proven_size_t i = 0; i < n; ++i) {
        char c[3] = { hex[2 * i], hex[2 * i + 1], 0 };
        out[i] = (proven_byte_t)strtoul(c, NULL, 16);
    }
    return n;
}

static proven_u8str_view_t sv(const char *s) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }
static bool sveq(proven_u8str_view_t a, const char *b) { return a.size == strlen(b) && memcmp(a.ptr, b, a.size) == 0; }
static bool same(proven_u8str_view_t a, proven_u8str_view_t b) { return a.size == b.size && (a.size == 0 || memcmp(a.ptr, b.ptr, a.size) == 0); }

static proven_http_header_t fields[512];
static bool never[512];
static proven_byte_t scratch[1 << 16];

static proven_err_t dec(const proven_byte_t *p, proven_size_t n, proven_size_t *count) {
    return proven_qpack_decode((proven_mem_view_t){ p, n }, fields, never, 512, (proven_mem_mut_t){ scratch, sizeof scratch }, 0, count);
}
static proven_err_t dec_hex(const char *hex, proven_size_t *count) {
    static proven_byte_t b[4096];
    const proven_size_t n = unhex(hex, b, sizeof b);
    return dec(b, n, count);
}

static proven_err_t dec_hex_d(proven_qpack_dec_t *d, const char *hex, proven_size_t *count) {
    static proven_byte_t b[256];
    const proven_size_t n = unhex(hex, b, sizeof b);
    proven_u64 ric = 0;
    return proven_qpack_decode_dyn(d, (proven_mem_view_t){ b, n }, fields, never, 512, (proven_mem_mut_t){ scratch, sizeof scratch }, 0, count, &ric);
}

static unsigned long long rng = 0x9e3779b97f4a7c15ull;
static unsigned next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)(rng >> 32); }

int main(void) {
    PROVEN_TEST_SUITE("QPACK: header compression for HTTP/3",
        "QPACK with a dynamic table of capacity 0: RFC 9204's static table and appendix B, the encoder's choices, round trips, every refusal and limit, the peer's encoder and decoder streams, and every change of encoded sections.",
        "Inspect src/proven/qpack.c. src/proven/qpack_static.h and tests/test_unit_qpack_vectors.h are generated from the RFC's text by a private program: regenerate, do not edit.");

    static proven_byte_t out[1 << 16];
    proven_size_t count = 0, len = 0;

    PROVEN_TEST_SECTION("The static table", "Ninety-nine entries numbered from 0, each encoded as one indexed line and decoded back.", "");
    {
        const char *names[][2] = { { ":authority", "" }, { ":path", "/" }, { ":method", "GET" }, { ":status", "200" }, { "x-frame-options", "sameorigin" },
                                   { "content-type", "text/html; charset=utf-8" }, { "strict-transport-security", "max-age=31536000; includesubdomains; preload" } };
        const unsigned want[] = { 0, 1, 17, 25, 98, 52, 58 };
        for (proven_size_t i = 0; i < sizeof want / sizeof want[0]; ++i) {
            proven_http_header_t f = { sv(names[i][0]), sv(names[i][1]) };
            len = 0;
            PROVEN_TEST_ASSERT(proven_qpack_encode(&f, 1, NULL, (proven_mem_mut_t){ out, sizeof out }, &len) == PROVEN_OK, "encodes", "");
            const proven_byte_t one[] = { 0, 0, (proven_byte_t)(want[i] < 63 ? 0xc0 | want[i] : 0xff) };
            PROVEN_TEST_ASSERT(len == (want[i] < 63 ? 3u : 4u) && memcmp(out, one, 3) == 0 && (want[i] < 63 || out[3] == want[i] - 63),
                "a static entry is one indexed field line at its index (RFC 9204 appendix A)", names[i][0]);
            PROVEN_TEST_ASSERT(dec(out, len, &count) == PROVEN_OK && count == 1 && sveq(fields[0].name, names[i][0]) && sveq(fields[0].value, names[i][1]),
                "and decodes back", names[i][0]);
        }
        /* Every index 0 to 98 decodes; 99 does not. */
        for (unsigned i = 0; i < 100; ++i) {
            proven_byte_t b[4] = { 0, 0 };
            proven_size_t n = 2;
            if (i < 63) b[n++] = (proven_byte_t)(0xc0 | i); else { b[n++] = 0xff; b[n++] = (proven_byte_t)(i - 63); }
            const proven_err_t e = dec(b, n, &count);
            PROVEN_TEST_ASSERT(i < 99 ? e == PROVEN_OK && count == 1 && fields[0].name.size > 0 : e == PROVEN_ERR_INVALID_FORMAT, "indices 0 to 98, and no further", "");
        }
    }

    PROVEN_TEST_SECTION("RFC 9204 appendix B", "B.1 decoded to its field; B.2 to B.5 refused, as a table of capacity 0 must: their field sections, their encoder-stream instructions, and their decoder-stream acknowledgements - but not a Stream Cancellation.", "");
    {
        unsigned seen = 0;
        for (proven_size_t i = 0; i < sizeof qpack_examples / sizeof qpack_examples[0]; ++i) {
            static proven_byte_t b[512];
            const proven_size_t n = unhex(qpack_examples[i].hex, b, sizeof b);
            const char *st = qpack_examples[i].stream;
            if (strcmp(qpack_examples[i].section, "B.1") == 0) {
                PROVEN_TEST_ASSERT(dec(b, n, &count) == PROVEN_OK && count == 1 && sveq(fields[0].name, ":path") && sveq(fields[0].value, "/index.html") && !never[0],
                    "B.1: a literal with the static name :path and the value /index.html", "");
                /* This encoder makes the same choice - the static name 1 - and codes the value
                 * with Huffman, which is shorter. */
                proven_http_header_t f = { sv(":path"), sv("/index.html") };
                len = 0;
                PROVEN_TEST_ASSERT(proven_qpack_encode(&f, 1, NULL, (proven_mem_mut_t){ out, sizeof out }, &len) == PROVEN_OK && len == 2 + 1 + 1 + 8 &&
                                   memcmp(out, "\x00\x00\x51\x88", 4) == 0, "the encoder: the same representation, the value Huffman-coded in 8 bytes", "");
                ++seen;
            } else if (strcmp(st, "Encoder") == 0) {
                proven_qpack_stream_t s = { 0 };
                PROVEN_TEST_ASSERT(proven_qpack_encoder_stream_feed(&s, (proven_mem_view_t){ b, n }) == PROVEN_QPACK_ENCODER_STREAM_ERROR,
                    "an encoder stream that sets a capacity or inserts: QPACK_ENCODER_STREAM_ERROR", qpack_examples[i].section);
                ++seen;
            } else if (strcmp(st, "Decoder") == 0) {
                proven_qpack_stream_t s = { 0 };
                const proven_u64 code = proven_qpack_decoder_stream_feed(&s, (proven_mem_view_t){ b, n });
                PROVEN_TEST_ASSERT(b[0] == 0x48 ? code == 0 : code == PROVEN_QPACK_DECODER_STREAM_ERROR,
                    "a Section Acknowledgement or Insert Count Increment: QPACK_DECODER_STREAM_ERROR; a Stream Cancellation: taken", qpack_examples[i].section);
                ++seen;
            } else {
                PROVEN_TEST_ASSERT(dec(b, n, &count) == PROVEN_ERR_INVALID_FORMAT, "a field section that needs the dynamic table: QPACK_DECOMPRESSION_FAILED", qpack_examples[i].section);
                ++seen;
            }
        }
        PROVEN_TEST_ASSERT(seen == 10, "ten streams in the appendix", "");
    }

    PROVEN_TEST_SECTION("The encoder's choices", "A whole static field indexed; a static name with a literal value; a literal name; Huffman only where shorter; the N bit for marked fields and always for credentials.", "");
    {
        const proven_http_header_t f[] = { { sv(":method"), sv("GET") }, { sv(":path"), sv("/x") }, { sv("x-custom"), sv("v") },
                                           { sv("authorization"), sv("Basic abc") }, { sv("cookie"), sv("a=b") }, { sv("x-k"), sv("aaaaaaaaaaaaaaaaaaaa") } };
        const bool marks[] = { false, false, false, false, true, false };
        len = 0;
        PROVEN_TEST_ASSERT(proven_qpack_encode(f, 6, marks, (proven_mem_mut_t){ out, sizeof out }, &len) == PROVEN_OK, "encodes", "");
        PROVEN_TEST_ASSERT(out[2] == 0xd1, ":method GET indexed (17)", "");
        PROVEN_TEST_ASSERT(out[3] == 0x51 && out[4] == 0x02 && out[5] == '/' && out[6] == 'x', ":path by static name 1, a two-byte value left plain (Huffman is no shorter)", "");
        PROVEN_TEST_ASSERT((out[7] & 0xe0) == 0x20 && (out[7] & 0x10) == 0, "x-custom: a literal name, no N bit", "");
        PROVEN_TEST_ASSERT(dec(out, len, &count) == PROVEN_OK && count == 6, "decodes", "");
        for (int i = 0; i < 6; ++i) PROVEN_TEST_ASSERT(same(fields[i].name, f[i].name) && same(fields[i].value, f[i].value), "the same list back", "");
        PROVEN_TEST_ASSERT(!never[0] && !never[1] && !never[2] && never[3] && never[4] && !never[5], "N on authorization (always) and on the marked cookie", "");
        PROVEN_TEST_ASSERT(len < 2 + 1 + 4 + 11 + 14 + 6 + 22, "the long run of one letter Huffman-coded", "");
        /* Too little room: refused before anything is written. */
        len = 0;
        PROVEN_TEST_ASSERT(proven_qpack_encode(f, 6, NULL, (proven_mem_mut_t){ out, proven_qpack_encode_bound(f, 6) - 1 }, &len) == PROVEN_ERR_OUT_OF_BOUNDS && len == 0,
            "less room than the bound: PROVEN_ERR_OUT_OF_BOUNDS", "");
    }

    PROVEN_TEST_SECTION("Round trips", "RFC 7541 appendix C's header lists and four thousand generated ones through the encoder and decoder, marks kept.", "");
    {
        unsigned lists = 0;
        for (proven_size_t i = 0; i < sizeof hpack_examples / sizeof hpack_examples[0]; ++i) {
            proven_http_header_t f[16];
            const unsigned nf = hpack_examples[i].field_count;
            for (unsigned k = 0; k < nf; ++k) f[k] = (proven_http_header_t){ sv(hpack_examples[i].fields[k].name), sv(hpack_examples[i].fields[k].value) };
            len = 0;
            PROVEN_TEST_ASSERT(proven_qpack_encode(f, nf, NULL, (proven_mem_mut_t){ out, sizeof out }, &len) == PROVEN_OK && dec(out, len, &count) == PROVEN_OK && count == nf, "an RFC 7541 list", hpack_examples[i].title);
            for (unsigned k = 0; k < nf; ++k) PROVEN_TEST_ASSERT(same(fields[k].name, f[k].name) && same(fields[k].value, f[k].value), "the same", hpack_examples[i].title);
            ++lists;
        }
        static char text[64][300];
        for (int round = 0; round < 4000; ++round) {
            proven_http_header_t f[64];
            bool marks[64];
            const unsigned nf = next() % 40;
            for (unsigned k = 0; k < nf; ++k) {
                const unsigned kind = next() % 4;
                const unsigned vl = kind == 3 ? 200 + next() % 90 : next() % 30;
                for (unsigned c = 0; c < vl; ++c) text[k][c] = (char)(kind == 0 ? 'a' + next() % 26 : kind == 1 ? 1 + next() % 255 : 32 + next() % 95);
                text[k][vl] = 0;
                f[k].name = next() % 3 == 0 ? sv(":path") : next() % 2 ? sv("content-type") : sv("x-generated-name");
                f[k].value = (proven_u8str_view_t){ (const proven_byte_t *)text[k], vl };
                if (next() % 5 == 0 && vl == 0) f[k].name = sv("age");
                marks[k] = next() % 7 == 0;
            }
            len = 0;
            PROVEN_TEST_ASSERT(proven_qpack_encode(f, nf, marks, (proven_mem_mut_t){ out, sizeof out }, &len) == PROVEN_OK && len <= proven_qpack_encode_bound(f, nf), "a generated list encodes within its bound", "");
            PROVEN_TEST_ASSERT(dec(out, len, &count) == PROVEN_OK && count == nf, "and decodes", "");
            for (unsigned k = 0; k < nf; ++k) PROVEN_TEST_ASSERT(same(fields[k].name, f[k].name) && same(fields[k].value, f[k].value) && never[k] == marks[k], "to the same fields and marks", "");
            ++lists;
        }
        PROVEN_TEST_INFO("{} lists", PROVEN_ARG((long long)(lists)));
    }

    PROVEN_TEST_SECTION("Refusals", "Each representation that needs the dynamic table, the prefixes a table of capacity 0 cannot have, and malformed lines: QPACK_DECOMPRESSION_FAILED.", "");
    {
        const char *bad[] = {
            "",                          /* no prefix */
            "00",                        /* half a prefix */
            "0100",                      /* Required Insert Count 1 */
            "0080",                      /* Sign 1 with Required Insert Count 0: a negative Base */
            "00ff00",                    /* the same, with a larger Delta Base */
            "000080",                    /* indexed, dynamic */
            "000010",                    /* indexed after the Base */
            "00004100",                  /* a literal with a dynamic name */
            "00000000",                  /* a literal named after the Base */
            "0000ff24",                  /* static index 99 */
            "00005f5400",                /* a literal with static name 99 */
            "000051",                    /* a value missing */
            "00005105616263",            /* a value cut short */
            "0000230161",                /* a literal name with no value */
            "00002801",                  /* a Huffman name cut short */
            "000051ff",                  /* a value length cut short */
            "000051ffffffffffffffffffff7f", /* a length past 2^62 */
            "00005181ff",                /* Huffman padding of more than seven bits */
            "000051 84 ffffffff",        /* EOS inside a Huffman string (spaces read as nothing) */
        };
        for (proven_size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
            char clean[64]; proven_size_t c = 0;
            for (const char *p = bad[i]; *p; ++p) if (*p != ' ') clean[c++] = *p;
            clean[c] = 0;
            PROVEN_TEST_ASSERT(dec_hex(clean, &count) == PROVEN_ERR_INVALID_FORMAT, "refused", bad[i]);
        }
        PROVEN_TEST_ASSERT(dec_hex("0000", &count) == PROVEN_OK && count == 0, "a prefix alone is an empty list", "");
        PROVEN_TEST_ASSERT(dec_hex("0005d1", &count) == PROVEN_OK && count == 1, "a positive Delta Base with nothing referring to it is harmless", "");
    }

    PROVEN_TEST_SECTION("Limits", "A list over the byte limit, over the field count and over the scratch: PROVEN_ERR_OUT_OF_BOUNDS, the section still checked to its end.", "");
    {
        proven_http_header_t f[20];
        static char v[20][200];
        for (int k = 0; k < 20; ++k) { memset(v[k], 'a' + k, 199); v[k][199] = 0; f[k] = (proven_http_header_t){ sv("x-long"), sv(v[k]) }; }
        len = 0;
        PROVEN_TEST_ASSERT(proven_qpack_encode(f, 20, NULL, (proven_mem_mut_t){ out, sizeof out }, &len) == PROVEN_OK, "encodes", "");
        const proven_size_t exact = 20 * (6 + 199 + 32);
        PROVEN_TEST_ASSERT(proven_qpack_decode((proven_mem_view_t){ out, len }, fields, NULL, 512, (proven_mem_mut_t){ scratch, sizeof scratch }, exact, &count) == PROVEN_OK && count == 20,
            "a list of exactly the limit is taken", "");
        PROVEN_TEST_ASSERT(proven_qpack_decode((proven_mem_view_t){ out, len }, fields, NULL, 512, (proven_mem_mut_t){ scratch, sizeof scratch }, exact - 1, &count) == PROVEN_ERR_OUT_OF_BOUNDS,
            "one byte over: PROVEN_ERR_OUT_OF_BOUNDS", "");
        PROVEN_TEST_ASSERT(proven_qpack_decode((proven_mem_view_t){ out, len }, fields, NULL, 19, (proven_mem_mut_t){ scratch, sizeof scratch }, 0, &count) == PROVEN_ERR_OUT_OF_BOUNDS,
            "one field over the array: PROVEN_ERR_OUT_OF_BOUNDS", "");
        PROVEN_TEST_ASSERT(proven_qpack_decode((proven_mem_view_t){ out, len }, fields, NULL, 512, (proven_mem_mut_t){ scratch, 1000 }, 0, &count) == PROVEN_ERR_OUT_OF_BOUNDS,
            "a scratch too small for the Huffman-coded values: PROVEN_ERR_OUT_OF_BOUNDS", "");
        /* Over a limit and malformed after it: still malformed. */
        out[len++] = 0x80;
        PROVEN_TEST_ASSERT(proven_qpack_decode((proven_mem_view_t){ out, len }, fields, NULL, 512, (proven_mem_mut_t){ scratch, 1000 }, 0, &count) == PROVEN_ERR_INVALID_FORMAT,
            "a dynamic reference after the scratch filled is still found", "");
    }

    PROVEN_TEST_SECTION("The peer's encoder and decoder streams", "A capacity of 0 and Stream Cancellations taken a byte at a time; anything else refused with its code.", "");
    {
        proven_qpack_stream_t s = { 0 };
        const proven_byte_t cap0[] = { 0x20 };
        PROVEN_TEST_ASSERT(proven_qpack_encoder_stream_feed(&s, (proven_mem_view_t){ cap0, 1 }) == 0, "Set Dynamic Table Capacity 0: taken", "");
        const proven_byte_t cap0_long[] = { 0x3f, 0x80, 0x80, 0x00 };   /* 31 + 0, padded: not 0 */
        for (int i = 0; i < 4; ++i) {
            const proven_u64 code = proven_qpack_encoder_stream_feed(&s, (proven_mem_view_t){ cap0_long + i, 1 });
            PROVEN_TEST_ASSERT(i < 3 ? code == 0 : code == PROVEN_QPACK_ENCODER_STREAM_ERROR, "a capacity of 31 split over four bytes: refused at the last", "");
        }
        proven_qpack_stream_t d = { 0 };
        const proven_byte_t cancels[] = { 0x48, 0x7f, 0x81, 0x01, 0x40 };   /* stream 8; stream 63 + 129; stream 0 */
        for (int i = 0; i < 5; ++i) PROVEN_TEST_ASSERT(proven_qpack_decoder_stream_feed(&d, (proven_mem_view_t){ cancels + i, 1 }) == 0, "Stream Cancellations a byte at a time", "");
        const proven_byte_t too_long[] = { 0x7f, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 };
        proven_qpack_stream_t t = { 0 };
        PROVEN_TEST_ASSERT(proven_qpack_decoder_stream_feed(&t, (proven_mem_view_t){ too_long, sizeof too_long }) == PROVEN_QPACK_DECODER_STREAM_ERROR,
            "an integer that does not end: refused", "");
        const proven_byte_t ack[] = { 0x80 }, inc[] = { 0x01 }, zero[] = { 0x00 };
        proven_qpack_stream_t a = { 0 }, b = { 0 }, z = { 0 };
        PROVEN_TEST_ASSERT(proven_qpack_decoder_stream_feed(&a, (proven_mem_view_t){ ack, 1 }) == PROVEN_QPACK_DECODER_STREAM_ERROR &&
                           proven_qpack_decoder_stream_feed(&b, (proven_mem_view_t){ inc, 1 }) == PROVEN_QPACK_DECODER_STREAM_ERROR &&
                           proven_qpack_decoder_stream_feed(&z, (proven_mem_view_t){ zero, 1 }) == PROVEN_QPACK_DECODER_STREAM_ERROR,
            "a Section Acknowledgement, an Insert Count Increment, an increment of 0: refused", "");
        const proven_byte_t dup[] = { 0x00 }, ins[] = { 0xc0 }, lit[] = { 0x40 };
        proven_qpack_stream_t e1 = { 0 }, e2 = { 0 }, e3 = { 0 };
        PROVEN_TEST_ASSERT(proven_qpack_encoder_stream_feed(&e1, (proven_mem_view_t){ dup, 1 }) == PROVEN_QPACK_ENCODER_STREAM_ERROR &&
                           proven_qpack_encoder_stream_feed(&e2, (proven_mem_view_t){ ins, 1 }) == PROVEN_QPACK_ENCODER_STREAM_ERROR &&
                           proven_qpack_encoder_stream_feed(&e3, (proven_mem_view_t){ lit, 1 }) == PROVEN_QPACK_ENCODER_STREAM_ERROR,
            "Duplicate, Insert with a name reference, Insert with a literal name: refused", "");
    }

    PROVEN_TEST_SECTION("Changed sections", "Every truncation and every single-bit change of encoded sections either refused or decoded to a list within its bounds.", "");
    {
        proven_http_header_t f[] = { { sv(":method"), sv("POST") }, { sv(":path"), sv("/upload/a/b?c=d") }, { sv(":scheme"), sv("https") },
                                     { sv(":authority"), sv("example.org") }, { sv("content-type"), sv("application/json") }, { sv("x-trace"), sv("0123456789abcdef") },
                                     { sv("authorization"), sv("Bearer t") } };
        static proven_byte_t orig[1024], b[1024];
        proven_size_t n = 0;
        PROVEN_TEST_ASSERT(proven_qpack_encode(f, 7, NULL, (proven_mem_mut_t){ orig, sizeof orig }, &n) == PROVEN_OK, "encodes", "");
        unsigned refused = 0, decoded = 0;
        for (proven_size_t cut = 0; cut < n; ++cut) {
            const proven_err_t e = dec(orig, cut, &count);
            if (e == PROVEN_OK) ++decoded; else { PROVEN_TEST_ASSERT(e == PROVEN_ERR_INVALID_FORMAT, "a cut section is refused or a shorter list", ""); ++refused; }
        }
        for (proven_size_t bit = 0; bit < n * 8; ++bit) {
            memcpy(b, orig, n);
            b[bit / 8] ^= (proven_byte_t)(1u << (bit % 8));
            const proven_err_t e = dec(b, n, &count);
            if (e == PROVEN_OK) { ++decoded; PROVEN_TEST_ASSERT(count <= n, "", ""); }
            else { PROVEN_TEST_ASSERT(e == PROVEN_ERR_INVALID_FORMAT, "a changed section is refused or decoded", ""); ++refused; }
        }
        PROVEN_TEST_INFO("{} refused, {} decoded", PROVEN_ARG((long long)(refused)), PROVEN_ARG((long long)(decoded)));
    }

    PROVEN_TEST_SECTION("The dynamic table, as a decoder (RFC-0032)", "RFC 9204 appendix B against a table of 220 bytes: a section that waits for its inserts, B.2 to B.5 decoded, the decoder's instructions as the appendix writes them, an evicted entry refused; a capacity over the maximum, an entry larger than the table, a count above what the section needs, a reference past it; Huffman-coded inserts; the encoder stream fed a byte at a time and in random pieces.", "Check proven_qpack_dec_feed, qp_enc_instr, qp_required and the dynamic branches of proven_qpack_decode_dyn in src/proven/qpack.c.");
    {
        static proven_byte_t enc_b2[64], enc_b3[64], enc_b4[8], enc_b5[64], sec_b2[8], sec_b4[8];
        const proven_size_t n_eb2 = unhex(qpack_examples[1].hex, enc_b2, sizeof enc_b2), n_sb2 = unhex(qpack_examples[2].hex, sec_b2, sizeof sec_b2);
        const proven_size_t n_eb3 = unhex(qpack_examples[4].hex, enc_b3, sizeof enc_b3), n_eb4 = unhex(qpack_examples[6].hex, enc_b4, sizeof enc_b4);
        const proven_size_t n_sb4 = unhex(qpack_examples[7].hex, sec_b4, sizeof sec_b4), n_eb5 = unhex(qpack_examples[9].hex, enc_b5, sizeof enc_b5);
        proven_qpack_dec_t *d = NULL;
        PROVEN_TEST_ASSERT(proven_qpack_dec_create(proven_heap_allocator(), 31, &d) == PROVEN_ERR_INVALID_ARG && proven_qpack_dec_create(proven_heap_allocator(), (1u << 20) + 1, &d) == PROVEN_ERR_INVALID_ARG,
            "a table of less than one entry, or of more than 1 MiB, is refused", "");
        PROVEN_TEST_ASSERT(proven_qpack_dec_create(proven_heap_allocator(), 220, &d) == PROVEN_OK, "a decoder whose table may be 220 bytes", "");
        proven_u64 ric = 0;
        PROVEN_TEST_ASSERT(proven_qpack_dec_required(d, (proven_mem_view_t){ sec_b2, n_sb2 }, &ric) == PROVEN_OK && ric == 2 &&
                           proven_qpack_decode_dyn(d, (proven_mem_view_t){ sec_b2, n_sb2 }, fields, never, 512, (proven_mem_mut_t){ scratch, sizeof scratch }, 0, &count, &ric) == PROVEN_ERR_AGAIN,
            "B.2's section before its inserts: it needs two, and waits (a blocked stream)", "");
        bool fed = true;
        for (proven_size_t i = 0; i < n_eb2; ++i) fed = fed && proven_qpack_dec_feed(d, (proven_mem_view_t){ enc_b2 + i, 1 }) == 0;
        PROVEN_TEST_ASSERT(fed && proven_qpack_dec_inserts(d) == 2, "B.2's encoder stream a byte at a time: the capacity, then two inserts", "");
        PROVEN_TEST_ASSERT(proven_qpack_decode_dyn(d, (proven_mem_view_t){ sec_b2, n_sb2 }, fields, never, 512, (proven_mem_mut_t){ scratch, sizeof scratch }, 0, &count, &ric) == PROVEN_OK &&
                           ric == 2 && count == 2 && sveq(fields[0].name, ":authority") && sveq(fields[0].value, "www.example.com") && sveq(fields[1].name, ":path") && sveq(fields[1].value, "/sample/path"),
            "then B.2's section: two entries after the Base", "");
        proven_byte_t ins[16];
        PROVEN_TEST_ASSERT(proven_qpack_put_section_ack(ins, 4) == 1 && ins[0] == 0x84, "its Section Acknowledgement is the appendix's byte, 84", "");
        PROVEN_TEST_ASSERT(proven_qpack_dec_feed(d, (proven_mem_view_t){ enc_b3, n_eb3 }) == 0 && proven_qpack_dec_inserts(d) == 3 && proven_qpack_put_insert_count(ins, 1) == 1 && ins[0] == 0x01,
            "B.3: an insert with a literal name; its Insert Count Increment is 01", "");
        PROVEN_TEST_ASSERT(proven_qpack_dec_feed(d, (proven_mem_view_t){ enc_b4, n_eb4 }) == 0 && proven_qpack_dec_inserts(d) == 4 &&
                           proven_qpack_decode_dyn(d, (proven_mem_view_t){ sec_b4, n_sb4 }, fields, never, 512, (proven_mem_mut_t){ scratch, sizeof scratch }, 0, &count, &ric) == PROVEN_OK &&
                           ric == 4 && count == 3 && sveq(fields[0].name, ":authority") && sveq(fields[0].value, "www.example.com") && sveq(fields[1].name, ":path") && sveq(fields[1].value, "/") &&
                           sveq(fields[2].name, "custom-key") && sveq(fields[2].value, "custom-value") && proven_qpack_put_stream_cancel(ins, 8) == 1 && ins[0] == 0x48,
            "B.4: a duplicate, then a section of a dynamic entry, a static one and another dynamic; its Stream Cancellation is 48", "");
        PROVEN_TEST_ASSERT(proven_qpack_dec_feed(d, (proven_mem_view_t){ enc_b5, n_eb5 }) == 0 && proven_qpack_dec_inserts(d) == 5, "B.5: an insert that evicts the oldest entry", "");
        PROVEN_TEST_ASSERT(dec_hex_d(d, "060080", &count) == PROVEN_OK && count == 1 && sveq(fields[0].name, "custom-key") && sveq(fields[0].value, "custom-value2"),
            "the new entry is there", "");
        PROVEN_TEST_ASSERT(dec_hex_d(d, "020080", &count) == PROVEN_ERR_INVALID_FORMAT, "and the evicted one is not: QPACK_DECOMPRESSION_FAILED", "");
        PROVEN_TEST_ASSERT(dec_hex_d(d, "060081", &count) == PROVEN_ERR_INVALID_FORMAT, "a Required Insert Count above what the section needs: refused", "");
        PROVEN_TEST_ASSERT(dec_hex_d(d, "060010", &count) == PROVEN_ERR_INVALID_FORMAT, "a reference at or past the Required Insert Count: refused", "");
        PROVEN_TEST_ASSERT(dec_hex_d(d, "0f00", &count) == PROVEN_ERR_INVALID_FORMAT, "an encoded count beyond twice the table's entries: refused", "");
        PROVEN_TEST_ASSERT(dec_hex_d(d, "06ff00", &count) == PROVEN_ERR_INVALID_FORMAT, "a Base below 0: refused", "");
        proven_qpack_dec_destroy(d);

        static const proven_byte_t over[] = { 0x3f, 0xbe, 0x01 };              /* capacity 221 */
        PROVEN_TEST_ASSERT(proven_qpack_dec_create(proven_heap_allocator(), 220, &d) == PROVEN_OK && proven_qpack_dec_feed(d, (proven_mem_view_t){ over, sizeof over }) == PROVEN_QPACK_ENCODER_STREAM_ERROR,
            "a capacity above the maximum: QPACK_ENCODER_STREAM_ERROR", "");
        proven_qpack_dec_destroy(d);
        static proven_byte_t big[64];
        big[0] = 0x3f; big[1] = 0x21;                                          /* capacity 64 */
        big[2] = 0x40 | 0x1f; big[3] = 40 - 31;                                /* a literal name of 40 bytes */
        memset(big + 4, 'a', 40);
        big[44] = 0x00;                                                        /* and an empty value: 72 bytes in all */
        PROVEN_TEST_ASSERT(proven_qpack_dec_create(proven_heap_allocator(), 220, &d) == PROVEN_OK && proven_qpack_dec_feed(d, (proven_mem_view_t){ big, 45 }) == PROVEN_QPACK_ENCODER_STREAM_ERROR,
            "an entry larger than the table's capacity: QPACK_ENCODER_STREAM_ERROR", "");
        proven_qpack_dec_destroy(d);

        /* Huffman-coded name and value, then the insert referred to as a whole field. */
        PROVEN_TEST_ASSERT(proven_qpack_dec_create(proven_heap_allocator(), 4096, &d) == PROVEN_OK, "a decoder of 4,096 bytes", "");
        proven_byte_t hb[128];
        proven_size_t hn = 0;
        hb[hn++] = 0x3f; hb[hn++] = 0xe1; hb[hn++] = 0x1f;                     /* capacity 4,096 */
        const proven_u8str_view_t hname = sv("x-huffman-name"), hvalue = sv("a value worth coding");
        const proven_size_t cn = proven_hpack_huffman_size(hname), cv = proven_hpack_huffman_size(hvalue);
        hb[hn++] = (proven_byte_t)(0x40 | 0x20 | cn);                          /* 01H, the length fits in 5 bits */
        PROVEN_TEST_ASSERT(cn < 31 && cv < 127 && proven_hpack_huffman_encode(hname, (proven_mem_mut_t){ hb, hn + cn }, &hn) == PROVEN_OK, "the name Huffman-coded", "");
        hb[hn++] = (proven_byte_t)(0x80 | cv);
        PROVEN_TEST_ASSERT(proven_hpack_huffman_encode(hvalue, (proven_mem_mut_t){ hb, hn + cv }, &hn) == PROVEN_OK, "the value Huffman-coded", "");
        PROVEN_TEST_ASSERT(proven_qpack_dec_feed(d, (proven_mem_view_t){ hb, hn }) == 0 && dec_hex_d(d, "0200" "80", &count) == PROVEN_OK && count == 1 &&
                           same(fields[0].name, hname) && same(fields[0].value, hvalue), "an insert of both, decoded back whole", "");
        proven_qpack_dec_destroy(d);

        /* The whole appendix's encoder stream in random pieces gives the same table. */
        static proven_byte_t all[256];
        proven_size_t an = 0;
        memcpy(all + an, enc_b2, n_eb2); an += n_eb2; memcpy(all + an, enc_b3, n_eb3); an += n_eb3; memcpy(all + an, enc_b4, n_eb4); an += n_eb4;
        bool same_all = true;
        for (int round = 0; round < 500; ++round) {
            PROVEN_TEST_ASSERT(proven_qpack_dec_create(proven_heap_allocator(), 220, &d) == PROVEN_OK, "a decoder", "");
            for (proven_size_t at = 0; at < an;) {
                proven_size_t k = 1 + next() % 9;
                if (k > an - at) k = an - at;
                if (proven_qpack_dec_feed(d, (proven_mem_view_t){ all + at, k }) != 0) same_all = false;
                at += k;
            }
            same_all = same_all && proven_qpack_dec_inserts(d) == 4 &&
                       proven_qpack_decode_dyn(d, (proven_mem_view_t){ sec_b4, n_sb4 }, fields, never, 512, (proven_mem_mut_t){ scratch, sizeof scratch }, 0, &count, &ric) == PROVEN_OK &&
                       count == 3 && sveq(fields[2].value, "custom-value");
            proven_qpack_dec_destroy(d);
        }
        PROVEN_TEST_ASSERT(same_all, "five hundred random splittings of B.2 to B.4's encoder stream build the same table", "");
    }

    PROVEN_TEST_SECTION("The dynamic table, as an encoder (RFC-0032)", "Lists encoded with a table of 4,096 bytes and decoded by this side's decoder, each instruction carried across and every acknowledgement and increment back: the same lists, references only to acknowledged entries, a repeated list a fraction of its first size; a table of 256 bytes under many fields, a section held unacknowledged keeping its entries from eviction; secrets never inserted; the decoder-stream instructions refused.", "Check proven_qpack_encode_dyn, qe_room, qe_find and proven_qpack_enc_feed in src/proven/qpack.c.");
    {
        static proven_byte_t sec[8192], ins[8192], back[64];
        proven_qpack_enc_t *e = NULL;
        proven_qpack_dec_t *d = NULL;
        PROVEN_TEST_ASSERT(proven_qpack_enc_create(proven_heap_allocator(), 4096, 2048, &e) == PROVEN_ERR_INVALID_ARG && proven_qpack_enc_create(proven_heap_allocator(), 16, 4096, &e) == PROVEN_ERR_INVALID_ARG,
            "a table larger than the peer allows, or under one entry, is refused", "");
        PROVEN_TEST_ASSERT(proven_qpack_enc_create(proven_heap_allocator(), 4096, 4096, &e) == PROVEN_OK && proven_qpack_dec_create(proven_heap_allocator(), 4096, &d) == PROVEN_OK,
            "an encoder and a decoder of 4,096 bytes", "");
        bool same_all = true, refs_ok = true;
        proven_size_t first_size = 0, last_size = 0;
        proven_u64 stream = 0, dec_known = 0;
        for (int round = 0; round < 3; ++round) {
            for (proven_size_t i = 0; i < sizeof hpack_examples / sizeof hpack_examples[0]; ++i) {
                proven_http_header_t f[16];
                const proven_size_t nf = hpack_examples[i].field_count;
                for (proven_size_t k = 0; k < nf; ++k) f[k] = (proven_http_header_t){ sv(hpack_examples[i].fields[k].name), sv(hpack_examples[i].fields[k].value) };
                proven_size_t sl = 0, il = 0;
                proven_u64 ric = 0, low = 0;
                PROVEN_TEST_ASSERT(proven_qpack_encode_dyn(e, f, nf, NULL, (proven_mem_mut_t){ sec, sizeof sec }, &sl, (proven_mem_mut_t){ ins, sizeof ins }, &il, &ric, &low) == PROVEN_OK, "encoded", "");
                if (ric > proven_qpack_enc_known(e)) refs_ok = false;           /* only what the peer has */
                if (i == 0 && round == 0) first_size = sl;
                if (i == 0 && round == 2) last_size = sl;
                PROVEN_TEST_ASSERT(proven_qpack_enc_sent(e, stream, ric, low), "kept until acknowledged", "");
                /* The peer: the inserts, its increment back; the section, its acknowledgement back. */
                PROVEN_TEST_ASSERT(proven_qpack_dec_feed(d, (proven_mem_view_t){ ins, il }) == 0, "the encoder stream taken", "");
                if (proven_qpack_dec_inserts(d) > dec_known) {
                    const proven_size_t bn = proven_qpack_put_insert_count(back, proven_qpack_dec_inserts(d) - dec_known);
                    dec_known = proven_qpack_dec_inserts(d);
                    PROVEN_TEST_ASSERT(proven_qpack_enc_feed(e, (proven_mem_view_t){ back, bn }) == 0, "the increment taken", "");
                }
                proven_u64 dric = 0;
                same_all = same_all && proven_qpack_decode_dyn(d, (proven_mem_view_t){ sec, sl }, fields, never, 512, (proven_mem_mut_t){ scratch, sizeof scratch }, 0, &count, &dric) == PROVEN_OK &&
                           dric == ric && count == nf;
                for (proven_size_t k = 0; k < nf && k < count; ++k) same_all = same_all && same(fields[k].name, f[k].name) && same(fields[k].value, f[k].value);
                if (ric > 0) {
                    const proven_size_t bn = proven_qpack_put_section_ack(back, stream);
                    PROVEN_TEST_ASSERT(proven_qpack_enc_feed(e, (proven_mem_view_t){ back, bn }) == 0, "the acknowledgement taken", "");
                }
                stream += 4;
            }
        }
        PROVEN_TEST_ASSERT(same_all && refs_ok, "RFC 7541 appendix C's lists three times over: decoded alike, never referring past what the peer acknowledged", "");
        PROVEN_TEST_ASSERT(proven_qpack_enc_inserts(e) > 0 && last_size * 3 < first_size, "the table is used: the first list again in under a third of its first size", "");
        PROVEN_TEST_INFO("the first list: {} bytes, then {}", PROVEN_ARG((unsigned)first_size), PROVEN_ARG((unsigned)last_size));
        proven_qpack_enc_destroy(e); proven_qpack_dec_destroy(d);

        /* A small table; one section that refers to an entry held unacknowledged while fifty new
         * fields want room. */
        PROVEN_TEST_ASSERT(proven_qpack_enc_create(proven_heap_allocator(), 256, 4096, &e) == PROVEN_OK && proven_qpack_dec_create(proven_heap_allocator(), 4096, &d) == PROVEN_OK,
            "an encoder that uses 256 bytes of a peer's 4,096: six entries of these fields", "");
        dec_known = 0;
        bool small_ok = true;
        static proven_byte_t held[256];
        proven_size_t held_n = 0;
        proven_u64 held_ric = 0, inserts_held = 0;
        for (unsigned i = 0; i < 52; ++i) {
            char nm[16], va[16];
            const unsigned which = i < 2 ? 0 : i;                         /* the same field twice, then fifty new ones */
            snprintf(nm, sizeof nm, "x-n%u", which); snprintf(va, sizeof va, "v%u", which);
            const proven_http_header_t f = { sv(nm), sv(va) };
            proven_size_t sl = 0, il = 0;
            proven_u64 ric = 0, low = 0;
            small_ok = small_ok && proven_qpack_encode_dyn(e, &f, 1, NULL, (proven_mem_mut_t){ sec, sizeof sec }, &sl, (proven_mem_mut_t){ ins, sizeof ins }, &il, &ric, &low) == PROVEN_OK &&
                       proven_qpack_enc_sent(e, (proven_u64)i * 4, ric, low) && proven_qpack_dec_feed(d, (proven_mem_view_t){ ins, il }) == 0;
            if (proven_qpack_dec_inserts(d) > dec_known) {
                const proven_size_t bn = proven_qpack_put_insert_count(back, proven_qpack_dec_inserts(d) - dec_known);
                dec_known = proven_qpack_dec_inserts(d);
                small_ok = small_ok && proven_qpack_enc_feed(e, (proven_mem_view_t){ back, bn }) == 0;
            }
            if (i == 1) { small_ok = small_ok && ric == 1; memcpy(held, sec, sl); held_n = sl; held_ric = ric; continue; }   /* refers to x-n0: held, not acknowledged */
            proven_u64 dric = 0;
            small_ok = small_ok && proven_qpack_decode_dyn(d, (proven_mem_view_t){ sec, sl }, fields, never, 512, (proven_mem_mut_t){ scratch, sizeof scratch }, 0, &count, &dric) == PROVEN_OK &&
                       count == 1 && same(fields[0].name, f.name) && same(fields[0].value, f.value);
            if (ric > 0) { const proven_size_t bn = proven_qpack_put_section_ack(back, (proven_u64)i * 4); small_ok = small_ok && proven_qpack_enc_feed(e, (proven_mem_view_t){ back, bn }) == 0; }
        }
        inserts_held = proven_qpack_enc_inserts(e);
        proven_u64 dric = 0;
        PROVEN_TEST_ASSERT(small_ok && inserts_held == 6 && proven_qpack_decode_dyn(d, (proven_mem_view_t){ held, held_n }, fields, never, 512, (proven_mem_mut_t){ scratch, sizeof scratch }, 0, &count, &dric) == PROVEN_OK &&
                           dric == held_ric && count == 1 && sveq(fields[0].name, "x-n0") && sveq(fields[0].value, "v0"),
            "fifty new fields fill the table and stop at six entries: the oldest is referred to by a section not acknowledged, which still decodes", "");
        {
            const proven_size_t bn = proven_qpack_put_section_ack(back, 4);
            const proven_http_header_t f = { sv("x-after"), sv("v") };
            proven_size_t sl = 0, il = 0;
            proven_u64 ric = 0, low = 0;
            PROVEN_TEST_ASSERT(proven_qpack_enc_feed(e, (proven_mem_view_t){ back, bn }) == 0 &&
                               proven_qpack_encode_dyn(e, &f, 1, NULL, (proven_mem_mut_t){ sec, sizeof sec }, &sl, (proven_mem_mut_t){ ins, sizeof ins }, &il, &ric, &low) == PROVEN_OK &&
                               proven_qpack_enc_inserts(e) == inserts_held + 1, "acknowledged, the entry may go: the next field is inserted", "");
        }
        proven_qpack_enc_destroy(e); proven_qpack_dec_destroy(d);

        /* Secrets, and the peer's instructions. */
        PROVEN_TEST_ASSERT(proven_qpack_enc_create(proven_heap_allocator(), 4096, 4096, &e) == PROVEN_OK, "an encoder", "");
        const proven_http_header_t secrets[] = { { sv("cookie"), sv("id=1") }, { sv("authorization"), sv("Bearer a-long-enough-token-value") }, { sv("x-marked"), sv("v") } };
        const bool marks[] = { false, false, true };
        proven_size_t sl = 0, il = 0;
        proven_u64 ric = 0, low = 0;
        PROVEN_TEST_ASSERT(proven_qpack_encode_dyn(e, secrets, 3, marks, (proven_mem_mut_t){ sec, sizeof sec }, &sl, (proven_mem_mut_t){ ins, sizeof ins }, &il, &ric, &low) == PROVEN_OK &&
                           il == 0 && proven_qpack_enc_inserts(e) == 0, "a short cookie, any authorization and a marked field: none inserted", "");
        const proven_http_header_t plain = { sv("x-plain"), sv("value") };
        PROVEN_TEST_ASSERT(proven_qpack_encode_dyn(e, &plain, 1, NULL, (proven_mem_mut_t){ sec, sizeof sec }, &sl, (proven_mem_mut_t){ ins, sizeof ins }, &il, &ric, &low) == PROVEN_OK &&
                           proven_qpack_enc_inserts(e) == 1, "a plain one is", "");
        PROVEN_TEST_ASSERT(proven_qpack_enc_feed(e, (proven_mem_view_t){ (const proven_byte_t *)"\x84", 1 }) == PROVEN_QPACK_DECODER_STREAM_ERROR, "an acknowledgement of no section: QPACK_DECODER_STREAM_ERROR", "");
        proven_qpack_enc_destroy(e);
        PROVEN_TEST_ASSERT(proven_qpack_enc_create(proven_heap_allocator(), 4096, 4096, &e) == PROVEN_OK &&
                           proven_qpack_enc_feed(e, (proven_mem_view_t){ (const proven_byte_t *)"\x00", 1 }) == PROVEN_QPACK_DECODER_STREAM_ERROR, "an increment of 0: refused", "");
        proven_qpack_enc_destroy(e);
        PROVEN_TEST_ASSERT(proven_qpack_enc_create(proven_heap_allocator(), 4096, 4096, &e) == PROVEN_OK &&
                           proven_qpack_enc_feed(e, (proven_mem_view_t){ (const proven_byte_t *)"\x01", 1 }) == PROVEN_QPACK_DECODER_STREAM_ERROR, "an increment past what was inserted: refused", "");
        proven_qpack_enc_destroy(e);
        PROVEN_TEST_ASSERT(proven_qpack_enc_create(proven_heap_allocator(), 4096, 4096, &e) == PROVEN_OK &&
                           proven_qpack_enc_feed(e, (proven_mem_view_t){ (const proven_byte_t *)"\x48", 1 }) == 0, "a Stream Cancellation of a stream with nothing outstanding: taken", "");
        proven_qpack_enc_destroy(e);
    }

    PROVEN_TEST_PASS("QPACK decodes with and without a dynamic table, encodes with and without one, and refuses as RFC 9204 says.");
    return 0;
}

#else

int main(void) {
    PROVEN_TEST_SUITE("QPACK: header compression for HTTP/3", "Built with PROVEN_NO_HTTP3: nothing to test.", "");
    PROVEN_TEST_PASS("skipped: PROVEN_NO_HTTP3");
    return 0;
}

#endif
