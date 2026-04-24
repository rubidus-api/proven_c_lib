#include "proven/sysio.h"
#include "proven/heap.h"
#include "../../platform/proven_sys_io.h"
#include "../../platform/proven_sys_env.h"

proven_file_t proven_sysio_stdin(void)  { return (proven_file_t){.internal = proven_sys_io_std_in().internal}; }
proven_file_t proven_sysio_stdout(void) { return (proven_file_t){.internal = proven_sys_io_std_out().internal}; }
proven_file_t proven_sysio_stderr(void) { return (proven_file_t){.internal = proven_sys_io_std_err().internal}; }

void proven_sysio_flush(proven_file_t file) {
    proven_sys_io_handle_t handle = { .internal = file.internal };
    proven_sys_io_flush(handle);
}

proven_err_t proven_sysio_print_impl(proven_file_t file, const char *fmt, const proven_arg_t *args, size_t args_count) {
    // Rely on the ubiquitous thread-safe global allocator to process prints.
    // In heavily constrained systems, you'd substitute this with a stack buffer.
    proven_allocator_t alloc = proven_heap_allocator();
    proven_u8str_t str = {0};
    
    proven_err_t err = proven_u8str_format_impl(alloc, &str, fmt, args, args_count);
    if (!proven_is_ok(err)) return err;

    // Send payload straight to kernel OS
    proven_sys_io_handle_t handle = { .internal = file.internal };
    (void)proven_sys_io_write(handle, str.internal.ptr, str.internal.len);
    
    // Explicitly flush after writing
    proven_sys_io_flush(handle);
    
    // Explicit clean-up since we used heap manually instead of Arena
    if (str.internal.ptr) {
        alloc.free_fn(alloc.ctx, str.internal.ptr);
    }
    
    return err;
}

proven_err_t proven_sysio_scan_file_impl(proven_file_t file, const char *fmt, const proven_scan_arg_t *args, size_t args_count) {
    char buf[4096];
    proven_sys_io_handle_t handle = { .internal = file.internal };
    
    // We read up to an arbitrary chunk size to evaluate locally.
    size_t bytes_read = proven_sys_io_read(handle, buf, sizeof(buf));
    if (bytes_read == 0) return PROVEN_ERR_NOT_FOUND;
    
    proven_u8str_view_t view = { .ptr = (const proven_byte_t*)buf, .size = bytes_read };
    proven_scan_t scan = proven_scan_init(view);
    return proven_scan_fmt_impl(&scan, fmt, args, args_count);
}

proven_result_u8str_t proven_env_get(proven_allocator_t alloc, proven_u8str_view_t key) {
    char key_buf[256];
    if (key.size >= 256) {
        return (proven_result_u8str_t){ .err = PROVEN_ERR_OUT_OF_BOUNDS, .value = {0} };
    }
    
    // Ensure null-terminated compatibility for PAL Layer
    for (size_t i = 0; i < key.size; ++i) {
        key_buf[i] = (char)key.ptr[i];
    }
    key_buf[key.size] = '\0';

    char val_buf[4096];
    size_t val_len = 0;
    
    if (!proven_sys_env_get(key_buf, val_buf, sizeof(val_buf), &val_len)) {
        return (proven_result_u8str_t){ .err = PROVEN_ERR_NOT_FOUND, .value = {0} };
    }

    proven_result_u8str_t res = proven_u8str_create(alloc, val_len);
    if (!proven_is_ok(res.err)) return res;

    (void)proven_u8str_append_view(alloc, &res.value, (proven_u8str_view_t){.ptr = (proven_u8*)val_buf, .size = val_len});
    return res;
}
