#include "proven/stream.h"
#include "../../platform/proven_sys_mem.h"

// -------------------------------------------------------------
// Writer
// -------------------------------------------------------------

proven_err_t proven_writer_write(proven_writer_t w, proven_mem_view_t chunk) {
    if (!proven_writer_is_valid(w)) return PROVEN_ERR_INVALID_ARG;
    if (chunk.size == 0) return PROVEN_OK;
    if (!chunk.ptr) return PROVEN_ERR_INVALID_ARG;
    return w.write_fn(w.ctx, chunk);
}

proven_err_t proven_writer_write_str(proven_writer_t w, proven_u8str_view_t view) {
    return proven_writer_write(w, proven_mem_view_from_u8(view));
}

proven_err_t proven_writer_flush(proven_writer_t w) {
    if (!proven_writer_is_valid(w)) return PROVEN_ERR_INVALID_ARG;
    if (!w.flush_fn) return PROVEN_OK;   /* nothing is held back */
    return w.flush_fn(w.ctx);
}

/* --- a file ----------------------------------------------------------- */

static proven_err_t writer_file_write(void *ctx, proven_mem_view_t chunk) {
    proven_file_t *file = (proven_file_t *)ctx;
    if (!file) return PROVEN_ERR_INVALID_ARG;
    return proven_fs_write_all(*file, chunk);
}

proven_writer_t proven_writer_from_file(proven_file_t *file) {
    if (!file) return (proven_writer_t){0};
    return (proven_writer_t){ .ctx = file, .write_fn = writer_file_write, .flush_fn = (void *)0 };
}

/* --- an owned string --------------------------------------------------- */

static proven_err_t writer_u8str_write(void *ctx, proven_mem_view_t chunk) {
    proven_writer_u8str_t *s = (proven_writer_u8str_t *)ctx;
    if (!s || !s->str) return PROVEN_ERR_INVALID_ARG;
    proven_u8str_view_t view = { .ptr = chunk.ptr, .size = chunk.size };
    return proven_u8str_append_grow(s->alloc, s->str, view);
}

proven_writer_t proven_writer_from_u8str(proven_writer_u8str_t *state, proven_u8str_t *str, proven_allocator_t alloc) {
    if (!state || !str) return (proven_writer_t){0};
    state->str = str;
    state->alloc = alloc;
    return (proven_writer_t){ .ctx = state, .write_fn = writer_u8str_write, .flush_fn = (void *)0 };
}

/* --- a fixed buffer ---------------------------------------------------- */

static proven_err_t writer_buf_write(void *ctx, proven_mem_view_t chunk) {
    proven_writer_buf_t *s = (proven_writer_buf_t *)ctx;
    if (!s || !s->buf.ptr) return PROVEN_ERR_INVALID_ARG;

    if (chunk.size > s->buf.size - s->len) {
        /* Refuse rather than truncate. A sink that silently drops the end of your
         * data is worse than one that says it cannot take it. */
        s->overflowed = true;
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }
    proven_sys_mem_copy(s->buf.ptr + s->len, chunk.ptr, chunk.size);
    s->len += chunk.size;
    return PROVEN_OK;
}

proven_writer_t proven_writer_from_buffer(proven_writer_buf_t *state) {
    if (!state || !state->buf.ptr) return (proven_writer_t){0};
    return (proven_writer_t){ .ctx = state, .write_fn = writer_buf_write, .flush_fn = (void *)0 };
}

/* --- buffering over another writer -------------------------------------- */

static proven_err_t writer_buffered_flush(void *ctx) {
    proven_writer_buffered_t *s = (proven_writer_buffered_t *)ctx;
    if (!s) return PROVEN_ERR_INVALID_ARG;
    if (s->len == 0) return PROVEN_OK;

    proven_mem_view_t held = { .ptr = s->buf.ptr, .size = s->len };
    proven_err_t err = proven_writer_write(s->inner, held);
    if (!proven_is_ok(err)) return err;

    /* Only drop what actually went out. A flush that fails must not silently
     * discard the bytes it failed to write. */
    s->len = 0;
    return proven_writer_flush(s->inner);
}

static proven_err_t writer_buffered_write(void *ctx, proven_mem_view_t chunk) {
    proven_writer_buffered_t *s = (proven_writer_buffered_t *)ctx;
    if (!s || !s->buf.ptr) return PROVEN_ERR_INVALID_ARG;

    /* A chunk bigger than the whole buffer is passed straight through: buffering it
     * would mean either failing or splitting it, and neither helps anyone. */
    if (chunk.size >= s->buf.size) {
        proven_err_t err = writer_buffered_flush(s);
        if (!proven_is_ok(err)) return err;
        return proven_writer_write(s->inner, chunk);
    }

    if (chunk.size > s->buf.size - s->len) {
        proven_err_t err = writer_buffered_flush(s);
        if (!proven_is_ok(err)) return err;
    }

    proven_sys_mem_copy(s->buf.ptr + s->len, chunk.ptr, chunk.size);
    s->len += chunk.size;
    return PROVEN_OK;
}

proven_writer_t proven_writer_buffered(proven_writer_buffered_t *state, proven_writer_t inner, proven_mem_mut_t buf) {
    if (!state || !buf.ptr || buf.size == 0 || !proven_writer_is_valid(inner)) {
        return (proven_writer_t){0};
    }
    state->inner = inner;
    state->buf = buf;
    state->len = 0;
    return (proven_writer_t){ .ctx = state, .write_fn = writer_buffered_write, .flush_fn = writer_buffered_flush };
}

// -------------------------------------------------------------
// Reader
// -------------------------------------------------------------

proven_result_size_t proven_reader_read(proven_reader_t r, proven_mem_mut_t dest) {
    proven_result_size_t res = {0};
    if (!proven_reader_is_valid(r)) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }
    if (dest.size == 0) return res;   /* OK, zero bytes */
    if (!dest.ptr) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }
    return r.read_fn(r.ctx, dest);
}

/* --- a file ------------------------------------------------------------ */

static proven_result_size_t reader_file_read(void *ctx, proven_mem_mut_t dest) {
    proven_result_size_t res = {0};
    proven_file_t *file = (proven_file_t *)ctx;
    if (!file) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }
    return proven_fs_read(*file, dest);
}

proven_reader_t proven_reader_from_file(proven_file_t *file) {
    if (!file) return (proven_reader_t){0};
    return (proven_reader_t){ .ctx = file, .read_fn = reader_file_read };
}

/* --- bytes you already have --------------------------------------------- */

static proven_result_size_t reader_view_read(void *ctx, proven_mem_mut_t dest) {
    proven_result_size_t res = {0};
    proven_reader_view_t *s = (proven_reader_view_t *)ctx;
    if (!s) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }
    proven_size_t left = s->view.size - s->cursor;
    if (left == 0) {
        res.err = PROVEN_ERR_EOF;
        return res;
    }
    proven_size_t n = left < dest.size ? left : dest.size;
    proven_sys_mem_copy(dest.ptr, s->view.ptr + s->cursor, n);
    s->cursor += n;
    res.err = PROVEN_OK;
    res.value = n;
    return res;
}

proven_reader_t proven_reader_from_view(proven_reader_view_t *state, proven_u8str_view_t view) {
    if (!state) return (proven_reader_t){0};
    if (view.size > 0 && !view.ptr) {
        view = (proven_u8str_view_t){ .ptr = (const proven_byte_t *)0, .size = 0 };
    }
    state->view = view;
    state->cursor = 0;
    return (proven_reader_t){ .ctx = state, .read_fn = reader_view_read };
}

/* --- buffering over another reader --------------------------------------- */

/* Pull more bytes in, compacting whatever has not been handed out yet to the front.
 * Returns false when the source is exhausted and nothing new arrived. */
static bool reader_buffered_fill(proven_reader_buffered_t *s) {
    if (s->eof) return false;

    if (s->cursor > 0) {
        proven_size_t keep = s->len - s->cursor;
        if (keep > 0) proven_sys_mem_move(s->buf.ptr, s->buf.ptr + s->cursor, keep);
        s->len = keep;
        s->cursor = 0;
    }
    if (s->len == s->buf.size) return false;   /* full, and nothing consumed */

    proven_mem_mut_t space = { .ptr = s->buf.ptr + s->len, .size = s->buf.size - s->len };
    proven_result_size_t r = proven_reader_read(s->inner, space);
    if (r.err == PROVEN_ERR_EOF || (proven_is_ok(r.err) && r.value == 0)) {
        s->eof = true;
        return false;
    }
    if (!proven_is_ok(r.err)) {
        s->eof = true;   /* a broken source is not going to get better */
        return false;
    }
    s->len += r.value;
    return true;
}

static proven_result_size_t reader_buffered_read(void *ctx, proven_mem_mut_t dest) {
    proven_result_size_t res = {0};
    proven_reader_buffered_t *s = (proven_reader_buffered_t *)ctx;
    if (!s || !s->buf.ptr) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }

    if (s->cursor == s->len && !reader_buffered_fill(s)) {
        res.err = PROVEN_ERR_EOF;
        return res;
    }

    proven_size_t have = s->len - s->cursor;
    proven_size_t n = have < dest.size ? have : dest.size;
    proven_sys_mem_copy(dest.ptr, s->buf.ptr + s->cursor, n);
    s->cursor += n;
    res.err = PROVEN_OK;
    res.value = n;
    return res;
}

proven_reader_t proven_reader_buffered(proven_reader_buffered_t *state, proven_reader_t inner, proven_mem_mut_t buf) {
    if (!state || !buf.ptr || buf.size == 0 || !proven_reader_is_valid(inner)) {
        return (proven_reader_t){0};
    }
    state->inner = inner;
    state->buf = buf;
    state->len = 0;
    state->cursor = 0;
    state->eof = false;
    return (proven_reader_t){ .ctx = state, .read_fn = reader_buffered_read };
}

proven_result_u8str_view_t proven_reader_read_line(proven_reader_buffered_t *s) {
    proven_result_u8str_view_t res = {0};
    if (!s || !s->buf.ptr) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }

    for (;;) {
        /* Is there a newline in what we already hold? */
        for (proven_size_t i = s->cursor; i < s->len; ++i) {
            if (s->buf.ptr[i] != (proven_byte_t)'\n') continue;

            proven_size_t end = i;
            if (end > s->cursor && s->buf.ptr[end - 1] == (proven_byte_t)'\r') --end;

            res.err = PROVEN_OK;
            res.val = (proven_u8str_view_t){ .ptr = s->buf.ptr + s->cursor, .size = end - s->cursor };
            s->cursor = i + 1;   /* step over the newline */
            return res;
        }

        /* No newline yet.
         *
         * Ask FIRST whether the buffer is simply full, because reader_buffered_fill
         * cannot tell us apart from a source that ended: it returns false for both.
         * Confusing the two is how a too-long line gets handed back truncated - as a
         * successful read of a line that was never a line - which is a corruption the
         * caller has no way to detect.
         *
         * cursor == 0 after a compaction means everything held is one unterminated
         * line, and there is nowhere left to put more of it. */
        if (s->cursor == 0 && s->len == s->buf.size) {
            res.err = PROVEN_ERR_OUT_OF_BOUNDS;
            return res;
        }

        if (!reader_buffered_fill(s)) {
            /* The source really is exhausted. A final line with no trailing newline
             * is still a line - dropping it is how the last record of a file goes
             * missing. */
            if (s->cursor < s->len) {
                res.err = PROVEN_OK;
                res.val = (proven_u8str_view_t){ .ptr = s->buf.ptr + s->cursor, .size = s->len - s->cursor };
                s->cursor = s->len;
                return res;
            }
            res.err = PROVEN_ERR_EOF;
            return res;
        }
    }
}

// -------------------------------------------------------------
// Formatting straight into a writer
// -------------------------------------------------------------

proven_fmt_result_t proven_fmt_to_writer_impl(proven_writer_t w, proven_mem_mut_t scratch,
                                              const char *fmt, const proven_arg_t *args, proven_size_t args_count) {
    proven_fmt_result_t res = {0};
    if (!proven_writer_is_valid(w) || !scratch.ptr || scratch.size == 0 || !fmt) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }

    /* Borrowed over the caller's memory: this allocates nothing, and the atomic
     * (non-truncating) mode means a line that does not fit is refused rather than
     * cut in half. A reader must never see half a line. */
    proven_u8str_t s = proven_u8str_borrow(scratch.ptr, scratch.size);
    res = proven_u8str_fmt_internal((proven_allocator_t){0}, &s, false, fmt,
                                    (proven_allocator_t){0}, args, args_count);
    if (!proven_is_ok(res.err)) return res;

    proven_err_t werr = proven_writer_write_str(w, proven_u8str_as_view(&s));
    if (!proven_is_ok(werr)) res.err = werr;
    return res;
}
