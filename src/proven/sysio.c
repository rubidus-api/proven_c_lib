#include "proven/sysio.h"
#include "proven/heap.h"
#include "../../platform/proven_sys_io.h"
#include "../../platform/proven_sys_env.h"

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
    if (!scanner || !alloc.alloc_fn || buffer_capacity == 0) return PROVEN_ERR_INVALID_ARG;
    
    scanner->file = file;
    scanner->alloc = alloc;
    scanner->capacity = buffer_capacity;
    scanner->cursor = 0;
    scanner->length = 0;
    scanner->eof = false;
    
    proven_result_mem_mut_t alloc_res = alloc.alloc_fn(alloc.ctx, buffer_capacity, 1);
    if (alloc_res.err != PROVEN_OK) return alloc_res.err;
    scanner->buffer = (proven_u8 *)alloc_res.value.ptr;
    
    return PROVEN_OK;
}

void proven_sysio_scanner_deinit(proven_sysio_scanner_t *scanner) {
    if (!scanner) return;
    if (scanner->buffer && scanner->alloc.free_fn) {
        scanner->alloc.free_fn(scanner->alloc.ctx, scanner->buffer);
    }
    scanner->buffer = NULL;
}

static proven_err_t scanner_fill(proven_sysio_scanner_t *scanner) {
    if (!scanner || scanner->eof) return PROVEN_ERR_EOF;
    
    // Move remaining data to start
    if (scanner->cursor > 0 && scanner->cursor < scanner->length) {
        proven_size_t remaining = scanner->length - scanner->cursor;
        for (proven_size_t i = 0; i < remaining; i++) {
            scanner->buffer[i] = scanner->buffer[scanner->cursor + i];
        }
        scanner->length = remaining;
    } else if (scanner->cursor >= scanner->length) {
        scanner->length = 0;
    }
    scanner->cursor = 0;
    
    if (scanner->length >= scanner->capacity) return PROVEN_OK; // full
    
#if defined(_WIN32) || defined(_WIN64)
    proven_sys_io_handle_t handle = { .handle = scanner->file.internal.ptr };
#else
    proven_sys_io_handle_t handle = { .fd = scanner->file.internal.fd };
#endif

    proven_sys_result_size_t read_res = proven_sys_io_read_once(handle, (char*)(scanner->buffer + scanner->length), scanner->capacity - scanner->length);
    if (read_res.err == PROVEN_ERR_EOF || (read_res.err == PROVEN_OK && read_res.value == 0)) {
        scanner->eof = true;
        return PROVEN_ERR_EOF;
    }
    if (!proven_is_ok(read_res.err)) return read_res.err;
    
    scanner->length += read_res.value;
    return PROVEN_OK;
}

proven_err_t proven_sysio_scanner_scan_impl(proven_sysio_scanner_t *scanner, const char *fmt, const proven_scan_arg_t *args, size_t args_count) {
    if (!scanner || !scanner->buffer) return PROVEN_ERR_INVALID_ARG;
    
    // Fill buffer if empty
    if (scanner->cursor >= scanner->length && !scanner->eof) {
        (void)scanner_fill(scanner);
    }
    
    if (scanner->length == 0 && scanner->eof) return PROVEN_ERR_EOF;
    
    proven_u8str_view_t view = { .ptr = (const proven_byte_t*)scanner->buffer + scanner->cursor, .size = scanner->length - scanner->cursor };
    proven_scan_t scan = proven_scan_init(view);
    
    proven_err_t err = proven_scan_fmt_internal(&scan, fmt, args, args_count);
    
    // Update cursor based on what was consumed
    scanner->cursor += scan.cursor;
    
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
    
    // We read up to a fixed chunk size to evaluate locally.
    proven_sys_result_size_t read_res = proven_sys_io_read_once(handle, buf, sizeof(buf));
    if (read_res.err == PROVEN_ERR_EOF || read_res.value == 0) {
        return PROVEN_ERR_NOT_FOUND;
    }
    if (!proven_is_ok(read_res.err)) {
        return read_res.err;
    }

    bool maybe_truncated = (read_res.value == sizeof(buf) && read_res.err == PROVEN_OK);

    proven_u8str_view_t view = { .ptr = (const proven_byte_t*)buf, .size = read_res.value };
    proven_scan_t scan = proven_scan_init(view);
    proven_err_t err = proven_scan_fmt_internal(&scan, fmt, args, args_count);

    if (!proven_is_ok(err)) {
        int64_t rewind_offset = -((int64_t)read_res.value);
        proven_sys_result_size_t seek_res = proven_sys_io_seek_relative(handle, rewind_offset);
        if (!proven_is_ok(seek_res.err)) {
            return seek_res.err;
        }
        if (maybe_truncated && scan.cursor == read_res.value) {
            return PROVEN_ERR_OUT_OF_BOUNDS;
        }
        return err;
    }

    if (maybe_truncated && scan.cursor == read_res.value) {
        int64_t rewind_offset = -((int64_t)read_res.value);
        proven_sys_result_size_t seek_res = proven_sys_io_seek_relative(handle, rewind_offset);
        if (!proven_is_ok(seek_res.err)) {
            return seek_res.err;
        }
        return PROVEN_ERR_OUT_OF_BOUNDS;
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
