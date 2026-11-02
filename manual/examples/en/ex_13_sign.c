#include "example.h"
#include <string.h>

/*
 * Signing with a private key, and the pieces a signed file format is built from: a key made from
 * its scalar and read back from DER, a signature over a digest and its verification, DER written
 * and read, and a key file's encryption opened with PBKDF2 and AES-CBC.
 */

static const proven_byte_t OID_EC[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01 };
static const proven_byte_t OID_P256[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07 };

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    // ---- a key, and a signature ------------------------------------------------------
    /* RFC 6979's example key. A real one comes from a file (proven_pkey_parse) or a generator. */
    static const proven_byte_t scalar[32] = {
        0xc9, 0xaf, 0xa9, 0xd8, 0x45, 0xba, 0x75, 0x16, 0x6b, 0x5c, 0x21, 0x57, 0x67, 0xb1, 0xd6, 0x93,
        0x4e, 0x50, 0xc3, 0xdb, 0x36, 0xe8, 0x9b, 0x12, 0x7b, 0x8a, 0x62, 0x2b, 0x12, 0x0f, 0x67, 0x21,
    };
    proven_pkey_t *key = NULL;
    EXAMPLE_REQUIRE(proven_pkey_from_ec(heap, PROVEN_PKEY_P256, (proven_mem_view_t){ scalar, 32 }, &key) == PROVEN_OK, "a P-256 key");
    EXAMPLE_REQUIRE(proven_pkey_kind(key) == PROVEN_PKEY_P256 && proven_pkey_bits(key) == 256, "of 256 bits");

    /* What is signed is a digest: the format says which hash. */
    proven_byte_t digest[32];
    proven_sha256((proven_mem_view_t){ (const proven_byte_t *)"sample", 6 }, digest);
    proven_byte_t sig[PROVEN_SIGN_MAX_SIZE];
    proven_size_t sig_len = 0;
    /* ECDSA's nonce comes from the key and the digest (RFC 6979): no generator, and the same
     * bytes every time - which a reproducible build of a signed file needs. */
    EXAMPLE_REQUIRE(proven_pkey_sign(key, PROVEN_SIGN_ECDSA, PROVEN_HMAC_SHA256, (proven_mem_view_t){ digest, 32 },
                                     (proven_rng_t){ 0 }, (proven_mem_mut_t){ sig, sizeof sig }, &sig_len) == PROVEN_OK, "signed, as DER");

    /* Anyone with the public half can check it: the point, or the SubjectPublicKeyInfo a
     * certificate carries (proven_cert_t's public_key_info). */
    proven_mem_view_t point;
    EXAMPLE_REQUIRE(proven_pkey_ec_point(key, &point) == PROVEN_OK && point.size == 65, "the public point");
    EXAMPLE_REQUIRE(proven_verify_ecdsa(PROVEN_PKEY_P256, point, PROVEN_SIGN_ECDSA, (proven_mem_view_t){ digest, 32 },
                                        (proven_mem_view_t){ sig, sig_len }) == PROVEN_OK, "verifies under the point");
    proven_byte_t spki[600];
    proven_size_t spki_len = 0;
    EXAMPLE_REQUIRE(proven_pkey_spki(key, (proven_mem_mut_t){ spki, sizeof spki }, &spki_len) == PROVEN_OK, "the SubjectPublicKeyInfo");
    EXAMPLE_REQUIRE(proven_verify_spki((proven_mem_view_t){ spki, spki_len }, PROVEN_SIGN_ECDSA, PROVEN_HMAC_SHA256,
                                       (proven_mem_view_t){ digest, 32 }, (proven_mem_view_t){ sig, sig_len }) == PROVEN_OK, "verifies under it too");
    digest[0] ^= 1;
    EXAMPLE_REQUIRE(proven_verify_spki((proven_mem_view_t){ spki, spki_len }, PROVEN_SIGN_ECDSA, PROVEN_HMAC_SHA256,
                                       (proven_mem_view_t){ digest, 32 }, (proven_mem_view_t){ sig, sig_len }) == PROVEN_ERR_UNTRUSTED,
                    "and a different digest does not");
    proven_rsa_components_t rsa;
    EXAMPLE_REQUIRE(proven_pkey_rsa_components(key, &rsa) == PROVEN_ERR_INVALID_STATE, "an EC key has no RSA parts");

    // ---- DER, written ---------------------------------------------------------------
    /* The key as an ECPrivateKey (RFC 5915): SEQUENCE { 1, OCTET STRING scalar, [0] curve, [1] point }.
     * Lengths are written when each element closes. */
    proven_mem_view_t secret;
    EXAMPLE_REQUIRE(proven_pkey_ec_scalar(key, &secret) == PROVEN_OK, "its scalar");
    proven_byte_t der[256];
    proven_der_writer_t w;
    proven_der_writer_init(&w, (proven_mem_mut_t){ der, sizeof der });
    proven_der_begin(&w, PROVEN_DER_SEQUENCE);
    proven_der_put_u64(&w, 1);
    proven_der_put(&w, PROVEN_DER_OCTET_STRING, secret);
    proven_der_begin(&w, PROVEN_DER_CONTEXT(0));
    proven_der_put(&w, PROVEN_DER_OID, (proven_mem_view_t){ OID_P256, sizeof OID_P256 });
    proven_der_end(&w);
    proven_der_begin(&w, PROVEN_DER_CONTEXT(1));
    proven_der_put_bits(&w, point);
    proven_der_end(&w);
    proven_der_end(&w);
    proven_size_t der_len = 0;
    EXAMPLE_REQUIRE(proven_der_writer_finish(&w, &der_len) == PROVEN_OK, "written");

    /* proven_pkey_parse takes it as DER, or as PEM text ("EC PRIVATE KEY", "PRIVATE KEY",
     * "RSA PRIVATE KEY"). An RSA key needs a generator, for its blinding; an EC key does not. */
    proven_pkey_t *again = NULL;
    EXAMPLE_REQUIRE(proven_pkey_parse(heap, (proven_mem_view_t){ der, der_len }, (proven_rng_t){ 0 }, &again) == PROVEN_OK, "read back");
    proven_mem_view_t point2;
    EXAMPLE_REQUIRE(proven_pkey_ec_point(again, &point2) == PROVEN_OK && memcmp(point2.ptr, point.ptr, 65) == 0, "the same key");
    proven_pkey_destroy(again);
    proven_mem_wipe((proven_mem_mut_t){ der, sizeof der });     /* it held the secret */

    /* A SET OF closed with proven_der_end_set_of is put in DER order, as CMS requires of the
     * attributes it signs. INTEGER 2 is written before INTEGER 1 here and comes out after it. */
    proven_byte_t set[32];
    proven_der_writer_init(&w, (proven_mem_mut_t){ set, sizeof set });
    proven_der_begin(&w, PROVEN_DER_SET);
    proven_der_put_uint(&w, (proven_mem_view_t){ (const proven_byte_t *)"\x02", 1 });
    proven_der_put_raw(&w, (proven_mem_view_t){ (const proven_byte_t *)"\x02\x01\x01", 3 });
    proven_der_end_set_of(&w);
    proven_size_t set_len = 0;
    EXAMPLE_REQUIRE(proven_der_writer_finish(&w, &set_len) == PROVEN_OK && set_len == 8 && set[4] == 1 && set[7] == 2, "in order");

    // ---- DER, read ------------------------------------------------------------------
    /* SubjectPublicKeyInfo ::= SEQUENCE { SEQUENCE { OID, OID }, BIT STRING }. Each read takes an
     * element off the front of a view; the content is a view into the input, never a copy. */
    proven_mem_view_t in = { spki, spki_len }, seq, alg, oid, bits;
    proven_u8 unused = 0;
    EXAMPLE_REQUIRE(proven_der_expect(&in, PROVEN_DER_SEQUENCE, &seq) == PROVEN_OK && in.size == 0, "the outer SEQUENCE, and nothing after it");
    EXAMPLE_REQUIRE(proven_der_expect(&seq, PROVEN_DER_SEQUENCE, &alg) == PROVEN_OK, "the algorithm");
    EXAMPLE_REQUIRE(proven_der_expect(&alg, PROVEN_DER_OID, &oid) == PROVEN_OK &&
                    proven_der_oid_is(oid, (proven_mem_view_t){ OID_EC, sizeof OID_EC }), "is id-ecPublicKey");
    EXAMPLE_REQUIRE(proven_der_read_bits(&seq, &bits, &unused) == PROVEN_OK && unused == 0 && bits.size == 65, "the point, as bits");
    /* The signature is SEQUENCE { INTEGER r, INTEGER s }: magnitudes without a sign byte. */
    proven_mem_view_t sv = { sig, sig_len }, rs, r, s;
    EXAMPLE_REQUIRE(proven_der_expect(&sv, PROVEN_DER_SEQUENCE, &rs) == PROVEN_OK && proven_der_read_uint(&rs, &r) == PROVEN_OK &&
                    proven_der_read_uint(&rs, &s) == PROVEN_OK && r.size <= 32 && s.size <= 32, "r and s");
    proven_der_item_t item;
    proven_mem_view_t version = { (const proven_byte_t *)"\x02\x01\x01", 3 };
    proven_u64 v = 0;
    EXAMPLE_REQUIRE(proven_der_read(&version, &item) == PROVEN_OK && item.tag == PROVEN_DER_INTEGER, "any element, by its tag");
    version = item.whole;
    EXAMPLE_REQUIRE(proven_der_read_u64(&version, &v) == PROVEN_OK && v == 1, "a small INTEGER");

    // ---- an encrypted key file's pieces ---------------------------------------------
    /* PBES2 (PKCS #8, PKCS #12): a key from the password with PBKDF2, then AES-CBC and PKCS #7
     * padding. Here one is made, then opened. The salt, count and IV are what the file says. */
    proven_byte_t aes_key[32];
    EXAMPLE_REQUIRE(proven_pbkdf2(PROVEN_HMAC_SHA256, (proven_mem_view_t){ (const proven_byte_t *)"correct horse", 13 },
                                  (proven_mem_view_t){ (const proven_byte_t *)"8 bytes!", 8 }, 2048,
                                  (proven_mem_mut_t){ aes_key, 32 }) == PROVEN_OK, "the key from the password");
    proven_byte_t box[16] = "fourteen bytes";
    memset(box + 14, 2, 2);                    /* 2 bytes of padding, each 2: one whole block */
    proven_byte_t iv[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }, iv2[16];
    memcpy(iv2, iv, 16);
    EXAMPLE_REQUIRE(proven_aes_cbc_encrypt((proven_mem_view_t){ aes_key, 32 }, iv, (proven_mem_mut_t){ box, 16 }) == PROVEN_OK, "encrypted");
    EXAMPLE_REQUIRE(proven_aes_cbc_decrypt((proven_mem_view_t){ aes_key, 32 }, iv2, (proven_mem_mut_t){ box, 16 }) == PROVEN_OK, "decrypted");
    proven_size_t plain_len = 0;
    EXAMPLE_REQUIRE(proven_pkcs7_unpad((proven_mem_view_t){ box, 16 }, 16, &plain_len) == PROVEN_OK && plain_len == 14 &&
                    memcmp(box, "fourteen bytes", 14) == 0, "and unpadded");
    proven_mem_wipe((proven_mem_mut_t){ aes_key, 32 });

    proven_pkey_destroy(key);
    return EXAMPLE_OK();
}
