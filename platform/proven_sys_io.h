#ifndef PROVEN_PLATFORM_SYS_IO_H
#define PROVEN_PLATFORM_SYS_IO_H

#include "proven/types.h"
#include "proven/error.h"
#include <stddef.h>

/**
 * @file proven_sys_io.h
 * @brief Platform Abstraction Layer specifically for Standard Console I/O.
 *        This eliminates the need for <stdio.h> via raw syscalls where possible,
 *        adhering strictly to isolated OS/Hardware constraints.
 */

typedef struct {
    union {
        void *handle;
        int fd;
    };
} proven_sys_io_handle_t;

[[nodiscard]] proven_sys_io_handle_t proven_sys_io_std_in(void);
[[nodiscard]] proven_sys_io_handle_t proven_sys_io_std_out(void);
[[nodiscard]] proven_sys_io_handle_t proven_sys_io_std_err(void);

typedef struct {
    proven_err_t err;
    proven_size_t value;
} proven_sys_result_size_t;

[[nodiscard]]
proven_sys_result_size_t proven_sys_io_write_once(proven_sys_io_handle_t handle, const void *buf, size_t size);

[[nodiscard]]
proven_sys_result_size_t proven_sys_io_write_all(proven_sys_io_handle_t handle, const void *buf, size_t size);

[[nodiscard]]
proven_sys_result_size_t proven_sys_io_read_once(proven_sys_io_handle_t handle, void *buf, size_t size);

[[nodiscard]]
proven_sys_result_size_t proven_sys_io_read_all(proven_sys_io_handle_t handle, void *buf, size_t size);

void proven_sys_io_flush(proven_sys_io_handle_t handle);

[[nodiscard]]
proven_sys_result_size_t proven_sys_io_seek_relative(proven_sys_io_handle_t handle, int64_t offset);

#endif /* PROVEN_PLATFORM_SYS_IO_H */
