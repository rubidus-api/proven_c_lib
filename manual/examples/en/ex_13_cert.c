#include "example.h"
#include <string.h>

/*
 * A certificate is a signed statement that a key belongs to a name. This reads one, asks which
 * names it is for, and then decides whether to believe it: a chain to a trust anchor, the dates,
 * and the name - with the error that says which of those failed.
 */

/* A root and a certificate it issued for example.test, made for this example. Ed25519, so they
 * are short. Valid from 2026 to 2036. */
static const char ROOT_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBAjCBtaADAgECAhQxTiZoH2JgH7bPWBktoxUyd0d3ZzAFBgMrZXAwFzEVMBMG\n"
    "A1UEAwwMRXhhbXBsZSBSb290MB4XDTI2MDEwMTAwMDAwMFoXDTM2MDEwMTAwMDAw\n"
    "MFowFzEVMBMGA1UEAwwMRXhhbXBsZSBSb290MCowBQYDK2VwAyEAJcpfTb5GYzM+\n"
    "bgKssEvuWEC6urqf1Znqz9I8CY2RdLujEzARMA8GA1UdEwEB/wQFMAMBAf8wBQYD\n"
    "K2VwA0EARcXC/GhPDj4gAU3zpeSAhJ033zALpz2P91wtEqbf2R4Z7drW+RSRQGp2\n"
    "Ky/CDeEyx//WjylXEoHVNvixwKrxAw==\n"
    "-----END CERTIFICATE-----\n"
    ;

static const char LEAF_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBPTCB8KADAgECAhRBU6wAvGvpDo28++Tq/BfZ/8DkNzAFBgMrZXAwFzEVMBMG\n"
    "A1UEAwwMRXhhbXBsZSBSb290MB4XDTI2MDEwMTAwMDAwMFoXDTM2MDEwMTAwMDAw\n"
    "MFowFzEVMBMGA1UEAwwMZXhhbXBsZS50ZXN0MCowBQYDK2VwAyEA3dq7G1ndiCbb\n"
    "IAS6O7cRIlxdACAdLSsPSRROKyglnTOjTjBMMAwGA1UdEwEB/wQCMAAwJwYDVR0R\n"
    "BCAwHoIMZXhhbXBsZS50ZXN0gg4qLmV4YW1wbGUudGVzdDATBgNVHSUEDDAKBggr\n"
    "BgEFBQcDATAFBgMrZXADQQBVDpl9ZAqf4j2R3NTG8JuKfPJuh+PD99QVdpjnqPAR\n"
    "dgg3sGIps+UTADlifcSOVlqJF6T9ya5lHpO5FwIpLpMI\n"
    "-----END CERTIFICATE-----\n"
    ;

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    // ---- PEM: text around the bytes ---------------------------------------------
    /* Certificates travel as PEM: Base64 between BEGIN and END lines. proven_pem_next finds the
     * next block and decodes it; the bytes inside are DER, which is what everything else takes. */
    proven_byte_t leaf_der[1024];
    proven_size_t pos = 0, leaf_len = 0;
    proven_u8str_view_t label;
    proven_mem_view_t leaf_pem = { (const proven_byte_t *)LEAF_PEM, sizeof LEAF_PEM - 1 };
    EXAMPLE_REQUIRE(proven_pem_next(leaf_pem, &pos, &label, (proven_mem_mut_t){ leaf_der, sizeof leaf_der }, &leaf_len) == PROVEN_OK,
                    "the block decodes");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(label, PROVEN_LIT("CERTIFICATE")), "and says what it is");
    EXAMPLE_REQUIRE(proven_pem_next(leaf_pem, &pos, &label, (proven_mem_mut_t){ leaf_der, 0 }, &(proven_size_t){ 0 }) == PROVEN_ERR_NOT_FOUND,
                    "there is no second block");

    // ---- reading one certificate --------------------------------------------------
    /* Parsing copies nothing: every field of proven_cert_t is a view into leaf_der, which must
     * stay alive as long as the struct is used. */
    proven_cert_t leaf;
    EXAMPLE_REQUIRE(proven_cert_parse((proven_mem_view_t){ leaf_der, leaf_len }, &leaf) == PROVEN_OK, "it parses");
    EXAMPLE_REQUIRE(leaf.version == 3 && leaf.key_kind == PROVEN_CERT_KEY_ED25519 && leaf.key.size == 32 && !leaf.is_ca,
                    "version 3, an Ed25519 key, not a CA");
    EXAMPLE_REQUIRE(leaf.not_before < 1811808000 && leaf.not_after > 1811808000, "valid in June 2027");

    proven_size_t at = 0, names = 0;
    proven_cert_name_kind_t kind;
    proven_mem_view_t name;
    while (proven_cert_alt_name_next(&leaf, &at, &kind, &name)) {
        if (kind == PROVEN_CERT_NAME_DNS) names++;
    }
    EXAMPLE_REQUIRE(names == 2, "two DNS names in subjectAltName");

    /* Which hosts is it for? Only subjectAltName counts. A wildcard stands for exactly one label. */
    EXAMPLE_REQUIRE(proven_cert_matches_host(&leaf, PROVEN_LIT("example.test")), "the name itself");
    EXAMPLE_REQUIRE(proven_cert_matches_host(&leaf, PROVEN_LIT("www.example.test")), "one label under the wildcard");
    EXAMPLE_REQUIRE(!proven_cert_matches_host(&leaf, PROVEN_LIT("a.b.example.test")), "but not two");
    EXAMPLE_REQUIRE(!proven_cert_matches_host(&leaf, PROVEN_LIT("example.org")), "and not another name");

    // ---- trust anchors ------------------------------------------------------------
    /* A store holds the certificates you already trust. It copies what it is given, so ROOT_PEM
     * could be freed after this. The library ships no roots: you supply them. */
    proven_cert_store_t *anchors = NULL;
    proven_size_t added = 0;
    EXAMPLE_REQUIRE(proven_cert_store_create(heap, &anchors) == PROVEN_OK, "a store");
    EXAMPLE_REQUIRE(proven_cert_store_add_pem(anchors, (proven_mem_view_t){ (const proven_byte_t *)ROOT_PEM, sizeof ROOT_PEM - 1 }, &added) == PROVEN_OK &&
                    added == 1 && proven_cert_store_count(anchors) == 1, "one anchor from the bundle");

    // ---- verifying ----------------------------------------------------------------
    /* chain[0] is the peer's certificate; anything else it sent follows, in any order. The time
     * is yours to supply: a library that read the clock itself could not be tested, and could
     * not run where there is no clock. */
    proven_mem_view_t chain[1] = { { leaf_der, leaf_len } };
    proven_cert_verify_options_t options = {
        .anchors = anchors,
        .host = PROVEN_LIT("www.example.test"),
        .now = 1811808000,                       /* 2027-06-01 */
        .use = PROVEN_CERT_USE_SERVER,
    };
    proven_cert_verify_result_t result;
    EXAMPLE_REQUIRE(proven_cert_verify(chain, 1, &options, &result) == PROVEN_OK && result.depth == 2, "a path of two: the leaf and the anchor");

    /* Each way of being wrong has its own answer. The error code is the coarse one; the fault in
     * the result is for the log line. */
    options.host = PROVEN_LIT("example.org");
    EXAMPLE_REQUIRE(proven_cert_verify(chain, 1, &options, &result) == PROVEN_ERR_NAME_MISMATCH &&
                    result.fault == PROVEN_CERT_FAULT_NAME_MISMATCH, "the chain is good and the name is not");
    options.host = PROVEN_LIT("www.example.test");
    options.now = 2127427200;                    /* 2037-06-01 */
    EXAMPLE_REQUIRE(proven_cert_verify(chain, 1, &options, &result) == PROVEN_ERR_EXPIRED, "ten years on, it has expired");
    options.now = 1811808000;
    leaf_der[leaf_len - 1] ^= 1;
    EXAMPLE_REQUIRE(proven_cert_verify(chain, 1, &options, &result) == PROVEN_ERR_UNTRUSTED &&
                    result.fault == PROVEN_CERT_FAULT_BAD_SIGNATURE, "one changed bit, and the signature no longer verifies");
    leaf_der[leaf_len - 1] ^= 1;

    proven_cert_store_t *empty = NULL;
    EXAMPLE_REQUIRE(proven_cert_store_create(heap, &empty) == PROVEN_OK, "a store");
    options.anchors = empty;
    EXAMPLE_REQUIRE(proven_cert_verify(chain, 1, &options, &result) == PROVEN_ERR_UNTRUSTED &&
                    result.fault == PROVEN_CERT_FAULT_NO_ISSUER, "with no anchors, nothing is trusted");

    // ---- a pin ---------------------------------------------------------------------
    /* A pin is the SHA-256 of the certificate's public key, as SubjectPublicKeyInfo. A program
     * that knows which key it expects compares this, in addition to or instead of the chain. */
    proven_byte_t pin[32], again[32];
    EXAMPLE_REQUIRE(proven_cert_key_sha256(&leaf, pin) == PROVEN_OK, "the pin");
    proven_sha256(leaf.public_key_info, again);
    EXAMPLE_REQUIRE(proven_mem_equal_ct((proven_mem_view_t){ pin, 32 }, (proven_mem_view_t){ again, 32 }), "is the hash of the key's encoding");

    // ---- the system's roots -------------------------------------------------------
    /* For talking to the public internet, the anchors are the operating system's. Whether a
     * machine has any is the machine's business, so this example accepts both answers. */
    proven_cert_store_t *system_roots = NULL;
    EXAMPLE_REQUIRE(proven_cert_store_create(heap, &system_roots) == PROVEN_OK, "a store");
    proven_err_t sys = proven_cert_store_add_system(system_roots, &added);
    EXAMPLE_REQUIRE(sys == PROVEN_OK || sys == PROVEN_ERR_NOT_FOUND, "the system store was read, or there is none");
    EXAMPLE_REQUIRE((sys == PROVEN_OK) == (proven_cert_store_count(system_roots) > 0), "and it is not empty when it was read");

    /* A single DER certificate can be added too. */
    proven_byte_t root_der[1024];
    proven_size_t root_len = 0;
    pos = 0;
    EXAMPLE_REQUIRE(proven_pem_next((proven_mem_view_t){ (const proven_byte_t *)ROOT_PEM, sizeof ROOT_PEM - 1 }, &pos, &label,
                                    (proven_mem_mut_t){ root_der, sizeof root_der }, &root_len) == PROVEN_OK, "the block decodes");
    EXAMPLE_REQUIRE(proven_cert_store_add_der(empty, (proven_mem_view_t){ root_der, root_len }) == PROVEN_OK, "the root as DER");
    EXAMPLE_REQUIRE(proven_cert_verify(chain, 1, &options, &result) == PROVEN_OK, "and now the once-empty store trusts the leaf");

    proven_cert_store_destroy(system_roots);
    proven_cert_store_destroy(empty);
    proven_cert_store_destroy(anchors);
    return EXAMPLE_OK();
}
