#include "example.h"

/*
 * Every value handed to the formatter arrives as a proven_arg_t, and every value
 * the scanner writes into arrives as a proven_scan_arg_t. Most of the time you
 * never see either: PROVEN_ARG(x) and PROVEN_SCAN_ARG(&x) pick the right
 * constructor from the type of what you passed, and that is the whole point of
 * them.
 *
 * Two situations take the choice back off the macro, and both are ordinary:
 *
 *   1. The macro cannot see a type it recognises. A `proven_u8str_view_t`, a
 *      broken-down date, a raw address printed for a diagnostic - these need the
 *      constructor named, because there is no plain C type for the macro to
 *      dispatch on that means what you mean.
 *
 *   2. You are building the argument list at RUNTIME. A logging helper that
 *      takes "whatever the caller already assembled" receives proven_arg_t
 *      values, not ints and strings - and a macro that turns a value into an
 *      argument cannot be handed something that is already an argument. That is
 *      what the identity constructors are for: they let the same macro-driven
 *      code path accept an argument that was built earlier.
 *
 * So this example writes both sides out by name. The width and signedness of a
 * destination is not decoration - it is what decides whether 70000 arrives as
 * 70000 or as 4464.
 */

/* A logging helper that takes arguments the caller has already built. Because
 * its parameters are proven_arg_t, PROVEN_ARG would be the wrong tool inside it
 * - the value is already an argument, and proven_arg_identity is what says so. */
static proven_fmt_result_t log_pair(proven_u8str_t *out, const char *fmt,
                                    proven_arg_t a, proven_arg_t b) {
    return proven_u8str_append_fmt(out, fmt, proven_arg_identity(a), proven_arg_identity(b));
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    proven_result_u8str_t line = proven_u8str_create(alloc, 256);
    EXAMPLE_REQUIRE(proven_is_ok(line.err), "creating the output string must succeed");
    if (!proven_is_ok(line.err)) {
        return 1;
    }
    proven_u8str_t out = line.value;

    /* --- naming the formatting argument yourself -------------------------- */

    /* A single character and a flag. Written by name here so it is visible that
     * a bool prints as true/false rather than as 1/0. */
    proven_fmt_result_t r = proven_u8str_append_fmt(&out, "flag={} mark={}",
                                                    proven_arg_bool(true),
                                                    proven_arg_char('!'));
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "formatting a bool and a char must succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&out), PROVEN_LIT("flag=true mark=!")),
                    "a bool renders as a word, not as a digit");

    EXAMPLE_REQUIRE(proven_is_ok(proven_u8str_reset(&out)), "clearing the line must succeed");

    /* Unsigned widths. The constructor is the declaration of what the value is:
     * an unsigned 32-bit count and an unsigned 64-bit byte total are different
     * facts, and writing them out says which one you have. */
    r = proven_u8str_append_fmt(&out, "files={} bytes={}",
                                proven_arg_u32(1200u),
                                proven_arg_u64(5368709120ULL));
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "formatting unsigned values must succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&out), PROVEN_LIT("files=1200 bytes=5368709120")),
                    "a 64-bit total is printed in full, not truncated to 32 bits");

    EXAMPLE_REQUIRE(proven_is_ok(proven_u8str_reset(&out)), "clearing the line must succeed");

    /* Three ways to give the formatter text, in order of how much they trust
     * the caller:
     *
     *   proven_arg_str_view  - a pointer AND a length. Nothing is scanned for a
     *                          terminator, so there is no terminator to be
     *                          missing. Prefer this.
     *   proven_arg_cstr      - a NUL-terminated C string. The formatter walks it
     *                          looking for the terminator, so it must be there,
     *                          and the memory must still be alive.
     *   proven_arg_ucstr     - the same, for `unsigned char *`, which is what a
     *                          byte buffer is usually typed as. It exists so the
     *                          caller does not have to write a cast that
     *                          silences a real warning.
     */
    const char *name = "report.txt";
    const unsigned char *tag = (const unsigned char *)"draft";
    proven_u8str_view_t note = PROVEN_LIT("first pass");

    r = proven_u8str_append_fmt(&out, "{} [{}] {}",
                                proven_arg_cstr(name),
                                proven_arg_ucstr(tag),
                                proven_arg_str_view(note));
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "formatting the three text forms must succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&out),
                                         PROVEN_LIT("report.txt [draft] first pass")),
                    "all three produce the same kind of output from different kinds of pointer");

    EXAMPLE_REQUIRE(proven_is_ok(proven_u8str_reset(&out)), "clearing the line must succeed");

    /* A date, and an address. Neither has a plain C type that means what it
     * means: a date is a struct, and an address printed for a diagnostic is a
     * deliberate act rather than something to fall into. */
    proven_datetime_t when = proven_time_breakdown(0);   /* the epoch: a fixed, checkable value */
    int local = 0;

    r = proven_u8str_append_fmt(&out, "at {} object {}",
                                proven_arg_datetime(when),
                                proven_arg_ptr(&local));
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "formatting a date and a pointer must succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_starts_with(proven_u8str_as_view(&out), PROVEN_LIT("at 1970-01-01")),
                    "the epoch breaks down to the first of January 1970");
    EXAMPLE_REQUIRE(proven_u8str_view_find(proven_u8str_as_view(&out), 0, PROVEN_LIT("0x")) != PROVEN_SIZE_MAX,
                    "and an address is rendered in hexadecimal");

    EXAMPLE_REQUIRE(proven_is_ok(proven_u8str_reset(&out)), "clearing the line must succeed");

    /* Arguments built here, passed on, and formatted there. */
    r = log_pair(&out, "status={} retries={}", proven_arg_cstr("ok"), proven_arg_u32(3u));
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "formatting pre-built arguments must succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&out), PROVEN_LIT("status=ok retries=3")),
                    "an argument built by the caller formats exactly as one built in place");

    /* --- naming the scanning destination yourself ------------------------- */

    /* On the way in, the constructor names the destination, and the destination
     * decides what "too big" means. These are the plain C types - short, int,
     * long, long long and their unsigned forms - for the very common case where
     * the variable you already have is one of them rather than a fixed-width
     * type. */
    proven_scan_t scan = proven_scan_init(
        PROVEN_LIT("h=-32000 uh=65000 i=-2000000 ui=4000000000 l=-9000000 ul=9000000 "
                   "ll=-9007199254740993 ull=18446744073709551615 f=2.5 word=alpha"));

    short h = 0;
    unsigned short uh = 0;
    int i = 0;
    unsigned int ui = 0;
    long l = 0;
    unsigned long ul = 0;
    long long ll = 0;
    unsigned long long ull = 0;
    double f = 0.0;
    proven_u8str_view_t word = {0};

    proven_err_t err = proven_scan_fmt_cursor(
        &scan, "h={} uh={} i={} ui={} l={} ul={} ll={} ull={} f={} word={}",
        proven_scan_arg_short(&h),
        proven_scan_arg_ushort(&uh),
        proven_scan_arg_int(&i),
        proven_scan_arg_uint(&ui),
        proven_scan_arg_long(&l),
        proven_scan_arg_ulong(&ul),
        proven_scan_arg_llong(&ll),
        proven_scan_arg_ullong(&ull),
        proven_scan_arg_f64(&f),
        proven_scan_arg_str_view(&word));
    EXAMPLE_REQUIRE(proven_is_ok(err), "scanning every plain integer width must succeed");
    EXAMPLE_REQUIRE(h == -32000 && uh == 65000u, "the short forms hold their values");
    EXAMPLE_REQUIRE(i == -2000000 && ui == 4000000000u, "and so do the int forms");
    EXAMPLE_REQUIRE(l == -9000000L && ul == 9000000UL, "and the long forms");
    EXAMPLE_REQUIRE(ll == -9007199254740993LL, "a value needing 64 bits arrives whole");
    EXAMPLE_REQUIRE(ull == 18446744073709551615ULL, "including the largest unsigned 64-bit value");
    EXAMPLE_REQUIRE(f > 2.4999 && f < 2.5001, "the floating-point destination reads a decimal");

    /* A scanned string view points INTO the text being scanned. Nothing was
     * copied and nothing was allocated, so it is valid exactly as long as that
     * text is - copy it into a proven_u8str_t if it must outlive the scan. */
    EXAMPLE_REQUIRE(proven_u8str_view_eq(word, PROVEN_LIT("alpha")), "the word is captured as a view");

    /* The destination's width is a promise the scanner keeps. 70000 does not fit
     * in a short, so it is refused rather than wrapped to 4464 - which is what a
     * cast would have produced, silently, in a file nobody looks at again. */
    proven_scan_t narrow = proven_scan_init(PROVEN_LIT("70000"));
    short too_small = 7;
    err = proven_scan_fmt_cursor(&narrow, "{}", proven_scan_arg_short(&too_small));
    EXAMPLE_REQUIRE(err != PROVEN_OK, "a value that does not fit the destination is refused");
    EXAMPLE_REQUIRE(too_small == 7, "and the destination keeps the value it had");

    /* The scanning identity constructor, for the same reason as the formatting
     * one: a helper that receives ready-made scan arguments cannot re-wrap them. */
    proven_scan_t again = proven_scan_init(PROVEN_LIT("41"));
    proven_i32 answer = 0;
    proven_scan_arg_t prebuilt = proven_scan_arg_i32(&answer);
    err = proven_scan_fmt_cursor(&again, "{}", proven_scan_arg_identity(prebuilt));
    EXAMPLE_REQUIRE(proven_is_ok(err) && answer == 41, "a pre-built scan argument works unchanged");

    printf("arguments: %s\n", proven_u8str_as_cstr(&out));

    proven_u8str_destroy(alloc, &out);
    return EXAMPLE_OK();
}
