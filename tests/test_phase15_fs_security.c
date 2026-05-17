#include "proven.h"
#include "proven_test.h"
#include "proven/sysio.h"



int main() {
    PROVEN_TEST_INFO("Running Phase 15 File Security & Locking Tests...");

    proven_allocator_t heap = proven_heap_allocator();
    proven_u8str_view_t test_file = PROVEN_LIT("lock_test.txt");
    
    // 1. Permission Test
    PROVEN_TEST_INFO("Testing file permission changes (chmod)...");
    {
        // Create file
        proven_result_file_t r = proven_fs_open(heap, test_file, PROVEN_FS_WRITE | PROVEN_FS_CREATE);
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(r.err), "Testing condition: PROVEN_IS_OK(r.err)", "Review logic surrounding PROVEN_IS_OK(r.err)");
        proven_fs_close(r.value);
        
        // Change to Read-Only (0444)
        proven_fs_perms_t ro_perms = PROVEN_FS_PERM_OWNER_R | PROVEN_FS_PERM_GROUP_R | PROVEN_FS_PERM_OTHER_R;
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_fs_chmod(heap, test_file, ro_perms)), "Testing condition: PROVEN_IS_OK(proven_fs_chmod(heap, test_file, ro_perms))", "Review logic surrounding PROVEN_IS_OK(proven_fs_chmod(heap, test_file, ro_perms))");
        
        // Change back to Default
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_fs_chmod(heap, test_file, PROVEN_FS_PERM_DEFAULT)), "Testing condition: PROVEN_IS_OK(proven_fs_chmod(heap, test_file, PROVEN_FS_PERM_DEFAULT))", "Review logic surrounding PROVEN_IS_OK(proven_fs_chmod(heap, test_file, PROVEN_FS_PERM_DEFAULT))");
    }

    // 2. Advisory Locking Test
    PROVEN_TEST_INFO("Testing advisory file locking...");
    {
        proven_result_file_t r = proven_fs_open(heap, test_file, PROVEN_FS_READ | PROVEN_FS_WRITE);
        if (!PROVEN_IS_OK(r.err)) {
            // Might fail if we can't open for write
            r = proven_fs_open(heap, test_file, PROVEN_FS_READ);
        }
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(r.err), "Testing condition: PROVEN_IS_OK(r.err)", "Review logic surrounding PROVEN_IS_OK(r.err)");
        
        // Acquire Shared Lock
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_fs_lock(r.value, PROVEN_FS_LOCK_SHARED, true)), "Testing condition: PROVEN_IS_OK(proven_fs_lock(r.value, PROVEN_FS_LOCK_SHARED, true))", "Review logic surrounding PROVEN_IS_OK(proven_fs_lock(r.value, PROVEN_FS_LOCK_SHARED, true))");
        
        // Release Lock
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_fs_lock(r.value, PROVEN_FS_LOCK_UNLOCK, true)), "Testing condition: PROVEN_IS_OK(proven_fs_lock(r.value, PROVEN_FS_LOCK_UNLOCK, true))", "Review logic surrounding PROVEN_IS_OK(proven_fs_lock(r.value, PROVEN_FS_LOCK_UNLOCK, true))");
        
        // Acquire Exclusive Lock
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_fs_lock(r.value, PROVEN_FS_LOCK_EXCLUSIVE, true)), "Testing condition: PROVEN_IS_OK(proven_fs_lock(r.value, PROVEN_FS_LOCK_EXCLUSIVE, true))", "Review logic surrounding PROVEN_IS_OK(proven_fs_lock(r.value, PROVEN_FS_LOCK_EXCLUSIVE, true))");
        
        // Release Lock
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_fs_lock(r.value, PROVEN_FS_LOCK_UNLOCK, true)), "Testing condition: PROVEN_IS_OK(proven_fs_lock(r.value, PROVEN_FS_LOCK_UNLOCK, true))", "Review logic surrounding PROVEN_IS_OK(proven_fs_lock(r.value, PROVEN_FS_LOCK_UNLOCK, true))");
        
        proven_fs_close(r.value);
    }

    // Cleanup
    (void)proven_fs_remove(heap, test_file);

    PROVEN_TEST_PASS("All Phase 15 File Security & Locking Tests Passed Successfully!");
    return 0;
}
