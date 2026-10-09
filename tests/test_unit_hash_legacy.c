#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * SHA-1 and MD5 against vectors that exist outside this repository:
 *
 *   SHA-1 - the FIPS 180-4 / RFC 3174 examples, including the million-'a' message.
 *   MD5   - the seven-message test suite printed in RFC 1321 appendix A.5.
 *   Both  - the digests `sha1sum` and `md5sum` print for runs of 'x' whose lengths sit on
 *           each side of the padding boundary (55, 56, 57) and of the block boundary
 *           (63, 64, 65), and two blocks on (119, 120).
 *   RFC 6455 section 1.3 - the WebSocket accept key, the reason SHA-1 is in the library.
 *
 * The vectors were written down before the implementation was run against them.
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
    PROVEN_TEST_SUITE("legacy digests: SHA-1 and MD5",
        "Two broken digests that formats still name - each against its own published vectors.",
        "Inspect src/proven/hash_legacy.c. A mismatch means the implementation disagrees with the standard, not merely with itself.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("SHA-1: the FIPS 180-4 vectors",
        "A format that says SHA-1 means THE SHA-1, bit for bit.",
        "Empty, \"abc\", the 56-byte example, a sentence, and the million-'a' vector.");
    // ---------------------------------------------------------------
    {
        proven_byte_t d[PROVEN_SHA1_SIZE];

        proven_sha1(sv(""), d);
        PROVEN_TEST_ASSERT(hex_eq(d, sizeof d, "da39a3ee5e6b4b0d3255bfef95601890afd80709"),
            "SHA-1 of the empty input", "");

        proven_sha1(sv("abc"), d);
        PROVEN_TEST_ASSERT(hex_eq(d, sizeof d, "a9993e364706816aba3e25717850c26c9cd0d89d"),
            "SHA-1 of \"abc\"", "");

        proven_sha1(sv("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"), d);
        PROVEN_TEST_ASSERT(hex_eq(d, sizeof d, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"),
            "SHA-1 of the 56-byte FIPS example (the length does not fit the first block)", "");

        proven_sha1(sv("The quick brown fox jumps over the lazy dog"), d);
        PROVEN_TEST_ASSERT(hex_eq(d, sizeof d, "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12"),
            "SHA-1 of the pangram", "");

        proven_sha1_t ctx;
        proven_sha1_init(&ctx);
        proven_byte_t chunk[997];
        memset(chunk, 'a', sizeof chunk);
        proven_size_t left = 1000000;
        while (left > 0) {
            proven_size_t n = left < sizeof chunk ? left : sizeof chunk;
            proven_sha1_update(&ctx, (proven_mem_view_t){ .ptr = chunk, .size = n });
            left -= n;
        }
        proven_sha1_final(&ctx, d);
        PROVEN_TEST_ASSERT(hex_eq(d, sizeof d, "34aa973cd4c4daa4f61eeb2bdbad27316534016f"),
            "SHA-1 of one million 'a', streamed in 997-byte chunks", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("MD5: the RFC 1321 test suite",
        "All seven messages of appendix A.5.",
        "MD5 is little-endian in its words, its length and its output - the opposite of SHA-1.");
    // ---------------------------------------------------------------
    {
        static const struct { const char *msg; const char *want; } v[] = {
            { "", "d41d8cd98f00b204e9800998ecf8427e" },
            { "a", "0cc175b9c0f1b6a831c399e269772661" },
            { "abc", "900150983cd24fb0d6963f7d28e17f72" },
            { "message digest", "f96b697d7cb7938d525a2f31aaf161d0" },
            { "abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b" },
            { "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
              "d174ab98d277d9f5a5611c2c9f419d9f" },
            { "12345678901234567890123456789012345678901234567890123456789012345678901234567890",
              "57edf4a22be3c955ac49da2e2107b67a" },
        };
        for (proven_size_t i = 0; i < sizeof v / sizeof v[0]; ++i) {
            proven_byte_t d[PROVEN_MD5_SIZE];
            proven_md5(sv(v[i].msg), d);
            PROVEN_TEST_ASSERT(hex_eq(d, sizeof d, v[i].want),
                "MD5 must match the RFC 1321 vector for this message",
                "Check the T table, the rotation amounts and the message-word order of the failing round.");
        }
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("both: lengths on each side of the padding and block boundaries",
        "The final block is where a digest goes wrong: 55 bytes leave exactly room for the padding, 56 do not.",
        "Expected values are what sha1sum and md5sum print for that many 'x' characters.");
    // ---------------------------------------------------------------
    {
        static const struct { proven_size_t len; const char *sha1; const char *md5; } v[] = {
            { 55,  "cef734ba81a024479e09eb5a75b6ddae62e6abf1", "04364420e25c512fd958a70738aa8f72" },
            { 56,  "901305367c259952f4e7af8323f480d59f81335b", "668a72d5ba17f08e62dabcafad6db14b" },
            { 57,  "025ecbd5d70f8fb3c5457cd96bab13fda305dc59", "693037871c4a9d3d8685018905cb530a" },
            { 63,  "0ddc4e0cccd9a12850deb5abb0853a4425559fec", "7dc2ca208106a2f703567bdff99d8981" },
            { 64,  "bb2fa3ee7afb9f54c6dfb5d021f14b1ffe40c163", "c1bb4f81d892b2d57947682aeb252456" },
            { 65,  "78c741ddc482e4cdf8c474a0876347a0905b6233", "1bc932052302d074bdec39795fe00cf6" },
            { 119, "4300320394f7ee239bcdce7d3b8bcee173a0cd5c", "ab347a5f68c8a443cfcddc633f12c24f" },
            { 120, "ceb2821639c4b6dcb10bce0e522ca2e608ce056d", "fb98667f98096de92620b64f46e1c5b5" },
        };
        proven_byte_t buf[120];
        memset(buf, 'x', sizeof buf);
        for (proven_size_t i = 0; i < sizeof v / sizeof v[0]; ++i) {
            proven_mem_view_t m = { .ptr = buf, .size = v[i].len };
            proven_byte_t s[PROVEN_SHA1_SIZE];
            proven_byte_t d[PROVEN_MD5_SIZE];
            proven_sha1(m, s);
            proven_md5(m, d);
            PROVEN_TEST_ASSERT(hex_eq(s, sizeof s, v[i].sha1),
                "SHA-1 at this boundary length", "Inspect legacy_pad: the 0x80 byte, the zero fill and the big-endian bit length.");
            PROVEN_TEST_ASSERT(hex_eq(d, sizeof d, v[i].md5),
                "MD5 at this boundary length", "Inspect legacy_pad: the 0x80 byte, the zero fill and the little-endian bit length.");
        }
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("both: every split point gives the one-shot digest",
        "A digest depends on the bytes, never on how they were fed.",
        "200 bytes cut in two at every position, then fed one byte at a time.");
    // ---------------------------------------------------------------
    {
        proven_byte_t buf[200];
        for (proven_size_t i = 0; i < sizeof buf; ++i) buf[i] = (proven_byte_t)(i * 7u + 3u);
        proven_byte_t s_want[PROVEN_SHA1_SIZE], d_want[PROVEN_MD5_SIZE];
        proven_sha1((proven_mem_view_t){ .ptr = buf, .size = sizeof buf }, s_want);
        proven_md5((proven_mem_view_t){ .ptr = buf, .size = sizeof buf }, d_want);

        bool sha1_ok = true, md5_ok = true;
        for (proven_size_t cut = 0; cut <= sizeof buf; ++cut) {
            proven_mem_view_t head = { .ptr = buf, .size = cut };
            proven_mem_view_t tail = { .ptr = buf + cut, .size = sizeof buf - cut };
            proven_byte_t s[PROVEN_SHA1_SIZE], d[PROVEN_MD5_SIZE];
            proven_sha1_t sc;
            proven_sha1_init(&sc);
            proven_sha1_update(&sc, head);
            proven_sha1_update(&sc, tail);
            proven_sha1_final(&sc, s);
            proven_md5_t mc;
            proven_md5_init(&mc);
            proven_md5_update(&mc, head);
            proven_md5_update(&mc, tail);
            proven_md5_final(&mc, d);
            if (memcmp(s, s_want, sizeof s) != 0) sha1_ok = false;
            if (memcmp(d, d_want, sizeof d) != 0) md5_ok = false;
        }
        PROVEN_TEST_ASSERT(sha1_ok, "SHA-1 over two updates equals the one-shot at all 201 split points", "");
        PROVEN_TEST_ASSERT(md5_ok, "MD5 over two updates equals the one-shot at all 201 split points", "");

        proven_byte_t s[PROVEN_SHA1_SIZE], d[PROVEN_MD5_SIZE];
        proven_sha1_t sc;
        proven_md5_t mc;
        proven_sha1_init(&sc);
        proven_md5_init(&mc);
        for (proven_size_t i = 0; i < sizeof buf; ++i) {
            proven_sha1_update(&sc, (proven_mem_view_t){ .ptr = buf + i, .size = 1 });
            proven_md5_update(&mc, (proven_mem_view_t){ .ptr = buf + i, .size = 1 });
        }
        proven_sha1_final(&sc, s);
        proven_md5_final(&mc, d);
        PROVEN_TEST_ASSERT(memcmp(s, s_want, sizeof s) == 0, "SHA-1 fed one byte at a time", "");
        PROVEN_TEST_ASSERT(memcmp(d, d_want, sizeof d) == 0, "MD5 fed one byte at a time", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("hex spelling, and inputs that are not there",
        "The hex form is what sha1sum and md5sum print; a null view of size zero is the empty message.",
        "");
    // ---------------------------------------------------------------
    {
        proven_byte_t s[PROVEN_SHA1_SIZE], d[PROVEN_MD5_SIZE];
        char shex[41], dhex[33];
        proven_sha1(sv("abc"), s);
        proven_sha1_to_hex(s, shex);
        PROVEN_TEST_ASSERT(strcmp(shex, "a9993e364706816aba3e25717850c26c9cd0d89d") == 0,
            "proven_sha1_to_hex writes 40 lowercase characters and a NUL", "");
        proven_md5(sv("abc"), d);
        proven_md5_to_hex(d, dhex);
        PROVEN_TEST_ASSERT(strcmp(dhex, "900150983cd24fb0d6963f7d28e17f72") == 0,
            "proven_md5_to_hex writes 32 lowercase characters and a NUL", "");

        proven_sha1((proven_mem_view_t){ NULL, 0 }, s);
        PROVEN_TEST_ASSERT(hex_eq(s, sizeof s, "da39a3ee5e6b4b0d3255bfef95601890afd80709"),
            "SHA-1 of a null view of size zero is the empty-message digest", "");
        proven_md5((proven_mem_view_t){ NULL, 0 }, d);
        PROVEN_TEST_ASSERT(hex_eq(d, sizeof d, "d41d8cd98f00b204e9800998ecf8427e"),
            "MD5 likewise", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the WebSocket accept key of RFC 6455 section 1.3",
        "SHA-1 of the client's key and a fixed GUID, in Base64 - the two calls a handshake needs.",
        "Key dGhlIHNhbXBsZSBub25jZQ== must give s3pPLMBiTxaQ9kYGzzhZRbK+xOo=.");
    // ---------------------------------------------------------------
    {
        proven_sha1_t ctx;
        proven_sha1_init(&ctx);
        proven_sha1_update(&ctx, sv("dGhlIHNhbXBsZSBub25jZQ=="));
        proven_sha1_update(&ctx, sv("258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
        proven_byte_t digest[PROVEN_SHA1_SIZE];
        proven_sha1_final(&ctx, digest);

        proven_byte_t accept[32];
        proven_size_t written = 0;
        proven_err_t err = proven_base64_encode((proven_mem_view_t){ .ptr = digest, .size = sizeof digest },
                                                accept, sizeof accept, &written);
        PROVEN_TEST_ASSERT(proven_is_ok(err) && written == 28 &&
                           memcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", 28) == 0,
            "the accept key matches the RFC's example", "");
    }

    PROVEN_TEST_PASS("SHA-1 and MD5 agree with their standards' vectors.");
    return 0;
}
