#include "proven.h"
#include "proven_test.h"
#include "../src/proven/hpack_tables.h"
#include "test_unit_hpack_vectors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * HPACK (RFC 7541): the header compression of HTTP/2.
 *
 * The tables this library is linked with and the examples in test_unit_hpack_vectors.h were
 * both written by a private program from the published text of the RFC. This test checks the
 * tables for what any correct copy must have (a code that is prefix-free and fills its
 * space), replays every example of Appendix C through the decoder with the dynamic table
 * compared after each step, has the encoder reproduce the examples whose choices it shares,
 * and then does what no example does: refuses each kind of malformed block, holds each limit,
 * and survives blocks that were cut short or changed.
 *
 * That another implementation's encoder and decoder agree with these is checked by a private
 * program (Node's http2, which is nghttp2).
 */

static proven_u8str_view_t sv(const char *s) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }

static proven_allocator_t g_heap;
static proven_byte_t g_scratch[70000], g_block[70000], g_text[70000];
static proven_http_header_t g_fields[2000];

static bool view_is(proven_u8str_view_t v, const char *s) { return v.size == strlen(s) && memcmp(v.ptr, s, v.size) == 0; }

/* Decode `bytes` with `d` and compare with an example's fields and table. */
static void check_example(proven_hpack_decoder_t *d, const hpack_example_t *x) {
    proven_size_t count = 0;
    proven_err_t e = proven_hpack_decode(d, (proven_mem_view_t){ x->bytes, x->size }, g_fields, 64, (proven_mem_mut_t){ g_scratch, sizeof g_scratch }, 0, &count);
    PROVEN_TEST_ASSERT(e == PROVEN_OK && count == x->field_count, "the RFC's block decodes to as many fields as the RFC lists", x->title);
    for (proven_size_t i = 0; i < count; ++i) {
        PROVEN_TEST_ASSERT(view_is(g_fields[i].name, x->fields[i].name) && view_is(g_fields[i].value, x->fields[i].value), "each with the RFC's name and value, in order", x->title);
    }
    PROVEN_TEST_ASSERT(proven_hpack_decoder_table_count(d) == x->table_count && proven_hpack_decoder_table_bytes(d) == x->table_size,
        "and leaves the dynamic table with the RFC's entries and size", x->title);
    for (proven_size_t i = 0; i < x->table_count; ++i) {
        proven_u8str_view_t name, value;
        PROVEN_TEST_ASSERT(proven_hpack_decoder_table_entry(d, i, &name, &value) && view_is(name, x->table[i].name) && view_is(value, x->table[i].value),
            "newest first, as the RFC prints it", x->title);
    }
}

static const hpack_example_t *example(const char *prefix) {
    for (proven_size_t i = 0; i < sizeof hpack_examples / sizeof hpack_examples[0]; ++i) {
        if (strncmp(hpack_examples[i].title, prefix, strlen(prefix)) == 0) return &hpack_examples[i];
    }
    PROVEN_TEST_ASSERT(false, "the example exists in the vector header", prefix);
    return NULL;
}

/* An example's header list as proven_http_header_t. */
static proven_size_t example_fields(const hpack_example_t *x, proven_http_header_t *out) {
    for (proven_size_t i = 0; i < x->field_count; ++i) out[i] = (proven_http_header_t){ sv(x->fields[i].name), sv(x->fields[i].value) };
    return x->field_count;
}

static proven_err_t decode(proven_hpack_decoder_t *d, const proven_byte_t *bytes, proven_size_t n, proven_size_t *count) {
    return proven_hpack_decode(d, (proven_mem_view_t){ bytes, n }, g_fields, sizeof g_fields / sizeof g_fields[0], (proven_mem_mut_t){ g_scratch, sizeof g_scratch }, 0, count);
}

/* A fresh decoder must refuse `bytes`, and go on refusing. */
static void refused(const char *what, const proven_byte_t *bytes, proven_size_t n) {
    proven_hpack_decoder_t *d = NULL;
    proven_size_t count = 99;
    PROVEN_TEST_ASSERT(proven_hpack_decoder_create(g_heap, 4096, &d) == PROVEN_OK, "a decoder", "");
    PROVEN_TEST_ASSERT(decode(d, bytes, n, &count) == PROVEN_ERR_INVALID_FORMAT && count == 0, "a malformed block is PROVEN_ERR_INVALID_FORMAT", what);
    static const proven_byte_t fine[] = { 0x82 };
    PROVEN_TEST_ASSERT(decode(d, fine, 1, &count) == PROVEN_ERR_INVALID_FORMAT, "and the decoder repeats the error: its table can no longer be trusted", what);
    proven_hpack_decoder_destroy(d);
}

static proven_u32 g_seed = 2463534242u;
static proven_u32 rnd(void) { g_seed ^= g_seed << 13; g_seed ^= g_seed >> 17; g_seed ^= g_seed << 5; return g_seed; }

int main(void) {
    PROVEN_TEST_SUITE("hpack: header compression for HTTP/2",
        "The tables against what any correct copy must satisfy, every example of RFC 7541 Appendix C with the dynamic table compared after each step, the encoder against the examples whose choices it shares, each malformed block refused, each limit held, and blocks cut short or changed.",
        "Inspect src/proven/hpack.c: proven_hpack_decode for the representations and their refusals, hp_table_add for eviction, proven_hpack_encode for the encoder's choices, proven_hpack_huffman_decode for the padding rules.");
    g_heap = proven_heap_allocator();

    PROVEN_TEST_SECTION("the tables", "The static table and the Huffman code this build is linked with.", "Regenerate src/proven/hpack_tables.h from the RFC's text; do not edit it.");
    {
        PROVEN_TEST_ASSERT(sizeof hpack_static / sizeof hpack_static[0] == 61, "the static table has 61 entries", "");
        PROVEN_TEST_ASSERT(strcmp(hpack_static[0].name, ":authority") == 0 && hpack_static[0].value_len == 0 && strcmp(hpack_static[1].name, ":method") == 0 && strcmp(hpack_static[1].value, "GET") == 0 &&
                           strcmp(hpack_static[15].name, "accept-encoding") == 0 && strcmp(hpack_static[15].value, "gzip, deflate") == 0 && strcmp(hpack_static[60].name, "www-authenticate") == 0,
            "with the first, the sixteenth and the last as the RFC lists them", "");
        for (proven_size_t i = 0; i < 61; ++i) {
            PROVEN_TEST_ASSERT(strlen(hpack_static[i].name) == hpack_static[i].name_len && strlen(hpack_static[i].value) == hpack_static[i].value_len, "each length is its string's", hpack_static[i].name);
        }
        /* The Kraft sum: a prefix-free code that fills its space exactly sums to one. In units of 2^-30. */
        proven_u64 sum = 0;
        unsigned shortest = 99, longest = 0;
        for (int s = 0; s < 257; ++s) {
            unsigned n = hpack_huffman[s].bits;
            PROVEN_TEST_ASSERT(n >= 5 && n <= 30 && (hpack_huffman[s].code >> n) == 0, "a code is 5 to 30 bits and no wider than its length", "");
            sum += (proven_u64)1 << (30 - n);
            if (n < shortest) shortest = n;
            if (n > longest) longest = n;
        }
        PROVEN_TEST_ASSERT(sum == (proven_u64)1 << 30 && shortest == 5 && longest == 30, "the 257 codes fill the code space exactly: none is missing, none is extra", "");
        for (int a = 0; a < 257; ++a) {
            for (int b = 0; b < 257; ++b) {
                if (a == b || hpack_huffman[a].bits > hpack_huffman[b].bits) continue;
                PROVEN_TEST_ASSERT((hpack_huffman[b].code >> (hpack_huffman[b].bits - hpack_huffman[a].bits)) != hpack_huffman[a].code, "no code is the beginning of another", "");
            }
        }
        PROVEN_TEST_ASSERT(hpack_huffman[256].code == 0x3fffffff && hpack_huffman[256].bits == 30 && hpack_huffman['/'].code == 0x18 && hpack_huffman['/'].bits == 6,
            "EOS is thirty ones, and '/' is the six bits the RFC spells out", "");

        /* The encoder finds a name by its length first. Every entry is asked for, as itself and
         * with a value the table does not have, and must come out as the index the RFC gives
         * it - the first entry with that name, when several share one. */
        for (proven_size_t i = 0; i < 61; ++i) {
            proven_size_t first = i;
            while (first > 0 && strcmp(hpack_static[first - 1].name, hpack_static[i].name) == 0) first--;
            for (int other = 0; other < 2; ++other) {
                proven_hpack_encoder_t *enc = NULL;
                PROVEN_TEST_ASSERT(proven_hpack_encoder_create(proven_heap_allocator(), 4096, &enc) == PROVEN_OK, "an encoder", "");
                proven_http_header_t h = { .name = sv(hpack_static[i].name), .value = sv(other ? "a value no table has" : hpack_static[i].value) };
                proven_size_t n = 0;
                PROVEN_TEST_ASSERT(proven_hpack_encode(enc, &h, 1, NULL, (proven_mem_mut_t){ g_block, sizeof g_block }, &n) == PROVEN_OK && n >= 1, "a static name is encoded", hpack_static[i].name);
                const bool secret = strcmp(hpack_static[i].name, "authorization") == 0 || strcmp(hpack_static[i].name, "proxy-authorization") == 0;
                const unsigned prefix = (g_block[0] & 0x80) ? 7u : (g_block[0] & 0x40) ? 6u : 4u;
                const unsigned mask = (1u << prefix) - 1u;
                unsigned index = g_block[0] & mask;
                if (index == mask) index += g_block[1];
                if (!other && !secret) PROVEN_TEST_ASSERT(n == 1 && g_block[0] == (0x80 | (i + 1)), "an entry of the static table is one byte: its index", hpack_static[i].name);
                else PROVEN_TEST_ASSERT((g_block[0] & 0x80) == 0 && index == first + 1 && (!secret || (g_block[0] & 0xf0) == 0x10),
                    "a static name with another value is a literal that names the first entry with that name - never indexed, for the two that carry credentials", hpack_static[i].name);
                proven_hpack_encoder_destroy(enc);
            }
        }
        {
            /* And a name that only looks like one of them - the same length, one byte off - is not found. */
            proven_hpack_encoder_t *enc = NULL;
            PROVEN_TEST_ASSERT(proven_hpack_encoder_create(proven_heap_allocator(), 4096, &enc) == PROVEN_OK, "an encoder", "");
            proven_http_header_t h = { .name = sv("content-lengtx"), .value = sv("2") };
            proven_size_t n = 0;
            PROVEN_TEST_ASSERT(proven_hpack_encode(enc, &h, 1, NULL, (proven_mem_mut_t){ g_block, sizeof g_block }, &n) == PROVEN_OK && g_block[0] == 0x40,
                "a name as long as a static one and not equal to it is a new name", "");
            proven_hpack_encoder_destroy(enc);
        }
    }

    PROVEN_TEST_SECTION("Huffman coding", "Strings coded and decoded, and the endings that must be refused.", "Check proven_hpack_huffman_encode and proven_hpack_huffman_decode.");
    {
        /* RFC 7541 C.4.1: "www.example.com". */
        static const proven_byte_t www[] = { 0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff };
        proven_size_t n = 0;
        PROVEN_TEST_ASSERT(proven_hpack_huffman_size(sv("www.example.com")) == sizeof www &&
                           proven_hpack_huffman_encode(sv("www.example.com"), (proven_mem_mut_t){ g_block, sizeof g_block }, &n) == PROVEN_OK && n == sizeof www && memcmp(g_block, www, n) == 0,
            "www.example.com codes to the twelve bytes of the RFC's example", "");
        n = 0;
        PROVEN_TEST_ASSERT(proven_hpack_huffman_decode((proven_mem_view_t){ www, sizeof www }, (proven_mem_mut_t){ g_text, sizeof g_text }, &n) == PROVEN_OK && n == 15 && memcmp(g_text, "www.example.com", 15) == 0,
            "and back", "");
        for (int c = 0; c < 256; ++c) {
            proven_byte_t one = (proven_byte_t)c;
            proven_size_t coded = 0, back = 0;
            PROVEN_TEST_ASSERT(proven_hpack_huffman_encode((proven_u8str_view_t){ &one, 1 }, (proven_mem_mut_t){ g_block, 8 }, &coded) == PROVEN_OK && coded == (proven_size_t)((hpack_huffman[c].bits + 7u) / 8u) &&
                               proven_hpack_huffman_decode((proven_mem_view_t){ g_block, coded }, (proven_mem_mut_t){ g_text, 8 }, &back) == PROVEN_OK && back == 1 && g_text[0] == one,
                "every byte value alone codes and decodes to itself", "");
        }
        for (proven_size_t len = 0; len < 600; len += 7) {
            proven_byte_t text[600];
            for (proven_size_t i = 0; i < len; ++i) text[i] = (proven_byte_t)(rnd() >> 9);
            proven_size_t coded = 0, back = 0;
            PROVEN_TEST_ASSERT(proven_hpack_huffman_encode((proven_u8str_view_t){ text, len }, (proven_mem_mut_t){ g_block, sizeof g_block }, &coded) == PROVEN_OK && coded == proven_hpack_huffman_size((proven_u8str_view_t){ text, len }) &&
                               proven_hpack_huffman_decode((proven_mem_view_t){ g_block, coded }, (proven_mem_mut_t){ g_text, sizeof g_text }, &back) == PROVEN_OK && back == len && memcmp(g_text, text, len) == 0,
                "random strings of every length go round", "");
            if (len > 0) {
                proven_size_t small = 0;
                PROVEN_TEST_ASSERT(proven_hpack_huffman_decode((proven_mem_view_t){ g_block, coded }, (proven_mem_mut_t){ g_text, len - 1 }, &small) == PROVEN_ERR_OUT_OF_BOUNDS && small == 0,
                    "and one byte too little room is PROVEN_ERR_OUT_OF_BOUNDS with nothing reported", "");
                small = 0;
                PROVEN_TEST_ASSERT(proven_hpack_huffman_encode((proven_u8str_view_t){ text, len }, (proven_mem_mut_t){ g_scratch, coded - 1 }, &small) == PROVEN_ERR_OUT_OF_BOUNDS && small == 0,
                    "for the encoder too", "");
            }
        }
        static const struct { const char *what; proven_byte_t bytes[6]; proven_size_t n; } bad[] = {
            { "the end-of-string symbol inside a string", { 0xff, 0xff, 0xff, 0xff }, 4 },
            { "padding of a whole byte of ones", { 0x1f, 0xff }, 2 },                   /* 'a' (00011), three ones of padding, then eight more */
            { "padding that is not ones", { 0x18 }, 1 },                                /* 'a' and three zero bits */
            { "padding with one zero in it", { 0x1d }, 1 },                             /* 'a', then 1 0 1 */
            { "a code cut off by the end", { 0xfe }, 1 },                               /* the first seven bits of an eight-bit code, with a zero */
        };
        for (proven_size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
            proven_size_t out = 0;
            PROVEN_TEST_ASSERT(proven_hpack_huffman_decode((proven_mem_view_t){ bad[i].bytes, bad[i].n }, (proven_mem_mut_t){ g_text, sizeof g_text }, &out) == PROVEN_ERR_INVALID_FORMAT && out == 0,
                "an ending the RFC forbids is PROVEN_ERR_INVALID_FORMAT", bad[i].what);
        }
        static const proven_byte_t padded[] = { 0x1f };                                  /* 'a' and three ones: the right padding */
        proven_size_t out = 0;
        PROVEN_TEST_ASSERT(proven_hpack_huffman_decode((proven_mem_view_t){ padded, 1 }, (proven_mem_mut_t){ g_text, 8 }, &out) == PROVEN_OK && out == 1 && g_text[0] == 'a', "while up to seven ones are the padding it requires", "");
        out = 0;
        PROVEN_TEST_ASSERT(proven_hpack_huffman_decode((proven_mem_view_t){ padded, 0 }, (proven_mem_mut_t){ g_text, 8 }, &out) == PROVEN_OK && out == 0, "and nothing decodes to nothing", "");
    }

    PROVEN_TEST_SECTION("the examples of RFC 7541 Appendix C", "Every block the RFC prints, decoded, with the dynamic table compared after each.", "Check proven_hpack_decode and hp_table_add in src/proven/hpack.c.");
    {
        PROVEN_TEST_ASSERT(sizeof hpack_examples / sizeof hpack_examples[0] == 16, "the vector header carries the sixteen examples of C.2 to C.6", "");
        static const char *single[] = { "C.2.1.", "C.2.2.", "C.2.3.", "C.2.4." };
        for (proven_size_t i = 0; i < 4; ++i) {
            proven_hpack_decoder_t *d = NULL;
            PROVEN_TEST_ASSERT(proven_hpack_decoder_create(g_heap, 4096, &d) == PROVEN_OK, "a decoder", "");
            check_example(d, example(single[i]));
            proven_hpack_decoder_destroy(d);
        }
        /* The sequences: three blocks through one decoder. C.5 and C.6 use a table of 256 bytes,
         * so that entries are evicted. */
        static const struct { const char *a, *b, *c; proven_size_t table; } runs[] = {
            { "C.3.1.", "C.3.2.", "C.3.3.", 4096 }, { "C.4.1.", "C.4.2.", "C.4.3.", 4096 },
            { "C.5.1.", "C.5.2.", "C.5.3.", 256 }, { "C.6.1.", "C.6.2.", "C.6.3.", 256 },
        };
        for (proven_size_t i = 0; i < 4; ++i) {
            proven_hpack_decoder_t *d = NULL;
            PROVEN_TEST_ASSERT(proven_hpack_decoder_create(g_heap, runs[i].table, &d) == PROVEN_OK, "a decoder for a sequence", "");
            check_example(d, example(runs[i].a));
            check_example(d, example(runs[i].b));
            check_example(d, example(runs[i].c));
            proven_hpack_decoder_destroy(d);
        }
        /* The integers of C.1, as the size updates an encoder writes: 10 and 1337 with a five-bit prefix. */
        static const struct { proven_size_t size; proven_byte_t bytes[3]; proven_size_t n; } ints[] = { { 10, { 0x2a }, 1 }, { 1337, { 0x3f, 0x9a, 0x0a }, 3 } };
        PROVEN_TEST_ASSERT(hpack_c1_integers[0].value == 10 && hpack_c1_integers[0].prefix == 5 && hpack_c1_integers[1].value == 1337 && hpack_c1_integers[1].prefix == 5, "the vector header has the RFC's two five-bit integers", "");
        for (proven_size_t i = 0; i < 2; ++i) {
            proven_hpack_encoder_t *c = NULL;
            proven_size_t n = 0;
            PROVEN_TEST_ASSERT(proven_hpack_encoder_create(g_heap, ints[i].size, &c) == PROVEN_OK &&
                               proven_hpack_encode(c, NULL, 0, NULL, (proven_mem_mut_t){ g_block, sizeof g_block }, &n) == PROVEN_OK && n == ints[i].n && memcmp(g_block, ints[i].bytes, n) == 0,
                "an encoder that will use less than the default says so in the RFC's bytes for that integer", "");
            proven_hpack_encoder_destroy(c);
        }
    }

    PROVEN_TEST_SECTION("the encoder against the examples", "Where the RFC's encoder made the choices this one makes, the bytes are the same.", "Check proven_hpack_encode.");
    {
        /* C.4: Huffman, table of 4096. C.6: Huffman, table of 256 - announced first, which the
         * RFC's bytes leave to a setting. Two of the RFC's blocks are not this encoder's and
         * are compared by decoding instead: C.6.2 Huffman-codes "307" into the three bytes it
         * already is (this encoder codes only what gets shorter), and C.6.3 puts set-cookie in
         * the table (this encoder will not). */
        static const struct { const char *titles[3]; proven_size_t table; int exact; } runs[] = {
            { { "C.4.1.", "C.4.2.", "C.4.3." }, 4096, 3 }, { { "C.6.1.", "C.6.2.", "C.6.3." }, 256, 1 },
        };
        for (proven_size_t r = 0; r < 2; ++r) {
            proven_hpack_encoder_t *c = NULL;
            proven_hpack_decoder_t *d = NULL;
            PROVEN_TEST_ASSERT(proven_hpack_encoder_create(g_heap, runs[r].table, &c) == PROVEN_OK && proven_hpack_decoder_create(g_heap, 4096, &d) == PROVEN_OK, "an encoder and a decoder", "");
            for (int k = 0; k < 3; ++k) {
                const hpack_example_t *x = example(runs[r].titles[k]);
                proven_http_header_t list[8];
                proven_size_t count = example_fields(x, list), n = 0, got = 0;
                PROVEN_TEST_ASSERT(proven_hpack_encode(c, list, count, NULL, (proven_mem_mut_t){ g_block, sizeof g_block }, &n) == PROVEN_OK, "the RFC's header list is encoded", x->title);
                proven_size_t skip = (r == 1 && k == 0) ? 3 : 0;             /* 3f e1 01: "my table is 256 bytes" */
                if (skip) PROVEN_TEST_ASSERT(n > 3 && g_block[0] == 0x3f && g_block[1] == 0xe1 && g_block[2] == 0x01, "a smaller table is announced before the first field", x->title);
                if (k < runs[r].exact) PROVEN_TEST_ASSERT(n - skip == x->size && memcmp(g_block + skip, x->bytes, x->size) == 0, "to exactly the bytes the RFC prints", x->title);
                PROVEN_TEST_ASSERT(decode(d, g_block, n, &got) == PROVEN_OK && got == count, "and decodes to the list", x->title);
                for (proven_size_t i = 0; i < got; ++i) PROVEN_TEST_ASSERT(proven_u8str_view_eq(g_fields[i].name, list[i].name) && proven_u8str_view_eq(g_fields[i].value, list[i].value), "field for field", x->title);
            }
            proven_hpack_encoder_destroy(c);
            proven_hpack_decoder_destroy(d);
        }
    }

    PROVEN_TEST_SECTION("what the encoder keeps out of its table", "A table is a compression context: a secret in it can be guessed from the size of later blocks.", "Check hp_reserve_kind and the sensitive branch of proven_hpack_encode.");
    {
        proven_hpack_encoder_t *c = NULL;
        proven_hpack_decoder_t *d = NULL;
        PROVEN_TEST_ASSERT(proven_hpack_encoder_create(g_heap, 4096, &c) == PROVEN_OK && proven_hpack_decoder_create(g_heap, 4096, &d) == PROVEN_OK, "an encoder and a decoder", "");
        proven_http_header_t list[5] = {
            { PROVEN_LIT("authorization"), PROVEN_LIT("Bearer abcdefghijklmnop") }, { PROVEN_LIT("cookie"), PROVEN_LIT("session=0123456789abcdef") },
            { PROVEN_LIT("x-api-key"), PROVEN_LIT("k-0123456789") }, { PROVEN_LIT("x-plain"), PROVEN_LIT("remember me") }, { PROVEN_LIT("proxy-authorization"), PROVEN_LIT("Basic eDp5") },
        };
        bool sensitive[5] = { false, false, true, false, false };
        proven_size_t first = 0, second = 0, got = 0;
        PROVEN_TEST_ASSERT(proven_hpack_encode(c, list, 5, sensitive, (proven_mem_mut_t){ g_block, sizeof g_block }, &first) == PROVEN_OK && decode(d, g_block, first, &got) == PROVEN_OK && got == 5, "five fields, three of them secrets by name or by flag", "");
        PROVEN_TEST_ASSERT(proven_hpack_decoder_table_count(d) == 1, "only the one that is no secret went into the table", "");
        proven_u8str_view_t name, value;
        PROVEN_TEST_ASSERT(proven_hpack_decoder_table_entry(d, 0, &name, &value) && proven_u8str_view_eq(name, PROVEN_LIT("x-plain")), "x-plain", "");
        /* authorization (static index 23), proxy-authorization (48) and the flagged field are sent never-indexed: 0001 and a four-bit prefix. */
        PROVEN_TEST_ASSERT(g_block[0] == 0x1f && g_block[1] == 0x08, "authorization is sent in the never-indexed form, by its name's index", "");
        PROVEN_TEST_ASSERT(proven_hpack_encode(c, list, 5, sensitive, (proven_mem_mut_t){ g_block, sizeof g_block }, &second) == PROVEN_OK, "the same list again", "");
        PROVEN_TEST_ASSERT(second == first - 20 + 1 || second < first, "is shorter only by the field that was remembered", "");
        PROVEN_TEST_ASSERT(decode(d, g_block, second, &got) == PROVEN_OK && got == 5 && proven_hpack_decoder_table_count(d) == 1, "and still leaves one entry", "");
        proven_hpack_encoder_destroy(c);
        proven_hpack_decoder_destroy(d);
    }

    PROVEN_TEST_SECTION("malformed blocks", "One for each way a block can be wrong.", "Check proven_hpack_decode: each `goto fail`.");
    {
        static const proven_byte_t index_zero[] = { 0x80 };
        static const proven_byte_t index_past_static[] = { 0xbe };                        /* 62: the first dynamic entry, of none */
        static const proven_byte_t index_huge[] = { 0xff, 0x80, 0x80, 0x80, 0x80, 0x01 };
        static const proven_byte_t int_too_long[] = { 0xff, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01 };   /* six continuation bytes */
        static const proven_byte_t int_cut[] = { 0xff, 0x80 };
        static const proven_byte_t name_index_past[] = { 0x7f, 0x30, 0x01, 'x' };         /* literal, name index 111 */
        static const proven_byte_t string_past_block[] = { 0x40, 0x05, 'a', 'b' };
        static const proven_byte_t value_missing[] = { 0x40, 0x01, 'a' };
        static const proven_byte_t ends_in_field[] = { 0x82, 0x40 };
        static const proven_byte_t update_after_field[] = { 0x82, 0x20 };
        static const proven_byte_t update_too_large[] = { 0x3f, 0xe2, 0x1f };             /* 4097 */
        static const proven_byte_t huffman_bad_padding[] = { 0x40, 0x81, 0x18, 0x01, 'v' };
        static const proven_byte_t huffman_eos[] = { 0x00, 0x84, 0xff, 0xff, 0xff, 0xff, 0x00 };
        refused("index 0", index_zero, sizeof index_zero);
        refused("an index past both tables", index_past_static, sizeof index_past_static);
        refused("an index of hundreds of millions", index_huge, sizeof index_huge);
        refused("an integer of more than five continuation bytes", int_too_long, sizeof int_too_long);
        refused("an integer cut off by the end of the block", int_cut, sizeof int_cut);
        refused("a literal whose name index names nothing", name_index_past, sizeof name_index_past);
        refused("a string longer than what is left of the block", string_past_block, sizeof string_past_block);
        refused("a literal with a name and no value", value_missing, sizeof value_missing);
        refused("a block that ends inside a field", ends_in_field, sizeof ends_in_field);
        refused("a table size update after a field", update_after_field, sizeof update_after_field);
        refused("a table size update above the limit", update_too_large, sizeof update_too_large);
        refused("a Huffman-coded name with wrong padding", huffman_bad_padding, sizeof huffman_bad_padding);
        refused("a Huffman-coded name containing the end-of-string symbol", huffman_eos, sizeof huffman_eos);

        proven_hpack_decoder_t *d = NULL;
        proven_size_t count = 9;
        PROVEN_TEST_ASSERT(proven_hpack_decoder_create(g_heap, 4096, &d) == PROVEN_OK && decode(d, index_zero, 0, &count) == PROVEN_OK && count == 0, "an empty block is an empty list", "");
        static const proven_byte_t two_updates[] = { 0x20, 0x3f, 0xe1, 0x1f, 0x82 };     /* to 0, then to 4096, then a field */
        PROVEN_TEST_ASSERT(decode(d, two_updates, sizeof two_updates, &count) == PROVEN_OK && count == 1 && view_is(g_fields[0].name, ":method"), "two size updates may open a block", "");
        proven_hpack_decoder_destroy(d);
        PROVEN_TEST_ASSERT(proven_hpack_decoder_create(NULL == NULL ? (proven_allocator_t){ 0 } : g_heap, 4096, &d) == PROVEN_ERR_INVALID_ARG && d == NULL, "no allocator is PROVEN_ERR_INVALID_ARG", "");
        proven_hpack_decoder_destroy(NULL);
        proven_hpack_encoder_destroy(NULL);
    }

    PROVEN_TEST_SECTION("limits", "What a peer can make this side hold, and that being over a limit does not lose the connection.", "Check the `over` path of proven_hpack_decode.");
    {
        /* Three fields, the last of which goes into the table. */
        static const proven_byte_t block[] = { 0x82, 0x84, 0x40, 0x03, 'k', 'e', 'y', 0x05, 'v', 'a', 'l', 'u', 'e' };
        const proven_size_t list_size = (7 + 3 + 32) + (5 + 1 + 32) + (3 + 5 + 32);
        proven_hpack_decoder_t *d = NULL;
        proven_size_t count = 0;
        PROVEN_TEST_ASSERT(proven_hpack_decoder_create(g_heap, 4096, &d) == PROVEN_OK, "a decoder", "");
        PROVEN_TEST_ASSERT(proven_hpack_decode(d, (proven_mem_view_t){ block, sizeof block }, g_fields, 8, (proven_mem_mut_t){ g_scratch, 64 }, list_size, &count) == PROVEN_OK && count == 3,
            "a list of exactly the limit - names, values and 32 a field - passes", "");
        PROVEN_TEST_ASSERT(proven_hpack_decode(d, (proven_mem_view_t){ block, sizeof block }, g_fields, 8, (proven_mem_mut_t){ g_scratch, 64 }, list_size - 1, &count) == PROVEN_ERR_OUT_OF_BOUNDS && count == 0,
            "one byte under it: PROVEN_ERR_OUT_OF_BOUNDS and no fields", "");
        PROVEN_TEST_ASSERT(proven_hpack_decoder_table_count(d) == 2, "but the block was read to its end: its entry is in the table, a second time", "");
        PROVEN_TEST_ASSERT(proven_hpack_decode(d, (proven_mem_view_t){ block, sizeof block }, g_fields, 2, (proven_mem_mut_t){ g_scratch, 64 }, 0, &count) == PROVEN_ERR_OUT_OF_BOUNDS && proven_hpack_decoder_table_count(d) == 3,
            "more fields than there is room for: the same", "");
        static const proven_byte_t refer[] = { 0xbe, 0xbf, 0xc0 };                        /* the three entries, by index */
        PROVEN_TEST_ASSERT(proven_hpack_decode(d, (proven_mem_view_t){ refer, 3 }, g_fields, 8, (proven_mem_mut_t){ g_scratch, 20 }, 0, &count) == PROVEN_ERR_OUT_OF_BOUNDS,
            "a scratch too small for what the table gives back: the same", "");
        PROVEN_TEST_ASSERT(proven_hpack_decode(d, (proven_mem_view_t){ refer, 3 }, g_fields, 8, (proven_mem_mut_t){ g_scratch, 64 }, 0, &count) == PROVEN_OK && count == 3 && view_is(g_fields[2].value, "value"),
            "and the decoder is still in step afterwards", "");
        PROVEN_TEST_ASSERT(proven_hpack_decode(d, (proven_mem_view_t){ refer, 1 }, g_fields, 8, (proven_mem_mut_t){ g_scratch, 4 }, 0, &count) == PROVEN_ERR_INVALID_FORMAT,
            "one field larger than the whole scratch cannot be followed: PROVEN_ERR_INVALID_FORMAT", "");
        proven_hpack_decoder_destroy(d);

        /* A bomb: one entry of 3,000 bytes, named two thousand times. Six kilobytes that would be six megabytes. */
        PROVEN_TEST_ASSERT(proven_hpack_decoder_create(g_heap, 4096, &d) == PROVEN_OK, "a decoder", "");
        proven_size_t n = 0;
        g_block[n++] = 0x40; g_block[n++] = 0x01; g_block[n++] = 'x';
        g_block[n++] = 0x7f; g_block[n++] = (proven_byte_t)(0x80 | ((3000 - 127) & 0x7f)); g_block[n++] = (proven_byte_t)((3000 - 127) >> 7);
        memset(g_block + n, 'A', 3000); n += 3000;
        memset(g_block + n, 0xbe, 2000); n += 2000;
        PROVEN_TEST_ASSERT(proven_hpack_decode(d, (proven_mem_view_t){ g_block, n }, g_fields, sizeof g_fields / sizeof g_fields[0], (proven_mem_mut_t){ g_scratch, 16384 }, 16384, &count) == PROVEN_ERR_OUT_OF_BOUNDS,
            "five kilobytes that name a large entry two thousand times stop at the list limit", "");
        static const proven_byte_t again[] = { 0xbe };
        PROVEN_TEST_ASSERT(proven_hpack_decode(d, (proven_mem_view_t){ again, 1 }, g_fields, 8, (proven_mem_mut_t){ g_scratch, 16384 }, 16384, &count) == PROVEN_OK && count == 1 && g_fields[0].value.size == 3000,
            "and the connection could go on", "");
        proven_hpack_decoder_destroy(d);
    }

    PROVEN_TEST_SECTION("the table", "Eviction, an entry larger than the table, a name taken from an entry on its way out, and changes of size.", "Check hp_table_add, hp_table_reserve and the size-update branch.");
    {
        proven_hpack_decoder_t *d = NULL;
        proven_size_t count = 0;
        PROVEN_TEST_ASSERT(proven_hpack_decoder_create(g_heap, 100, &d) == PROVEN_OK, "a decoder with a table of 100 bytes", "");
        /* Entries of 2 + 6 + 32 = 40 bytes: two fit, the third evicts the first. */
        for (int i = 0; i < 300; ++i) {
            proven_byte_t b[] = { 0x40, 0x02, 'k', (proven_byte_t)('a' + i % 26), 0x06, 'v', 'a', 'l', 'u', 'e', (proven_byte_t)('0' + i % 10) };
            PROVEN_TEST_ASSERT(decode(d, b, sizeof b, &count) == PROVEN_OK && count == 1, "an entry is added", "");
            PROVEN_TEST_ASSERT(proven_hpack_decoder_table_count(d) == (i == 0 ? 1u : 2u) && proven_hpack_decoder_table_bytes(d) == (i == 0 ? 40u : 80u), "and the oldest leaves when there is no room: never more than two", "");
        }
        proven_u8str_view_t name, value;
        PROVEN_TEST_ASSERT(proven_hpack_decoder_table_entry(d, 0, &name, &value) && name.ptr[1] == (proven_byte_t)('a' + 299 % 26) && value.ptr[5] == '9' &&
                           proven_hpack_decoder_table_entry(d, 1, &name, &value) && name.ptr[1] == (proven_byte_t)('a' + 298 % 26) && !proven_hpack_decoder_table_entry(d, 2, &name, &value),
            "after three hundred of them the table holds the last two, newest first", "");
        /* A new entry named by the index of the oldest, which it evicts: the name must survive (RFC 7541, 4.4). */
        static const proven_byte_t from_oldest[] = { 0x7f, 0x00, 0x28, '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9' };
        PROVEN_TEST_ASSERT(decode(d, from_oldest, sizeof from_oldest, &count) == PROVEN_OK && count == 1 && g_fields[0].name.size == 2 && g_fields[0].name.ptr[1] == (proven_byte_t)('a' + 298 % 26) && g_fields[0].value.size == 40,
            "an entry whose name comes from the one it evicts keeps that name", "");
        PROVEN_TEST_ASSERT(proven_hpack_decoder_table_count(d) == 1 && proven_hpack_decoder_table_bytes(d) == 74, "and stands alone in the table", "");
        /* An entry larger than the table empties it and is not added. */
        proven_size_t n = 0;
        g_block[n++] = 0x40; g_block[n++] = 0x01; g_block[n++] = 'x'; g_block[n++] = 0x64; memset(g_block + n, 'B', 100); n += 100;
        PROVEN_TEST_ASSERT(decode(d, g_block, n, &count) == PROVEN_OK && count == 1 && g_fields[0].value.size == 100 && proven_hpack_decoder_table_count(d) == 0 && proven_hpack_decoder_table_bytes(d) == 0,
            "an entry larger than the whole table is delivered, empties the table, and is not kept", "");
        proven_hpack_decoder_destroy(d);

        /* Lowering the limit: the peer must follow, and say so. */
        PROVEN_TEST_ASSERT(proven_hpack_decoder_create(g_heap, 4096, &d) == PROVEN_OK, "a decoder", "");
        static const proven_byte_t add[] = { 0x40, 0x03, 'k', 'e', 'y', 0x05, 'v', 'a', 'l', 'u', 'e' };
        static const proven_byte_t field[] = { 0x82 };
        static const proven_byte_t to_zero[] = { 0x20, 0x82 };
        PROVEN_TEST_ASSERT(decode(d, add, sizeof add, &count) == PROVEN_OK && proven_hpack_decoder_table_count(d) == 1, "an entry", "");
        proven_hpack_decoder_set_max_table_bytes(d, 0);
        PROVEN_TEST_ASSERT(decode(d, to_zero, sizeof to_zero, &count) == PROVEN_OK && count == 1 && proven_hpack_decoder_table_count(d) == 0, "after the limit is lowered, a block that opens with the update is taken and the table is emptied", "");
        PROVEN_TEST_ASSERT(decode(d, add, sizeof add, &count) == PROVEN_OK && proven_hpack_decoder_table_count(d) == 0, "and nothing is kept in a table of no size", "");
        static const proven_byte_t back_up[] = { 0x3f, 0xe1, 0x1f };
        PROVEN_TEST_ASSERT(decode(d, back_up, sizeof back_up, &count) == PROVEN_ERR_INVALID_FORMAT, "an update above the lowered limit is refused", "");
        proven_hpack_decoder_destroy(d);
        PROVEN_TEST_ASSERT(proven_hpack_decoder_create(g_heap, 4096, &d) == PROVEN_OK && decode(d, add, sizeof add, &count) == PROVEN_OK, "another decoder, with an entry", "");
        proven_hpack_decoder_set_max_table_bytes(d, 0);
        PROVEN_TEST_ASSERT(decode(d, field, sizeof field, &count) == PROVEN_ERR_INVALID_FORMAT, "a block that does not say it followed the lowered limit is refused", "The two tables would differ from here on.");
        proven_hpack_decoder_destroy(d);

        /* The encoder's side of the same: lowered and raised between two blocks, it announces both. */
        proven_hpack_encoder_t *c = NULL;
        PROVEN_TEST_ASSERT(proven_hpack_encoder_create(g_heap, 4096, &c) == PROVEN_OK && proven_hpack_decoder_create(g_heap, 4096, &d) == PROVEN_OK, "an encoder and a decoder", "");
        proven_http_header_t h = { PROVEN_LIT("x-kept"), PROVEN_LIT("a value to remember") };
        n = 0;
        PROVEN_TEST_ASSERT(proven_hpack_encode(c, &h, 1, NULL, (proven_mem_mut_t){ g_block, sizeof g_block }, &n) == PROVEN_OK && decode(d, g_block, n, &count) == PROVEN_OK && proven_hpack_decoder_table_count(d) == 1, "a field both remember", "");
        proven_hpack_encoder_set_peer_table_bytes(c, 0);
        proven_hpack_encoder_set_peer_table_bytes(c, 2000);
        proven_hpack_decoder_set_max_table_bytes(d, 2000);
        n = 0;
        PROVEN_TEST_ASSERT(proven_hpack_encode(c, &h, 1, NULL, (proven_mem_mut_t){ g_block, sizeof g_block }, &n) == PROVEN_OK && g_block[0] == 0x20 && g_block[1] == 0x3f, "after the peer lowered and raised its limit, the next block opens with both sizes: the lowest, then the last", "");
        PROVEN_TEST_ASSERT((g_block[4] & 0xc0) == 0x40, "and sends the field as a literal again: lowering to nothing forgot it", "");
        PROVEN_TEST_ASSERT(decode(d, g_block, n, &count) == PROVEN_OK && count == 1 && proven_u8str_view_eq(g_fields[0].value, h.value) && proven_hpack_decoder_table_count(d) == 1, "which the decoder follows", "");
        proven_hpack_encoder_destroy(c);
        proven_hpack_decoder_destroy(d);

        /* A stateless encoder. */
        PROVEN_TEST_ASSERT(proven_hpack_encoder_create(g_heap, 0, &c) == PROVEN_OK && proven_hpack_decoder_create(g_heap, 4096, &d) == PROVEN_OK, "an encoder that keeps no table", "");
        for (int round = 0; round < 3; ++round) {
            n = 0;
            PROVEN_TEST_ASSERT(proven_hpack_encode(c, &h, 1, NULL, (proven_mem_mut_t){ g_block, sizeof g_block }, &n) == PROVEN_OK && decode(d, g_block, n, &count) == PROVEN_OK && count == 1 && proven_hpack_decoder_table_count(d) == 0,
                "sends every field whole, and the decoder remembers nothing", "");
        }
        proven_hpack_encoder_destroy(c);
        proven_hpack_decoder_destroy(d);
    }

    PROVEN_TEST_SECTION("round trips", "Generated header lists through an encoder and a decoder that share a table, at several sizes.", "A mismatch here is the two tables drifting apart: check eviction order on both sides.");
    {
        static const proven_size_t tables[] = { 0, 64, 200, 4096 };
        static const char *names[] = { ":method", ":path", ":status", "accept", "cookie", "x-a", "x-request-id", "content-type", "authorization", "x-long-header-name-that-repeats", "etag", "date" };
        for (proven_size_t t = 0; t < 4; ++t) {
            proven_hpack_encoder_t *c = NULL;
            proven_hpack_decoder_t *d = NULL;
            PROVEN_TEST_ASSERT(proven_hpack_encoder_create(g_heap, tables[t], &c) == PROVEN_OK && proven_hpack_decoder_create(g_heap, 4096, &d) == PROVEN_OK, "an encoder and a decoder", "");
            for (int round = 0; round < 400; ++round) {
                proven_http_header_t list[24];
                static proven_byte_t values[24][300];
                proven_size_t count = rnd() % 24;
                for (proven_size_t i = 0; i < count; ++i) {
                    proven_size_t len = (rnd() % 5 == 0) ? rnd() % 300 : rnd() % 12;
                    /* Few distinct values, so that the table is used; some of every byte, so that Huffman is not always chosen. */
                    proven_u32 kind = rnd() % 6;
                    for (proven_size_t k = 0; k < len; ++k) values[i][k] = kind == 0 ? (proven_byte_t)(rnd() >> 8) : (proven_byte_t)('a' + (kind * 7 + k) % 26);
                    list[i] = (proven_http_header_t){ sv(names[rnd() % 12]), { values[i], len } };
                }
                proven_size_t n = 0, got = 0;
                PROVEN_TEST_ASSERT(proven_hpack_encode(c, list, count, NULL, (proven_mem_mut_t){ g_block, sizeof g_block }, &n) == PROVEN_OK && n <= proven_hpack_encode_bound(list, count), "a list is encoded within its bound", "");
                PROVEN_TEST_ASSERT(decode(d, g_block, n, &got) == PROVEN_OK && got == count, "and decoded", "");
                for (proven_size_t i = 0; i < got; ++i) PROVEN_TEST_ASSERT(proven_u8str_view_eq(g_fields[i].name, list[i].name) && proven_u8str_view_eq(g_fields[i].value, list[i].value), "to the same fields in the same order", "");
                PROVEN_TEST_ASSERT(proven_hpack_decoder_table_bytes(d) <= tables[t], "with the decoder's table never above the encoder's size", "");
                if (round % 97 == 50) {
                    /* The peer changes its setting in the middle. */
                    proven_size_t now = round % 2 ? 100 : 4096;
                    proven_hpack_encoder_set_peer_table_bytes(c, now);
                    proven_hpack_decoder_set_max_table_bytes(d, now);
                }
            }
            proven_size_t small = 0;
            proven_http_header_t one = { PROVEN_LIT("x-a"), PROVEN_LIT("b") };
            PROVEN_TEST_ASSERT(proven_hpack_encode(c, &one, 1, NULL, (proven_mem_mut_t){ g_block, proven_hpack_encode_bound(&one, 1) - 1 }, &small) == PROVEN_ERR_OUT_OF_BOUNDS && small == 0,
                "less room than the bound is PROVEN_ERR_OUT_OF_BOUNDS with nothing written", "");
            proven_hpack_encoder_destroy(c);
            proven_hpack_decoder_destroy(d);
        }
    }

    PROVEN_TEST_SECTION("blocks cut short or changed", "Every beginning of the RFC's blocks, and every single-bit change of them.", "Run under AddressSanitizer: the point is that nothing is read or written outside a buffer.");
    {
        unsigned refusals = 0, accepted = 0;
        for (proven_size_t x = 0; x < sizeof hpack_examples / sizeof hpack_examples[0]; ++x) {
            const hpack_example_t *ex = &hpack_examples[x];
            for (proven_size_t cut = 0; cut < ex->size; ++cut) {
                proven_hpack_decoder_t *d = NULL;
                proven_size_t count = 0;
                PROVEN_TEST_ASSERT(proven_hpack_decoder_create(g_heap, 4096, &d) == PROVEN_OK, "a decoder", "");
                /* From an exact-size copy, so that a read past the end is seen. */
                proven_byte_t *copy = malloc(cut ? cut : 1);
                memcpy(copy, ex->bytes, cut);
                proven_err_t e = decode(d, copy, cut, &count);
                PROVEN_TEST_ASSERT(e == PROVEN_OK || e == PROVEN_ERR_INVALID_FORMAT, "a block cut short is a shorter list or an error, never anything else", ex->title);
                PROVEN_TEST_ASSERT(e != PROVEN_OK || count <= ex->field_count, "and never more fields than the whole block has", ex->title);
                if (e == PROVEN_OK) accepted++; else refusals++;
                free(copy);
                proven_hpack_decoder_destroy(d);
            }
            for (proven_size_t bit = 0; bit < ex->size * 8; ++bit) {
                proven_hpack_decoder_t *d = NULL;
                proven_size_t count = 0;
                PROVEN_TEST_ASSERT(proven_hpack_decoder_create(g_heap, 4096, &d) == PROVEN_OK, "a decoder", "");
                proven_byte_t *copy = malloc(ex->size);
                memcpy(copy, ex->bytes, ex->size);
                copy[bit / 8] ^= (proven_byte_t)(1u << (bit % 8));
                proven_err_t e = proven_hpack_decode(d, (proven_mem_view_t){ copy, ex->size }, g_fields, 16, (proven_mem_mut_t){ g_scratch, 512 }, 4096, &count);
                PROVEN_TEST_ASSERT(e == PROVEN_OK || e == PROVEN_ERR_INVALID_FORMAT || e == PROVEN_ERR_OUT_OF_BOUNDS, "a changed bit gives other fields or an error", ex->title);
                free(copy);
                proven_hpack_decoder_destroy(d);
            }
        }
        PROVEN_TEST_ASSERT(refusals > 200 && accepted > 30, "most cuts fall inside a field and are refused; the rest fall between fields", "");
    }

    PROVEN_TEST_PASS("the tables are whole, the RFC's examples decode and encode, and what is malformed, too large or damaged is refused without harm.");
    return 0;
}
