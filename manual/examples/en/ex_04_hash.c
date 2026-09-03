#include "example.h"

/*
 * Hashing, by use case. The module gives you exactly one function per job, so the only
 * decision is which job you have - and getting THAT wrong is the whole danger.
 */

int main(void) {
    proven_mem_view_t data = proven_mem_view_from_u8(PROVEN_LIT("the quick brown fox"));

    /* Job 1: hash a key into your own table, trusted input. Fast, non-cryptographic. */
    proven_u64 table_hash = proven_hash_bytes(data);
    EXAMPLE_REQUIRE(table_hash != 0, "FNV-1a produces a spread-out 64-bit value");

    /* Job 2: hash a key from UNTRUSTED input. Same purpose, but an attacker who picks the
     * input still cannot make everything collide, because they do not have the key. Pick
     * the key once at startup from real randomness; a fixed key defeats the point. */
    proven_byte_t key[16] = { 0 };   /* in real code: fill from a random source, once */
    proven_u64 safe_hash = proven_hash_keyed(data, key);
    EXAMPLE_REQUIRE(safe_hash != table_hash, "a keyed hash is a different function");

    /* Job 3: did these bytes get corrupted? A checksum, not a hash. Interoperates with
     * gzip/zlib/PNG, which all carry this exact CRC-32. */
    proven_u32 checksum = proven_crc32(data);
    /* The canonical CRC-32 sanity value, so you can see it is the real one: */
    EXAMPLE_REQUIRE(proven_crc32(proven_mem_view_from_u8(PROVEN_LIT("123456789"))) == 0xcbf43926u,
                    "CRC-32 of \"123456789\" is the shared check value");
    (void)checksum;

    /* Job 4: fingerprint content - dedup, content-addressing, "are these the same file",
     * answered safely even against someone trying to forge a match. This is the one you
     * reach for when the answer must not be foolable. */
    proven_byte_t digest[PROVEN_SHA256_SIZE];
    proven_sha256(data, digest);

    char hex[65];
    proven_sha256_to_hex(digest, hex);
    /* The same spelling sha256sum and git print, so it interoperates: */
    EXAMPLE_REQUIRE(hex[64] == '\0' && proven_cstr_len(hex) == 64,
                    "a SHA-256 fingerprint is 64 lowercase hex characters");

    /* SHA-256 streams, for content you cannot hold in memory at once - the digest depends
     * only on the bytes, never on how they were chunked. */
    proven_sha256_t ctx;
    proven_sha256_init(&ctx);
    proven_sha256_update(&ctx, proven_mem_view_from_u8(PROVEN_LIT("the quick ")));
    proven_sha256_update(&ctx, proven_mem_view_from_u8(PROVEN_LIT("brown fox")));
    proven_byte_t streamed[PROVEN_SHA256_SIZE];
    proven_sha256_final(&ctx, streamed);

    bool same = true;
    for (proven_size_t i = 0; i < PROVEN_SHA256_SIZE; ++i) {
        if (streamed[i] != digest[i]) same = false;
    }
    EXAMPLE_REQUIRE(same, "two updates of the halves equal one hash of the whole");

    return EXAMPLE_OK();
}
