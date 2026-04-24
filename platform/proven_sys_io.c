#include "proven_sys_io.h"

// Hardware/OS Separation Matrix

#if defined(_WIN32) || defined(_WIN64)
// Windows 11 / Windows OS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__) && (defined(__x86_64__) || defined(__i386__) || defined(__aarch64__) || defined(__arm__) || defined(__thumb__))
// Linux with Supported Hardware -> RAW SYSCALLS (No C Standard Library)
#else
// Fallback: Other OS or Unrecognized Hardware -> Use standard C library
#include <stdio.h>
#endif

proven_sys_io_handle_t proven_sys_io_std_in(void) {
#if defined(_WIN32) || defined(_WIN64)
    return (proven_sys_io_handle_t){ .internal = (void*)GetStdHandle(STD_INPUT_HANDLE) };
#elif defined(__linux__) && (defined(__x86_64__) || defined(__i386__) || defined(__aarch64__) || defined(__arm__) || defined(__thumb__))
    return (proven_sys_io_handle_t){ .internal = (void*)(long)0 }; // STDIN_FILENO
#else
    return (proven_sys_io_handle_t){ .internal = (void*)stdin };
#endif
}

proven_sys_io_handle_t proven_sys_io_std_out(void) {
#if defined(_WIN32) || defined(_WIN64)
    return (proven_sys_io_handle_t){ .internal = (void*)GetStdHandle(STD_OUTPUT_HANDLE) };
#elif defined(__linux__) && (defined(__x86_64__) || defined(__i386__) || defined(__aarch64__) || defined(__arm__) || defined(__thumb__))
    return (proven_sys_io_handle_t){ .internal = (void*)(long)1 }; // STDOUT_FILENO
#else
    return (proven_sys_io_handle_t){ .internal = (void*)stdout };
#endif
}

proven_sys_io_handle_t proven_sys_io_std_err(void) {
#if defined(_WIN32) || defined(_WIN64)
    return (proven_sys_io_handle_t){ .internal = (void*)GetStdHandle(STD_ERROR_HANDLE) };
#elif defined(__linux__) && (defined(__x86_64__) || defined(__i386__) || defined(__aarch64__) || defined(__arm__) || defined(__thumb__))
    return (proven_sys_io_handle_t){ .internal = (void*)(long)2 }; // STDERR_FILENO
#else
    return (proven_sys_io_handle_t){ .internal = (void*)stderr };
#endif
}

size_t proven_sys_io_write(proven_sys_io_handle_t handle, const void *buf, size_t size) {
    if (size == 0) return 0;
#if defined(_WIN32) || defined(_WIN64)
    DWORD written = 0;
    if (WriteFile((HANDLE)handle.internal, buf, (DWORD)size, &written, NULL)) {
        return (size_t)written;
    }
    return 0;
#elif defined(__linux__) && defined(__x86_64__)
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a" (ret)
        : "a" (1), "D" ((long)handle.internal), "S" (buf), "d" (size)
        : "rcx", "r11", "memory"
    );
    return ret > 0 ? (size_t)ret : 0;
#elif defined(__linux__) && defined(__i386__)
    long ret;
    __asm__ volatile (
        "int $0x80"
        : "=a" (ret)
        : "a" (4), "b" ((long)handle.internal), "c" (buf), "d" (size)
        : "memory"
    );
    return ret > 0 ? (size_t)ret : 0;
#elif defined(__linux__) && defined(__aarch64__)
    register long x8 __asm__("x8") = 64; // sys_write
    register long x0 __asm__("x0") = (long)handle.internal;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = (long)size;
    __asm__ volatile (
        "svc #0"
        : "=r" (x0)
        : "r" (x8), "r" (x0), "r" (x1), "r" (x2)
        : "memory"
    );
    return x0 > 0 ? (size_t)x0 : 0;
#elif defined(__linux__) && (defined(__arm__) || defined(__thumb__))
    register long r7 __asm__("r7") = 4; // sys_write
    register long r0 __asm__("r0") = (long)handle.internal;
    register long r1 __asm__("r1") = (long)buf;
    register long r2 __asm__("r2") = (long)size;
    __asm__ volatile (
        "svc #0"
        : "=r" (r0)
        : "r" (r7), "r" (r0), "r" (r1), "r" (r2)
        : "memory"
    );
    return r0 > 0 ? (size_t)r0 : 0;
#else
    size_t w = fwrite(buf, 1, size, (FILE*)handle.internal);
    fflush((FILE*)handle.internal);
    return w;
#endif
}

size_t proven_sys_io_read(proven_sys_io_handle_t handle, void *buf, size_t size) {
    if (size == 0) return 0;
#if defined(_WIN32) || defined(_WIN64)
    DWORD read = 0;
    if (ReadFile((HANDLE)handle.internal, buf, (DWORD)size, &read, NULL)) {
        return (size_t)read;
    }
    return 0;
#elif defined(__linux__) && defined(__x86_64__)
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a" (ret)
        : "a" (0), "D" ((long)handle.internal), "S" (buf), "d" (size)
        : "rcx", "r11", "memory"
    );
    return ret > 0 ? (size_t)ret : 0;
#elif defined(__linux__) && defined(__i386__)
    long ret;
    __asm__ volatile (
        "int $0x80"
        : "=a" (ret)
        : "a" (3), "b" ((long)handle.internal), "c" (buf), "d" (size)
        : "memory"
    );
    return ret > 0 ? (size_t)ret : 0;
#elif defined(__linux__) && defined(__aarch64__)
    register long x8 __asm__("x8") = 63; // sys_read
    register long x0 __asm__("x0") = (long)handle.internal;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = (long)size;
    __asm__ volatile (
        "svc #0"
        : "=r" (x0)
        : "r" (x8), "r" (x0), "r" (x1), "r" (x2)
        : "memory"
    );
    return x0 > 0 ? (size_t)x0 : 0;
#elif defined(__linux__) && (defined(__arm__) || defined(__thumb__))
    register long r7 __asm__("r7") = 3; // sys_read
    register long r0 __asm__("r0") = (long)handle.internal;
    register long r1 __asm__("r1") = (long)buf;
    register long r2 __asm__("r2") = (long)size;
    __asm__ volatile (
        "svc #0"
        : "=r" (r0)
        : "r" (r7), "r" (r0), "r" (r1), "r" (r2)
        : "memory"
    );
    return r0 > 0 ? (size_t)r0 : 0;
#else
    return fread(buf, 1, size, (FILE*)handle.internal);
#endif
}

void proven_sys_io_flush(proven_sys_io_handle_t handle) {
#if defined(_WIN32) || defined(_WIN64)
    FlushFileBuffers((HANDLE)handle.internal);
#elif defined(__linux__) && (defined(__x86_64__) || defined(__i386__) || defined(__aarch64__) || defined(__arm__) || defined(__thumb__))
    // Raw syscalls are inherently unbuffered in userspace, written directly to the OS kernel.
    // Explicit sync/fsync requires fs-level syscalls which aren't strictly necessary for stdio.
    (void)handle;
#else
    fflush((FILE*)handle.internal);
#endif
}
