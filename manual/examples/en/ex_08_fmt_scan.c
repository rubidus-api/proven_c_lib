#include "example.h"

/*
 * Formatting and scanning are the two halves of the same idea: `{}` renders typed
 * values into text, and the scanner reads text back into typed destinations. Both
 * are type-checked at the call site (_Generic picks the constructor), so there is
 * no format-string/argument mismatch to get wrong at runtime.
 *
 * The choice that matters is *where the bytes go*:
 *
 *   append_fmt       - fixed capacity, atomic. Too long? Nothing is written and
 *                      you get PROVEN_ERR_OUT_OF_BOUNDS. No allocator involved,
 *                      so it works on a stack buffer.
 *   append_fmt_grow  - allocator-backed. Grows to fit; on allocation failure the
 *                      string is left exactly as it was.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- fixed capacity: no allocator, no allocation ------------------------ */
    /* borrow wraps caller memory, so this string lives entirely on the stack. `cap`
     * includes the NUL, so 32 bytes hold 31 of content. Nothing to destroy. */
    proven_byte_t stack_buf[32];
    proven_u8str_t fixed = proven_u8str_borrow(stack_buf, sizeof stack_buf);

    proven_fmt_result_t r = proven_u8str_append_fmt(&fixed, "port={}", PROVEN_ARG(8080));
    EXAMPLE_REQUIRE(PROVEN_FMT_IS_OK(r), "a short line should fit in 32 bytes");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&fixed), PROVEN_LIT("port=8080")),
                    "the fixed-capacity append should have rendered the port");

    /* Atomic means atomic: an append that does not fit changes nothing. The string
     * is still valid and still holds what it held before - no truncated tail to
     * clean up. (Use append_fmt_trunc if a truncated tail is what you want.) */
    proven_fmt_result_t too_long = proven_u8str_append_fmt(
        &fixed, " and a great deal more text than will ever fit here {}", PROVEN_ARG(1));
    EXAMPLE_REQUIRE(too_long.err == PROVEN_ERR_OUT_OF_BOUNDS, "the overlong append must fail");
    EXAMPLE_REQUIRE(too_long.required > too_long.written, "it reports what it would have needed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&fixed), PROVEN_LIT("port=8080")),
                    "a failed atomic append must leave the string untouched");

    /* --- specs: fill, align, width, hex ------------------------------------- */
    proven_result_u8str_t created = proven_u8str_create(alloc, 8);   /* deliberately small */
    EXAMPLE_REQUIRE(proven_is_ok(created.err), "creating the output string should succeed");
    if (!proven_is_ok(created.err)) return 1;
    proven_u8str_t out = created.value;

    /* grow reallocates as needed, so the initial capacity is a hint, not a limit.
     * `{:0>4}` = fill '0', align right, width 4. `{:x}` = lowercase hex, no 0x. */
    r = proven_u8str_append_fmt_grow(alloc, &out, "id={:0>4} tag={:*^9} addr=0x{:x}",
                                     PROVEN_ARG(7),
                                     PROVEN_ARG(PROVEN_LIT("ok")),
                                     PROVEN_ARG(48879));
    EXAMPLE_REQUIRE(PROVEN_FMT_IS_OK(r), "the growing append should succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&out),
                                         PROVEN_LIT("id=0007 tag=***ok**** addr=0xbeef")),
                    "fill/align/width/hex should render exactly this");
    printf("%s\n", proven_u8str_as_cstr(&out));

    /* --- untrusted text is bounded, never trusted to be NUL-terminated ------ */
    /* PROVEN_ARG on a char* means "walk it until a NUL turns up" - fine for a
     * literal, a buffer-overread for anything that came off a socket. This buffer
     * has no NUL at all; PROVEN_ARG_CSTR_N stops at the length instead, so it reads
     * only what actually exists. Use it for anything you did not create yourself. */
    const char untrusted[4] = {'a', 'b', 'c', 'd'};   /* no terminator, on purpose */
    EXAMPLE_REQUIRE(proven_is_ok(proven_u8str_reset(&out)), "reset should keep the buffer");
    r = proven_u8str_append_fmt_grow(alloc, &out, "payload={}",
                                     PROVEN_ARG_CSTR_N(untrusted, sizeof untrusted));
    EXAMPLE_REQUIRE(PROVEN_FMT_IS_OK(r), "the bounded append should succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&out), PROVEN_LIT("payload=abcd")),
                    "the bounded argument should render its whole 4 bytes and stop");

    /* --- format a record, then scan it back --------------------------------- */
    proven_i64 sensor_id = 42;
    double reading = 3.14159;

    EXAMPLE_REQUIRE(proven_is_ok(proven_u8str_reset(&out)), "reset should keep the buffer");
    r = proven_u8str_append_fmt_grow(alloc, &out, "{} {} {}",
                                     PROVEN_ARG(sensor_id),
                                     PROVEN_ARG(PROVEN_LIT("boiler")),
                                     PROVEN_ARG(reading));
    EXAMPLE_REQUIRE(PROVEN_FMT_IS_OK(r), "formatting the record should succeed");
    printf("record: %s\n", proven_u8str_as_cstr(&out));

    /* One scanner over one view. Each call advances the cursor past what it
     * consumed, so the calls compose left to right - and each one can fail
     * independently, which is the difference between a parser and a guess. */
    proven_scan_t sc = proven_scan_init(proven_u8str_as_view(&out));

    proven_result_i64_t id = proven_scan_i64(&sc);
    EXAMPLE_REQUIRE(proven_is_ok(id.err), "the first field should parse as an integer");
    EXAMPLE_REQUIRE(id.val == sensor_id, "the integer should round-trip");

    /* scan_str returns a view *into the scanned string* - it copies nothing and
     * owns nothing, so it is only valid while `out` is. */
    proven_result_u8str_view_t name = proven_scan_str(&sc);
    EXAMPLE_REQUIRE(proven_is_ok(name.err), "the second field should parse as a word");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(name.val, PROVEN_LIT("boiler")), "the name should round-trip");

    proven_result_f64_t temp = proven_scan_f64(&sc);
    EXAMPLE_REQUIRE(proven_is_ok(temp.err), "the third field should parse as a float");

    /* Exactly equal, not approximately: the scanner is correctly rounded, so it
     * returns the nearest double to the text - and the text the formatter produced
     * (six fractional digits) names this value unambiguously. Bit-for-bit, this is
     * the same double we started with. For a value that needs more than six
     * fractional digits, format it with the shortest policy
     * (proven_float_format_options_shortest) and the same round-trip holds. */
    EXAMPLE_REQUIRE(temp.val == reading, "the float must round-trip exactly, not approximately");

    /* The input is fully consumed: nothing was silently left on the table. */
    proven_result_i64_t extra = proven_scan_i64(&sc);
    EXAMPLE_REQUIRE(!proven_is_ok(extra.err), "there should be nothing left to scan");

    proven_u8str_destroy(alloc, &out);
    return EXAMPLE_OK();
}
