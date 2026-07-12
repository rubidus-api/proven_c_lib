#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * The atomic fixed-capacity format promises one thing: if it fails, the string is
 * exactly what it was. Not nearly. Exactly.
 *
 * That promise used to be structural - the formatter measured the whole output before
 * writing a byte, so a format that could not fit never touched the buffer. It is not
 * structural any more. Measuring first meant rendering every value twice, and for a
 * double that is the correctly-rounded decimal engine run twice with the first answer
 * thrown away; the allocation-free path a logger uses paid for a whole extra render of
 * every argument. It writes as it goes now, and restores the length if it fails.
 *
 * So the promise now rests on a rollback, and a rollback is a thing that can be wrong.
 * This test is what keeps it honest: after a failure the visible string - bytes, length,
 * NUL - must be indistinguishable from the string before the call, whether the failure
 * was "it does not fit", "the format is malformed", or "the argument count is wrong",
 * and whether the failure came before any output or halfway through it.
 */

static void expect_unchanged(proven_u8str_t *s, const char *what,
                             const proven_byte_t *before, proven_size_t before_len) {
    proven_u8str_view_t v = proven_u8str_as_view(s);
    PROVEN_TEST_ASSERT(v.size == before_len,
        what,
        "A failed atomic format left the string a different length. The rollback in the fixed-capacity path of proven_u8str_fmt_internal did not put the length back.");
    PROVEN_TEST_ASSERT(memcmp(v.ptr, before, before_len) == 0,
        what,
        "A failed atomic format changed the visible bytes.");
    PROVEN_TEST_ASSERT(proven_u8str_as_cstr(s)[before_len] == '\0',
        what,
        "A failed atomic format left the string without its NUL terminator, so as_cstr reads past the end.");
}

int main(void) {
    PROVEN_TEST_SUITE("the fixed-capacity format is atomic on failure",
        "A failed format must leave the string byte-for-byte as it was - it writes as it goes now, so this rests on a rollback rather than on never having written.",
        "Inspect the single-pass branch of proven_u8str_fmt_internal in src/proven/fmt.c: on any error it restores internal.len and reseals the NUL.");

    proven_byte_t buf[32];
    proven_u8str_t s = proven_u8str_borrow(buf, sizeof buf);

    proven_fmt_result_t r = proven_u8str_append_fmt(&s, "keep=");
    PROVEN_TEST_ASSERT(proven_is_ok(r.err), "setup: seed the string", "");

    proven_byte_t before[32];
    proven_size_t before_len = proven_u8str_as_view(&s).size;
    memcpy(before, proven_u8str_as_view(&s).ptr, before_len);

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("output that does not fit changes nothing",
        "The bytes are physically written up to the capacity now, and then taken back. The caller must not be able to tell.",
        "");
    // ---------------------------------------------------------------
    {
        /* 32 bytes of capacity, and this wants far more. */
        r = proven_u8str_append_fmt(&s, "{}{}{}", PROVEN_ARG("0123456789abcdef"),
                                    PROVEN_ARG("0123456789abcdef"), PROVEN_ARG("0123456789abcdef"));
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_OUT_OF_BOUNDS, "an oversized format must be refused", "");
        PROVEN_TEST_ASSERT(r.written == 0, "and must report that it wrote nothing", "");
        PROVEN_TEST_ASSERT(r.required == 48,
            "and must still report how many bytes it needed, so the caller can size a buffer and retry",
            "The single pass counts every byte the format wants while copying only the bytes that fit. That count is the whole reason a second pass is unnecessary.");
        expect_unchanged(&s, "the string is unchanged after an overflow", before, before_len);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a failure that happens HALFWAY through the output changes nothing",
        "This is the case a rollback can get wrong: bytes are already in the buffer when the error is discovered.",
        "The bad spec is the third field, so the first two have already been rendered into the caller's memory when the format fails.");
    // ---------------------------------------------------------------
    {
        r = proven_u8str_append_fmt(&s, "a={} b={} c={:q}", PROVEN_ARG(1), PROVEN_ARG(2), PROVEN_ARG(3));
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_INVALID_FORMAT, "an unknown type letter must be refused", "");
        expect_unchanged(&s, "the string is unchanged after a mid-output format error", before, before_len);

        /* Same shape, but the argument cannot honour the spec: also discovered late. */
        r = proven_u8str_append_fmt(&s, "a={} b={:x}", PROVEN_ARG(1), PROVEN_ARG(2.5));
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_INVALID_FORMAT, "hex on a double must be refused", "");
        expect_unchanged(&s, "the string is unchanged after a late applicability failure", before, before_len);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a wrong argument count changes nothing",
        "The count is checked after the run, by which time the output is already in the buffer.",
        "");
    // ---------------------------------------------------------------
    {
        proven_arg_t args[3] = { proven_arg_none(), PROVEN_ARG(1), PROVEN_ARG(2) };
        r = proven_u8str_fmt_internal((proven_allocator_t){0}, &s, false, "only={}",
                                      (proven_allocator_t){0}, args, 3);
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_INVALID_ARG, "an unconsumed argument must be an error", "");
        expect_unchanged(&s, "the string is unchanged after an argument-count error", before, before_len);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("and a format that succeeds still appends",
        "The rollback must not fire on the happy path.",
        "");
    // ---------------------------------------------------------------
    {
        r = proven_u8str_append_fmt(&s, "{}", PROVEN_ARG(42));
        PROVEN_TEST_ASSERT(proven_is_ok(r.err), "a format that fits must succeed", "");
        PROVEN_TEST_ASSERT(r.written == 2 && r.required == 2, "and must report what it wrote", "");
        PROVEN_TEST_ASSERT(strcmp(proven_u8str_as_cstr(&s), "keep=42") == 0,
            "and the output must be appended to what was there", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("an exact fit is not an overflow",
        "The NUL needs a byte too. Off by one here means either a rejected string that fits or a buffer overrun.",
        "");
    // ---------------------------------------------------------------
    {
        proven_byte_t tight[8];
        proven_u8str_t t = proven_u8str_borrow(tight, sizeof tight);   /* 7 usable + NUL */
        proven_fmt_result_t f = proven_u8str_append_fmt(&t, "{}", PROVEN_ARG("1234567"));
        PROVEN_TEST_ASSERT(proven_is_ok(f.err) && strcmp(proven_u8str_as_cstr(&t), "1234567") == 0,
            "seven bytes into an eight-byte buffer must fit exactly", "");

        proven_u8str_t u = proven_u8str_borrow(tight, sizeof tight);
        f = proven_u8str_append_fmt(&u, "{}", PROVEN_ARG("12345678"));
        PROVEN_TEST_ASSERT(f.err == PROVEN_ERR_OUT_OF_BOUNDS && f.required == 8,
            "eight bytes into an eight-byte buffer must not fit: the NUL needs one",
            "");
        PROVEN_TEST_ASSERT(proven_u8str_as_view(&u).size == 0,
            "and the string must still be empty", "");
    }

    PROVEN_TEST_PASS("the fixed-capacity format is atomic on failure.");
    return 0;
}
