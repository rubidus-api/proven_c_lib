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

    /* --- sizing the destination buffer, instead of guessing ---------------- */

    /* Every encode and decode above wrote into a fixed array that was obviously
     * big enough. Real code allocates the destination, and then the size has to
     * be computed rather than remembered. That is what the four size functions
     * are for - one per direction, per encoding. */
    proven_allocator_t alloc = proven_heap_allocator();

    proven_size_t need = proven_base64_encoded_size(data.size);
    EXAMPLE_REQUIRE(need > 0, "an encoded size must be computed, not guessed");

    proven_result_mem_mut_t out = alloc.alloc_fn(alloc.ctx, need, 1);
    EXAMPLE_REQUIRE(proven_is_ok(out.err), "allocating exactly the encoded size must succeed");

    proven_size_t wrote = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_base64_encode(data, out.value.ptr, out.value.size, &wrote)),
                    "encoding into a buffer sized by the size function must fit exactly");
    EXAMPLE_REQUIRE(wrote == need, "and use all of it: this size is exact for standard base64");

    /* The decode direction is an UPPER BOUND, not an exact count: base64 text
     * may carry padding, so the decoder reports how many bytes it really wrote.
     * Size the buffer with the bound; use the reported count afterwards. */
    proven_size_t bound = proven_base64_decoded_size(wrote);
    EXAMPLE_REQUIRE(bound >= data.size, "the decoded bound must cover the real output");

    proven_result_mem_mut_t plain = alloc.alloc_fn(alloc.ctx, bound, 1);
    EXAMPLE_REQUIRE(proven_is_ok(plain.err), "allocating the decode bound must succeed");

    proven_size_t got = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_base64_decode(
                        (proven_mem_view_t){ out.value.ptr, wrote }, plain.value.ptr, plain.value.size, &got)),
                    "decoding back must succeed");
    EXAMPLE_REQUIRE(got == data.size && proven_memcmp(plain.value.ptr, data.ptr, got) == 0,
                    "and reproduce the original bytes exactly");

    /* Hex has the same pair. Its decoded bound is exact when the input length is
     * even, which valid hex always is - an odd length is malformed input, and
     * the decoder says so rather than dropping the last digit. */
    proven_size_t hex_len = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_hex_encode(data, hex, sizeof hex, &hex_len)),
                    "re-encode to hex, since the refused call above left its count unusable");

    proven_size_t hex_bound = proven_hex_decoded_size(hex_len);
    EXAMPLE_REQUIRE(hex_bound == data.size, "two hex characters decode to one byte");

    proven_byte_t from_hex[32];
    proven_size_t hex_got = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_hex_decode(
                        (proven_mem_view_t){ hex, hex_len }, from_hex, sizeof from_hex, &hex_got)),
                    "hex decodes back to the original bytes");
    EXAMPLE_REQUIRE(hex_got == data.size && proven_memcmp(from_hex, data.ptr, hex_got) == 0,
                    "and the round trip is exact");

    /* Hex is case-insensitive on the way in, which matters because different
     * tools print different cases of the same digest. */
    EXAMPLE_REQUIRE(proven_is_ok(proven_hex_decode(
                        proven_mem_view_from_u8(PROVEN_LIT("DEADBEEF")), from_hex, sizeof from_hex, &hex_got)),
                    "uppercase hex decodes too");
    EXAMPLE_REQUIRE(hex_got == 4, "and yields one byte per two characters");

    /* An odd number of characters cannot be a whole number of bytes. */
    EXAMPLE_REQUIRE(proven_hex_decode(proven_mem_view_from_u8(PROVEN_LIT("abc")),
                                      from_hex, sizeof from_hex, &hex_got) == PROVEN_ERR_INVALID_ENCODING,
                    "an odd-length hex string is malformed, not silently truncated");

    alloc.free_fn(alloc.ctx, plain.value.ptr);
    alloc.free_fn(alloc.ctx, out.value.ptr);

    (void)url; (void)un;
    return EXAMPLE_OK();
}
