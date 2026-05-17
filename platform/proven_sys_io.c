#include "proven_sys_io.h"

// Hardware/OS Separation Matrix

#if defined(_WIN32) || defined(_WIN64)
// Windows 11 / Windows OS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__) && (defined(__x86_64__) || defined(__i386__) || defined(__aarch64__))
// Linux ABIs with stable inline raw syscall support in this file.
// ARM32 Linux uses the POSIX fallback below: EABI reserves r7 for the
// syscall number, but Thumb/frame-pointer configurations can make r7
// unavailable to inline asm under modern cross compilers.
#else
// POSIX Fallback: macOS, BSD, ARM32 Linux, etc. -> Use POSIX FDs and unistd.h
#include <unistd.h>
#include <errno.h>
#endif

proven_sys_io_handle_t proven_sys_io_std_in(void) {
#if defined(_WIN32) || defined(_WIN64)
    return (proven_sys_io_handle_t){ .handle = GetStdHandle(STD_INPUT_HANDLE) };
#else
    return (proven_sys_io_handle_t){ .fd = 0 }; 
#endif
}

proven_sys_io_handle_t proven_sys_io_std_out(void) {
#if defined(_WIN32) || defined(_WIN64)
    return (proven_sys_io_handle_t){ .handle = GetStdHandle(STD_OUTPUT_HANDLE) };
#else
    return (proven_sys_io_handle_t){ .fd = 1 };
#endif
}

proven_sys_io_handle_t proven_sys_io_std_err(void) {
#if defined(_WIN32) || defined(_WIN64)
    return (proven_sys_io_handle_t){ .handle = GetStdHandle(STD_ERROR_HANDLE) };
#else
    return (proven_sys_io_handle_t){ .fd = 2 };
#endif
}

proven_sys_result_size_t proven_sys_io_write_once(proven_sys_io_handle_t handle, const void *buf, size_t size) {
#if defined(_WIN32) || defined(_WIN64)
    if (!handle.handle) return (proven_sys_result_size_t){ PROVEN_ERR_INVALID_ARG, 0 };
#else
    if (handle.fd < 0) return (proven_sys_result_size_t){ PROVEN_ERR_INVALID_ARG, 0 };
#endif
    if (size == 0) return (proven_sys_result_size_t){ PROVEN_OK, 0 };
    if (!buf) return (proven_sys_result_size_t){ PROVEN_ERR_INVALID_ARG, 0 };

#if defined(_WIN32) || defined(_WIN64)
    DWORD chunk = size > 0x7FFFFFFF ? 0x7FFFFFFF : (DWORD)size;
    DWORD written = 0;
    if (!WriteFile((HANDLE)handle.handle, buf, chunk, &written, NULL)) {
        return (proven_sys_result_size_t){ PROVEN_ERR_IO, 0 };
    }
    if (written == 0) return (proven_sys_result_size_t){ PROVEN_ERR_IO, 0 };
    return (proven_sys_result_size_t){ PROVEN_OK, (size_t)written };
#elif defined(__linux__) && defined(__x86_64__)
    long ret;
    long real_fd = (long)handle.fd;
    size_t chunk = size > 0x7ffff000u ? 0x7ffff000u : size;
    do {
        __asm__ volatile (
            "syscall"
            : "=a" (ret)
            : "a" (1), "D" (real_fd), "S" (buf), "d" (chunk)
            : "rcx", "r11", "memory"
        );
    } while (ret == -4); // -EINTR
    if (ret < 0) return (proven_sys_result_size_t){ PROVEN_ERR_IO, 0 };
    if (ret == 0) return (proven_sys_result_size_t){ PROVEN_ERR_IO, 0 };
    return (proven_sys_result_size_t){ PROVEN_OK, (size_t)ret };
#elif defined(__linux__) && defined(__i386__)
    long ret;
    long real_fd = (long)handle.fd;
    size_t chunk = size > 0x7ffff000u ? 0x7ffff000u : size;
    do {
        __asm__ volatile (
            "int $0x80"
            : "=a" (ret)
            : "a" (4), "b" (real_fd), "c" (buf), "d" (chunk)
            : "memory"
        );
    } while (ret == -4); // -EINTR
    if (ret < 0) return (proven_sys_result_size_t){ PROVEN_ERR_IO, 0 };
    if (ret == 0) return (proven_sys_result_size_t){ PROVEN_ERR_IO, 0 };
    return (proven_sys_result_size_t){ PROVEN_OK, (size_t)ret };
#elif defined(__linux__) && defined(__aarch64__)
    long ret;
    long real_fd = (long)handle.fd;
    size_t chunk = size > 0x7ffff000u ? 0x7ffff000u : size;
    do {
        register long x8 __asm__("x8") = 64; // sys_write
        register long x0 __asm__("x0") = real_fd;
        register const void *x1 __asm__("x1") = buf;
        register size_t x2 __asm__("x2") = chunk;
        __asm__ volatile (
            "svc #0"
            : "+r" (x0)
            : "r" (x8), "r" (x1), "r" (x2)
            : "memory"
        );
        ret = x0;
    } while (ret == -4); // -EINTR
    if (ret < 0) return (proven_sys_result_size_t){ PROVEN_ERR_IO, 0 };
    if (ret == 0) return (proven_sys_result_size_t){ PROVEN_ERR_IO, 0 };
    return (proven_sys_result_size_t){ PROVEN_OK, (size_t)ret };
#elif defined(__linux__) && defined(PROVEN_SYS_IO_ARM_RAW_SYSCALLS) && (defined(__arm__) || defined(__thumb__))
    long ret;
    long real_fd = (long)handle.fd;
    size_t chunk = size > 0x7ffff000u ? 0x7ffff000u : size;
    do {
        register long r7 __asm__("r7") = 4; // sys_write
        register long r0 __asm__("r0") = real_fd;
        register const void *r1 __asm__("r1") = buf;
        register size_t r2 __asm__("r2") = chunk;
        __asm__ volatile (
            "swi #0"
            : "+r" (r0)
            : "r" (r7), "r" (r1), "r" (r2)
            : "memory"
        );
        ret = r0;
    } while (ret == -4); // -EINTR
    if (ret < 0) return (proven_sys_result_size_t){ PROVEN_ERR_IO, 0 };
    if (ret == 0) return (proven_sys_result_size_t){ PROVEN_ERR_IO, 0 };
    return (proven_sys_result_size_t){ PROVEN_OK, (size_t)ret };
#else
    ssize_t ret;
    do {
        ret = write(handle.fd, buf, size);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0) return (proven_sys_result_size_t){ PROVEN_ERR_IO, 0 };
    if (ret == 0) return (proven_sys_result_size_t){ PROVEN_ERR_IO, 0 };
    return (proven_sys_result_size_t){ PROVEN_OK, (size_t)ret };
#endif
}

proven_sys_result_size_t proven_sys_io_write_all(proven_sys_io_handle_t handle, const void *buf, size_t size) {
    if (size == 0) return (proven_sys_result_size_t){ PROVEN_OK, 0 };
    if (!buf) return (proven_sys_result_size_t){ PROVEN_ERR_INVALID_ARG, 0 };

    size_t total_written = 0;
    while (total_written < size) {
        proven_sys_result_size_t res = proven_sys_io_write_once(handle, (const unsigned char*)buf + total_written, size - total_written);
        if (res.err != PROVEN_OK) {
            return res; // Forward error
        }
        total_written += res.value;
    }
    return (proven_sys_result_size_t){ PROVEN_OK, total_written };
}

proven_sys_result_size_t proven_sys_io_read_once(proven_sys_io_handle_t handle, void *buf, size_t size) {
#if defined(_WIN32) || defined(_WIN64)
    if (!handle.handle) return (proven_sys_result_size_t){PROVEN_ERR_INVALID_ARG, 0};
#else
    if (handle.fd < 0) return (proven_sys_result_size_t){PROVEN_ERR_INVALID_ARG, 0};
#endif
    if (size == 0) return (proven_sys_result_size_t){PROVEN_OK, 0};
    if (!buf) return (proven_sys_result_size_t){PROVEN_ERR_INVALID_ARG, 0};

#if defined(_WIN32) || defined(_WIN64)
    DWORD chunk = size > 0x7FFFFFFF ? 0x7FFFFFFF : (DWORD)size;
    DWORD read_bytes = 0;
    if (!ReadFile((HANDLE)handle.handle, (unsigned char*)buf, chunk, &read_bytes, NULL)) {
        return (proven_sys_result_size_t){ PROVEN_ERR_IO, 0 };
    }
    if (read_bytes == 0) return (proven_sys_result_size_t){ PROVEN_ERR_EOF, 0 };
    return (proven_sys_result_size_t){ PROVEN_OK, (size_t)read_bytes };
#elif defined(__linux__) && defined(__x86_64__)
    long ret;
    long real_fd = (long)handle.fd;
    size_t chunk = size > 0x7ffff000u ? 0x7ffff000u : size;
    do {
        __asm__ volatile (
            "syscall"
            : "=a" (ret)
            : "a" (0), "D" (real_fd), "S" ((unsigned char*)buf), "d" (chunk)
            : "rcx", "r11", "memory"
        );
    } while (ret == -4); // -EINTR
    if (ret < 0) return (proven_sys_result_size_t){ PROVEN_ERR_IO, 0 };
    if (ret == 0) return (proven_sys_result_size_t){ PROVEN_ERR_EOF, 0 };
    return (proven_sys_result_size_t){ PROVEN_OK, (size_t)ret };
#elif defined(__linux__) && defined(__i386__)
    long ret;
    long real_fd = (long)handle.fd;
    size_t chunk = size > 0x7ffff000u ? 0x7ffff000u : size;
    do {
        __asm__ volatile (
            "int $0x80"
            : "=a" (ret)
            : "a" (3), "b" (real_fd), "c" ((unsigned char*)buf), "d" (chunk)
            : "memory"
        );
    } while (ret == -4); // -EINTR
    if (ret < 0) return (proven_sys_result_size_t){ PROVEN_ERR_IO, 0 };
    if (ret == 0) return (proven_sys_result_size_t){ PROVEN_ERR_EOF, 0 };
    return (proven_sys_result_size_t){ PROVEN_OK, (size_t)ret };
#elif defined(__linux__) && defined(__aarch64__)
    long ret;
    long real_fd = (long)handle.fd;
    size_t chunk = size > 0x7ffff000u ? 0x7ffff000u : size;
    do {
        register long x8 __asm__("x8") = 63; // sys_read
        register long x0 __asm__("x0") = real_fd;
        register void *x1 __asm__("x1") = (unsigned char*)buf;
        register size_t x2 __asm__("x2") = chunk;
        __asm__ volatile (
            "svc #0"
            : "+r" (x0)
            : "r" (x8), "r" (x1), "r" (x2)
            : "memory"
        );
        ret = x0;
    } while (ret == -4); // -EINTR
    if (ret < 0) return (proven_sys_result_size_t){ PROVEN_ERR_IO, 0 };
    if (ret == 0) return (proven_sys_result_size_t){ PROVEN_ERR_EOF, 0 };
    return (proven_sys_result_size_t){ PROVEN_OK, (size_t)ret };
#elif defined(__linux__) && defined(PROVEN_SYS_IO_ARM_RAW_SYSCALLS) && (defined(__arm__) || defined(__thumb__))
    long ret;
    long real_fd = (long)handle.fd;
    size_t chunk = size > 0x7ffff000u ? 0x7ffff000u : size;
    do {
        register long r7 __asm__("r7") = 3; // sys_read
        register long r0 __asm__("r0") = real_fd;
        register void *r1 __asm__("r1") = (unsigned char*)buf;
        register size_t r2 __asm__("r2") = chunk;
        __asm__ volatile (
            "swi #0"
            : "+r" (r0)
            : "r" (r7), "r" (r1), "r" (r2)
            : "memory"
        );
        ret = r0;
    } while (ret == -4); // -EINTR
    if (ret < 0) return (proven_sys_result_size_t){ PROVEN_ERR_IO, 0 };
    if (ret == 0) return (proven_sys_result_size_t){ PROVEN_ERR_EOF, 0 };
    return (proven_sys_result_size_t){ PROVEN_OK, (size_t)ret };
#else
    ssize_t ret;
    do {
        ret = read(handle.fd, (unsigned char*)buf, size);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0) return (proven_sys_result_size_t){ PROVEN_ERR_IO, 0 };
    if (ret == 0) return (proven_sys_result_size_t){ PROVEN_ERR_EOF, 0 };
    return (proven_sys_result_size_t){ PROVEN_OK, (size_t)ret };
#endif
}

proven_sys_result_size_t proven_sys_io_read_all(proven_sys_io_handle_t handle, void *buf, size_t size) {
    if (size == 0) return (proven_sys_result_size_t){ PROVEN_OK, 0 };
    if (!buf) return (proven_sys_result_size_t){ PROVEN_ERR_INVALID_ARG, 0 };

    size_t total_read = 0;
    while (total_read < size) {
        proven_sys_result_size_t res = proven_sys_io_read_once(handle, (unsigned char*)buf + total_read, size - total_read);
        if (res.err == PROVEN_ERR_EOF) {
            // Reached EOF before fulfilling the full request
            return (proven_sys_result_size_t){ PROVEN_ERR_EOF, total_read };
        }
        if (res.err != PROVEN_OK) {
            return res;
        }
        total_read += res.value;
    }
    return (proven_sys_result_size_t){ PROVEN_OK, total_read };
}

void proven_sys_io_flush(proven_sys_io_handle_t handle) {
#if defined(_WIN32) || defined(_WIN64)
    if (!handle.handle) return;
    FlushFileBuffers((HANDLE)handle.handle);
#elif defined(__linux__) && (defined(__x86_64__) || defined(__i386__) || defined(__aarch64__))
    // Raw syscalls are inherently unbuffered in userspace, written directly to the OS kernel.
    // Explicit sync/fsync requires fs-level syscalls which aren't strictly necessary for stdio.
    (void)handle;
#else
    // POSIX fds (write()) are unbuffered at this level.
    (void)handle;
#endif
}

proven_sys_result_size_t proven_sys_io_seek_relative(proven_sys_io_handle_t handle, int64_t offset) {
#if defined(_WIN32) || defined(_WIN64)
    if (!handle.handle) return (proven_sys_result_size_t){ PROVEN_ERR_INVALID_ARG, 0 };
    LARGE_INTEGER liDistanceToMove;
    liDistanceToMove.QuadPart = offset;
    LARGE_INTEGER liNewFilePointer;
    if (!SetFilePointerEx((HANDLE)handle.handle, liDistanceToMove, &liNewFilePointer, FILE_CURRENT)) {
        return (proven_sys_result_size_t){ PROVEN_ERR_IO, 0 };
    }
    return (proven_sys_result_size_t){ PROVEN_OK, (size_t)liNewFilePointer.QuadPart };
#elif defined(__linux__) && defined(__x86_64__)
    long ret;
    long real_fd = (long)handle.fd;
    __asm__ volatile (
        "syscall"
        : "=a" (ret)
        : "a" (8), "D" (real_fd), "S" (offset), "d" (1) // 8=sys_lseek, 1=SEEK_CUR
        : "rcx", "r11", "memory"
    );
    if (ret < 0) return (proven_sys_result_size_t){ PROVEN_ERR_IO, 0 };
    return (proven_sys_result_size_t){ PROVEN_OK, (size_t)ret };
#elif defined(__linux__) && defined(__i386__)
    // For i386 sys_lseek is 19. But sys_llseek is _llseek (140) to support 64-bit offsets.
    long ret;
    long real_fd = (long)handle.fd;
    uint64_t result_off = 0;
    __asm__ volatile (
        "int $0x80"
        : "=a" (ret)
        : "a" (140), "b" (real_fd), "c" ((unsigned long)((unsigned long long)offset >> 32)), "d" ((unsigned long)(offset & 0xFFFFFFFF)), "S" (&result_off), "D" (1) // 1=SEEK_CUR
        : "memory"
    );
    if (ret < 0) return (proven_sys_result_size_t){ PROVEN_ERR_IO, 0 };
    return (proven_sys_result_size_t){ PROVEN_OK, (size_t)result_off };
#elif defined(__linux__) && defined(__aarch64__)
    register long x8 __asm__("x8") = 62; // sys_lseek
    register long x0 __asm__("x0") = (long)handle.fd;
    register long x1 __asm__("x1") = (long)offset;
    register long x2 __asm__("x2") = 1; // 1=SEEK_CUR
    __asm__ volatile (
        "svc #0"
        : "=r" (x0)
        : "r" (x8), "r" (x0), "r" (x1), "r" (x2)
        : "memory"
    );
    if (x0 < 0) return (proven_sys_result_size_t){ PROVEN_ERR_IO, 0 };
    return (proven_sys_result_size_t){ PROVEN_OK, (size_t)x0 };
#elif defined(__linux__) && defined(PROVEN_SYS_IO_ARM_RAW_SYSCALLS) && (defined(__arm__) || defined(__thumb__))
    register long r7 __asm__("r7") = 140; // sys_llseek (often used for 64-bit seek on 32-bit ARM)
    register long r0 __asm__("r0") = (long)handle.fd;
    long long off64 = offset;
    register long r1 __asm__("r1") = (long)(off64 >> 32);
    register long r2 __asm__("r2") = (long)(off64 & 0xFFFFFFFF);
    uint64_t result_off = 0;
    register void *r3 __asm__("r3") = &result_off;
    register long r4 __asm__("r4") = 1; // 1=SEEK_CUR
    __asm__ volatile (
        "swi #0"
        : "=r" (r0)
        : "r" (r7), "r" (r0), "r" (r1), "r" (r2), "r" (r3), "r" (r4)
        : "memory"
    );
    if (r0 < 0) {
        // Fallback to sys_lseek (19) if llseek isn't wrapping correctly
        register long r7_fb __asm__("r7") = 19; 
        register long r0_fb __asm__("r0") = (long)handle.fd;
        register long r1_fb __asm__("r1") = (long)offset;
        register long r2_fb __asm__("r2") = 1;
        __asm__ volatile (
            "swi #0"
            : "=r" (r0_fb)
            : "r" (r7_fb), "r" (r0_fb), "r" (r1_fb), "r" (r2_fb)
            : "memory"
        );
        if (r0_fb < 0) return (proven_sys_result_size_t){ PROVEN_ERR_IO, 0 };
        return (proven_sys_result_size_t){ PROVEN_OK, (size_t)r0_fb };
    }
    return (proven_sys_result_size_t){ PROVEN_OK, (size_t)result_off };
#else
    off_t ret = lseek(handle.fd, (off_t)offset, SEEK_CUR);
    if (ret == (off_t)-1) return (proven_sys_result_size_t){ PROVEN_ERR_IO, 0 };
    return (proven_sys_result_size_t){ PROVEN_OK, (size_t)ret };
#endif
}
