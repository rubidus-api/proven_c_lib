#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * The manual makes CLAIMS. Each one is a proposition about the library that is either true or
 * false, and the reader is entitled to assume every one of them holds. Prose cannot be
 * test-driven, but a claim can be TESTED — you write the assertion the sentence implies, and the
 * build decides whether the sentence is still true.
 *
 * This is the same thing test_docs_manual_ch08_contracts does for the scanner chapter, done for
 * the modules added in the v26.07.13 line. It exists because prose ages worst of anything in a
 * repository: the README said "`proven` exposes no fsync" for a month after `proven_fs_sync`
 * shipped, and nothing objected, because nobody had written down what that sentence was asserting.
 *
 * The rule for adding to this file: when you write a sentence in the manual that a reader could
 * act on — a value, a boundary, a refusal, a guarantee — write the assertion for it here. If you
 * cannot state the assertion, the sentence is too vague to be in the manual.
 *
 * Each check below quotes the claim it is testing.
 */

int main(void) {
    PROVEN_TEST_SUITE("every factual claim the new chapters make is true",
        "The manual's statements about the hashes, the encoders, the generators and the streams, turned into assertions. A sentence a reader can act on is a proposition the build can check.",
        "A failure names the claim. Either the code changed and the manual did not, or the manual was wrong when it was written - decide which before changing either.");

    proven_allocator_t heap = proven_heap_allocator();

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("chapter 4, hashing",
        "The values and guarantees the hashing section states as fact.",
        "");
    // ---------------------------------------------------------------
    {
        /* CLAIM: "a checksum; interoperates with gzip/zlib/PNG" - which is only meaningful if it
         * is the standard CRC-32, whose published check value is 0xcbf43926 for "123456789". */
        PROVEN_TEST_ASSERT(proven_crc32(proven_mem_view_from_u8(PROVEN_LIT("123456789"))) == 0xcbf43926u,
            "the manual calls proven_crc32 the CRC that gzip/zlib/PNG carry - so it must produce the shared check value",
            "A different value means it is not that CRC, and the interoperability the chapter promises does not exist.");

        /* CLAIM: "proven_crc32_update ... Start from 0; the value you hold between calls is the
         * real CRC, so you can store it, log it, and resume." */
        proven_u32 whole = proven_crc32(proven_mem_view_from_u8(PROVEN_LIT("123456789")));
        proven_u32 c = proven_crc32_update(0, proven_mem_view_from_u8(PROVEN_LIT("1234")));
        c = proven_crc32_update(c, proven_mem_view_from_u8(PROVEN_LIT("56789")));
        PROVEN_TEST_ASSERT(c == whole,
            "chaining chunks through proven_crc32_update must equal the one-shot CRC",
            "The chapter tells the reader they may store the intermediate value and resume. If chaining differs, that instruction corrupts their checksum.");

        /* CLAIM: "PROVEN_SHA256_SIZE 32 - the digest; size your output buffer with this",
         * and: "proven_sha256_to_hex ... 64 lowercase hex characters. NUL-terminated." */
        PROVEN_TEST_ASSERT(PROVEN_SHA256_SIZE == 32,
            "PROVEN_SHA256_SIZE must be the digest size the chapter tells callers to allocate", "");

        proven_byte_t digest[PROVEN_SHA256_SIZE];
        proven_sha256(proven_mem_view_from_u8(PROVEN_LIT("abc")), digest);
        char hex[65];
        proven_sha256_to_hex(digest, hex);
        PROVEN_TEST_ASSERT(strlen(hex) == 64 && hex[64] == '\0',
            "proven_sha256_to_hex must write 64 characters and a NUL, as the chapter states", "");
        /* And it must be the digest the rest of the world computes for "abc". */
        PROVEN_TEST_ASSERT(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
            "and it must be the standard SHA-256 of \"abc\" - the chapter calls it the spelling sha256sum and git print",
            "If this differs, the fingerprint does not interoperate with anything, which is the only reason to use it.");

        /* CLAIM: "the same input gives the same output on any target" - endianness-independence
         * cannot be tested on one machine, but its consequence can: the digest of the same bytes
         * must not depend on how they were CHUNKED. */
        proven_sha256_t ctx;
        proven_sha256_init(&ctx);
        proven_sha256_update(&ctx, proven_mem_view_from_u8(PROVEN_LIT("a")));
        proven_sha256_update(&ctx, proven_mem_view_from_u8(PROVEN_LIT("b")));
        proven_sha256_update(&ctx, proven_mem_view_from_u8(PROVEN_LIT("c")));
        proven_byte_t streamed[PROVEN_SHA256_SIZE];
        proven_sha256_final(&ctx, streamed);
        PROVEN_TEST_ASSERT(memcmp(streamed, digest, PROVEN_SHA256_SIZE) == 0,
            "the streaming digest must equal the one-shot: the chapter says it depends only on the bytes, never on how they were chunked", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("chapter 4, encoding",
        "Every promise the encoding section makes about refusal, sizing, and the two alphabets.",
        "");
    // ---------------------------------------------------------------
    {
        proven_byte_t out[64];
        proven_size_t w = 0;

        /* CLAIM: "proven_base64url_encode ... URL-safe alphabet, NO padding". */
        proven_err_t e = proven_base64url_encode(
            proven_mem_view_from_u8(PROVEN_LIT("f")), out, sizeof out, &w);
        PROVEN_TEST_ASSERT(proven_is_ok(e) && w == 2 && memcmp(out, "Zg", 2) == 0,
            "the URL form must emit no '=' padding, as the chapter states",
            "A '=' in a token is the thing the chapter says this form exists to avoid.");

        /* CLAIM: "proven_base64_decode ... Accepts BOTH alphabets, padded or not." */
        proven_size_t dn = 0;
        PROVEN_TEST_ASSERT(proven_is_ok(proven_base64_decode(
                proven_mem_view_from_u8(PROVEN_LIT("Zg==")), out, sizeof out, &dn)) && dn == 1 && out[0] == 'f',
            "padded standard Base64 must decode", "");
        PROVEN_TEST_ASSERT(proven_is_ok(proven_base64_decode(
                proven_mem_view_from_u8(PROVEN_LIT("Zg")), out, sizeof out, &dn)) && dn == 1 && out[0] == 'f',
            "and the unpadded URL form must decode to the same byte - the chapter promises both", "");

        /* CLAIM: "proven_base64_decoded_size ... An upper bound for PADDED AND UNPADDED text",
         * and the counter-example warns that `3 * (n/4)` gives 0 for "QQ". */
        PROVEN_TEST_ASSERT(proven_base64_decoded_size(2) >= 1,
            "decoded_size must be an upper bound for unpadded text, as the chapter's caution insists",
            "The chapter tells the reader to size their buffer with this. If it under-reports, that instruction fails on the library's own base64url output.");

        /* CLAIM: "a short buffer is PROVEN_ERR_OUT_OF_BOUNDS, never a truncated prefix" -
         * and the section header calls this the 'Refuse, never truncate' class. */
        proven_byte_t tiny[3];
        memset(tiny, 0xAA, sizeof tiny);
        PROVEN_TEST_ASSERT(proven_hex_encode(proven_mem_view_from_u8(PROVEN_LIT("foobar")),
                                             tiny, sizeof tiny, &w) == PROVEN_ERR_OUT_OF_BOUNDS,
            "a short output buffer must be refused", "");
        PROVEN_TEST_ASSERT(tiny[0] == 0xAA && tiny[2] == 0xAA,
            "and the refused call must write NOTHING - the chapter promises no truncated prefix", "");

        /* CLAIM: "Whitespace is not skipped, on purpose. A pasted, line-wrapped Base64 blob is
         * INVALID_ENCODING, not a silently different result." */
        PROVEN_TEST_ASSERT(proven_base64_decode(proven_mem_view_from_u8(PROVEN_LIT("Zm9v YmFy")),
                                                out, sizeof out, &dn) == PROVEN_ERR_INVALID_ENCODING,
            "embedded whitespace must be INVALID_ENCODING, exactly as the chapter says", "");

        /* CLAIM: "validates the WHOLE input before writing a single byte, so a stray character
         * near the end cannot leave you holding a half-decoded prefix." */
        memset(out, 0xAA, sizeof out);
        PROVEN_TEST_ASSERT(proven_hex_decode(proven_mem_view_from_u8(PROVEN_LIT("6162!!")),
                                             out, sizeof out, &dn) == PROVEN_ERR_INVALID_ENCODING,
            "a stray character is INVALID_ENCODING", "");
        PROVEN_TEST_ASSERT(out[0] == 0xAA && dn == 0,
            "and NOTHING is committed - the valid prefix \"6162\" must not have been written",
            "This is the claim that makes the module worth having over a two-line loop.");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("chapter 5, randomness",
        "The guarantees the randomness section states, including the ones that are security properties.",
        "");
    // ---------------------------------------------------------------
    {
        /* CLAIM: "Seed the reproducible generator. Any seed is fine - even 0" and the counter-
         * example's premise: the same seed replays the same run. */
        proven_xoshiro256ss_t a, b;
        proven_xoshiro256ss_seed(&a, 0);
        proven_xoshiro256ss_seed(&b, 0);
        bool replays = true, nonzero = false;
        for (int i = 0; i < 64; ++i) {
            proven_u64 x = proven_xoshiro256ss_next(&a);
            if (x != proven_xoshiro256ss_next(&b)) replays = false;
            if (x != 0) nonzero = true;
        }
        PROVEN_TEST_ASSERT(replays, "the same seed must replay the same run, as the chapter states", "");
        PROVEN_TEST_ASSERT(nonzero,
            "and seed 0 must not be degenerate - the chapter tells the reader any seed is fine",
            "An all-zero xoshiro state emits zeros forever. The chapter's promise rests on the SplitMix64 expansion.");

        /* CLAIM: "proven_chacha_rng ... invalid if the generator was never successfully seeded",
         * and "the generator is left INERT - it yields zeros and an invalid trait". */
        proven_chacha_rng_t g;
        memset(&g, 0, sizeof g);
        PROVEN_TEST_ASSERT(!proven_rng_is_valid(proven_chacha_rng(&g)),
            "an unseeded generator must present an INVALID trait, as the reference table states",
            "If it is valid, every helper draws from a generator with no key - which is the failure the chapter's caution is about.");

        proven_byte_t zeros[128];
        memset(zeros, 0xFF, sizeof zeros);
        proven_chacha_rng_fill(&g, zeros, sizeof zeros);
        bool all_zero = true;
        for (size_t i = 0; i < sizeof zeros; ++i) if (zeros[i] != 0) all_zero = false;
        PROVEN_TEST_ASSERT(all_zero,
            "and it must yield zeros - a visibly dead value, which is what the chapter promises a caller who ignores the seeding bool",
            "Plausible-looking bytes here are exactly the security hole the counter-example warns about.");

        /* CLAIM: "proven_rng_below ... Uniform in [0, bound), UNBIASED", and the counter-example
         * says `% 6` is not. We cannot prove uniformity in a unit test, but we CAN prove the
         * bound is respected and 0 is handled as the table says. */
        proven_xoshiro256ss_t s;
        proven_xoshiro256ss_seed(&s, 7);
        proven_rng_t rng = proven_xoshiro256ss_rng(&s);
        for (int i = 0; i < 5000; ++i) {
            PROVEN_TEST_ASSERT(proven_rng_below(rng, 6) < 6,
                "every draw must be strictly below the bound, as the reference table states", "");
        }
        PROVEN_TEST_ASSERT(proven_rng_below(rng, 0) == 0,
            "a bound of 0 must be 0 - the table says so rather than leaving it undefined", "");

        /* CLAIM: "proven_rng_f64 ... Uniform in [0, 1). 53 bits; never returns 1.0." */
        for (int i = 0; i < 5000; ++i) {
            double d = proven_rng_f64(rng);
            PROVEN_TEST_ASSERT(d >= 0.0 && d < 1.0, "f64 must be in [0,1) and never 1.0", "");
        }

        /* CLAIM: "proven_rng_range ... The full INT64_MIN..INT64_MAX span does not overflow",
         * and "Returns lo if hi < lo". */
        PROVEN_TEST_ASSERT(proven_rng_range(rng, 10, 3) == 10,
            "an inverted range must return lo, as the table states", "");
        (void)proven_rng_range(rng, INT64_MIN, INT64_MAX);   /* must not trap: UBSan is watching */
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("chapter 5, streams",
        "The refusals and lifetimes the streams sections promise.",
        "");
    // ---------------------------------------------------------------
    {
        /* CLAIM: "A line longer than the buffer is refused, not truncated. PROVEN_ERR_OUT_OF_BOUNDS",
         * and: "A line that exactly FILLS the buffer is fine: the newline does not have to fit
         * too, and neither does a final line with no newline at all." */
        proven_u8str_view_t path = PROVEN_LIT("claims_lines.tmp");
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_write_file(heap, path,
            proven_mem_view_from_u8(PROVEN_LIT("abcd\ntoolong\n")))), "setup", "");

        proven_result_file_t rf = proven_fs_open(heap, path, PROVEN_FS_READ);
        PROVEN_TEST_ASSERT(proven_is_ok(rf.err), "setup: open", "");

        proven_byte_t buf[4];   /* exactly the size of the first line */
        proven_sysio_lines_t lines;
        PROVEN_TEST_ASSERT(proven_is_ok(proven_sysio_lines_open(&lines, rf.value,
            (proven_mem_mut_t){ .ptr = buf, .size = sizeof buf })), "setup: line reader", "");

        proven_result_u8str_view_t l1 = proven_sysio_read_line(&lines);
        PROVEN_TEST_ASSERT(proven_is_ok(l1.err) && l1.val.size == 4 &&
                           memcmp(l1.val.ptr, "abcd", 4) == 0,
            "a line that exactly fills the buffer must be RETURNED - the chapter says the newline does not have to fit too",
            "OUT_OF_BOUNDS here would make the chapter's parenthesis false, and would lose data.");

        proven_result_u8str_view_t l2 = proven_sysio_read_line(&lines);
        PROVEN_TEST_ASSERT(l2.err == PROVEN_ERR_OUT_OF_BOUNDS,
            "and a line LONGER than the buffer must be refused, not truncated",
            "A truncated line returned as a success is the corruption the chapter says this refusal exists to prevent.");

        (void)proven_fs_close(rf.value);
        (void)proven_fs_remove(heap, path);

        /* CLAIM: "proven_writer_is_valid ... A zeroed handle is invalid, and every constructor
         * returns one on bad arguments - so this is the check, not a NULL test." */
        proven_sysio_out_t out;
        proven_writer_t bad = proven_sysio_stdout_buffered(&out,
            (proven_mem_mut_t){ .ptr = NULL, .size = 0 });
        PROVEN_TEST_ASSERT(!proven_writer_is_valid(bad),
            "a constructor given bad arguments must return an INVALID handle, as the table states", "");
        PROVEN_TEST_ASSERT(!proven_reader_is_valid(proven_sysio_stdin_reader(NULL)),
            "and so must the reader side", "");
    }

    PROVEN_TEST_PASS("every claim these chapters make, that a reader could act on, is true.");
    return 0;
}
