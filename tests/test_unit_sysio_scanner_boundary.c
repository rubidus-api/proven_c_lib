#include "proven_test.h"
#include "proven/fs.h"
#include "proven/sysio.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    PROVEN_TEST_SUITE(
        "test_unit_sysio_scanner_boundary",
        "Verify buffered sysio scanning can resume across a chunk boundary without accepting a truncated token.",
        "If this fails, inspect proven_sysio_scanner_scan_impl and the stream-fragment path in src/proven/scan.c. The scanner should refill and retry rather than stopping at the first fragment edge."
    );

    proven_allocator_t alloc = proven_heap_allocator();
    const char *path = "test_unit_sysio_scanner_boundary.tmp";
    const char *payload = "12 345";

    proven_result_file_t opened = proven_fs_open(alloc, (proven_u8str_view_t){ .ptr = (const proven_byte_t*)path, .size = (proven_size_t)strlen(path) }, PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(opened.err), "open boundary test file", "Check file creation and write flags.");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_fs_write_all(opened.value, (proven_mem_view_t){ .ptr = (const proven_byte_t*)payload, .size = (proven_size_t)strlen(payload) })), "write boundary payload", "Check temp file write support.");
    (void)proven_fs_close(opened.value);

    opened = proven_fs_open(alloc, (proven_u8str_view_t){ .ptr = (const proven_byte_t*)path, .size = (proven_size_t)strlen(path) }, PROVEN_FS_READ);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(opened.err), "reopen boundary test file", "Check file read access.");

    proven_sysio_scanner_t scanner;
    proven_err_t init_err = proven_sysio_scanner_init(&scanner, opened.value, alloc, 5u);
    PROVEN_TEST_ASSERT(init_err == PROVEN_OK, "init small buffered scanner", "Check allocator availability.");

    proven_u64 first = 0u;
    proven_err_t first_err = proven_sysio_scanner_scan(&scanner, "{}", PROVEN_SCAN_ARG(&first));
    PROVEN_TEST_ASSERT(first_err == PROVEN_OK, "first token should parse from the initial fragment", "Inspect the sysio scanner refill path if the first token fails before the boundary is reached.");
    PROVEN_TEST_ASSERT(first == 12u, "first token value mismatch", "Inspect the integer scan path if the first token is not preserved.");

    proven_u64 second = 0u;
    proven_err_t second_err = proven_sysio_scanner_scan(&scanner, "{}", PROVEN_SCAN_ARG(&second));
    PROVEN_TEST_ASSERT(second_err == PROVEN_OK, "boundary-crossing token should be completed by a refill", "Inspect the buffered scanner refill loop and the stream-fragment scan mode.");
    PROVEN_TEST_ASSERT(second == 345u, "boundary-crossing token value mismatch", "Inspect the refill loop and the integer scan path if the second token is truncated.");

    proven_u64 trailing = 0u;
    proven_err_t trailing_err = proven_sysio_scanner_scan(&scanner, "{}", PROVEN_SCAN_ARG(&trailing));
    PROVEN_TEST_ASSERT(trailing_err == PROVEN_ERR_EOF, "scanner should report EOF after the final token", "Inspect cursor and EOF handling if extra data appears after the last token.");
    PROVEN_TEST_ASSERT(trailing == 0u, "EOF should not write a new value", "Inspect failure-atomic output staging after the final token.");

    proven_sysio_scanner_deinit(&scanner);
    (void)proven_fs_close(opened.value);
    (void)remove(path);

    PROVEN_TEST_PASS("Buffered sysio scanner boundary regression checks passed.");
    return 0;
}
