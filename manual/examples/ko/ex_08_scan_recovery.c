#include "example.h"

/*
 * The scanner's error codes, and how to recover from them.
 *
 * The scanner is not scanf. It never writes through a pointer it was not given,
 * it never guesses a width, and it tells you which of several different things
 * went wrong. That last part only helps if you know what the codes mean - so
 * this program provokes each one on purpose.
 */

static proven_u8str_view_t v(const char *s) {
    return proven_u8str_view_from_cstr(s);
}

int main(void) {
    /* --- the primitives restore the cursor when they fail ----------------- */
    /* A failed scan is a non-event: the cursor is where it was, so you can try
     * to parse the same position as something else. */
    {
        proven_scan_t sc = proven_scan_init(v("abc"));
        proven_result_i64_t n = proven_scan_i64(&sc);
        EXAMPLE_REQUIRE(n.err == PROVEN_ERR_INVALID_ARG, "'abc' is not an integer");
        EXAMPLE_REQUIRE(sc.cursor == 0, "a failed integer scan leaves the cursor alone");

        /* So the same position can be read as a word instead. */
        proven_result_u8str_view_t w = proven_scan_str(&sc);
        EXAMPLE_REQUIRE(proven_is_ok(w.err) && proven_u8str_view_eq(w.val, PROVEN_LIT("abc")),
                        "the same bytes parse fine as a word");
    }

    /* --- a number that does not fit is OVERFLOW, not a wrapped value ------ */
    {
        proven_scan_t sc = proven_scan_init(v("9223372036854775808"));   /* INT64_MAX + 1 */
        proven_result_i64_t n = proven_scan_i64(&sc);
        EXAMPLE_REQUIRE(n.err == PROVEN_ERR_OVERFLOW, "one past INT64_MAX must not wrap");
        EXAMPLE_REQUIRE(sc.cursor == 0, "the cursor is restored on overflow too");
    }

    /* --- but a float that underflows is NOT an error ---------------------- */
    /* Too large is OVERFLOW; too small is zero, with the sign kept. That
     * asymmetry is deliberate - underflow to zero is the correctly rounded
     * answer, while overflow has no correct finite answer at all. */
    {
        proven_scan_t big = proven_scan_init(v("1e309"));
        proven_result_f64_t b = proven_scan_f64(&big);
        EXAMPLE_REQUIRE(b.err == PROVEN_ERR_OVERFLOW, "1e309 does not fit a double");

        proven_scan_t tiny = proven_scan_init(v("-1e-400"));
        proven_result_f64_t t = proven_scan_f64(&tiny);
        EXAMPLE_REQUIRE(proven_is_ok(t.err), "1e-400 underflows, which is not an error");
        EXAMPLE_REQUIRE(t.val == 0.0, "it rounds to zero");
    }

    /* --- the integer scanners are decimal only ---------------------------- */
    /* "0x10" is not sixteen. It is a zero, followed by text the scanner has not
     * been asked to look at. This surprises people, so it is worth knowing. */
    {
        proven_scan_t sc = proven_scan_init(v("0x10"));
        proven_result_i64_t n = proven_scan_i64(&sc);
        EXAMPLE_REQUIRE(proven_is_ok(n.err) && n.val == 0, "0x10 scans as the integer 0");
        EXAMPLE_REQUIRE(sc.cursor == 1, "and the cursor stops before the 'x'");
    }

    /* --- scanning stops at the first byte that cannot belong to the value -- */
    {
        proven_scan_t sc = proven_scan_init(v("12abc"));
        proven_result_i64_t n = proven_scan_i64(&sc);
        EXAMPLE_REQUIRE(proven_is_ok(n.err) && n.val == 12, "12abc yields 12");
        EXAMPLE_REQUIRE(sc.cursor == 2, "and leaves 'abc' for whoever asks next");
    }

    /* --- unsigned means unsigned ------------------------------------------ */
    {
        proven_scan_t sc = proven_scan_init(v("-1"));
        proven_result_u64_t n = proven_scan_u64(&sc);
        EXAMPLE_REQUIRE(n.err == PROVEN_ERR_INVALID_ARG,
                        "-1 is rejected rather than wrapping to a huge unsigned value");
    }

    /* --- navigating to a value: skip_until ------------------------------- */
    /* skip_until leaves the cursor ON the target, not past it, so you decide
     * how much of it to consume. */
    {
        proven_scan_t sc = proven_scan_init(v("port=8080"));
        proven_err_t err = proven_scan_skip_until(&sc, PROVEN_LIT("="));
        EXAMPLE_REQUIRE(proven_is_ok(err), "the '=' is there");
        EXAMPLE_REQUIRE(sc.cursor == 4, "the cursor sits on the '=' itself");

        ++sc.cursor;                                  /* step over it */
        proven_result_i64_t port = proven_scan_i64(&sc);
        EXAMPLE_REQUIRE(proven_is_ok(port.err) && port.val == 8080, "the port parses");

        /* Not finding it is NOT_FOUND, and the cursor does not move - the
         * scanner does not consume the input it failed to navigate. */
        proven_scan_t sc2 = proven_scan_init(v("port=8080"));
        proven_err_t missing = proven_scan_skip_until(&sc2, PROVEN_LIT("#"));
        EXAMPLE_REQUIRE(missing == PROVEN_ERR_NOT_FOUND, "there is no '#'");
        EXAMPLE_REQUIRE(sc2.cursor == 0, "and the cursor stayed put");
    }

    /* --- the structural scanner ------------------------------------------- */
    {
        int id = 0;
        double ratio = 0.0;
        proven_u8str_view_t name = {0};

        proven_err_t err = proven_scan_fmt(v("id=7 ratio=0.5 name=ada"),
                                           "id={} ratio={} name={}",
                                           PROVEN_SCAN_ARG(&id),
                                           PROVEN_SCAN_ARG(&ratio),
                                           PROVEN_SCAN_ARG(&name));
        EXAMPLE_REQUIRE(proven_is_ok(err), "the line matches the shape");
        EXAMPLE_REQUIRE(id == 7 && ratio == 0.5, "the values land in the right places");
        EXAMPLE_REQUIRE(proven_u8str_view_eq(name, PROVEN_LIT("ada")), "including the word");
    }

    /* --- the structural scanner is NOT transactional ---------------------- */
    /*
     * This is the one that bites. When a literal fails to match, the scan
     * returns an error - but the placeholders BEFORE the mismatch have already
     * been written through. `id` is 7 even though the call failed.
     *
     * So: on failure, treat every destination as clobbered. If you need
     * all-or-nothing, scan into locals and only publish them once the call
     * succeeded, which is what the code below does.
     */
    {
        int id = -1;
        double ratio = -1.0;
        proven_err_t err = proven_scan_fmt(v("id=7 ratio=0.5"),
                                           "id={} XXX={}",       /* the literal is wrong */
                                           PROVEN_SCAN_ARG(&id),
                                           PROVEN_SCAN_ARG(&ratio));
        EXAMPLE_REQUIRE(err == PROVEN_ERR_NOT_FOUND, "the literal 'XXX=' is not in the input");
        EXAMPLE_REQUIRE(id == 7, "and yet id was already written: the scan is not atomic");

        /* The safe shape: scan into locals, publish on success. */
        int good_id = 0;
        double good_ratio = 0.0;
        int published_id = -1;
        proven_err_t ok = proven_scan_fmt(v("id=7 ratio=0.5"), "id={} ratio={}",
                                          PROVEN_SCAN_ARG(&good_id), PROVEN_SCAN_ARG(&good_ratio));
        if (proven_is_ok(ok)) published_id = good_id;
        EXAMPLE_REQUIRE(published_id == 7, "publish only what a successful scan produced");
    }

    /* --- running out of input, and having input left over ------------------ */
    {
        int a = 0, b = 0;
        proven_err_t short_input = proven_scan_fmt(v("5"), "{} {}",
                                                   PROVEN_SCAN_ARG(&a), PROVEN_SCAN_ARG(&b));
        EXAMPLE_REQUIRE(!proven_is_ok(short_input), "two placeholders, one value: that fails");

        /* Trailing input is NOT an error. The scanner matched what you asked for
         * and stopped; it does not police what you did not ask about. If the
         * whole line must be consumed, check that yourself. */
        int only = 0;
        proven_scan_t sc = proven_scan_init(v("7 8"));
        proven_err_t err = proven_scan_fmt_cursor(&sc, "{}", PROVEN_SCAN_ARG(&only));
        EXAMPLE_REQUIRE(proven_is_ok(err) && only == 7, "the first value scans");
        EXAMPLE_REQUIRE(sc.cursor < sc.view.size, "and '8' is still sitting there, unconsumed");
    }

    /* --- narrow destinations are range-checked ---------------------------- */
    {
        short small = 0;
        proven_err_t err = proven_scan_fmt(v("70000"), "{}", PROVEN_SCAN_ARG(&small));
        EXAMPLE_REQUIRE(err == PROVEN_ERR_OVERFLOW,
                        "70000 does not fit a short, and the scanner says so rather than truncating");
    }

    return EXAMPLE_OK();
}
