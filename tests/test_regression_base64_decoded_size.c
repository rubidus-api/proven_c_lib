#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * Two defects the standing audit found in the new encode module, both about the decode side.
 *
 *   1. proven_base64_decoded_size under-reported for UNPADDED input. The library's own
 *      proven_base64url_encode emits unpadded Base64, and proven_base64_decode accepts it - but
 *      the size function returned (n/4)*3, which floors away the 1 or 2 bytes an unpadded tail of
 *      length n%4 == 2 or 3 actually carries. A caller who sized their buffer by the documented
 *      function got PROVEN_ERR_OUT_OF_BOUNDS decoding valid base64url: the library could not
 *      round-trip its own URL-safe output through its own sizing. The unit test missed it by
 *      using one big buffer for everything - exactly the blind spot an over-sized buffer hides.
 *
 *   2. The decoders lacked the {out == NULL, out_cap > 0} guard the encoders have. That shape -
 *      a NULL output pointer with a claimed capacity - reached the write loop and stored through
 *      NULL (SEGV). The sibling encode functions return INVALID_ARG for exactly this; the
 *      decoders now do too.
 */

int main(void) {
    PROVEN_TEST_SUITE("Base64 decode: the sizing round-trips its own output, and a NULL out is refused",
        "proven_base64_decoded_size must be an upper bound for UNPADDED input too, so a caller can decode the library's own base64url output; and the decoders must refuse a NULL output buffer rather than store through it.",
        "Inspect proven_base64_decoded_size and the argument guards of proven_base64_decode / proven_hex_decode in src/proven/encode.c.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("decoded_size is an upper bound for unpadded lengths",
        "n%4 == 2 carries 1 byte and n%4 == 3 carries 2 bytes; the floor formula dropped them.",
        "");
    // ---------------------------------------------------------------
    {
        /* The concrete cases the audit named. */
        PROVEN_TEST_ASSERT(proven_base64_decoded_size(2) >= 1,
            "2 unpadded chars decode to 1 byte; the size must be at least 1",
            "It used to return 0, so a caller's correctly-sized buffer was zero bytes.");
        PROVEN_TEST_ASSERT(proven_base64_decoded_size(3) >= 2,
            "3 unpadded chars decode to 2 bytes", "");
        PROVEN_TEST_ASSERT(proven_base64_decoded_size(6) >= 4,
            "6 unpadded chars decode to 4 bytes", "");

        /* And it must remain an upper bound for the padded (multiple-of-4) lengths it always got right. */
        PROVEN_TEST_ASSERT(proven_base64_decoded_size(4) >= 3 && proven_base64_decoded_size(8) >= 6,
            "the padded lengths stay correct upper bounds", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the library can decode its own base64url output using its own sizing",
        "This is the round-trip that failed: encode URL-safe (unpadded), size the buffer with the documented function, decode.",
        "");
    // ---------------------------------------------------------------
    {
        for (proven_size_t len = 1; len <= 32; ++len) {
            proven_byte_t raw[32];
            for (proven_size_t i = 0; i < len; ++i) raw[i] = (proven_byte_t)(i * 7 + 1);

            proven_byte_t enc[64];
            proven_size_t en = 0;
            PROVEN_TEST_ASSERT(proven_is_ok(proven_base64url_encode(
                (proven_mem_view_t){ raw, len }, enc, sizeof enc, &en)), "encode url-safe", "");

            /* Size the output buffer EXACTLY as the header tells the caller to. */
            proven_size_t cap = proven_base64_decoded_size(en);
            proven_byte_t dec[64];
            PROVEN_TEST_ASSERT(cap <= sizeof dec, "sanity: fits our scratch", "");

            proven_size_t dn = 0;
            proven_err_t e = proven_base64_decode((proven_mem_view_t){ enc, en }, dec, cap, &dn);
            PROVEN_TEST_ASSERT(proven_is_ok(e),
                "decoding the library's own base64url into a decoded_size() buffer must succeed",
                "OUT_OF_BOUNDS here means the documented sizing is smaller than the real output.");
            PROVEN_TEST_ASSERT(dn == len && memcmp(dec, raw, len) == 0,
                "and it must round-trip to the original bytes", "");
        }
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a NULL output buffer with a claimed capacity is refused, not stored through",
        "The encoders guard this shape and return INVALID_ARG; the decoders used to reach the write loop and SEGV.",
        "");
    // ---------------------------------------------------------------
    {
        proven_size_t w = 0;
        PROVEN_TEST_ASSERT(proven_base64_decode(proven_mem_view_from_u8(PROVEN_LIT("QQ")), NULL, 8, &w) == PROVEN_ERR_INVALID_ARG,
            "base64_decode with a NULL out and nonzero cap is INVALID_ARG", "");
        PROVEN_TEST_ASSERT(proven_hex_decode(proven_mem_view_from_u8(PROVEN_LIT("6161")), NULL, 8, &w) == PROVEN_ERR_INVALID_ARG,
            "hex_decode with a NULL out and nonzero cap is INVALID_ARG", "");

        /* Symmetry with the encoders, which already did this. */
        PROVEN_TEST_ASSERT(proven_base64_encode(proven_mem_view_from_u8(PROVEN_LIT("x")), NULL, 8, &w) == PROVEN_ERR_INVALID_ARG,
            "the encoder guards the same shape (control)", "");
    }

    PROVEN_TEST_PASS("base64url round-trips through its own sizing, and a NULL output is refused on both sides.");
    return 0;
}
