#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * Written from the four algorithms' OWN specifications, before proven/hash.c was
 * implemented (docs/TESTING.md §5.1). Every expected value below is an official
 * known-answer vector - the differential oracle a hash function is judged against - so
 * this test is not "does the code agree with itself" but "does the code agree with the
 * standard the rest of the world implements". That is exactly the case where writing the
 * test first is not a discipline you impose but the natural thing to do: the answers exist
 * before the code does.
 *
 *   FNV-1a 64  - the canonical FNV reference vectors.
 *   SipHash-2-4 - the reference vectors from Aumasson & Bernstein's paper (key = 00..0f,
 *                 message = 00..len-1), read as the little-endian u64 the reference emits.
 *   CRC-32/IEEE - the check value 0xCBF43926 for "123456789" that every CRC-32 shares,
 *                 plus a few more.
 *   SHA-256    - the FIPS 180-4 examples, including the million-'a' vector.
 */

static proven_mem_view_t sv(const char *s) {
    return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}

static bool hex_eq(const proven_byte_t *bytes, proven_size_t n, const char *hex) {
    if (strlen(hex) != n * 2u) return false;
    static const char *d = "0123456789abcdef";
    for (proven_size_t i = 0; i < n; ++i) {
        char hi = d[bytes[i] >> 4], lo = d[bytes[i] & 0xf];
        if (hex[i * 2] != hi || hex[i * 2 + 1] != lo) return false;
    }
    return true;
}

int main(void) {
    PROVEN_TEST_SUITE("hashing, by use case",
        "FNV-1a and SipHash for tables, CRC-32 for corruption, SHA-256 for fingerprinting - each against its own official vectors.",
        "Inspect src/proven/hash.c. A mismatch here means the implementation does not agree with the standard, not merely with itself.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("FNV-1a 64: the canonical reference vectors",
        "A fast table hash is only useful if it is the fast table hash everyone means by FNV-1a.",
        "Offset basis 0xcbf29ce484222325, prime 0x100000001b3, xor-then-multiply.");
    // ---------------------------------------------------------------
    PROVEN_TEST_ASSERT(proven_hash_bytes(sv("")) == 0xcbf29ce484222325ull,
        "FNV-1a of the empty input is the offset basis", "");
    PROVEN_TEST_ASSERT(proven_hash_bytes(sv("a")) == 0xaf63dc4c8601ec8cull,
        "FNV-1a of \"a\"", "");
    PROVEN_TEST_ASSERT(proven_hash_bytes(sv("foobar")) == 0x85944171f73967e8ull,
        "FNV-1a of \"foobar\"", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("SipHash-2-4: the reference vectors, and the property that matters",
        "The output must match the paper's vectors, and a change of key must change the output - that is what an attacker cannot get around.",
        "Key = bytes 00..0f; message for length L = bytes 00..L-1; result read as a little-endian u64.");
    // ---------------------------------------------------------------
    {
        proven_byte_t key[16];
        for (int i = 0; i < 16; ++i) key[i] = (proven_byte_t)i;
        proven_byte_t msg[16];
        for (int i = 0; i < 16; ++i) msg[i] = (proven_byte_t)i;

        struct { proven_size_t len; proven_u64 want; } v[] = {
            { 0,  0x726fdb47dd0e0e31ull },
            { 1,  0x74f839c593dc67fdull },
            { 7,  0xab0200f58b01d137ull },
            { 8,  0x93f5f5799a932462ull },
            { 15, 0xa129ca6149be45e5ull },
        };
        for (proven_size_t i = 0; i < sizeof v / sizeof v[0]; ++i) {
            proven_u64 got = proven_hash_keyed((proven_mem_view_t){ .ptr = msg, .size = v[i].len }, key);
            PROVEN_TEST_ASSERT(got == v[i].want,
                "SipHash-2-4 must match the reference vector for this length",
                "A mismatch means the round count, the finalization, or the byte order is wrong.");
        }

        /* The security property, in the small: a different key gives a different hash. */
        proven_byte_t key2[16];
        memcpy(key2, key, 16);
        key2[0] ^= 0xff;
        PROVEN_TEST_ASSERT(proven_hash_keyed(sv("collide me"), key) !=
                           proven_hash_keyed(sv("collide me"), key2),
            "the same input under two different keys must not hash the same",
            "If the key does not affect the output, it is not keyed, and an attacker does not need it.");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("CRC-32: the check value everyone shares",
        "0xCBF43926 for \"123456789\" is THE CRC-32 sanity value; if it matches, the polynomial, the reflection, and the final xor are all right.",
        "IEEE 802.3 / zlib: poly 0xEDB88320 reflected, init and xor-out 0xFFFFFFFF.");
    // ---------------------------------------------------------------
    PROVEN_TEST_ASSERT(proven_crc32(sv("")) == 0x00000000u,
        "CRC-32 of the empty input is 0", "");
    PROVEN_TEST_ASSERT(proven_crc32(sv("a")) == 0xe8b7be43u,
        "CRC-32 of \"a\"", "");
    PROVEN_TEST_ASSERT(proven_crc32(sv("abc")) == 0x352441c2u,
        "CRC-32 of \"abc\"", "");
    PROVEN_TEST_ASSERT(proven_crc32(sv("123456789")) == 0xcbf43926u,
        "CRC-32 of \"123456789\" is the standard check value 0xCBF43926",
        "Every conforming CRC-32 produces this. If it does not, the implementation is not CRC-32/IEEE.");
    PROVEN_TEST_ASSERT(proven_crc32(sv("The quick brown fox jumps over the lazy dog")) == 0x414fa339u,
        "CRC-32 of the pangram", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("CRC-32 streaming equals CRC-32 over the concatenation",
        "A checksum you can feed a stream to is only correct if splitting the stream does not change the answer.",
        "Start from 0, fold each chunk, and the result must equal the one-shot over the whole.");
    // ---------------------------------------------------------------
    {
        const char *whole = "123456789";
        proven_u32 streamed = proven_crc32_update(0, sv("123"));
        streamed = proven_crc32_update(streamed, sv("456"));
        streamed = proven_crc32_update(streamed, sv("789"));
        PROVEN_TEST_ASSERT(streamed == proven_crc32(sv(whole)),
            "CRC-32 fed in three chunks equals CRC-32 of the whole",
            "If it does not, the running value is not being carried correctly between chunks.");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("SHA-256: the FIPS 180-4 vectors",
        "The digest that is safe to fingerprint content with must be THE SHA-256, bit for bit.",
        "Empty, \"abc\", the 56-byte example, and the million-'a' vector.");
    // ---------------------------------------------------------------
    {
        proven_byte_t d[PROVEN_SHA256_SIZE];

        proven_sha256(sv(""), d);
        PROVEN_TEST_ASSERT(hex_eq(d, sizeof d,
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
            "SHA-256 of the empty input", "");

        proven_sha256(sv("abc"), d);
        PROVEN_TEST_ASSERT(hex_eq(d, sizeof d,
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
            "SHA-256 of \"abc\"", "");

        proven_sha256(sv("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"), d);
        PROVEN_TEST_ASSERT(hex_eq(d, sizeof d,
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
            "SHA-256 of the 56-byte FIPS example (crosses the block/length-padding boundary)", "");

        /* One million 'a', fed in awkward chunks: the digest depends only on the bytes,
         * not on how they were split, and a length that pushes into a second padding block
         * is exactly where a length-counter bug hides. */
        proven_sha256_t ctx;
        proven_sha256_init(&ctx);
        proven_byte_t chunk[997];
        memset(chunk, 'a', sizeof chunk);
        proven_size_t left = 1000000;
        while (left > 0) {
            proven_size_t n = left < sizeof chunk ? left : sizeof chunk;
            proven_sha256_update(&ctx, (proven_mem_view_t){ .ptr = chunk, .size = n });
            left -= n;
        }
        proven_sha256_final(&ctx, d);
        PROVEN_TEST_ASSERT(hex_eq(d, sizeof d,
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),
            "SHA-256 of one million 'a', streamed in 997-byte chunks", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("SHA-256 hex spelling matches sha256sum / git",
        "A fingerprint people paste into logs and filenames is 64 lowercase hex characters.",
        "");
    // ---------------------------------------------------------------
    {
        proven_byte_t d[PROVEN_SHA256_SIZE];
        char hex[65];
        proven_sha256(sv("abc"), d);
        proven_sha256_to_hex(d, hex);
        PROVEN_TEST_ASSERT(strcmp(hex,
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
            "the hex spelling of SHA-256(\"abc\") matches what sha256sum prints", "");
        PROVEN_TEST_ASSERT(hex[64] == '\0', "and it is NUL-terminated", "");
    }

    PROVEN_TEST_PASS("every hash agrees with its own standard's vectors.");
    return 0;
}
