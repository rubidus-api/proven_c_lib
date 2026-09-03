#include "example.h"
#include <string.h>

/*
 * Reading numbers out of text, and writing them back.
 *
 * The text here is the sort a program actually meets: a log line with mixed
 * widths and signs, and a measurements file where the interesting number is
 * buried in prose. That means three jobs the earlier chapter-8 examples do not
 * cover:
 *
 *   - Scanning into types WIDER than int, and into unsigned ones, where the
 *     destination type is the whole question - a byte count that does not fit in
 *     32 bits is the classic overflow nobody notices until the file is 5 GB.
 *   - Positioning the cursor by hand - skipping whitespace, or skipping ahead to
 *     wherever the next number starts - when the input is not a rigid format.
 *   - Converting a decimal string to a double and back, exactly, without going
 *     through the C library's locale-dependent conversions.
 *
 * The float part matters more than it sounds. strtod reads "3,5" as 3.5 in one
 * locale and as 3 in another, and the same program then disagrees with itself
 * across machines. The parser used here is locale-free by construction: a comma
 * is never a decimal point, whatever the environment says.
 */

int main(void) {
    /* --- 1. scanning into the type the value needs ------------------------ */

    /* A log line: a request id that needs 64 bits, a byte count that is never
     * negative and can exceed 4 GB, a small status code, and a negative offset. */
    proven_scan_t scan = proven_scan_init(
        PROVEN_LIT("id=9007199254740993 bytes=5368709120 status=404 delta=-17"));

    proven_i64 id = 0;
    proven_u64 bytes = 0;
    proven_u32 status = 0;
    proven_i32 delta = 0;

    /* Each argument constructor names the destination type, so the scanner
     * writes the right width and reports an overflow instead of wrapping. The
     * generic PROVEN_SCAN_ARG() macro picks these for you from the pointer's
     * type; the named forms are what it picks, and what you write when you want
     * the choice visible. */
    proven_err_t err = proven_scan_fmt_cursor(&scan, "id={} bytes={} status={} delta={}",
                                       proven_scan_arg_i64(&id),
                                       proven_scan_arg_u64(&bytes),
                                       proven_scan_arg_u32(&status),
                                       proven_scan_arg_i32(&delta));
    EXAMPLE_REQUIRE(proven_is_ok(err), "scanning the log line must succeed");
    EXAMPLE_REQUIRE(id == 9007199254740993LL, "a 64-bit id survives, which a 32-bit destination could not");
    EXAMPLE_REQUIRE(bytes == 5368709120ULL, "and so does a byte count larger than 4 GiB");
    EXAMPLE_REQUIRE(status == 404u, "the small unsigned value reads normally");
    EXAMPLE_REQUIRE(delta == -17, "and a signed destination accepts the minus sign");

    /* The destination type is a contract, not a hint. A negative number has no
     * unsigned representation, and the scanner refuses rather than wrapping it
     * to something enormous. */
    proven_scan_t neg = proven_scan_init(PROVEN_LIT("-17"));
    proven_u32 nowhere = 12345;
    err = proven_scan_fmt_cursor(&neg, "{}", proven_scan_arg_u32(&nowhere));
    EXAMPLE_REQUIRE(err != PROVEN_OK, "a negative value cannot be scanned into an unsigned destination");
    EXAMPLE_REQUIRE(nowhere == 12345, "and the destination is left as it was");

    /* --- 2. moving the cursor when the input is not rigid ----------------- */

    /* Free-form text: the numbers matter, the words between them do not. */
    proven_scan_t notes = proven_scan_init(
        PROVEN_LIT("   sample A measured 42 units; sample B measured -8 units"));

    /* skip_whitespace advances past spaces, tabs and newlines. It is what you
     * call between fields of a format you are driving by hand - it never fails,
     * because "there was no whitespace" is not an error. */
    proven_scan_skip_whitespace(&notes);
    EXAMPLE_REQUIRE(notes.cursor == 3, "the three leading spaces are consumed");

    /* skip_until_number runs the cursor forward to the first digit, or to a sign
     * immediately followed by one. It is the call that turns "find the number in
     * this line" from a hand-written loop into one statement. */
    proven_scan_skip_until_number(&notes);
    proven_i32 first = 0;
    err = proven_scan_fmt_cursor(&notes, "{}", proven_scan_arg_i32(&first));
    EXAMPLE_REQUIRE(proven_is_ok(err) && first == 42, "the first number in the line is 42");

    proven_scan_skip_until_number(&notes);
    proven_i32 second = 0;
    err = proven_scan_fmt_cursor(&notes, "{}", proven_scan_arg_i32(&second));
    EXAMPLE_REQUIRE(proven_is_ok(err) && second == -8,
                    "and the next one keeps its sign, because the sign is part of the number");

    /* At the end there is nothing left to find, and the cursor stops rather than
     * running off: the scan is over, not broken. */
    proven_scan_skip_until_number(&notes);
    EXAMPLE_REQUIRE(notes.cursor == notes.view.size, "with no number left, the cursor lands at the end");

    /* --- 3. decimal text to double, without a locale --------------------- */

    proven_parse_double_result_t d = proven_parse_double_ascii(PROVEN_LIT("3.14159 rest"));
    EXAMPLE_REQUIRE(proven_is_ok(d.err), "parsing a decimal number must succeed");
    EXAMPLE_REQUIRE(d.val > 3.14158 && d.val < 3.14160, "and produce the value the digits spell");

    /* consumed says where the number ended, so the caller can carry on from
     * there - which is how you parse a list without copying it into pieces. */
    EXAMPLE_REQUIRE(d.consumed == 7, "consumed reports exactly the bytes the number used");

    /* A comma is not a decimal point here and never will be, whatever locale the
     * program is running under. The number ends at the comma. */
    proven_parse_double_result_t comma = proven_parse_double_ascii(PROVEN_LIT("3,5"));
    EXAMPLE_REQUIRE(proven_is_ok(comma.err) && comma.consumed == 1 && comma.val == 3.0,
                    "a comma ends the number: the parser is locale-free by construction");

    /* proven_parse_f64_ascii is the same function under its earlier name, kept
     * for code written before the current one existed. New code should use
     * proven_parse_double_ascii; both are here so an old call site still reads
     * correctly. */
    proven_parse_f64_result_t same = proven_parse_f64_ascii(PROVEN_LIT("3.14159 rest"));
    EXAMPLE_REQUIRE(same.val == d.val && same.consumed == d.consumed,
                    "the compatibility name is the same parser");

    /* Text that is not a number at all is an error, not a silent zero - which is
     * what atof() would have handed back with nothing to distinguish it from a
     * genuine "0". */
    proven_parse_double_result_t junk = proven_parse_double_ascii(PROVEN_LIT("not a number"));
    EXAMPLE_REQUIRE(!proven_is_ok(junk.err), "unparsable text is reported, not turned into 0");
    EXAMPLE_REQUIRE(junk.consumed == 0, "and nothing was consumed");

    /* --- 4. writing a float back ------------------------------------------ */

    /* proven_arg_f64 is how a floating-point value enters the formatter. Both
     * float and double go through it, so the digits a program prints do not
     * depend on the width of the variable it happened to be kept in. */
    proven_allocator_t alloc = proven_heap_allocator();
    proven_result_u8str_t line = proven_u8str_create(alloc, 64);
    EXAMPLE_REQUIRE(proven_is_ok(line.err), "creating the output string must succeed");

    proven_fmt_result_t out = proven_u8str_append_fmt(&line.value, "measured {} units",
                                                      proven_arg_f64(d.val));
    EXAMPLE_REQUIRE(proven_is_ok(out.err), "formatting the parsed value must succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_starts_with(proven_u8str_as_view(&line.value),
                                                  PROVEN_LIT("measured 3.14159")),
                    "and spell the number it was given");

    /* The policy form is the explicit one, for when the caller needs a
     * particular spelling rather than the default. SHORTEST asks for the fewest
     * digits that read back as exactly this value - which is the property a
     * serialiser wants, because it makes the round trip exact without printing
     * seventeen digits for numbers that do not need them. */
    char shortest[64];
    proven_size_t wrote = 0;
    float measured = 0.1f;
    proven_err_t ferr = proven_float_format_f32_policy(shortest, sizeof shortest, measured,
                                                       PROVEN_FLOAT_FORMAT_POLICY_RYU,
                                                       proven_float_format_options_shortest(),
                                                       &wrote);
    EXAMPLE_REQUIRE(proven_is_ok(ferr), "formatting a float in shortest mode must succeed");
    EXAMPLE_REQUIRE(wrote > 0 && shortest[wrote] == '\0', "the result is written and terminated");
    EXAMPLE_REQUIRE(strcmp(shortest, "0.1") == 0,
                    "0.1f prints as 0.1: the shortest text that reads back as the same float");

    /* And it round-trips: the text reads back as the value it came from. That is
     * the guarantee shortest mode exists to provide. */
    proven_parse_double_result_t roundtrip = proven_parse_double_ascii(
        (proven_u8str_view_t){ .ptr = (const proven_byte_t *)shortest, .size = wrote });
    EXAMPLE_REQUIRE(proven_is_ok(roundtrip.err), "the shortest form parses back");
    EXAMPLE_REQUIRE((float)roundtrip.val == measured, "as exactly the float it was printed from");

    printf("scanned id=%lld bytes=%llu; formatted %s and %s\n",
           (long long)id, (unsigned long long)bytes, proven_u8str_as_cstr(&line.value), shortest);

    proven_u8str_destroy(alloc, &line.value);
    return EXAMPLE_OK();
}
