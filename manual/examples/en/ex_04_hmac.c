#include "example.h"
#include <string.h>

/*
 * A digest says what the bytes are. A MAC says who vouched for them, and a KDF turns one
 * secret into the several keys a protocol needs. Plus the two calls that go with any secret:
 * a comparison that does not leak, and a clear that is not optimised away.
 */

static bool hex_is(const proven_byte_t *got, proven_size_t n, const char *want) {
    char text[129];
    proven_size_t len = 0;
    return proven_hex_encode((proven_mem_view_t){ got, n }, (proven_byte_t *)text, sizeof text, &len) == PROVEN_OK &&
           len == strlen(want) && memcmp(text, want, len) == 0;
}

int main(void) {
    // ---- SHA-512 and SHA-384 -------------------------------------------------
    /* The same shape as SHA-256: one-shot, or init / update / final for what does not fit in
     * memory. SHA-384 is SHA-512 with other starting values, cut to 48 bytes. */
    proven_byte_t d512[PROVEN_SHA512_SIZE], d384[PROVEN_SHA384_SIZE], again[PROVEN_SHA512_SIZE];
    proven_sha512(proven_mem_view_from_u8(PROVEN_LIT("abc")), d512);
    EXAMPLE_REQUIRE(hex_is(d512, 8, "ddaf35a193617aba"), "SHA-512 of abc begins as FIPS 180-4 prints it");
    proven_sha384(proven_mem_view_from_u8(PROVEN_LIT("abc")), d384);
    EXAMPLE_REQUIRE(hex_is(d384, 8, "cb00753f45a35e8b"), "and SHA-384");

    proven_sha512_t running;
    proven_sha512_init(&running);
    proven_sha512_update(&running, proven_mem_view_from_u8(PROVEN_LIT("a")));
    proven_sha512_update(&running, proven_mem_view_from_u8(PROVEN_LIT("bc")));
    proven_sha512_final(&running, again);
    EXAMPLE_REQUIRE(memcmp(d512, again, sizeof d512) == 0, "in pieces, the same digest");

    proven_sha384_t running384;
    proven_sha384_init(&running384);
    proven_sha384_update(&running384, proven_mem_view_from_u8(PROVEN_LIT("abc")));
    proven_sha384_final(&running384, again);
    EXAMPLE_REQUIRE(memcmp(d384, again, sizeof d384) == 0, "and for SHA-384");

    // ---- HMAC: a message somebody with the key vouched for --------------------
    /* A server signs a token and later checks that what comes back is what it issued. */
    proven_byte_t key[32];
    EXAMPLE_REQUIRE(proven_random_bytes(key, sizeof key), "a key from the system: 32 bytes for a 32-byte MAC");
    proven_mem_view_t token = proven_mem_view_from_u8(PROVEN_LIT("user=ada;expires=1767225600"));
    proven_byte_t mac[PROVEN_HMAC_MAX_SIZE];
    EXAMPLE_REQUIRE(proven_hmac(PROVEN_HMAC_SHA256, (proven_mem_view_t){ key, sizeof key }, token, mac) == PROVEN_OK, "the token's MAC");
    EXAMPLE_REQUIRE(proven_hmac_size(PROVEN_HMAC_SHA256) == 32, "32 bytes of it; the buffer is sized for the largest");

    /* Checking: compute it again and compare WITHOUT leaking where the two differ. memcmp
     * stops at the first wrong byte, and the time it took tells a forger how far they got. */
    proven_byte_t check[PROVEN_HMAC_MAX_SIZE];
    proven_hmac_t h;
    EXAMPLE_REQUIRE(proven_hmac_init(&h, PROVEN_HMAC_SHA256, (proven_mem_view_t){ key, sizeof key }) == PROVEN_OK, "the streaming form, for a message in pieces");
    proven_hmac_update(&h, proven_mem_view_from_u8(PROVEN_LIT("user=ada;")));
    proven_hmac_update(&h, proven_mem_view_from_u8(PROVEN_LIT("expires=1767225600")));
    proven_hmac_final(&h, check);                   /* also wipes the key out of `h` */
    EXAMPLE_REQUIRE(proven_mem_equal_ct((proven_mem_view_t){ mac, 32 }, (proven_mem_view_t){ check, 32 }), "the token is genuine");

    /* Someone changes the token. They cannot make the MAC that goes with it. */
    proven_mem_view_t forged = proven_mem_view_from_u8(PROVEN_LIT("user=eve;expires=1767225600"));
    EXAMPLE_REQUIRE(proven_hmac(PROVEN_HMAC_SHA256, (proven_mem_view_t){ key, sizeof key }, forged, check) == PROVEN_OK &&
                    !proven_mem_equal_ct((proven_mem_view_t){ mac, 32 }, (proven_mem_view_t){ check, 32 }), "a changed token does not carry the old MAC");

    /* A published vector, so that this is the HMAC everyone else computes: RFC 4231, case 2. */
    EXAMPLE_REQUIRE(proven_hmac(PROVEN_HMAC_SHA256, proven_mem_view_from_u8(PROVEN_LIT("Jefe")),
                                proven_mem_view_from_u8(PROVEN_LIT("what do ya want for nothing?")), check) == PROVEN_OK &&
                    hex_is(check, 32, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"), "RFC 4231, test case 2");

    // ---- HKDF: one secret, several keys --------------------------------------
    /* Two sides have agreed on a shared secret. It is not yet a key: derive one for each
     * purpose, each bound to a label, so that knowing one says nothing about another. */
    proven_byte_t shared[32];
    EXAMPLE_REQUIRE(proven_random_bytes(shared, sizeof shared), "stands for the output of a key exchange");
    proven_mem_view_t secret = { shared, sizeof shared };
    proven_mem_view_t salt = proven_mem_view_from_u8(PROVEN_LIT("example-protocol v1"));       /* not secret; may be empty */

    proven_byte_t enc_key[32], mac_key[32];
    EXAMPLE_REQUIRE(proven_hkdf(PROVEN_HMAC_SHA256, salt, secret, proven_mem_view_from_u8(PROVEN_LIT("encryption")), (proven_mem_mut_t){ enc_key, sizeof enc_key }) == PROVEN_OK &&
                    proven_hkdf(PROVEN_HMAC_SHA256, salt, secret, proven_mem_view_from_u8(PROVEN_LIT("authentication")), (proven_mem_mut_t){ mac_key, sizeof mac_key }) == PROVEN_OK,
                    "two keys from one secret");
    EXAMPLE_REQUIRE(!proven_mem_equal_ct((proven_mem_view_t){ enc_key, 32 }, (proven_mem_view_t){ mac_key, 32 }), "different labels, unrelated keys");

    /* The two steps apart: extract once, expand as often as needed. */
    proven_byte_t prk[PROVEN_HMAC_MAX_SIZE], enc_again[32];
    EXAMPLE_REQUIRE(proven_hkdf_extract(PROVEN_HMAC_SHA256, salt, secret, prk) == PROVEN_OK &&
                    proven_hkdf_expand(PROVEN_HMAC_SHA256, (proven_mem_view_t){ prk, 32 }, proven_mem_view_from_u8(PROVEN_LIT("encryption")),
                                       (proven_mem_mut_t){ enc_again, sizeof enc_again }) == PROVEN_OK &&
                    proven_mem_equal_ct((proven_mem_view_t){ enc_key, 32 }, (proven_mem_view_t){ enc_again, 32 }), "extract then expand is the same derivation");

    /* HKDF gives at most 255 blocks of the hash; asking for more is refused, not truncated. */
    static proven_byte_t too_much[255 * 32 + 1];
    EXAMPLE_REQUIRE(proven_hkdf_expand(PROVEN_HMAC_SHA256, (proven_mem_view_t){ prk, 32 }, proven_mem_view_from_u8(PROVEN_LIT("")),
                                       (proven_mem_mut_t){ too_much, sizeof too_much }) == PROVEN_ERR_OUT_OF_BOUNDS, "more than 8160 bytes from SHA-256: refused");

    // ---- When a secret is done with -------------------------------------------
    /* Clear it. An ordinary loop here would be deleted by the optimiser - nothing reads the
     * buffer afterwards - and the key would stay in memory. This write is not removed. */
    proven_mem_wipe((proven_mem_mut_t){ key, sizeof key });
    proven_mem_wipe((proven_mem_mut_t){ shared, sizeof shared });
    proven_mem_wipe((proven_mem_mut_t){ prk, sizeof prk });
    proven_mem_wipe((proven_mem_mut_t){ enc_key, sizeof enc_key });
    proven_mem_wipe((proven_mem_mut_t){ mac_key, sizeof mac_key });
    proven_byte_t zeros[32] = {0};
    EXAMPLE_REQUIRE(proven_mem_equal_ct((proven_mem_view_t){ key, 32 }, (proven_mem_view_t){ zeros, 32 }), "the key is gone from its buffer");

    return EXAMPLE_OK();
}
