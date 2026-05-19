#include "proven_test.h"
#include "proven/fs.h"
#include "proven/sysio.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    PROVEN_TEST_SUITE(
        "test_sysio_scanner_boundary",
        "Verify buffered sysio scanning rejects tokens that run off the end of the current buffer and keeps the file handle reusable.",
        "If this fails, inspect proven_sysio_scanner_scan_impl and make sure it does not accept a token that only fits by truncation at the chunk boundary."
    );

    proven_allocator_t alloc = proven_heap_allocator();
    const char *path = "test_sysio_scanner_boundary.tmp";
    const char *payload = "1234567890";

    proven_result_file_t opened = proven_fs_open(alloc, (proven_u8str_view_t){ .ptr = (const proven_byte_t*)path, .size = (proven_size_t)strlen(path) }, PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(opened.err), "open boundary test file", "Check file creation and write flags.");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_fs_write_all(opened.value, (proven_mem_view_t){ .ptr = (const proven_byte_t*)payload, .size = (proven_size_t)strlen(payload) })), "write boundary payload", "Check temp file write support.");
    proven_fs_close(opened.value);

    opened = proven_fs_open(alloc, (proven_u8str_view_t){ .ptr = (const proven_byte_t*)path, .size = (proven_size_t)strlen(path) }, PROVEN_FS_READ);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(opened.err), "reopen boundary test file", "Check file read access.");

    proven_sysio_scanner_t scanner;
    proven_err_t init_err = proven_sysio_scanner_init(&scanner, opened.value, alloc, 4u);
    PROVEN_TEST_ASSERT(init_err == PROVEN_OK, "init small buffered scanner", "Check allocator availability.");

    proven_u64 number = 0u;
    proven_err_t scan_err = proven_sysio_scanner_scan(&scanner, "{}", PROVEN_SCAN_ARG(&number));
    PROVEN_TEST_ASSERT(scan_err == PROVEN_ERR_OUT_OF_BOUNDS, "boundary-crossing token reports out-of-bounds", "Inspect the buffered scanner boundary policy and stop accepting tokens that only fit by truncation.");
    PROVEN_TEST_ASSERT(number == 0u, "failed boundary scan leaves destination untouched", "Check scanner staging and failure-atomic output commit.");
    PROVEN_TEST_ASSERT(scanner.cursor == 0u, "failed boundary scan leaves scanner cursor at the starting position", "Inspect cursor commit and restore logic in proven_sysio_scanner_scan_impl.");
    PROVEN_TEST_ASSERT(scanner.length == 0u, "failed boundary scan clears the temporary buffered chunk", "Inspect scan rollback and file rewind behavior.");

    char peek[5] = {0};
    proven_result_size_t peek_res = proven_fs_read(opened.value, (proven_mem_mut_t){ .ptr = (proven_byte_t*)peek, .size = 4u });
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(peek_res.err), "failed scan leaves the file handle reusable", "Inspect scan rollback and rewind behavior.");
    PROVEN_TEST_ASSERT(peek_res.value == 4u, "reusable file handle exposes the original prefix again", "Inspect the file rewind path after boundary rejection.");
    PROVEN_TEST_ASSERT(memcmp(peek, "1234", 4u) == 0, "reusable file handle still starts from the beginning", "Inspect the scan rollback and file rewind path.");

    proven_sysio_scanner_deinit(&scanner);
    proven_fs_close(opened.value);
    (void)remove(path);

    PROVEN_TEST_PASS("Buffered sysio scanner boundary regression checks passed.");
    return 0;
}
