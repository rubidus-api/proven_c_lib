#include "proven.h"
#include "proven_test.h"
#include "proven/sysio.h"

#include <string.h>


int main() {
    PROVEN_TEST_INFO("Running Phase 13 File System Tests...");

    proven_allocator_t heap = proven_heap_allocator();
    
    proven_u8str_view_t path = PROVEN_LIT("test_file.txt");
    proven_u8str_view_t content = PROVEN_LIT("Hello, Proven FS!");

    // 1. Write Test
    PROVEN_TEST_INFO("Testing file writing...");
    proven_result_file_t open_res = proven_fs_open(heap, path, PROVEN_FS_WRITE | PROVEN_FS_CREATE);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(open_res.err), "Testing condition: PROVEN_IS_OK(open_res.err)", "Review logic surrounding PROVEN_IS_OK(open_res.err)");
    proven_file_t file = open_res.value;

    proven_result_size_t write_res = proven_fs_write(file, proven_mem_view_from_u8(content));
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(write_res.err), "Testing condition: PROVEN_IS_OK(write_res.err)", "Review logic surrounding PROVEN_IS_OK(write_res.err)");
    PROVEN_TEST_ASSERT(write_res.value == content.size, "Testing condition: write_res.value == content.size", "Review logic surrounding write_res.value == content.size");

    proven_fs_close(file);

    // 2. Read All Test
    PROVEN_TEST_INFO("Testing reading all file content...");
    proven_result_mem_mut_t read_all_res = proven_fs_read_all(heap, path);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(read_all_res.err), "Testing condition: PROVEN_IS_OK(read_all_res.err)", "Review logic surrounding PROVEN_IS_OK(read_all_res.err)");
    
    proven_mem_mut_t read_mem = read_all_res.value;
    PROVEN_TEST_ASSERT(read_mem.size == content.size, "Testing condition: read_mem.size == content.size", "Review logic surrounding read_mem.size == content.size");
    PROVEN_TEST_ASSERT(memcmp(read_mem.ptr, content.ptr, content.size) == 0, "Testing condition: memcmp(read_mem.ptr, content.ptr, content.size) == 0", "Review logic surrounding memcmp(read_mem.ptr, content.ptr, content.size) == 0");

    // Clean up memory
    heap.free_fn(heap.ctx, read_mem.ptr);

    // 3. File Size Test
    PROVEN_TEST_INFO("Testing file size retrieval...");
    open_res = proven_fs_open(heap, path, PROVEN_FS_READ);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(open_res.err), "Testing condition: PROVEN_IS_OK(open_res.err)", "Review logic surrounding PROVEN_IS_OK(open_res.err)");
    file = open_res.value;
    
    proven_result_size_t size_res = proven_fs_size(file);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(size_res.err), "Testing condition: PROVEN_IS_OK(size_res.err)", "Review logic surrounding PROVEN_IS_OK(size_res.err)");
    PROVEN_TEST_ASSERT(size_res.value == content.size, "Testing condition: size_res.value == content.size", "Review logic surrounding size_res.value == content.size");
    
    proven_fs_close(file);

    // 4. Absolute path classifier should recognize Windows absolute forms too.
    PROVEN_TEST_INFO("Testing absolute path classifier edge cases...");
    PROVEN_TEST_ASSERT(proven_fs_is_absolute(PROVEN_LIT("/usr/bin")),
        "POSIX absolute path is recognized",
        "Check leading slash handling");
    PROVEN_TEST_ASSERT(proven_fs_is_absolute(PROVEN_LIT("C:/Windows")),
        "Windows drive path with slash is recognized",
        "Check drive-root handling");
    PROVEN_TEST_ASSERT(proven_fs_is_absolute(PROVEN_LIT("C:\\Windows")),
        "Windows drive path with backslash is recognized",
        "Check drive-root handling");
    PROVEN_TEST_ASSERT(proven_fs_is_absolute(PROVEN_LIT("\\\\server\\share\\dir")),
        "Windows UNC path is recognized as absolute",
        "Check double-backslash handling");
    PROVEN_TEST_ASSERT(proven_fs_is_absolute(PROVEN_LIT("\\\\?\\C:\\long\\path")),
        "Windows extended drive path is recognized as absolute",
        "Check extended path handling");
    PROVEN_TEST_ASSERT(proven_fs_is_absolute(PROVEN_LIT("\\\\?\\UNC\\server\\share")),
        "Windows extended UNC path is recognized as absolute",
        "Check extended UNC handling");
    PROVEN_TEST_ASSERT(!proven_fs_is_absolute(PROVEN_LIT("relative/path")),
        "Relative POSIX-style path is not absolute",
        "Check relative path handling");
    PROVEN_TEST_ASSERT(!proven_fs_is_absolute(PROVEN_LIT("C:relative")),
        "Windows drive-relative path is not fully absolute",
        "Check drive-relative handling");

    PROVEN_TEST_PASS("All Phase 13 File System Tests Passed Successfully!");
    return 0;
}
