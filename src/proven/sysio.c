#include "proven/sysio.h"
#include "proven/heap.h"
#include "../../platform/proven_sys_io.h"
#include "../../platform/proven_sys_env.h"
#include "../../platform/proven_sys_mem.h"

proven_file_t proven_sysio_stdin(void) {
    proven_sys_io_handle_t sh = proven_sys_io_std_in();
    proven_file_t f = {0};
#if defined(_WIN32) || defined(_WIN64)
    f.internal.ptr = sh.handle;
#else
    f.internal.fd = sh.fd;
#endif
    return f;
}

proven_file_t proven_sysio_stdout(void) {
    proven_sys_io_handle_t sh = proven_sys_io_std_out();
    proven_file_t f = {0};
#if defined(_WIN32) || defined(_WIN64)
    f.internal.ptr = sh.handle;
#else
    f.internal.fd = sh.fd;
#endif
    return f;
}

proven_file_t proven_sysio_stderr(void) {
    proven_sys_io_handle_t sh = proven_sys_io_std_err();
    proven_file_t f = {0};
#if defined(_WIN32) || defined(_WIN64)
    f.internal.ptr = sh.handle;
#else
    f.internal.fd = sh.fd;
#endif
    return f;
}

void proven_sysio_flush(proven_file_t file) {
#if defined(_WIN32) || defined(_WIN64)
    proven_sys_io_handle_t handle = { .handle = file.internal.ptr };
#else
    proven_sys_io_handle_t handle = { .fd = file.internal.fd };
#endif
    proven_sys_io_flush(handle);
}

// -----------------------------------------------------------------------------
// Buffered Scanner for sysio (Safe for pipes/stdin)
// -----------------------------------------------------------------------------

[[nodiscard]] proven_err_t proven_sysio_scanner_init(proven_sysio_scanner_t *scanner, proven_file_t file, proven_allocator_t alloc, proven_size_t buffer_capacity) {
    if (!scanner || !proven_alloc_is_valid(alloc) || buffer_capacity == 0) return PROVEN_ERR_INVALID_ARG;

    *scanner = (proven_sysio_scanner_t){0};

    proven_result_mem_mut_t alloc_res = alloc.alloc_fn(alloc.ctx, buffer_capacity, 1);
    if (!proven_is_ok(alloc_res.err)) return alloc_res.err;
    if (!alloc_res.value.ptr) return PROVEN_ERR_INVALID_ARG;

    scanner->file = file;
    scanner->alloc = alloc;
    scanner->capacity = buffer_capacity;
    scanner->cursor = 0;
    scanner->length = 0;
    scanner->eof = false;
    scanner->buffer = (proven_u8 *)alloc_res.value.ptr;
    
    return PROVEN_OK;
}

void proven_sysio_scanner_deinit(proven_sysio_scanner_t *scanner) {
    if (!scanner) return;
    if (scanner->buffer && scanner->alloc.free_fn) {
        scanner->alloc.free_fn(scanner->alloc.ctx, scanner->buffer);
    }
    *scanner = (proven_sysio_scanner_t){0};
}

static proven_err_t scanner_fill(proven_sysio_scanner_t *scanner, proven_size_t *read_bytes_out) {
    if (read_bytes_out) *read_bytes_out = 0;
    if (!scanner || scanner->eof) return PROVEN_ERR_EOF;

    // Move remaining data to start.
    if (scanner->cursor > 0 && scanner->cursor < scanner->length) {
        proven_size_t remaining = scanner->length - scanner->cursor;
        proven_sys_mem_move(scanner->buffer, scanner->buffer + scanner->cursor, remaining);
        scanner->length = remaining;
    } else if (scanner->cursor >= scanner->length) {
        scanner->length = 0;
    }
    scanner->cursor = 0;

    if (scanner->length >= scanner->capacity) return PROVEN_ERR_OUT_OF_BOUNDS;

#if defined(_WIN32) || defined(_WIN64)
    proven_sys_io_handle_t handle = { .handle = scanner->file.internal.ptr };
#else
    proven_sys_io_handle_t handle = { .fd = scanner->file.internal.fd };
#endif

    proven_size_t request_size = scanner->capacity - scanner->length;
    proven_sys_result_size_t read_res = proven_sys_io_read_once(handle, (char*)(scanner->buffer + scanner->length), request_size);
    if (!proven_is_ok(read_res.err)) {
        if (read_res.err == PROVEN_ERR_EOF || read_res.value == 0) {
            scanner->eof = true;
            return PROVEN_ERR_EOF;
        }
        return read_res.err;
    }

    scanner->length += read_res.value;
    if (read_bytes_out) *read_bytes_out = read_res.value;
    if (read_res.value < request_size) {
        scanner->eof = true;
        if (read_res.value == 0) {
            return PROVEN_ERR_EOF;
        }
    }
    return PROVEN_OK;
}

typedef union {
    proven_i32 i32;
    proven_u32 u32;
    proven_i64 i64;
    proven_u64 u64;
    short s;
    unsigned short us;
    int i;
    unsigned int ui;
    long l;
    unsigned long ul;
    long long ll;
    unsigned long long ull;
    double f64;
    proven_u8str_view_t str_view;
} proven_sysio_scan_value_t;

typedef struct {
    proven_scan_arg_type_t type;
    const proven_scan_arg_t *user;
    proven_scan_arg_t scratch;
    proven_sysio_scan_value_t value;
} proven_sysio_scan_stage_t;

static void scanner_stage_init(proven_sysio_scan_stage_t *stage, const proven_scan_arg_t *arg) {
    if (!stage || !arg) return;
    stage->type = arg->type;
    stage->user = arg;
    stage->scratch = *arg;
    switch (arg->type) {
        case PROVEN_SCAN_ARG_TYPE_NONE:
            break;
        case PROVEN_SCAN_ARG_TYPE_I32:
            stage->value.i32 = 0;
            stage->scratch.ptr.i32 = arg->ptr.i32 ? &stage->value.i32 : NULL;
            break;
        case PROVEN_SCAN_ARG_TYPE_U32:
            stage->value.u32 = 0;
            stage->scratch.ptr.u32 = arg->ptr.u32 ? &stage->value.u32 : NULL;
            break;
        case PROVEN_SCAN_ARG_TYPE_I64:
            stage->value.i64 = 0;
            stage->scratch.ptr.i64 = arg->ptr.i64 ? &stage->value.i64 : NULL;
            break;
        case PROVEN_SCAN_ARG_TYPE_U64:
            stage->value.u64 = 0;
            stage->scratch.ptr.u64 = arg->ptr.u64 ? &stage->value.u64 : NULL;
            break;
        case PROVEN_SCAN_ARG_TYPE_SHORT:
            stage->value.s = 0;
            stage->scratch.ptr.s = arg->ptr.s ? &stage->value.s : NULL;
            break;
        case PROVEN_SCAN_ARG_TYPE_USHORT:
            stage->value.us = 0;
            stage->scratch.ptr.us = arg->ptr.us ? &stage->value.us : NULL;
            break;
        case PROVEN_SCAN_ARG_TYPE_INT:
            stage->value.i = 0;
            stage->scratch.ptr.i = arg->ptr.i ? &stage->value.i : NULL;
            break;
        case PROVEN_SCAN_ARG_TYPE_UINT:
            stage->value.ui = 0;
            stage->scratch.ptr.ui = arg->ptr.ui ? &stage->value.ui : NULL;
            break;
        case PROVEN_SCAN_ARG_TYPE_LONG:
            stage->value.l = 0;
            stage->scratch.ptr.l = arg->ptr.l ? &stage->value.l : NULL;
            break;
        case PROVEN_SCAN_ARG_TYPE_ULONG:
            stage->value.ul = 0;
            stage->scratch.ptr.ul = arg->ptr.ul ? &stage->value.ul : NULL;
            break;
        case PROVEN_SCAN_ARG_TYPE_LLONG:
            stage->value.ll = 0;
            stage->scratch.ptr.ll = arg->ptr.ll ? &stage->value.ll : NULL;
            break;
        case PROVEN_SCAN_ARG_TYPE_ULLONG:
            stage->value.ull = 0;
            stage->scratch.ptr.ull = arg->ptr.ull ? &stage->value.ull : NULL;
            break;
        case PROVEN_SCAN_ARG_TYPE_F64:
            stage->value.f64 = 0.0;
            stage->scratch.ptr.f64 = arg->ptr.f64 ? &stage->value.f64 : NULL;
            break;
        case PROVEN_SCAN_ARG_TYPE_STR_VIEW:
            stage->value.str_view = (proven_u8str_view_t){0};
            stage->scratch.ptr.str_view = arg->ptr.str_view ? &stage->value.str_view : NULL;
            break;
        default:
            break;
    }
}

static void scanner_stage_commit(const proven_sysio_scan_stage_t *stage) {
    if (!stage || !stage->user) return;
    switch (stage->type) {
        case PROVEN_SCAN_ARG_TYPE_NONE:
            break;
        case PROVEN_SCAN_ARG_TYPE_I32:
            if (stage->user->ptr.i32) *stage->user->ptr.i32 = stage->value.i32;
            break;
        case PROVEN_SCAN_ARG_TYPE_U32:
            if (stage->user->ptr.u32) *stage->user->ptr.u32 = stage->value.u32;
            break;
        case PROVEN_SCAN_ARG_TYPE_I64:
            if (stage->user->ptr.i64) *stage->user->ptr.i64 = stage->value.i64;
            break;
        case PROVEN_SCAN_ARG_TYPE_U64:
            if (stage->user->ptr.u64) *stage->user->ptr.u64 = stage->value.u64;
            break;
        case PROVEN_SCAN_ARG_TYPE_SHORT:
            if (stage->user->ptr.s) *stage->user->ptr.s = stage->value.s;
            break;
        case PROVEN_SCAN_ARG_TYPE_USHORT:
            if (stage->user->ptr.us) *stage->user->ptr.us = stage->value.us;
            break;
        case PROVEN_SCAN_ARG_TYPE_INT:
            if (stage->user->ptr.i) *stage->user->ptr.i = stage->value.i;
            break;
        case PROVEN_SCAN_ARG_TYPE_UINT:
            if (stage->user->ptr.ui) *stage->user->ptr.ui = stage->value.ui;
            break;
        case PROVEN_SCAN_ARG_TYPE_LONG:
            if (stage->user->ptr.l) *stage->user->ptr.l = stage->value.l;
            break;
        case PROVEN_SCAN_ARG_TYPE_ULONG:
            if (stage->user->ptr.ul) *stage->user->ptr.ul = stage->value.ul;
            break;
        case PROVEN_SCAN_ARG_TYPE_LLONG:
            if (stage->user->ptr.ll) *stage->user->ptr.ll = stage->value.ll;
            break;
        case PROVEN_SCAN_ARG_TYPE_ULLONG:
            if (stage->user->ptr.ull) *stage->user->ptr.ull = stage->value.ull;
            break;
        case PROVEN_SCAN_ARG_TYPE_F64:
            if (stage->user->ptr.f64) *stage->user->ptr.f64 = stage->value.f64;
            break;
        case PROVEN_SCAN_ARG_TYPE_STR_VIEW:
            if (stage->user->ptr.str_view) *stage->user->ptr.str_view = stage->value.str_view;
            break;
        default:
            break;
    }
}

static proven_err_t scanner_rewind_file(proven_sysio_scanner_t *scanner, proven_size_t bytes) {
    if (!scanner || bytes == 0) return PROVEN_OK;
#if defined(_WIN32) || defined(_WIN64)
    proven_sys_io_handle_t handle = { .handle = scanner->file.internal.ptr };
#else
    proven_sys_io_handle_t handle = { .fd = scanner->file.internal.fd };
#endif
    return proven_sys_io_seek_relative(handle, -((int64_t)bytes)).err;
}

proven_err_t proven_sysio_scanner_scan_impl(proven_sysio_scanner_t *scanner, const char *fmt, const proven_scan_arg_t *args, size_t args_count) {
    if (!scanner || !scanner->buffer || !fmt) return PROVEN_ERR_INVALID_ARG;
    if (args_count > 0 && !args) return PROVEN_ERR_INVALID_ARG;
    if (args_count == 0) return PROVEN_ERR_INVALID_ARG;

    proven_size_t start_cursor = scanner->cursor;
    proven_size_t start_length = scanner->length;
    bool start_eof = scanner->eof;
    proven_size_t bytes_read_total = 0;

    if (scanner->cursor >= scanner->length && scanner->eof) {
        return PROVEN_ERR_EOF;
    }

    proven_allocator_t alloc = scanner->alloc;
    if (!proven_alloc_is_valid(alloc)) {
        return PROVEN_ERR_INVALID_ARG;
    }

    proven_result_mem_mut_t stage_mem = alloc.alloc_fn(alloc.ctx, args_count * sizeof(proven_sysio_scan_stage_t), alignof(proven_sysio_scan_stage_t));
    if (!proven_is_ok(stage_mem.err) || !stage_mem.value.ptr) {
        return stage_mem.err;
    }

    proven_result_mem_mut_t scratch_mem = alloc.alloc_fn(alloc.ctx, args_count * sizeof(proven_scan_arg_t), alignof(proven_scan_arg_t));
    if (!proven_is_ok(scratch_mem.err) || !scratch_mem.value.ptr) {
        alloc.free_fn(alloc.ctx, stage_mem.value.ptr);
        return scratch_mem.err;
    }

    proven_sysio_scan_stage_t *stages = (proven_sysio_scan_stage_t *)stage_mem.value.ptr;
    proven_scan_arg_t *scratch_args = (proven_scan_arg_t *)scratch_mem.value.ptr;
    for (size_t i = 0; i < args_count; i++) {
        scanner_stage_init(&stages[i], &args[i]);
        scratch_args[i] = stages[i].scratch;
    }

    proven_err_t err = PROVEN_OK;
    for (;;) {
        if (scanner->cursor >= scanner->length && !scanner->eof) {
            proven_size_t read_bytes = 0;
            proven_err_t fill_err = scanner_fill(scanner, &read_bytes);
            if (!proven_is_ok(fill_err)) {
                err = fill_err;
                break;
            }
            bytes_read_total += read_bytes;
        }

        if (scanner->length == 0 && scanner->eof) {
            err = PROVEN_ERR_EOF;
            break;
        }

        proven_u8str_view_t view = { .ptr = (const proven_byte_t*)scanner->buffer + scanner->cursor, .size = scanner->length - scanner->cursor };
        proven_scan_t scan = proven_scan_init(view);
        err = proven_scan_fmt_internal(&scan, fmt, scratch_args, args_count);

        if (proven_is_ok(err) && !scanner->eof && scan.cursor == scan.view.size) {
            err = PROVEN_ERR_NEED_MORE;
        }

        if (err == PROVEN_ERR_NEED_MORE) {
            proven_size_t read_bytes = 0;
            proven_err_t fill_err = scanner_fill(scanner, &read_bytes);
            if (proven_is_ok(fill_err)) {
                bytes_read_total += read_bytes;
                continue;
            }
            if (fill_err == PROVEN_ERR_EOF && scanner->length > 0) {
                continue;
            }
            err = fill_err;
            break;
        }

        if (!proven_is_ok(err)) {
            break;
        }

        for (size_t i = 0; i < args_count; i++) {
            scanner_stage_commit(&stages[i]);
        }
        scanner->cursor += scan.cursor;
        err = PROVEN_OK;
        break;
    }

    if (!proven_is_ok(err)) {
        if (bytes_read_total > 0) {
            (void)scanner_rewind_file(scanner, bytes_read_total);
        }
        scanner->cursor = start_cursor;
        scanner->length = start_length;
        scanner->eof = start_eof;
    }

    alloc.free_fn(alloc.ctx, scratch_args);
    alloc.free_fn(alloc.ctx, stages);
    return err;
}

proven_err_t proven_sysio_print_impl(proven_file_t file, const char *fmt, const proven_arg_t *args, size_t args_count) {
    // Rely on the ubiquitous thread-safe global allocator to process prints.
    // In heavily constrained systems, you'd substitute this with a stack buffer.
    proven_allocator_t alloc = proven_heap_allocator();
    proven_u8str_t str = {0};
    
    proven_fmt_result_t fmt_res = proven_u8str_fmt_internal(alloc, &str, false, fmt, (proven_allocator_t){0}, args, args_count);
    proven_err_t err = fmt_res.err;
    if (!proven_is_ok(err)) return err;

    // Send payload straight to kernel OS
#if defined(_WIN32) || defined(_WIN64)
    proven_sys_io_handle_t handle = { .handle = file.internal.ptr };
#else
    proven_sys_io_handle_t handle = { .fd = file.internal.fd };
#endif

    const proven_byte_t *ptr = str.internal.ptr;
    proven_sys_result_size_t w_res = proven_sys_io_write_all(handle, ptr, str.internal.len);
    if (!proven_is_ok(w_res.err)) {
        err = w_res.err;
    }
    
    // Explicitly flush after writing
    proven_sys_io_flush(handle);
    
    // Explicit clean-up since we used heap manually instead of Arena
    if (str.internal.ptr) {
        alloc.free_fn(alloc.ctx, str.internal.ptr);
    }
    
    return err;
}

proven_err_t proven_sysio_scan_chunk_impl(proven_file_t file, const char *fmt, const proven_scan_arg_t *args, size_t args_count) {
    char buf[4096];
#if defined(_WIN32) || defined(_WIN64)
    proven_sys_io_handle_t handle = { .handle = file.internal.ptr };
#else
    proven_sys_io_handle_t handle = { .fd = file.internal.fd };
#endif

    // This helper is intentionally limited to seekable inputs.
    // Probe with a zero-offset seek so pipes/stdin/sockets are rejected before any bytes are consumed.
    proven_sys_result_size_t seek_probe = proven_sys_io_seek_relative(handle, 0);
    if (!proven_is_ok(seek_probe.err)) {
        return PROVEN_ERR_UNSUPPORTED;
    }
    
    // We read up to a fixed chunk size to evaluate locally.
    proven_sys_result_size_t read_res = proven_sys_io_read_once(handle, buf, sizeof(buf));
    if (read_res.err == PROVEN_ERR_EOF || read_res.value == 0) {
        return PROVEN_ERR_NOT_FOUND;
    }
    if (!proven_is_ok(read_res.err)) {
        return read_res.err;
    }
    bool buffer_filled = (read_res.value == sizeof(buf));

    proven_u8str_view_t view = { .ptr = (const proven_byte_t*)buf, .size = read_res.value };
    proven_scan_t scan = proven_scan_init(view);
    proven_err_t err = proven_scan_fmt_internal(&scan, fmt, args, args_count);

    if (!proven_is_ok(err)) {
        int64_t rewind_offset = -((int64_t)read_res.value);
        proven_sys_result_size_t seek_res = proven_sys_io_seek_relative(handle, rewind_offset);
        if (!proven_is_ok(seek_res.err)) {
            return seek_res.err;
        }
        return err;
    }

    if (buffer_filled && scan.cursor == read_res.value) {
        char probe;
        proven_sys_result_size_t probe_res = proven_sys_io_read_once(handle, &probe, 1);
        if (!proven_is_ok(probe_res.err) && probe_res.err != PROVEN_ERR_EOF) {
            int64_t rewind_offset = -((int64_t)read_res.value);
            proven_sys_result_size_t seek_res = proven_sys_io_seek_relative(handle, rewind_offset);
            if (!proven_is_ok(seek_res.err)) {
                return seek_res.err;
            }
            return probe_res.err;
        }
        if (probe_res.err != PROVEN_ERR_EOF) {
            int64_t rewind_offset = -1;
            proven_sys_result_size_t seek_res = proven_sys_io_seek_relative(handle, rewind_offset);
            if (!proven_is_ok(seek_res.err)) {
                return seek_res.err;
            }
            rewind_offset = -((int64_t)read_res.value);
            seek_res = proven_sys_io_seek_relative(handle, rewind_offset);
            if (!proven_is_ok(seek_res.err)) {
                return seek_res.err;
            }
            return PROVEN_ERR_OUT_OF_BOUNDS;
        }
    }

    // Rewind any unconsumed bytes to prevent data loss (evaporation)
    if (scan.cursor < read_res.value) {
        int64_t rewind_offset = -((int64_t)(read_res.value - scan.cursor));
        proven_sys_result_size_t seek_res = proven_sys_io_seek_relative(handle, rewind_offset);
        if (!proven_is_ok(seek_res.err)) {
            err = seek_res.err;
        }
    }

    return err;
}

proven_result_u8str_t proven_env_get(proven_allocator_t alloc, proven_u8str_view_t key) {
    if (key.size > 0 && !key.ptr) {
        return (proven_result_u8str_t){ .err = PROVEN_ERR_INVALID_ARG };
    }
    // Reject interior NUL for safety
    for (size_t i = 0; i < key.size; ++i) {
        if (key.ptr[i] == 0) return (proven_result_u8str_t){ .err = PROVEN_ERR_INVALID_ARG };
    }

    proven_size_t key_cap;
    if (PROVEN_CKD_ADD(&key_cap, key.size, 1u)) {
        return (proven_result_u8str_t){ .err = PROVEN_ERR_OVERFLOW };
    }

    char key_stack[256];
    char *key_cstr = key_stack;
    bool key_is_allocated = false;
    if (key_cap > sizeof(key_stack)) {
        if (!proven_alloc_is_valid(alloc)) {
            return (proven_result_u8str_t){ .err = PROVEN_ERR_INVALID_ARG };
        }
        proven_result_mem_mut_t key_mem = alloc.alloc_fn(alloc.ctx, key_cap, 1);
        if (!proven_is_ok(key_mem.err)) {
            return (proven_result_u8str_t){ .err = key_mem.err };
        }
        key_cstr = (char*)key_mem.value.ptr;
        key_is_allocated = true;
    }
    
    // Ensure null-terminated compatibility for PAL Layer
    for (size_t i = 0; i < key.size; ++i) {
        key_cstr[i] = (char)key.ptr[i];
    }
    key_cstr[key.size] = '\0';

    char val_buf[4096];
    size_t val_len = 0;
    proven_result_u8str_t result = {0};
    
    proven_err_t pal_err = proven_sys_env_get(key_cstr, val_buf, sizeof(val_buf), &val_len);
    
    if (pal_err == PROVEN_OK) {
        result = proven_u8str_create(alloc, val_len);
        if (!proven_is_ok(result.err)) goto cleanup_key;

        proven_err_t app_err = proven_u8str_append_grow(alloc, &result.value, (proven_u8str_view_t){.ptr = (const proven_byte_t*)val_buf, .size = val_len});
        if (!proven_is_ok(app_err)) {
            proven_u8str_destroy(alloc, &result.value);
            result = (proven_result_u8str_t){ .err = app_err };
        }
    } else if (pal_err == PROVEN_ERR_OUT_OF_BOUNDS) {
        // Large environment variable, dynamic allocation needed
        proven_size_t val_cap;
        if (PROVEN_CKD_ADD(&val_cap, val_len, 1u)) {
            result = (proven_result_u8str_t){ .err = PROVEN_ERR_OVERFLOW };
            goto cleanup_key;
        }
        if (!proven_alloc_is_valid(alloc)) {
            result = (proven_result_u8str_t){ .err = PROVEN_ERR_INVALID_ARG };
            goto cleanup_key;
        }
        proven_result_mem_mut_t alloc_res = alloc.alloc_fn(alloc.ctx, val_cap, 1);
        if (!proven_is_ok(alloc_res.err)) {
            result = (proven_result_u8str_t){ .err = alloc_res.err };
            goto cleanup_key;
        }
        
        char *big_buf = (char*)alloc_res.value.ptr;
        pal_err = proven_sys_env_get(key_cstr, big_buf, val_cap, &val_len);
        
        if (pal_err != PROVEN_OK) {
            alloc.free_fn(alloc.ctx, big_buf);
            result = (proven_result_u8str_t){ .err = pal_err };
            goto cleanup_key;
        }
        
        result = proven_u8str_create(alloc, val_len);
        if (!proven_is_ok(result.err)) {
            alloc.free_fn(alloc.ctx, big_buf);
            goto cleanup_key;
        }
        
        proven_err_t app_err = proven_u8str_append_grow(alloc, &result.value, (proven_u8str_view_t){.ptr = (const proven_byte_t*)big_buf, .size = val_len});
        alloc.free_fn(alloc.ctx, big_buf);
        
        if (!proven_is_ok(app_err)) {
            proven_u8str_destroy(alloc, &result.value);
            result = (proven_result_u8str_t){ .err = app_err };
        }
    } else {
        result = (proven_result_u8str_t){ .err = pal_err };
    }

cleanup_key:
    if (key_is_allocated) {
        alloc.free_fn(alloc.ctx, key_cstr);
    }
    return result;
}
