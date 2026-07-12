#ifndef PROVEN_SYSIO_H
#define PROVEN_SYSIO_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/u8str.h"
#include "proven/fmt.h"
#include "proven/fs.h"
#include "proven/scan.h"

/**
 * @file sysio.h
 * @brief Console I/O and Environment variable access cleanly eliminating <stdio.h> needs.
 */

// -----------------------------------------------------------------------------
// Standard Data Streams
// -----------------------------------------------------------------------------

/**
 * @brief Retrieves the standard input handle.
 */
[[nodiscard]] proven_file_t proven_sysio_stdin(void);

/**
 * @brief Retrieves the standard output handle.
 */
[[nodiscard]] proven_file_t proven_sysio_stdout(void);

/**
 * @brief Retrieves the standard error handle.
 */
[[nodiscard]] proven_file_t proven_sysio_stderr(void);

/**
 * @brief Historically "flushes the buffer". There is no buffer.
 *
 * proven writes go straight to the OS - there is no user-space buffering
 * anywhere in this library - so on POSIX this call does nothing at all. On
 * Windows it currently calls FlushFileBuffers, which is a *disk* sync, a far
 * stronger and much slower thing.
 *
 * @note Do not use this. It means nothing on one platform and something
 *       expensive on the other, and it means neither of the things its old
 *       description implied: it is not a buffer flush (there is no buffer) and
 *       it is not a portable durability barrier (it is a no-op on POSIX).
 *       Nothing in the library depends on it.
 * @note This is tracked as B-004 in docs/BACKLOG.md. `flush` acquires an honest
 *       meaning when the buffered writer lands, and durability gets its own
 *       explicit call rather than hiding inside a word that promises less.
 */
void proven_sysio_flush(proven_file_t file);

// -----------------------------------------------------------------------------
// Buffered Scanner for sysio (Safe for pipes/stdin)
// -----------------------------------------------------------------------------

/**
 * @brief Buffered scanner for system I/O, providing safe scanning for both
 * seekable and non-seekable streams (pipes, stdin).
 */
typedef struct {
    proven_file_t file;
    proven_allocator_t alloc;
    proven_u8 *buffer;
    proven_size_t capacity;
    proven_size_t cursor;
    proven_size_t length;
    bool eof;
} proven_sysio_scanner_t;

/**
 * @brief Initializes a buffered sysio scanner.
 *
 * @note Size `buffer_capacity` for the longest TOKEN you expect. Whitespace in front of a
 *       token does not count: it is consumed before the scan proper, so a run of it longer
 *       than the buffer is not a problem. A token that does not fit is
 *       PROVEN_ERR_OUT_OF_BOUNDS, never a truncated value.
 * @note A failed scan restores every byte a scan could still read. It may consume the
 *       whitespace ahead of the token - no scan can address that whitespace anyway, and a
 *       scanner that could not drop it would be wedged forever by a whitespace run longer
 *       than its buffer.
 * @param scanner The scanner object to initialize.
 * @param file The file handle to read from.
 * @param alloc The allocator for the internal buffer. The allocator must satisfy
 *        the full proven_allocator_t contract.
 * @param buffer_capacity The size of the internal buffer (e.g., 4096).
 * @return PROVEN_ERR_INVALID_ARG for a null scanner, an invalid allocator, or a
 *         zero buffer size. On failure, the scanner is left zero-safe.
 */
[[nodiscard]] proven_err_t proven_sysio_scanner_init(proven_sysio_scanner_t *scanner, proven_file_t file, proven_allocator_t alloc, proven_size_t buffer_capacity);

/**
 * @brief Deinitializes the sysio scanner and frees its internal buffer.
 */
void proven_sysio_scanner_deinit(proven_sysio_scanner_t *scanner);

/**
 * @brief Low-level implementation for buffered scanning.
 *
 * The scanner refills and retries when a token reaches the end of the current
 * loaded fragment before EOF. If the source is exhausted, it returns EOF; if a
 * token cannot fit within the fixed buffer after refill, it returns
 * PROVEN_ERR_OUT_OF_BOUNDS and restores the scanner state and file position on
 * seekable inputs instead of accepting a truncated token.
 */
[[nodiscard]]
proven_err_t proven_sysio_scanner_scan_impl(proven_sysio_scanner_t *scanner, const char *fmt, const proven_scan_arg_t *args, size_t args_count);

/**
 * @brief Type-safe formatted scanning using a buffered scanner.
 */
#define proven_sysio_scanner_scan(scanner, fmt, ...) \
    proven_sysio_scanner_scan_impl(scanner, fmt, \
        ((proven_scan_arg_t[]){ proven_scan_arg_none() __VA_OPT__(,) __VA_ARGS__ }), \
        (sizeof((proven_scan_arg_t[]){ proven_scan_arg_none() __VA_OPT__(,) __VA_ARGS__ }) / sizeof(proven_scan_arg_t)))

// -----------------------------------------------------------------------------
// Type-Safe Printing Console macros
// -----------------------------------------------------------------------------

/* Deliberately not [[nodiscard]]: proven_print / proven_eprint expand to this
 * and are used as printf is - a failed write to a console is conventionally
 * ignored. The scan entry points above and below are annotated, because
 * dropping their error means reading data that was never parsed. */
proven_err_t proven_sysio_print_impl(proven_file_t handle, const char *fmt, const proven_arg_t *args, size_t args_count);
/**
 * @brief Type-safe formatted scanning from a file descriptor.
 *
 * Reads at most one fixed-size chunk (4096 bytes). The helper is intended for
 * seekable inputs; if the handle cannot be rewound, it returns
 * PROVEN_ERR_UNSUPPORTED before consuming data. If the chunk fills before a
 * complete token is available, the function returns PROVEN_ERR_OUT_OF_BOUNDS
 * instead of accepting a silently truncated parse, and the file cursor is restored
 * to the start of the chunk.
 */
[[nodiscard]]
proven_err_t proven_sysio_scan_chunk_impl(proven_file_t handle, const char *fmt, const proven_scan_arg_t *args, size_t args_count);

/**
 * @brief Type-safe formatted printing to stdout.
 * Replacing printf(fmt, ...).
 */
#define proven_print(fmt, ...) \
    proven_sysio_print_impl(proven_sysio_stdout(), fmt, \
        ((const proven_arg_t[]){ proven_arg_none() __VA_OPT__(,) __VA_ARGS__ }), \
        (sizeof((proven_arg_t[]){ proven_arg_none() __VA_OPT__(,) __VA_ARGS__ }) / sizeof(proven_arg_t)))

/**
 * @brief Type-safe formatted printing to stdout with a trailing newline.
 * Replacing printf(fmt "\n", ...).
 */
#define proven_println(fmt, ...) \
    proven_print(fmt "\n" __VA_OPT__(,) __VA_ARGS__)

/**
 * @brief Type-safe formatted printing to stderr.
 * Replacing fprintf(stderr, fmt, ...).
 */
#define proven_eprint(fmt, ...) \
    proven_sysio_print_impl(proven_sysio_stderr(), fmt, \
        ((const proven_arg_t[]){ proven_arg_none() __VA_OPT__(,) __VA_ARGS__ }), \
        (sizeof((proven_arg_t[]){ proven_arg_none() __VA_OPT__(,) __VA_ARGS__ }) / sizeof(proven_arg_t)))

/**
 * @brief Type-safe formatted printing to stderr with a trailing newline.
 */
#define proven_eprintln(fmt, ...) \
    proven_eprint(fmt "\n" __VA_OPT__(,) __VA_ARGS__)

// -----------------------------------------------------------------------------
// Type-Safe Scanning Console macros
// -----------------------------------------------------------------------------

/**
 * @brief Type-safe formatting scanner from a file descriptor.
 */
#define proven_scan_fmt_from_file(file, fmt, ...) \
    proven_sysio_scan_chunk_impl(file, fmt, \
        (proven_scan_arg_t[]){ proven_scan_arg_none() __VA_OPT__(,) __VA_ARGS__ }, \
        (sizeof((proven_scan_arg_t[]){ proven_scan_arg_none() __VA_OPT__(,) __VA_ARGS__ }) / sizeof(proven_scan_arg_t)))

/**
 * @brief Type-safe formatting scanner from standard input.
 */
#define proven_scan_fmt_from_stdin(fmt, ...) \
    proven_scan_fmt_from_file(proven_sysio_stdin(), fmt __VA_OPT__(,) __VA_ARGS__)

// -----------------------------------------------------------------------------
// Environment Variables
// -----------------------------------------------------------------------------

/**
 * @brief Fetches an environment variable by name.
 * @param alloc The allocator to securely hold the resulting string.
 * @param key The environment variable name view.
 * @return An error if not found, or a dynamically allocated string containing the value.
 */
[[nodiscard]] proven_result_u8str_t proven_env_get(proven_allocator_t alloc, proven_u8str_view_t key);

#endif /* PROVEN_SYSIO_H */
