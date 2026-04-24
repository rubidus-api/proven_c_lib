#include "proven.h"
#include "proven_test.h"
#include "proven/sysio.h"

#include <string.h>


int main() {
    PROVEN_TEST_INFO("Running Phase 14 Advanced File System Tests...");

    proven_allocator_t heap = proven_heap_allocator();
    
    proven_u8str_view_t dir_name = PROVEN_LIT("test_dir");
    proven_u8str_view_t file_a = PROVEN_LIT("test_dir/file_a.txt");
    proven_u8str_view_t file_b = PROVEN_LIT("test_dir/file_b.txt");
    proven_u8str_view_t file_c = PROVEN_LIT("test_dir/file_c.txt");
    
    // 1. Directory Creation
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_fs_mkdir(dir_name)), "Testing condition: PROVEN_IS_OK(proven_fs_mkdir(dir_name))", "Review logic surrounding PROVEN_IS_OK(proven_fs_mkdir(dir_name))");
    
    // 2. File Creation
    proven_u8str_view_t content = PROVEN_LIT("Some data");
    {
        proven_result_file_t r = proven_fs_open(file_a, PROVEN_FS_WRITE | PROVEN_FS_CREATE);
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(r.err), "Testing condition: PROVEN_IS_OK(r.err)", "Review logic surrounding PROVEN_IS_OK(r.err)");
        (void)proven_fs_write(r.value, proven_mem_view_from_u8(content));
        proven_fs_close(r.value);
    }
    {
        proven_result_file_t r = proven_fs_open(file_b, PROVEN_FS_WRITE | PROVEN_FS_CREATE);
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(r.err), "Testing condition: PROVEN_IS_OK(r.err)", "Review logic surrounding PROVEN_IS_OK(r.err)");
        proven_fs_close(r.value);
    }

    // 3. Rename/Move
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_fs_rename(file_b, file_c)), "Testing condition: PROVEN_IS_OK(proven_fs_rename(file_b, file_c))", "Review logic surrounding PROVEN_IS_OK(proven_fs_rename(file_b, file_c))");

    // 4. Directory Listing & Sorting
    proven_result_array_t list_res = proven_fs_list(heap, dir_name);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(list_res.err), "Testing condition: PROVEN_IS_OK(list_res.err)", "Review logic surrounding PROVEN_IS_OK(list_res.err)");
    
    proven_array_t *arr = &list_res.value;
    PROVEN_TEST_INFO("Entries in {}: {}", PROVEN_ARG(dir_name), PROVEN_ARG((proven_i32)arr->len));
    
    // Test sorting (A, then C)
    PROVEN_TEST_ASSERT(arr->len >= 2, "Testing condition: arr->len >= 2", "Review logic surrounding arr->len >= 2");
    proven_fs_entry_t *entries = (proven_fs_entry_t*)arr->internal.ptr;
    
    // A should be first
    proven_u8str_view_t name_0 = proven_u8str_as_view(&entries[0].name);
    PROVEN_TEST_ASSERT(memcmp(name_0.ptr, "file_a.txt", 10) == 0, "Testing condition: memcmp(name_0.ptr, 'file_a.txt', 10) == 0", "Review logic surrounding memcmp(name_0.ptr, 'file_a.txt', 10) == 0");
    
    // Cleanup list strings
    for (proven_size_t i = 0; i < arr->len; ++i) {
        proven_u8str_destroy(heap, &entries[i].name);
    }
    PROVEN_ARRAY_DESTROY(arr);

    // 5. Cleanup
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_fs_remove(file_a)), "Testing condition: PROVEN_IS_OK(proven_fs_remove(file_a))", "Review logic surrounding PROVEN_IS_OK(proven_fs_remove(file_a))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_fs_remove(file_c)), "Testing condition: PROVEN_IS_OK(proven_fs_remove(file_c))", "Review logic surrounding PROVEN_IS_OK(proven_fs_remove(file_c))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_fs_rmdir(dir_name)), "Testing condition: PROVEN_IS_OK(proven_fs_rmdir(dir_name))", "Review logic surrounding PROVEN_IS_OK(proven_fs_rmdir(dir_name))");

    PROVEN_TEST_INFO("All Phase 14 Advanced File System Tests Passed Successfully!");
    return 0;
}
