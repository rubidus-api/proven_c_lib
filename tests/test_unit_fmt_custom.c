#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * Formatting a type the library has never heard of.
 *
 * The formatter used to be a closed set: i32, u64, double, string, pointer, datetime.
 * A `vec3`, a `uuid`, a `rect` could not be printed at all. The two ways out were both
 * bad: pre-format the value into a scratch string and pass THAT (an allocation and a
 * copy per value, in the logging path, which is the one path that must not allocate),
 * or print the fields one at a time and give up on ever aligning the column.
 *
 * PROVEN_ARG_OF(&obj, render) closes that gap. The renderer writes into a sink, so it
 * composes - it can call the formatter again - and width/fill/alignment still work,
 * because the formatter measures the output with a counting sink before emitting it.
 */

typedef struct { int x, y; } point_t;

static proven_err_t render_point(proven_fmt_sink_t out, const void *obj) {
    const point_t *p = (const point_t *)obj;
    proven_byte_t tmp[64];
    proven_u8str_t s = proven_u8str_borrow(tmp, sizeof tmp);
    proven_arg_t args[3] = { proven_arg_none(), PROVEN_ARG(p->x), PROVEN_ARG(p->y) };
    proven_fmt_result_t r = proven_u8str_fmt_internal((proven_allocator_t){0}, &s, false, "({}, {})",
                                                      (proven_allocator_t){0}, args, 3);
    if (!proven_is_ok(r.err)) return r.err;
    return proven_fmt_put(out, proven_u8str_as_view(&s));
}

/* A renderer that fails. Its error must reach the caller, not be swallowed. */
static proven_err_t render_broken(proven_fmt_sink_t out, const void *obj) {
    (void)out; (void)obj;
    return PROVEN_ERR_IO;
}

/* A renderer that lies: it answers differently on the measuring pass than on the real
 * one. Left unchecked, it would silently produce a field of the wrong width in an
 * aligned column - which is precisely the class of bug the formatter refuses to have. */
static int lying_calls = 0;
static proven_err_t render_lying(proven_fmt_sink_t out, const void *obj) {
    (void)obj;
    const char *s = (lying_calls++ == 0) ? "ab" : "abcdefgh";
    return proven_fmt_put(out, (proven_u8str_view_t){ (const proven_u8 *)s, strlen(s) });
}

static const char *fmt_one(proven_byte_t *buf, proven_size_t cap, proven_fmt_result_t *out,
                           const char *f, proven_arg_t a) {
    proven_u8str_t s = proven_u8str_borrow(buf, cap);
    proven_arg_t args[2] = { proven_arg_none(), a };
    *out = proven_u8str_fmt_internal((proven_allocator_t){0}, &s, false, f,
                                     (proven_allocator_t){0}, args, 2);
    return proven_u8str_as_cstr(&s);
}

int main(void) {
    PROVEN_TEST_SUITE("formatting a type the library has never heard of",
        "PROVEN_ARG_OF renders a user type through a user function, with width and alignment applied to the result.",
        "Inspect render_custom and the two sinks in src/proven/fmt.c.");

    proven_byte_t buf[128];
    proven_fmt_result_t r;
    point_t p = { .x = 3, .y = -7 };

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a user type renders",
        "The renderer composes: it calls the formatter again, into a borrowed buffer, and hands the bytes to the sink.",
        "");
    // ---------------------------------------------------------------
    {
        const char *got = fmt_one(buf, sizeof buf, &r, "{}", PROVEN_ARG_OF(&p, render_point));
        PROVEN_TEST_ASSERT(proven_is_ok(r.err) && strcmp(got, "(3, -7)") == 0,
            "PROVEN_ARG_OF(&p, render_point) must render as (3, -7)", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("width, fill and alignment apply to the rendered result",
        "This is what the measuring pass is FOR: a column of user types lines up like any other column.",
        "render_custom runs the renderer once with a counting sink to learn the width, then once for real.");
    // ---------------------------------------------------------------
    {
        const char *got = fmt_one(buf, sizeof buf, &r, "[{:>12}]", PROVEN_ARG_OF(&p, render_point));
        PROVEN_TEST_ASSERT(proven_is_ok(r.err) && strcmp(got, "[     (3, -7)]") == 0,
            "right-aligned in 12 columns", "");

        got = fmt_one(buf, sizeof buf, &r, "[{:<12}]", PROVEN_ARG_OF(&p, render_point));
        PROVEN_TEST_ASSERT(proven_is_ok(r.err) && strcmp(got, "[(3, -7)     ]") == 0,
            "left-aligned in 12 columns", "");

        got = fmt_one(buf, sizeof buf, &r, "[{:*^11}]", PROVEN_ARG_OF(&p, render_point));
        PROVEN_TEST_ASSERT(proven_is_ok(r.err) && strcmp(got, "[**(3, -7)**]") == 0,
            "centred with a fill character", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a spec the library cannot interpret for your type is refused",
        "{:x} on a point has no meaning. Inventing one and reporting success is how a formatter starts lying.",
        "The applicability guard rejects a type letter, a precision, '#' and a sign on a custom argument.");
    // ---------------------------------------------------------------
    {
        (void)fmt_one(buf, sizeof buf, &r, "{:x}", PROVEN_ARG_OF(&p, render_point));
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_INVALID_FORMAT, "{:x} on a user type must be refused", "");

        (void)fmt_one(buf, sizeof buf, &r, "{:.2}", PROVEN_ARG_OF(&p, render_point));
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_INVALID_FORMAT, "{:.2} on a user type must be refused", "");

        (void)fmt_one(buf, sizeof buf, &r, "{:+}", PROVEN_ARG_OF(&p, render_point));
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_INVALID_FORMAT, "{:+} on a user type must be refused", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a renderer's failure reaches the caller",
        "An error the formatter swallows is an error the caller acts on as if it were success.",
        "");
    // ---------------------------------------------------------------
    {
        (void)fmt_one(buf, sizeof buf, &r, "{}", PROVEN_ARG_OF(&p, render_broken));
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_IO, "the renderer's own error code must be returned", "");

        (void)fmt_one(buf, sizeof buf, &r, "{}", proven_arg_custom(&p, NULL));
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_INVALID_ARG, "a NULL renderer is an invalid argument, not a crash", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a renderer that answers differently on the two passes is caught",
        "The contract says deterministic. A renderer that is not would produce a field of the wrong width, and the caller would never know.",
        "render_custom compares the bytes actually emitted against the measured length.");
    // ---------------------------------------------------------------
    {
        lying_calls = 0;
        (void)fmt_one(buf, sizeof buf, &r, "{:>10}", PROVEN_ARG_OF(&p, render_lying));
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_INVALID_ARG,
            "a renderer whose second pass disagrees with its first must be an error",
            "Emitting it would silently break the column it was being aligned into.");
    }

    PROVEN_TEST_PASS("user types format, align, and fail honestly.");
    return 0;
}
