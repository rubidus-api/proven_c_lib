#include "proven.h"
#include "proven_test.h"
#include "proven/sysio.h"

#include <string.h>


int main() {
    PROVEN_TEST_INFO("Running Phase 13 File System Tests...");

    proven_allocator_t heap = proven_heap_allocator();
    
    proven_u8str_view_t path = PROVEN_LIT("test_file.txt");
    proven_u8str_view_t content = PROVEN_LIT("Hello, Proven FS!");

    // ============================================
    // 1. Write Test
    // ============================================
    proven_result_file_t open_res = proven_fs_open(path, PROVEN_FS_WRITE | PROVEN_FS_CREATE);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(open_res.err), "Testing condition: PROVEN_IS_OK(open_res.err)", "Review logic surrounding PROVEN_IS_OK(open_res.err)");
    proven_file_t file = open_res.value;

    proven_result_size_t write_res = proven_fs_write(file, proven_mem_view_from_u8(content));
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(write_res.err), "Testing condition: PROVEN_IS_OK(write_res.err)", "Review logic surrounding PROVEN_IS_OK(write_res.err)");
    PROVEN_TEST_ASSERT(write_res.value == content.size, "Testing condition: write_res.value == content.size", "Review logic surrounding write_res.value == content.size");

    proven_fs_close(file);

    // ============================================
    // 2. Read All Test
    // ============================================
    proven_result_mem_mut_t read_all_res = proven_fs_read_all(heap, path);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(read_all_res.err), "Testing condition: PROVEN_IS_OK(read_all_res.err)", "Review logic surrounding PROVEN_IS_OK(read_all_res.err)");
    
    proven_mem_mut_t read_mem = read_all_res.value;
    PROVEN_TEST_ASSERT(read_mem.size == content.size, "Testing condition: read_mem.size == content.size", "Review logic surrounding read_mem.size == content.size");
    PROVEN_TEST_ASSERT(memcmp(read_mem.ptr, content.ptr, content.size) == 0, "Testing condition: memcmp(read_mem.ptr, content.ptr, content.size) == 0", "Review logic surrounding memcmp(read_mem.ptr, content.ptr, content.size) == 0");

    // Clean up memory
    heap.free_fn(heap.ctx, read_mem.ptr);

    // ============================================
    // 3. File Size Test
    // ============================================
    open_res = proven_fs_open(path, PROVEN_FS_READ);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(open_res.err), "Testing condition: PROVEN_IS_OK(open_res.err)", "Review logic surrounding PROVEN_IS_OK(open_res.err)");
    file = open_res.value;
    
    proven_result_size_t size_res = proven_fs_size(file);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(size_res.err), "Testing condition: PROVEN_IS_OK(size_res.err)", "Review logic surrounding PROVEN_IS_OK(size_res.err)");
    PROVEN_TEST_ASSERT(size_res.value == content.size, "Testing condition: size_res.value == content.size", "Review logic surrounding size_res.value == content.size");
    
    proven_fs_close(file);

    PROVEN_TEST_INFO("All Phase 13 File System Tests Passed Successfully!");
    return 0;
}
