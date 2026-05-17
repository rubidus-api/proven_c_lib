#include "proven.h"
#include "proven_test.h"
#include "proven/fs.h"
#include "proven/mmap.h"
#include "proven/fmt.h"
#include "proven/sysio.h"
#include <string.h>

#define TEST_FILE "mmap_test.bin"
#define TEST_DATA "Hello, Memory Mapped World!"

int main() {
    proven_byte_t backing[8192];
    proven_arena_t arena = proven_arena_create((proven_mem_mut_t){backing, sizeof(backing)});
    proven_allocator_t alloc = proven_arena_as_allocator(&arena);
    
    // 1. Create a test file
    PROVEN_TEST_INFO("Creating test file for memory mapping...");
    proven_u8str_view_t path = PROVEN_LIT(TEST_FILE);
    proven_result_file_t file_res = proven_fs_open(alloc, path, PROVEN_FS_WRITE | PROVEN_FS_READ | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
    if (file_res.err != PROVEN_OK) {
        PROVEN_TEST_INFO("Failed to create test file");
        return 1;
    }
    proven_file_t file = file_res.value;

    (void)proven_fs_write(file, (proven_mem_view_t){ .ptr = (void*)TEST_DATA, .size = strlen(TEST_DATA) });

    // 2. Map the file
    PROVEN_TEST_INFO("Testing invalid mmap flags...");
    proven_result_mmap_t invalid_mmap_1 = proven_mmap_create(file, 0, 0, PROVEN_MMAP_READ, 0);
    PROVEN_TEST_ASSERT(invalid_mmap_1.err == PROVEN_ERR_INVALID_ARG, "mmap should reject 0 flags", "");
    
    proven_result_mmap_t invalid_mmap_2 = proven_mmap_create(file, 0, 0, PROVEN_MMAP_READ, PROVEN_MMAP_PRIVATE | PROVEN_MMAP_SHARED);
    PROVEN_TEST_ASSERT(invalid_mmap_2.err == PROVEN_ERR_INVALID_ARG, "mmap should reject PRIVATE|SHARED", "");

    proven_result_mmap_t invalid_mmap_3 = proven_mmap_create(file, 0, 0, 0, PROVEN_MMAP_PRIVATE);
    PROVEN_TEST_ASSERT(invalid_mmap_3.err == PROVEN_ERR_INVALID_ARG, "mmap should reject 0 prot", "");

    proven_result_mmap_t invalid_mmap_4 = proven_mmap_create(file, 0, 0, PROVEN_MMAP_READ | 0x8, PROVEN_MMAP_PRIVATE);
    PROVEN_TEST_ASSERT(invalid_mmap_4.err == PROVEN_ERR_INVALID_ARG, "mmap should reject unknown prot bits", "");

    PROVEN_TEST_INFO("Mapping file into memory...");
    proven_result_mmap_t mmap_res = proven_mmap_create(file, 0, 0, PROVEN_MMAP_READ | PROVEN_MMAP_WRITE, PROVEN_MMAP_SHARED);
    if (mmap_res.err != PROVEN_OK) {
        PROVEN_TEST_INFO("Failed to map file");
        return 1;
    }
    proven_mmap_t map = mmap_res.value;

    // 3. Verify content
    PROVEN_TEST_INFO("Verifying mapped content...");
    PROVEN_TEST_INFO("Mapped content: {}", PROVEN_ARG(((proven_u8str_view_t){.ptr = map.ptr, .size = map.size})));
    if (memcmp(map.ptr, TEST_DATA, strlen(TEST_DATA)) != 0) {
        PROVEN_TEST_INFO("Mapped content mismatch!");
        return 1;
    }

    // 4. Modify memory and sync
    PROVEN_TEST_INFO("Modifying memory and syncing to disk...");
    char *cptr = (char*)map.ptr;
    cptr[0] = 'J'; // "Jello..."
    
    proven_err_t sync_err = proven_mmap_sync(&map);
    if (sync_err != PROVEN_OK) {
        PROVEN_TEST_INFO("Sync failed");
        return 1;
    }

    // 5. Clean up
    (void)proven_mmap_destroy(&map);
    proven_fs_close(file);

    // 6. Verify modification on disk
    PROVEN_TEST_INFO("Verifying modification persists on disk...");
    file_res = proven_fs_open(alloc, path, PROVEN_FS_READ);
    char buf[64] = {0};
    (void)proven_fs_read(file_res.value, (proven_mem_mut_t){ .ptr = (proven_byte_t*)buf, .size = sizeof(buf) - 1 });
    PROVEN_TEST_INFO("After modification: {}", PROVEN_ARG((const char*)buf));
    
    if (buf[0] != 'J') {
        PROVEN_TEST_INFO("Disk content mismatch!");
        return 1;
    }

    proven_fs_close(file_res.value);
    (void)proven_fs_remove(alloc, path);

    PROVEN_TEST_INFO("Phase 17 [mmap] passed!");

    proven_arena_destroy(&arena);
    return 0;
}
