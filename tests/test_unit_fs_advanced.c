#include "proven.h"
#include "proven_test.h"
#include "proven/sysio.h"

#include <string.h>
#if !defined(_WIN32) && !defined(_WIN64)
#include <unistd.h>   /* getuid / getgid for the owner-field check */
#endif


int main() {
    PROVEN_TEST_INFO("Running Phase 14 Advanced File System Tests...");

    proven_allocator_t heap = proven_heap_allocator();
    
    proven_u8str_view_t dir_name = PROVEN_LIT("test_fs_advanced_dir");
    proven_u8str_view_t file_a = PROVEN_LIT("test_fs_advanced_dir/file_a.txt");
    proven_u8str_view_t file_b = PROVEN_LIT("test_fs_advanced_dir/file_b.txt");
    proven_u8str_view_t file_c = PROVEN_LIT("test_fs_advanced_dir/file_c.txt");
    
    // 0. Cleanup from eventual previous failed run
    (void)proven_fs_remove(heap, file_a);
    (void)proven_fs_remove(heap, file_b);
    (void)proven_fs_remove(heap, file_c);
    (void)proven_fs_rmdir(heap, dir_name);

    // 1. Directory Creation
    PROVEN_TEST_INFO("Testing directory creation...");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_fs_mkdir(heap, dir_name)), "Testing condition: PROVEN_IS_OK(proven_fs_mkdir(heap, dir_name))", "Review logic surrounding PROVEN_IS_OK(proven_fs_mkdir(heap, dir_name))");
    
    // 2. File Creation
    PROVEN_TEST_INFO("Testing file creation in nested directory...");
    proven_u8str_view_t content = PROVEN_LIT("Some data");
    {
        proven_result_file_t r = proven_fs_open(heap, file_a, PROVEN_FS_WRITE | PROVEN_FS_CREATE);
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(r.err), "Testing condition: PROVEN_IS_OK(r.err)", "Review logic surrounding PROVEN_IS_OK(r.err)");
        (void)proven_fs_write(r.value, proven_mem_view_from_u8(content));
        (void)proven_fs_close(r.value);
    }
    {
        proven_result_file_t r = proven_fs_open(heap, file_b, PROVEN_FS_WRITE | PROVEN_FS_CREATE);
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(r.err), "Testing condition: PROVEN_IS_OK(r.err)", "Review logic surrounding PROVEN_IS_OK(r.err)");
        (void)proven_fs_close(r.value);
    }

    // 3. Rename/Move
    PROVEN_TEST_INFO("Testing file rename/move...");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_fs_rename(heap, file_b, file_c)), "Testing condition: PROVEN_IS_OK(proven_fs_rename(heap, file_b, file_c))", "Review logic surrounding PROVEN_IS_OK(proven_fs_rename(heap, file_b, file_c))");

    // 4. Directory Listing & Sorting
    PROVEN_TEST_INFO("Testing directory listing...");
    proven_result_array_t list_res = proven_fs_list(heap, dir_name);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(list_res.err), "Testing condition: PROVEN_IS_OK(list_res.err)", "Review logic surrounding PROVEN_IS_OK(list_res.err)");
    
    proven_array_t *arr = &list_res.value;
    PROVEN_TEST_INFO("Entries in {}: {}", PROVEN_ARG(dir_name), PROVEN_ARG((proven_i32)arr->len));
    
    // Test sorting (A, then C)
    PROVEN_TEST_ASSERT(arr->len >= 2, "Testing condition: arr->len >= 2", "Review logic surrounding arr->len >= 2");
    proven_fs_entry_t *entries = (proven_fs_entry_t*)arr->data;
    
    // We expect "file_a.txt" and "file_c.txt" to be present
    bool found_a = false;
    bool found_c = false;
    for (proven_size_t i = 0; i < arr->len; i++) {
        proven_u8str_view_t nm = proven_u8str_as_view(&entries[i].name);
        PROVEN_TEST_INFO("Found file: {}", PROVEN_ARG(nm));
        if (nm.size == 10 && memcmp(nm.ptr, "file_a.txt", 10) == 0) found_a = true;
        if (nm.size == 10 && memcmp(nm.ptr, "file_c.txt", 10) == 0) found_c = true;
    }
    PROVEN_TEST_ASSERT(found_a && found_c, "Testing condition: found_a && found_c", "file_a.txt or file_c.txt was not found in directory listing");
    
    // Cleanup list strings
    proven_fs_list_destroy(heap, arr);

    // 4b. Stat exposes owner/group (uid/gid). On POSIX they equal the calling
    //     process's ids for a file we just created; on Windows they are 0.
    PROVEN_TEST_INFO("Testing stat owner/group (uid/gid)...");
    {
        proven_fs_stat_t st;
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_fs_stat(heap, file_a, &st)),
            "Testing condition: proven_fs_stat(file_a) ok",
            "Review proven_fs_stat / proven_sys_fs_stat owner-field population");
#if !defined(_WIN32) && !defined(_WIN64)
        PROVEN_TEST_ASSERT(st.uid == (unsigned long long)getuid(),
            "stat uid matches getuid() for a just-created file",
            "Check st_uid propagation in proven_sys_fs_stat (POSIX)");
        PROVEN_TEST_ASSERT(st.gid == (unsigned long long)getgid(),
            "stat gid matches getgid() for a just-created file",
            "Check st_gid propagation in proven_sys_fs_stat (POSIX)");
#else
        PROVEN_TEST_ASSERT(st.uid == 0 && st.gid == 0,
            "stat uid/gid are 0 on Windows (no POSIX ownership)",
            "Check the Windows branch of proven_sys_fs_stat");
#endif
    }

    // 5. Cleanup
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_fs_remove(heap, file_a)), "Testing condition: PROVEN_IS_OK(proven_fs_remove(heap, file_a))", "Review logic surrounding PROVEN_IS_OK(proven_fs_remove(heap, file_a))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_fs_remove(heap, file_c)), "Testing condition: PROVEN_IS_OK(proven_fs_remove(heap, file_c))", "Review logic surrounding PROVEN_IS_OK(proven_fs_remove(heap, file_c))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_fs_rmdir(heap, dir_name)), "Testing condition: PROVEN_IS_OK(proven_fs_rmdir(heap, dir_name))", "Review logic surrounding PROVEN_IS_OK(proven_fs_rmdir(heap, dir_name))");

    PROVEN_TEST_PASS("All Phase 14 Advanced File System Tests Passed Successfully!");
    return 0;
}
