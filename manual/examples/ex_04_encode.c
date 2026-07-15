#include "example.h"

/*
 * Bytes to text, by use case. The rule is the same one hashing follows: one function per job,
 * and the danger is picking the wrong job. Hex for something a human reads; Base64URL for
 * something that goes in a URL; standard Base64 for something that goes on the wire.
 */

int main(void) {
    proven_mem_view_t data = proven_mem_view_from_u8(PROVEN_LIT("the quick brown fox"));

    /* Job 1: a digest a human will read or paste - hex, the spelling sha256sum and git use. */
    proven_byte_t hex[64];   /* proven_hex_encoded_size(19) = 38 */
    proven_size_t hn = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_hex_encode(data, hex, sizeof hex, &hn)),
                    "hex encode into a buffer sized by proven_hex_encoded_size");
    EXAMPLE_REQUIRE(hn == proven_hex_encoded_size(data.size), "two hex chars per byte");

    /* Job 2: a token that goes in a URL - Base64URL, so nothing needs percent-escaping and
     * there is no '=' padding for a parser to trip over. */
    proven_byte_t token_bytes[16] = { 0 };   /* in real code: proven_random_bytes(token_bytes, 16) */
    proven_byte_t url[32];
    proven_size_t un = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_base64url_encode(
                        (proven_mem_view_t){ token_bytes, sizeof token_bytes }, url, sizeof url, &un)),
                    "base64url encode a token");
    /* No '=' in a URL-safe token. */
    bool has_pad = false;
    for (proven_size_t i = 0; i < un; ++i) if (url[i] == '=') has_pad = true;
    EXAMPLE_REQUIRE(!has_pad, "the URL form emits no padding");

    /* Job 3: bytes on the wire - standard Base64, the +/= alphabet HTTP and MIME expect. */
    proven_byte_t b64[64];
    proven_size_t bn = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_base64_encode(data, b64, sizeof b64, &bn)),
                    "standard base64 encode");

    /* And it round-trips: decode gives back exactly the bytes. A decoder that accepts both
     * alphabets and padded-or-not is deliberate - real input comes in every shape. */
    proven_byte_t back[32];
    proven_size_t dn = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_base64_decode(
                        (proven_mem_view_t){ b64, bn }, back, sizeof back, &dn)),
                    "decode the base64 back");
    EXAMPLE_REQUIRE(dn == data.size && proven_memcmp(back, data.ptr, dn) == 0,
                    "what comes back is exactly what went in");

    /* The point of a validating decoder: junk is refused, not guessed. A caller who fed this
     * to a two-line loop would read past the end or get a silently short result. */
    proven_err_t bad = proven_base64_decode(
        proven_mem_view_from_u8(PROVEN_LIT("not valid base64!!")), back, sizeof back, &dn);
    EXAMPLE_REQUIRE(bad == PROVEN_ERR_INVALID_ENCODING,
                    "a stray character is INVALID_ENCODING, with nothing committed");

    /* And a buffer one byte too small is refused, never truncated. */
    proven_byte_t tiny[4];
    EXAMPLE_REQUIRE(proven_hex_encode(data, tiny, sizeof tiny, &hn) == PROVEN_ERR_OUT_OF_BOUNDS,
                    "a too-small output buffer is OUT_OF_BOUNDS, not a truncated prefix");

    (void)hex; (void)url; (void)un;
    return EXAMPLE_OK();
}
