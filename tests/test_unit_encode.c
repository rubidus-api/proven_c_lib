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

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a size that cannot be represented is reported, not wrapped (RFC-0006 H-001)",
        "The size helpers multiply and add in size_t. At the top of the range those operations wrap, and a wrapped size is a small number: it passes a capacity check it should have failed, and the encoder then writes past what the caller reserved.",
        "The helpers answer SIZE_MAX for an output size that cannot be represented. Valid hex output is always even and valid padded Base64 output is always a multiple of four, so neither can be SIZE_MAX by accident. Zero cannot be the sentinel: it is the honest answer for empty input.");
    // ---------------------------------------------------------------
    {
        const proven_size_t M = PROVEN_SIZE_MAX;

        /* Ordinary values first: the sentinel must not have moved anything real. */
        PROVEN_TEST_ASSERT(proven_hex_encoded_size(0) == 0, "hex: no input, no output", "");
        for (proven_size_t n = 1; n <= 6; ++n) {
            PROVEN_TEST_ASSERT(proven_hex_encoded_size(n) == n * 2, "hex: two characters per byte", "");
        }
        PROVEN_TEST_ASSERT(proven_hex_encoded_size(M / 2) == (M / 2) * 2,
            "hex: the largest representable input still answers exactly", "M/2 bytes encode to M-1 characters, which fits.");
        PROVEN_TEST_ASSERT(proven_hex_encoded_size(M / 2 + 1) == M,
            "hex: one byte more cannot be represented and says so",
            "This used to answer 0 - and 0 passes every capacity check there is.");
        PROVEN_TEST_ASSERT(proven_hex_encoded_size(M) == M, "hex: and so does the largest size_t", "");

        PROVEN_TEST_ASSERT(proven_base64_encoded_size(0) == 0, "base64: no input, no output", "");
        PROVEN_TEST_ASSERT(proven_base64_encoded_size(1) == 4 && proven_base64_encoded_size(3) == 4 &&
                           proven_base64_encoded_size(4) == 8 && proven_base64_encoded_size(6) == 8,
            "base64: four characters per three-byte group, padded", "");
        PROVEN_TEST_ASSERT(proven_base64_encoded_size(M) == M,
            "base64: the largest size_t cannot be represented and says so",
            "The old form computed (n + 2) / 3, and n + 2 wrapped to 1 - so the answer was 0.");
        PROVEN_TEST_ASSERT(proven_base64_encoded_size(M - 1) == M && proven_base64_encoded_size(M - 2) == M,
            "base64: and neither can the two below it", "");

        /* The decoded bound is a different shape: its largest value fits, so it needs no
         * sentinel - only arithmetic that does not wrap on the way there. */
        PROVEN_TEST_ASSERT(proven_base64_decoded_size(0) == 0, "base64 decode bound: nothing decodes to nothing", "");
        PROVEN_TEST_ASSERT(proven_base64_decoded_size(4) == 3 && proven_base64_decoded_size(5) == 6 &&
                           proven_base64_decoded_size(8) == 6,
            "base64 decode bound: three bytes per four characters, rounded up for an unpadded tail", "");
        PROVEN_TEST_ASSERT(proven_base64_decoded_size(M) == (M / 4) * 3 + 3,
            "base64 decode bound: the largest size_t is answered exactly, not wrapped",
            "The old form rounded n up to a multiple of four first, and n + 3 wrapped.");
        PROVEN_TEST_ASSERT(proven_hex_decoded_size(M) == M / 2, "hex decode bound: n / 2 cannot overflow", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("an encoder refuses an impossible size before it touches memory (RFC-0006 H-001)",
        "Fixing the helpers is not enough: the encoders computed their own capacity, with the same wrapping arithmetic, and a wrapped `need` passes `need > out_cap` and is then written past.",
        "The view below is SYNTHETIC - a deliberately impossible size over a small real buffer. It is an early-validation probe and nothing else: the check is precisely that the call returns before reading or writing a byte, which is why it is safe to run under a sanitizer. It is not a claim that a process can allocate an object this large.");
    // ---------------------------------------------------------------
    {
        const proven_size_t M = PROVEN_SIZE_MAX;
        proven_byte_t small_in[8] = {0};
        proven_byte_t small_out[8];
        proven_size_t written = 12345;

        /* An input whose hex output cannot be represented. */
        proven_mem_view_t impossible_hex = { small_in, M / 2 + 1 };
        memset(small_out, 0xAB, sizeof small_out);
        PROVEN_TEST_ASSERT(proven_hex_encode(impossible_hex, small_out, M, &written) == PROVEN_ERR_OVERFLOW,
            "hex: an unrepresentable output size is PROVEN_ERR_OVERFLOW",
            "Even with the largest capacity a caller could name, the size itself does not exist. That is a different answer from OUT_OF_BOUNDS, which means the size exists and the buffer is smaller.");
        PROVEN_TEST_ASSERT(written == 0, "hex: nothing is reported as written on refusal", "");
        PROVEN_TEST_ASSERT(small_out[0] == 0xAB && small_out[7] == 0xAB,
            "hex: the output buffer is untouched on refusal", "A refusal that writes is not a refusal.");

        proven_mem_view_t impossible_b64 = { small_in, M };
        memset(small_out, 0xAB, sizeof small_out);
        written = 12345;
        PROVEN_TEST_ASSERT(proven_base64_encode(impossible_b64, small_out, M, &written) == PROVEN_ERR_OVERFLOW,
            "base64: an unrepresentable output size is PROVEN_ERR_OVERFLOW", "");
        PROVEN_TEST_ASSERT(written == 0 && small_out[0] == 0xAB,
            "base64: nothing written, nothing reported", "");
        written = 12345;
        PROVEN_TEST_ASSERT(proven_base64url_encode(impossible_b64, small_out, M, &written) == PROVEN_ERR_OVERFLOW,
            "base64url: the unpadded form refuses the same way", "Both forms share one implementation; both must refuse.");
        PROVEN_TEST_ASSERT(written == 0, "base64url: nothing reported as written", "");

        /* A size that IS representable but does not fit stays OUT_OF_BOUNDS - the two
         * refusals must not collapse into one. */
        written = 12345;
        PROVEN_TEST_ASSERT(proven_hex_encode(vv("abcd"), small_out, 4, &written) == PROVEN_ERR_OUT_OF_BOUNDS,
            "a representable size that does not fit is still OUT_OF_BOUNDS", "");
        PROVEN_TEST_ASSERT(written == 0, "and reports nothing written", "");

        /* Exact capacity still succeeds: the new arithmetic must not be off by one. */
        proven_byte_t exact[8];
        written = 0;
        PROVEN_TEST_ASSERT(proven_is_ok(proven_hex_encode(vv("abcd"), exact, 8, &written)) && written == 8,
            "an output buffer of exactly the right size is accepted", "");
        written = 0;
        PROVEN_TEST_ASSERT(proven_is_ok(proven_base64_encode(vv("abcde"), exact, 8, &written)) && written == 8,
            "and so is an exactly-sized Base64 output", "");
    }

    PROVEN_TEST_PASS("hex and Base64 encode to the standard, decode as the inverse, and refuse what they cannot honestly represent.");
    return 0;
}
