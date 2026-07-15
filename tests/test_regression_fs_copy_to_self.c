#include "proven/fs.h"
#include "proven/heap.h"
#include "proven_test.h"
#include <stdbool.h>

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    proven_u8str_view_t path = PROVEN_LIT("test_copy_self.txt");

    // 1. Create a file with content
    PROVEN_TEST_INFO("Creating test file for self-copy regression...");
    proven_result_file_t f_res = proven_fs_open(heap, path, PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
    PROVEN_TEST_ASSERT(proven_is_ok(f_res.err), "failed to open test file", "");
    
    proven_u8str_view_t content = PROVEN_LIT("Hello World");
    proven_err_t w_err = proven_fs_write_all(f_res.value, proven_mem_view_from_u8(content));
    PROVEN_TEST_ASSERT(proven_is_ok(w_err), "failed to write to test file", "");
    (void)proven_fs_close(f_res.value);

    // 2. Try to copy to itself
    PROVEN_TEST_INFO("Attempting copy to itself (should fail)...");
    proven_err_t c_err = proven_fs_copy(heap, path, path);
    PROVEN_TEST_ASSERT(!proven_is_ok(c_err), "copy to self should fail or at least not truncate", "");
    PROVEN_TEST_ASSERT(c_err == PROVEN_ERR_INVALID_ARG, "Expected PROVEN_ERR_INVALID_ARG on self-copy", "");

    // 3. Verify content is still there
    PROVEN_TEST_INFO("Verifying content remains after failed self-copy...");
    proven_result_mem_mut_t r_res = proven_fs_read_all(heap, path);
    PROVEN_TEST_ASSERT(proven_is_ok(r_res.err), "failed to read file after self-copy attempt", "");
    PROVEN_TEST_ASSERT(r_res.value.size == content.size, "File size changed after self-copy attempt!", "");
    
    bool match = true;
    for (proven_size_t i = 0; i < content.size; ++i) {
        if (r_res.value.ptr[i] != content.ptr[i]) {
            match = false;
            break;
        }
    }
    PROVEN_TEST_ASSERT(match, "File content corrupted after self-copy attempt!", "");

    heap.free_fn(heap.ctx, r_res.value.ptr);
    (void)proven_fs_remove(heap, path);

    // 4. Test hard link self-copy
    PROVEN_TEST_INFO("Attempting copy to hard-linked same file (should fail)...");
    proven_u8str_view_t link_path = PROVEN_LIT("test_copy_link.txt");
    (void)proven_fs_open(heap, path, PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC); // recreate
    (void)proven_fs_write_all(proven_fs_open(heap, path, PROVEN_FS_WRITE).value, proven_mem_view_from_u8(content));
    
    proven_err_t l_err = proven_fs_link(heap, path, link_path);
    if (proven_is_ok(l_err)) {
        proven_err_t c_err2 = proven_fs_copy(heap, path, link_path);
        PROVEN_TEST_ASSERT(!proven_is_ok(c_err2), "copy to hard-linked self should fail", "");
        
        proven_result_size_t s_res = proven_fs_size(proven_fs_open(heap, path, PROVEN_FS_READ).value);
        PROVEN_TEST_ASSERT(s_res.value == content.size, "Hardlink source truncated after copy!", "");
        
        (void)proven_fs_remove(heap, link_path);
    }
    (void)proven_fs_remove(heap, path);

    PROVEN_TEST_PASS("test_regression_fs_copy_to_self passed");
    return 0;
}
