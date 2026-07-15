#include "proven_test.h"
#include "proven/sysio.h"
#include "proven/heap.h"
#include <string.h>

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();
    
    // We can't easily test stdin/pipes here without complex setup,
    // but we can test the scanner logic using a file that we treat as a scanner source.
    
    const char *test_path = "test_unit_sysio_scanner.txt";
    proven_result_file_t res_write = proven_fs_open(alloc, proven_u8str_view_from_cstr(test_path), PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
    PROVEN_TEST_ASSERT(res_write.err == PROVEN_OK, "Failed to open test file for writing", "Check FS permissions");
    proven_file_t f = res_write.value;
    
    const char *content = "123 456 Hello";
    proven_result_size_t writer = proven_fs_write(f, proven_mem_view_from_u8(proven_u8str_view_from_cstr(content)));
    PROVEN_TEST_ASSERT(writer.err == PROVEN_OK, "Failed to write to test file", "Check FS");
    (void)proven_fs_close(f);
    
    // Now open for reading via scanner
    proven_result_file_t res_read = proven_fs_open(alloc, proven_u8str_view_from_cstr(test_path), PROVEN_FS_READ);
    PROVEN_TEST_ASSERT(res_read.err == PROVEN_OK, "Failed to open test file for reading", "Check FS");
    f = res_read.value;
    
    proven_sysio_scanner_t scanner;
    proven_err_t err = proven_sysio_scanner_init(&scanner, f, alloc, 4096);
    PROVEN_TEST_ASSERT(err == PROVEN_OK, "Failed to init scanner", "Check memory");
    
    proven_i32 val1 = 0;
    err = proven_sysio_scanner_scan(&scanner, "{}", PROVEN_SCAN_ARG(&val1));
    PROVEN_TEST_ASSERT(err == PROVEN_OK, "Failed to scan first int", "Check scanner");
    PROVEN_TEST_ASSERT(val1 == 123, "Scanned value mismatch", "Check content");
    
    proven_i32 val2 = 0;
    err = proven_sysio_scanner_scan(&scanner, "{}", PROVEN_SCAN_ARG(&val2));
    PROVEN_TEST_ASSERT(err == PROVEN_OK, "Failed to scan second int", "Check scanner");
    PROVEN_TEST_ASSERT(val2 == 456, "Scanned value mismatch", "Check content");
    
    proven_u8str_view_t word = {0};
    err = proven_sysio_scanner_scan(&scanner, "{}", PROVEN_SCAN_ARG(&word));
    PROVEN_TEST_ASSERT(err == PROVEN_OK, "Failed to scan word", "Check scanner");
    PROVEN_TEST_ASSERT(proven_u8str_view_eq(word, proven_u8str_view_from_cstr("Hello")), "Scanned word mismatch", "Check content");
    
    proven_sysio_scanner_deinit(&scanner);
    (void)proven_fs_close(f);
    
    PROVEN_TEST_PASS("SysIO Buffered Scanner Test Passed!");
    return 0;
}
