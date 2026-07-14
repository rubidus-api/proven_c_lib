#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * Written from the contract in include/proven/encode.h before any of it existed
 * (docs/TESTING.md §5.1). Hex and Base64 are standards, so the encoding half is judged against
 * the STANDARD'S OWN vectors - RFC 4648's "", "f", "fo", "foo", "foob", "fooba", "foobar",
 * whose Base64 is the canonical example every implementation is checked against, verified here
 * against Python's base64/binascii before being trusted. A round-trip test proves the two
 * directions agree; the vectors prove they agree with the rest of the world.
 *
 * The decode half is judged on what it REFUSES: a stray character, a wrong length, a buffer one
 * byte too small. A decoder that reads text from outside the program and trusts it is a memory
 * bug or a silent truncation waiting to happen, and refusing is the whole value.
 */

/* the RFC 4648 §10 progression */
static const char *RFC_IN[]  = { "",  "f",    "fo",   "foo",  "foob",     "fooba",     "foobar"     };
static const char *RFC_B64[] = { "",  "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=",  "Zm9vYmFy"   };
static const char *RFC_URL[] = { "",  "Zg",   "Zm8",  "Zm9v", "Zm9vYg",   "Zm9vYmE",   "Zm9vYmFy"   };
static const char *RFC_HEX[] = { "",  "66",   "666f", "666f6f","666f6f62","666f6f6261","666f6f626172"};

static proven_mem_view_t vv(const char *s) {
    return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}

int main(void) {
    PROVEN_TEST_SUITE("hex and Base64 by use case",
        "Encoding matches RFC 4648's own vectors; decoding round-trips and refuses malformed input and undersized buffers rather than guessing.",
        "Inspect src/proven/encode.c. An encoding mismatch is a wrong alphabet or padding; a decode that accepts junk is the memory bug this module exists to prevent.");

    proven_byte_t buf[64];
    proven_size_t w = 0;

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("hex encode matches, and the size function is right",
        "Two lowercase chars per byte, no terminator, and the size you must allocate is a call not a guess.",
        "");
    // ---------------------------------------------------------------
    for (int i = 0; i < 7; ++i) {
        proven_mem_view_t in = vv(RFC_IN[i]);
        PROVEN_TEST_ASSERT(proven_hex_encoded_size(in.size) == strlen(RFC_HEX[i]),
            "hex_encoded_size must be two per byte", "");
        proven_err_t e = proven_hex_encode(in, buf, sizeof buf, &w);
        PROVEN_TEST_ASSERT(proven_is_ok(e) && w == strlen(RFC_HEX[i]) &&
                           memcmp(buf, RFC_HEX[i], w) == 0,
            "hex must match the known value, lowercase", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("Base64 and Base64URL match RFC 4648",
        "The standard alphabet padded with '=', and the URL alphabet with no padding.",
        "");
    // ---------------------------------------------------------------
    for (int i = 0; i < 7; ++i) {
        proven_mem_view_t in = vv(RFC_IN[i]);

        proven_err_t e = proven_base64_encode(in, buf, sizeof buf, &w);
        PROVEN_TEST_ASSERT(proven_is_ok(e) && w == strlen(RFC_B64[i]) && memcmp(buf, RFC_B64[i], w) == 0,
            "standard Base64 must match RFC 4648, padding included", "");

        e = proven_base64url_encode(in, buf, sizeof buf, &w);
        PROVEN_TEST_ASSERT(proven_is_ok(e) && w == strlen(RFC_URL[i]) && memcmp(buf, RFC_URL[i], w) == 0,
            "Base64URL must use -/_ and emit no padding", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("decode is the exact inverse, for both alphabets and padded or not",
        "A decoder that only accepts what it emits rejects half the Base64 in the world.",
        "");
    // ---------------------------------------------------------------
    for (int i = 0; i < 7; ++i) {
        proven_mem_view_t want = vv(RFC_IN[i]);

        /* hex, both cases */
        proven_err_t e = proven_hex_decode(vv(RFC_HEX[i]), buf, sizeof buf, &w);
        PROVEN_TEST_ASSERT(proven_is_ok(e) && w == want.size && memcmp(buf, want.ptr, w) == 0,
            "hex decode must invert the encode", "");

        char upper[32];
        for (proven_size_t k = 0; k <= strlen(RFC_HEX[i]); ++k) {
            char c = RFC_HEX[i][k];
            upper[k] = (c >= 'a' && c <= 'f') ? (char)(c - 32) : c;
        }
        e = proven_hex_decode(vv(upper), buf, sizeof buf, &w);
        PROVEN_TEST_ASSERT(proven_is_ok(e) && w == want.size && memcmp(buf, want.ptr, w) == 0,
            "UPPERCASE hex must decode the same", "");

        /* base64: padded standard, and unpadded url - both must decode to the same bytes */
        e = proven_base64_decode(vv(RFC_B64[i]), buf, sizeof buf, &w);
        PROVEN_TEST_ASSERT(proven_is_ok(e) && w == want.size && memcmp(buf, want.ptr, w) == 0,
            "standard padded Base64 must decode", "");
        e = proven_base64_decode(vv(RFC_URL[i]), buf, sizeof buf, &w);
        PROVEN_TEST_ASSERT(proven_is_ok(e) && w == want.size && memcmp(buf, want.ptr, w) == 0,
            "unpadded URL Base64 must decode to the same bytes", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("malformed input is refused, not guessed",
        "This is the reason to have a bounds-safe decoder instead of a two-line loop.",
        "A stray byte, a bad length, or bad padding must be INVALID_ENCODING with nothing committed.");
    // ---------------------------------------------------------------
    {
        PROVEN_TEST_ASSERT(proven_hex_decode(vv("abc"), buf, sizeof buf, &w) == PROVEN_ERR_INVALID_ENCODING,
            "odd-length hex is invalid", "");
        PROVEN_TEST_ASSERT(proven_hex_decode(vv("gg"), buf, sizeof buf, &w) == PROVEN_ERR_INVALID_ENCODING,
            "a non-hex character is invalid", "");
        PROVEN_TEST_ASSERT(proven_hex_decode(vv("6 6"), buf, sizeof buf, &w) == PROVEN_ERR_INVALID_ENCODING,
            "embedded whitespace is invalid, not skipped", "");

        PROVEN_TEST_ASSERT(proven_base64_decode(vv("Zg="), buf, sizeof buf, &w) == PROVEN_ERR_INVALID_ENCODING,
            "a Base64 length that is not a valid padded/unpadded form is invalid", "");
        PROVEN_TEST_ASSERT(proven_base64_decode(vv("Z!=="), buf, sizeof buf, &w) == PROVEN_ERR_INVALID_ENCODING,
            "a stray character in Base64 is invalid", "");
        PROVEN_TEST_ASSERT(proven_base64_decode(vv("===="), buf, sizeof buf, &w) == PROVEN_ERR_INVALID_ENCODING,
            "all-padding is invalid", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("an output buffer one byte too small is refused, not truncated",
        "A silently truncated encoding is a wrong answer that looks like a right one.",
        "");
    // ---------------------------------------------------------------
    {
        proven_byte_t tiny[3];
        /* "foobar" hex needs 12 chars; give it 3 */
        PROVEN_TEST_ASSERT(proven_hex_encode(vv("foobar"), tiny, sizeof tiny, &w) == PROVEN_ERR_OUT_OF_BOUNDS,
            "hex encode into a too-small buffer is refused", "");
        PROVEN_TEST_ASSERT(proven_base64_encode(vv("foobar"), tiny, sizeof tiny, &w) == PROVEN_ERR_OUT_OF_BOUNDS,
            "base64 encode into a too-small buffer is refused", "");
        PROVEN_TEST_ASSERT(proven_base64_decode(vv("Zm9vYmFy"), tiny, sizeof tiny, &w) == PROVEN_ERR_OUT_OF_BOUNDS,
            "base64 decode into a too-small buffer is refused", "");
        /* refusal must not have written a partial prefix */
        proven_byte_t sentinel[8];
        memset(sentinel, 0xAA, sizeof sentinel);
        (void)proven_hex_encode(vv("foobar"), sentinel, 3, &w);
        PROVEN_TEST_ASSERT(sentinel[0] == 0xAA && sentinel[2] == 0xAA,
            "a refused encode writes nothing", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("round-trip over arbitrary bytes, including NUL and high bytes",
        "Text encodings must be transparent to binary: a token full of zero bytes must survive.",
        "");
    // ---------------------------------------------------------------
    {
        proven_byte_t raw[256];
        for (int i = 0; i < 256; ++i) raw[i] = (proven_byte_t)i;
        proven_mem_view_t in = { .ptr = raw, .size = sizeof raw };

        proven_byte_t enc[512], dec[256];
        proven_size_t en = 0, dn = 0;

        PROVEN_TEST_ASSERT(proven_is_ok(proven_base64_encode(in, enc, sizeof enc, &en)), "encode all 256 bytes", "");
        PROVEN_TEST_ASSERT(proven_is_ok(proven_base64_decode((proven_mem_view_t){ enc, en }, dec, sizeof dec, &dn)),
            "decode back", "");
        PROVEN_TEST_ASSERT(dn == sizeof raw && memcmp(dec, raw, sizeof raw) == 0,
            "every one of the 256 byte values must round-trip through Base64", "");

        PROVEN_TEST_ASSERT(proven_is_ok(proven_hex_encode(in, enc, sizeof enc, &en)), "hex encode", "");
        PROVEN_TEST_ASSERT(proven_is_ok(proven_hex_decode((proven_mem_view_t){ enc, en }, dec, sizeof dec, &dn)),
            "hex decode back", "");
        PROVEN_TEST_ASSERT(dn == sizeof raw && memcmp(dec, raw, sizeof raw) == 0,
            "and through hex", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the empty input, and the argument guards",
        "",
        "");
    // ---------------------------------------------------------------
    {
        PROVEN_TEST_ASSERT(proven_is_ok(proven_hex_encode(vv(""), buf, sizeof buf, &w)) && w == 0,
            "encoding nothing writes nothing and succeeds", "");
        PROVEN_TEST_ASSERT(proven_is_ok(proven_base64_decode(vv(""), buf, sizeof buf, &w)) && w == 0,
            "decoding nothing succeeds", "");
        /* {NULL, >0} must be refused, not dereferenced */
        PROVEN_TEST_ASSERT(proven_hex_encode((proven_mem_view_t){ NULL, 4 }, buf, sizeof buf, &w) == PROVEN_ERR_INVALID_ARG,
            "a NULL view with a nonzero size is INVALID_ARG", "");
    }

    PROVEN_TEST_PASS("hex and Base64 encode to the standard, decode as the inverse, and refuse what they cannot honestly represent.");
    return 0;
}
