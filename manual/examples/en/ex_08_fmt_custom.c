#include "example.h"

/*
 * Formatting a type the library has never heard of.
 *
 * `PROVEN_ARG` is built on `_Generic`, which can only dispatch on types it was told
 * about - and it cannot be told about yours. So before `PROVEN_ARG_OF` existed, a
 * `rect_t` simply could not be printed. You either pre-formatted it into a scratch
 * string and passed that (an allocation and a copy per value, in the logging path,
 * which is the one path that must not allocate), or you printed the fields one at a
 * time and gave up on ever aligning the column.
 *
 * A renderer receives a *sink*, not a buffer. That is what makes it compose: it can
 * call the formatter again, and its output is just bytes going somewhere. And because
 * the formatter measures the renderer's output before emitting it - by running it once
 * against a counting sink - width, fill and alignment work on a user type exactly as
 * they do on an int.
 */

typedef struct { int w, h; } rect_t;

static proven_err_t render_rect(proven_fmt_sink_t out, const void *obj) {
    const rect_t *r = (const rect_t *)obj;

    /* Compose: the formatter, into a stack buffer, no allocator anywhere. */
    proven_byte_t tmp[64];
    proven_u8str_t s = proven_u8str_borrow(tmp, sizeof tmp);
    proven_fmt_result_t f = proven_u8str_append_fmt(&s, "{}x{}",
                                                    PROVEN_ARG(r->w), PROVEN_ARG(r->h));
    if (!PROVEN_FMT_IS_OK(f)) return f.err;

    return proven_fmt_put(out, proven_u8str_as_view(&s));
}

int main(void) {
    rect_t a = { .w = 1920, .h = 1080 };
    rect_t b = { .w = 640,  .h = 480  };

    proven_byte_t buf[128];
    proven_u8str_t line = proven_u8str_borrow(buf, sizeof buf);

    /* Just like any other argument. */
    proven_fmt_result_t r = proven_u8str_append_fmt(&line, "mode={}", PROVEN_ARG_OF(&a, render_rect));
    EXAMPLE_REQUIRE(PROVEN_FMT_IS_OK(r), "a user type should format");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&line), PROVEN_LIT("mode=1920x1080")),
                    "the renderer's bytes should be what came out");

    /* And it aligns, which is the whole reason the formatter measures it first: a
     * column of user-defined values lines up like a column of anything else. */
    (void)proven_u8str_reset(&line);
    r = proven_u8str_append_fmt(&line, "[{:>10}]\n[{:>10}]",
                                PROVEN_ARG_OF(&a, render_rect),
                                PROVEN_ARG_OF(&b, render_rect));
    EXAMPLE_REQUIRE(PROVEN_FMT_IS_OK(r), "two user types should format");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&line),
                                         PROVEN_LIT("[ 1920x1080]\n[   640x480]")),
                    "both rows should be right-aligned to the same width");

    /* A spec the library cannot interpret for your type is refused, not guessed at.
     * `{:x}` on a rectangle has no meaning, and answering it with something plausible
     * while reporting success is how a formatter starts lying to you. */
    (void)proven_u8str_reset(&line);
    r = proven_u8str_append_fmt(&line, "{:x}", PROVEN_ARG_OF(&a, render_rect));
    EXAMPLE_REQUIRE(r.err == PROVEN_ERR_INVALID_FORMAT,
                    "a type letter on a user type should be an error");

    return EXAMPLE_OK();
}
