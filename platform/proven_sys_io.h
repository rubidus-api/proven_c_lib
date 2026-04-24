#ifndef PROVEN_PLATFORM_SYS_IO_H
#define PROVEN_PLATFORM_SYS_IO_H

#include "proven/types.h"
#include <stddef.h>

/**
 * @file proven_sys_io.h
 * @brief Platform Abstraction Layer specifically for Standard Console I/O.
 *        This eliminates the need for <stdio.h> via raw syscalls where possible,
 *        adhering strictly to isolated OS/Hardware constraints.
 */

typedef struct {
    void *internal;
} proven_sys_io_handle_t;

[[nodiscard]] proven_sys_io_handle_t proven_sys_io_std_in(void);
[[nodiscard]] proven_sys_io_handle_t proven_sys_io_std_out(void);
[[nodiscard]] proven_sys_io_handle_t proven_sys_io_std_err(void);

[[nodiscard]]
size_t proven_sys_io_write(proven_sys_io_handle_t handle, const void *buf, size_t size);

[[nodiscard]]
size_t proven_sys_io_read(proven_sys_io_handle_t handle, void *buf, size_t size);

void proven_sys_io_flush(proven_sys_io_handle_t handle);

#endif /* PROVEN_PLATFORM_SYS_IO_H */
