/* Generated from the published text of RFC 7541 (Appendix C) by a private program; do not
 * edit. Each example: the encoded bytes, the header list they decode to, and the dynamic
 * table afterwards (newest first) with its size. Examples C.3 to C.6 are sequences: each
 * group of three shares one table, and C.5 and C.6 use a table of 256 bytes. */

typedef struct { const char *name; const char *value; } hpack_pair_t;
typedef struct { const char *title; const unsigned char *bytes; unsigned size; const hpack_pair_t *fields; unsigned field_count;
                 const hpack_pair_t *table; unsigned table_count; unsigned table_size; } hpack_example_t;

static const struct { unsigned long value; unsigned prefix; } hpack_c1_integers[] = { { 10, 5 }, { 1337, 5 }, { 42, 8 } };

static const unsigned char hpack_c_bytes_0[] = { 0x40, 0x0a, 0x63, 0x75, 0x73, 0x74, 0x6f, 0x6d, 0x2d, 0x6b, 0x65, 0x79, 0x0d, 0x63, 0x75, 0x73, 0x74, 0x6f, 0x6d, 0x2d, 0x68, 0x65, 0x61, 0x64, 0x65, 0x72 };
static const hpack_pair_t hpack_c_fields_0[] = { { "custom-key", "custom-header" } };
static const hpack_pair_t hpack_c_table_0[] = { { "custom-key", "custom-header" } };
static const unsigned char hpack_c_bytes_1[] = { 0x04, 0x0c, 0x2f, 0x73, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2f, 0x70, 0x61, 0x74, 0x68 };
static const hpack_pair_t hpack_c_fields_1[] = { { ":path", "/sample/path" } };
static const hpack_pair_t hpack_c_table_1[] = { { 0, 0 } };
static const unsigned char hpack_c_bytes_2[] = { 0x10, 0x08, 0x70, 0x61, 0x73, 0x73, 0x77, 0x6f, 0x72, 0x64, 0x06, 0x73, 0x65, 0x63, 0x72, 0x65, 0x74 };
static const hpack_pair_t hpack_c_fields_2[] = { { "password", "secret" } };
static const hpack_pair_t hpack_c_table_2[] = { { 0, 0 } };
static const unsigned char hpack_c_bytes_3[] = { 0x82 };
static const hpack_pair_t hpack_c_fields_3[] = { { ":method", "GET" } };
static const hpack_pair_t hpack_c_table_3[] = { { 0, 0 } };
static const unsigned char hpack_c_bytes_4[] = { 0x82, 0x86, 0x84, 0x41, 0x0f, 0x77, 0x77, 0x77, 0x2e, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2e, 0x63, 0x6f, 0x6d };
static const hpack_pair_t hpack_c_fields_4[] = { { ":method", "GET" }, { ":scheme", "http" }, { ":path", "/" }, { ":authority", "www.example.com" } };
static const hpack_pair_t hpack_c_table_4[] = { { ":authority", "www.example.com" } };
static const unsigned char hpack_c_bytes_5[] = { 0x82, 0x86, 0x84, 0xbe, 0x58, 0x08, 0x6e, 0x6f, 0x2d, 0x63, 0x61, 0x63, 0x68, 0x65 };
static const hpack_pair_t hpack_c_fields_5[] = { { ":method", "GET" }, { ":scheme", "http" }, { ":path", "/" }, { ":authority", "www.example.com" }, { "cache-control", "no-cache" } };
static const hpack_pair_t hpack_c_table_5[] = { { "cache-control", "no-cache" }, { ":authority", "www.example.com" } };
static const unsigned char hpack_c_bytes_6[] = { 0x82, 0x87, 0x85, 0xbf, 0x40, 0x0a, 0x63, 0x75, 0x73, 0x74, 0x6f, 0x6d, 0x2d, 0x6b, 0x65, 0x79, 0x0c, 0x63, 0x75, 0x73, 0x74, 0x6f, 0x6d, 0x2d, 0x76, 0x61, 0x6c, 0x75, 0x65 };
static const hpack_pair_t hpack_c_fields_6[] = { { ":method", "GET" }, { ":scheme", "https" }, { ":path", "/index.html" }, { ":authority", "www.example.com" }, { "custom-key", "custom-value" } };
static const hpack_pair_t hpack_c_table_6[] = { { "custom-key", "custom-value" }, { "cache-control", "no-cache" }, { ":authority", "www.example.com" } };
static const unsigned char hpack_c_bytes_7[] = { 0x82, 0x86, 0x84, 0x41, 0x8c, 0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff };
static const hpack_pair_t hpack_c_fields_7[] = { { ":method", "GET" }, { ":scheme", "http" }, { ":path", "/" }, { ":authority", "www.example.com" } };
static const hpack_pair_t hpack_c_table_7[] = { { ":authority", "www.example.com" } };
static const unsigned char hpack_c_bytes_8[] = { 0x82, 0x86, 0x84, 0xbe, 0x58, 0x86, 0xa8, 0xeb, 0x10, 0x64, 0x9c, 0xbf };
static const hpack_pair_t hpack_c_fields_8[] = { { ":method", "GET" }, { ":scheme", "http" }, { ":path", "/" }, { ":authority", "www.example.com" }, { "cache-control", "no-cache" } };
static const hpack_pair_t hpack_c_table_8[] = { { "cache-control", "no-cache" }, { ":authority", "www.example.com" } };
static const unsigned char hpack_c_bytes_9[] = { 0x82, 0x87, 0x85, 0xbf, 0x40, 0x88, 0x25, 0xa8, 0x49, 0xe9, 0x5b, 0xa9, 0x7d, 0x7f, 0x89, 0x25, 0xa8, 0x49, 0xe9, 0x5b, 0xb8, 0xe8, 0xb4, 0xbf };
static const hpack_pair_t hpack_c_fields_9[] = { { ":method", "GET" }, { ":scheme", "https" }, { ":path", "/index.html" }, { ":authority", "www.example.com" }, { "custom-key", "custom-value" } };
static const hpack_pair_t hpack_c_table_9[] = { { "custom-key", "custom-value" }, { "cache-control", "no-cache" }, { ":authority", "www.example.com" } };
static const unsigned char hpack_c_bytes_10[] = { 0x48, 0x03, 0x33, 0x30, 0x32, 0x58, 0x07, 0x70, 0x72, 0x69, 0x76, 0x61, 0x74, 0x65, 0x61, 0x1d, 0x4d, 0x6f, 0x6e, 0x2c, 0x20, 0x32, 0x31, 0x20, 0x4f, 0x63, 0x74, 0x20, 0x32, 0x30, 0x31, 0x33, 0x20, 0x32, 0x30, 0x3a, 0x31, 0x33, 0x3a, 0x32, 0x31, 0x20, 0x47, 0x4d, 0x54, 0x6e, 0x17, 0x68, 0x74, 0x74, 0x70, 0x73, 0x3a, 0x2f, 0x2f, 0x77, 0x77, 0x77, 0x2e, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2e, 0x63, 0x6f, 0x6d };
static const hpack_pair_t hpack_c_fields_10[] = { { ":status", "302" }, { "cache-control", "private" }, { "date", "Mon, 21 Oct 2013 20:13:21 GMT" }, { "location", "https://www.example.com" } };
static const hpack_pair_t hpack_c_table_10[] = { { "location", "https://www.example.com" }, { "date", "Mon, 21 Oct 2013 20:13:21 GMT" }, { "cache-control", "private" }, { ":status", "302" } };
static const unsigned char hpack_c_bytes_11[] = { 0x48, 0x03, 0x33, 0x30, 0x37, 0xc1, 0xc0, 0xbf };
static const hpack_pair_t hpack_c_fields_11[] = { { ":status", "307" }, { "cache-control", "private" }, { "date", "Mon, 21 Oct 2013 20:13:21 GMT" }, { "location", "https://www.example.com" } };
static const hpack_pair_t hpack_c_table_11[] = { { ":status", "307" }, { "location", "https://www.example.com" }, { "date", "Mon, 21 Oct 2013 20:13:21 GMT" }, { "cache-control", "private" } };
static const unsigned char hpack_c_bytes_12[] = { 0x88, 0xc1, 0x61, 0x1d, 0x4d, 0x6f, 0x6e, 0x2c, 0x20, 0x32, 0x31, 0x20, 0x4f, 0x63, 0x74, 0x20, 0x32, 0x30, 0x31, 0x33, 0x20, 0x32, 0x30, 0x3a, 0x31, 0x33, 0x3a, 0x32, 0x32, 0x20, 0x47, 0x4d, 0x54, 0xc0, 0x5a, 0x04, 0x67, 0x7a, 0x69, 0x70, 0x77, 0x38, 0x66, 0x6f, 0x6f, 0x3d, 0x41, 0x53, 0x44, 0x4a, 0x4b, 0x48, 0x51, 0x4b, 0x42, 0x5a, 0x58, 0x4f, 0x51, 0x57, 0x45, 0x4f, 0x50, 0x49, 0x55, 0x41, 0x58, 0x51, 0x57, 0x45, 0x4f, 0x49, 0x55, 0x3b, 0x20, 0x6d, 0x61, 0x78, 0x2d, 0x61, 0x67, 0x65, 0x3d, 0x33, 0x36, 0x30, 0x30, 0x3b, 0x20, 0x76, 0x65, 0x72, 0x73, 0x69, 0x6f, 0x6e, 0x3d, 0x31 };
static const hpack_pair_t hpack_c_fields_12[] = { { ":status", "200" }, { "cache-control", "private" }, { "date", "Mon, 21 Oct 2013 20:13:22 GMT" }, { "location", "https://www.example.com" }, { "content-encoding", "gzip" }, { "set-cookie", "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1" } };
static const hpack_pair_t hpack_c_table_12[] = { { "set-cookie", "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1" }, { "content-encoding", "gzip" }, { "date", "Mon, 21 Oct 2013 20:13:22 GMT" } };
static const unsigned char hpack_c_bytes_13[] = { 0x48, 0x82, 0x64, 0x02, 0x58, 0x85, 0xae, 0xc3, 0x77, 0x1a, 0x4b, 0x61, 0x96, 0xd0, 0x7a, 0xbe, 0x94, 0x10, 0x54, 0xd4, 0x44, 0xa8, 0x20, 0x05, 0x95, 0x04, 0x0b, 0x81, 0x66, 0xe0, 0x82, 0xa6, 0x2d, 0x1b, 0xff, 0x6e, 0x91, 0x9d, 0x29, 0xad, 0x17, 0x18, 0x63, 0xc7, 0x8f, 0x0b, 0x97, 0xc8, 0xe9, 0xae, 0x82, 0xae, 0x43, 0xd3 };
static const hpack_pair_t hpack_c_fields_13[] = { { ":status", "302" }, { "cache-control", "private" }, { "date", "Mon, 21 Oct 2013 20:13:21 GMT" }, { "location", "https://www.example.com" } };
static const hpack_pair_t hpack_c_table_13[] = { { "location", "https://www.example.com" }, { "date", "Mon, 21 Oct 2013 20:13:21 GMT" }, { "cache-control", "private" }, { ":status", "302" } };
static const unsigned char hpack_c_bytes_14[] = { 0x48, 0x83, 0x64, 0x0e, 0xff, 0xc1, 0xc0, 0xbf };
static const hpack_pair_t hpack_c_fields_14[] = { { ":status", "307" }, { "cache-control", "private" }, { "date", "Mon, 21 Oct 2013 20:13:21 GMT" }, { "location", "https://www.example.com" } };
static const hpack_pair_t hpack_c_table_14[] = { { ":status", "307" }, { "location", "https://www.example.com" }, { "date", "Mon, 21 Oct 2013 20:13:21 GMT" }, { "cache-control", "private" } };
static const unsigned char hpack_c_bytes_15[] = { 0x88, 0xc1, 0x61, 0x96, 0xd0, 0x7a, 0xbe, 0x94, 0x10, 0x54, 0xd4, 0x44, 0xa8, 0x20, 0x05, 0x95, 0x04, 0x0b, 0x81, 0x66, 0xe0, 0x84, 0xa6, 0x2d, 0x1b, 0xff, 0xc0, 0x5a, 0x83, 0x9b, 0xd9, 0xab, 0x77, 0xad, 0x94, 0xe7, 0x82, 0x1d, 0xd7, 0xf2, 0xe6, 0xc7, 0xb3, 0x35, 0xdf, 0xdf, 0xcd, 0x5b, 0x39, 0x60, 0xd5, 0xaf, 0x27, 0x08, 0x7f, 0x36, 0x72, 0xc1, 0xab, 0x27, 0x0f, 0xb5, 0x29, 0x1f, 0x95, 0x87, 0x31, 0x60, 0x65, 0xc0, 0x03, 0xed, 0x4e, 0xe5, 0xb1, 0x06, 0x3d, 0x50, 0x07 };
static const hpack_pair_t hpack_c_fields_15[] = { { ":status", "200" }, { "cache-control", "private" }, { "date", "Mon, 21 Oct 2013 20:13:22 GMT" }, { "location", "https://www.example.com" }, { "content-encoding", "gzip" }, { "set-cookie", "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1" } };
static const hpack_pair_t hpack_c_table_15[] = { { "set-cookie", "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1" }, { "content-encoding", "gzip" }, { "date", "Mon, 21 Oct 2013 20:13:22 GMT" } };

static const hpack_example_t hpack_examples[] = {
    { "C.2.1.  Literal Header Field with Indexing", hpack_c_bytes_0, 26, hpack_c_fields_0, 1, hpack_c_table_0, 1, 55 },
    { "C.2.2.  Literal Header Field without Indexing", hpack_c_bytes_1, 14, hpack_c_fields_1, 1, hpack_c_table_1, 0, 0 },
    { "C.2.3.  Literal Header Field Never Indexed", hpack_c_bytes_2, 17, hpack_c_fields_2, 1, hpack_c_table_2, 0, 0 },
    { "C.2.4.  Indexed Header Field", hpack_c_bytes_3, 1, hpack_c_fields_3, 1, hpack_c_table_3, 0, 0 },
    { "C.3.1.  First Request", hpack_c_bytes_4, 20, hpack_c_fields_4, 4, hpack_c_table_4, 1, 57 },
    { "C.3.2.  Second Request", hpack_c_bytes_5, 14, hpack_c_fields_5, 5, hpack_c_table_5, 2, 110 },
    { "C.3.3.  Third Request", hpack_c_bytes_6, 29, hpack_c_fields_6, 5, hpack_c_table_6, 3, 164 },
    { "C.4.1.  First Request", hpack_c_bytes_7, 17, hpack_c_fields_7, 4, hpack_c_table_7, 1, 57 },
    { "C.4.2.  Second Request", hpack_c_bytes_8, 12, hpack_c_fields_8, 5, hpack_c_table_8, 2, 110 },
    { "C.4.3.  Third Request", hpack_c_bytes_9, 24, hpack_c_fields_9, 5, hpack_c_table_9, 3, 164 },
    { "C.5.1.  First Response", hpack_c_bytes_10, 70, hpack_c_fields_10, 4, hpack_c_table_10, 4, 222 },
    { "C.5.2.  Second Response", hpack_c_bytes_11, 8, hpack_c_fields_11, 4, hpack_c_table_11, 4, 222 },
    { "C.5.3.  Third Response", hpack_c_bytes_12, 98, hpack_c_fields_12, 6, hpack_c_table_12, 3, 215 },
    { "C.6.1.  First Response", hpack_c_bytes_13, 54, hpack_c_fields_13, 4, hpack_c_table_13, 4, 222 },
    { "C.6.2.  Second Response", hpack_c_bytes_14, 8, hpack_c_fields_14, 4, hpack_c_table_14, 4, 222 },
    { "C.6.3.  Third Response", hpack_c_bytes_15, 79, hpack_c_fields_15, 6, hpack_c_table_15, 3, 215 },
};
