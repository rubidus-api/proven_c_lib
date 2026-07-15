#include "proven.h"
#include "proven_test.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32) || defined(_WIN64)
int main(void) {
    PROVEN_TEST_SUITE("filesystem permissions and entry types",
        "Permission preservation and non-regular entry types.",
        "POSIX-only; skipped on Windows.");
    PROVEN_TEST_INFO("POSIX-only; skipped on this platform");
    PROVEN_TEST_PASS("skipped");
    return 0;
}
#else

#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <dirent.h>
#include <fcntl.h>

/*
 * Four findings from the filesystem audit, all of them the same shape: the end state was
 * right and the window, or the type, or the report was wrong.
 *
 *   - proven_fs_copy created the destination with the process umask, so copying a 0600
 *     file produced a 0644 one. `cp` does not do that.
 *   - write_file_atomic chmod'd the temp at the END, so the entire new contents of a 0600
 *     file sat in a world-readable temp for the whole duration of the write. A window is
 *     all a secret needs.
 *   - a symlink, a FIFO, a socket or a device came back as PROVEN_FS_TYPE_FILE, which
 *     tells a caller it may open it and read bytes out of it. A dangling symlink cannot
 *     even be opened.
 *   - proven_mmap_sync on a PRIVATE (copy-on-write) mapping returned PROVEN_OK and
 *     persisted nothing.
 */

/* Atomics, not volatile: volatile orders nothing between threads, and TSan is right to
 * say so. The watcher and the writer really do touch these concurrently. */
typedef struct { _Atomic int stop; _Atomic int leaked; _Atomic int widest; } watcher_t;

static void *watch_temps(void *arg) {
    watcher_t *w = (watcher_t *)arg;
    while (__atomic_load_n(&w->stop, __ATOMIC_RELAXED) == 0) {
        DIR *d = opendir("test_fs_perms.d");
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!strstr(e->d_name, ".pvtmp")) continue;
            char p[512];
            snprintf(p, sizeof p, "test_fs_perms.d/%s", e->d_name);
            struct stat st;
            /* Only a temp that already HOLDS bytes can leak anything. */
            if (stat(p, &st) == 0 && st.st_size > 0) {
                int m = (int)(st.st_mode & 0777);
                if (m > __atomic_load_n(&w->widest, __ATOMIC_RELAXED)) {
                    __atomic_store_n(&w->widest, m, __ATOMIC_RELAXED);
                }
                if (m & 0077) __atomic_store_n(&w->leaked, 1, __ATOMIC_RELAXED);
            }
        }
        closedir(d);
    }
    return NULL;
}

int main(void) {
    PROVEN_TEST_SUITE("filesystem permissions and entry types",
        "A copy carries the source's mode; an atomic write never exposes its contents under a wider mode; a symlink is not a file; a private mapping cannot be synced.",
        "Inspect proven_fs_copy and internal_write_file_atomic in src/proven/fs.c, the dir_step type mapping in platform/proven_sys_fs.c, and proven_mmap_sync.");

    proven_allocator_t heap = proven_heap_allocator();

    (void)system("rm -rf test_fs_perms.d && mkdir -p test_fs_perms.d");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a copy carries the source's mode",
        "Copying a 0600 file used to produce a 0644 one: every byte of a private file, world-readable, from the moment it was written.",
        "proven_fs_copy must stat the source and chmod the destination BEFORE the contents go in.");
    // ---------------------------------------------------------------
    {
        proven_u8str_view_t src = PROVEN_LIT("test_fs_perms.d/secret.txt");
        proven_u8str_view_t dst = PROVEN_LIT("test_fs_perms.d/secret.copy");
        proven_byte_t data[] = "top secret";

        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_write_file(heap, src,
            (proven_mem_view_t){ .ptr = data, .size = sizeof data })), "setup: write the source", "");
        PROVEN_TEST_ASSERT(chmod("test_fs_perms.d/secret.txt", 0600) == 0, "setup: chmod 0600", "");

        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_copy(heap, src, dst)), "the copy must succeed", "");

        struct stat st;
        PROVEN_TEST_ASSERT(stat("test_fs_perms.d/secret.copy", &st) == 0, "the copy must exist", "");
        PROVEN_TEST_ASSERT((st.st_mode & 0777) == 0600,
            "a copy of a 0600 file must be 0600",
            "It used to be 0644: the destination was created with the process umask and the source's mode was never carried across.");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("an atomic write never exposes its contents under a wider mode",
        "The temp used to be chmod'd at the END, so the whole payload sat in a 0644 file for the duration of the write.",
        "A watcher thread stats every temp that already holds bytes. If it ever sees group/other bits, the window is open.");
    // ---------------------------------------------------------------
    {
        proven_u8str_view_t path = PROVEN_LIT("test_fs_perms.d/secret.txt");

        /* Big enough that the write takes long enough to be caught in the act. */
        const proven_size_t n = 16u << 20;
        proven_result_mem_mut_t buf = heap.alloc_fn(heap.ctx, n, 16);
        PROVEN_TEST_ASSERT(proven_is_ok(buf.err), "setup: a large payload", "");
        memset(buf.value.ptr, 'S', n);

        watcher_t w = {0};
        pthread_t th;
        PROVEN_TEST_ASSERT(pthread_create(&th, NULL, watch_temps, &w) == 0, "setup: a watcher", "");

        proven_err_t e = proven_fs_write_file_atomic(heap, path,
            (proven_mem_view_t){ .ptr = buf.value.ptr, .size = n });
        __atomic_store_n(&w.stop, 1, __ATOMIC_RELAXED);
        (void)pthread_join(th, NULL);
        heap.free_fn(heap.ctx, buf.value.ptr);

        PROVEN_TEST_ASSERT(proven_is_ok(e), "the atomic write must succeed", "");

        struct stat st;
        PROVEN_TEST_ASSERT(stat("test_fs_perms.d/secret.txt", &st) == 0 && (st.st_mode & 0777) == 0600,
            "the final file must still be 0600", "");
        PROVEN_TEST_ASSERT(__atomic_load_n(&w.leaked, __ATOMIC_RELAXED) == 0,
            "the temp must never hold the new contents under a group- or world-readable mode",
            "It used to be created with the umask and chmod'd only at the end - so the entire payload of a 0600 file was readable by anyone for as long as the write took.");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("copying a read-only file twice still works",
        "Carrying the source's mode means the destination ends up 0400 - and open(O_WRONLY) on a 0400 file fails, so the SECOND copy could not even open it.",
        "A backup loop worked once and failed forever after, with the destination silently keeping its old contents. The copy makes an unwritable destination writable first: it is about to overwrite it anyway.");
    // ---------------------------------------------------------------
    {
        proven_u8str_view_t src = PROVEN_LIT("test_fs_perms.d/ro-src.txt");
        proven_u8str_view_t dst = PROVEN_LIT("test_fs_perms.d/ro-dst.txt");
        proven_byte_t d1[] = "first";

        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_write_file(heap, src,
            (proven_mem_view_t){ .ptr = d1, .size = sizeof d1 })), "setup: a source", "");
        PROVEN_TEST_ASSERT(chmod("test_fs_perms.d/ro-src.txt", 0400) == 0, "setup: make it read-only", "");

        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_copy(heap, src, dst)), "the first copy must succeed", "");
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_copy(heap, src, dst)),
            "and the SECOND copy onto the same destination must succeed too",
            "It used to fail with PROVEN_ERR_IO: the destination carried the source's 0400 and could no longer be opened for writing.");

        struct stat st;
        PROVEN_TEST_ASSERT(stat("test_fs_perms.d/ro-dst.txt", &st) == 0 && (st.st_mode & 0777) == 0400,
            "and the destination must still end up 0400", "");

        proven_result_mem_mut_t back = proven_fs_read_all(heap, dst);
        PROVEN_TEST_ASSERT(proven_is_ok(back.err) && back.value.size == sizeof d1 &&
                           memcmp(back.value.ptr, d1, sizeof d1) == 0,
            "and hold the source's contents", "");
        heap.free_fn(heap.ctx, back.value.ptr);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a symlink and a FIFO are not regular files",
        "They used to come back as PROVEN_FS_TYPE_FILE, which tells a caller it may open them and read bytes out of them.",
        "A dangling symlink cannot even be opened. Inspect the is_regular mapping in platform/proven_sys_fs.c.");
    // ---------------------------------------------------------------
    {
        PROVEN_TEST_ASSERT(symlink("/nonexistent/target", "test_fs_perms.d/dangling") == 0,
            "setup: a dangling symlink", "");
        PROVEN_TEST_ASSERT(symlink("secret.txt", "test_fs_perms.d/good1") == 0,
            "setup: a symlink to a real file", "");
        PROVEN_TEST_ASSERT(mkfifo("test_fs_perms.d/pipe", 0600) == 0, "setup: a FIFO", "");

        proven_result_dir_t dr = proven_fs_dir_open(heap, PROVEN_LIT("test_fs_perms.d"));
        PROVEN_TEST_ASSERT(proven_is_ok(dr.err), "the directory must open", "");
        proven_fs_dir_t dir = dr.value;

        bool saw_dangling = false, saw_fifo = false, saw_regular = false, saw_good_link = false;
        for (;;) {
            proven_fs_dir_entry_t entry = {0};
            proven_err_t e = proven_fs_dir_next(&dir, &entry);
            if (e == PROVEN_ERR_EOF) break;
            PROVEN_TEST_ASSERT(proven_is_ok(e), "the walk must not fail", "");

            proven_u8str_view_t n = entry.name;
            if (n.size == 8 && memcmp(n.ptr, "dangling", 8) == 0) {
                saw_dangling = true;
                PROVEN_TEST_ASSERT(entry.type == PROVEN_FS_TYPE_OTHER,
                    "a dangling symlink must be PROVEN_FS_TYPE_OTHER",
                    "It used to be reported as a regular file - one a caller cannot even open.");
            } else if (n.size == 4 && memcmp(n.ptr, "pipe", 4) == 0) {
                saw_fifo = true;
                PROVEN_TEST_ASSERT(entry.type == PROVEN_FS_TYPE_OTHER,
                    "a FIFO must be PROVEN_FS_TYPE_OTHER",
                    "Reading a FIFO as if it were a file blocks forever on a writer that never comes.");
            } else if (n.size == 10 && memcmp(n.ptr, "secret.txt", 10) == 0) {
                saw_regular = true;
                PROVEN_TEST_ASSERT(entry.type == PROVEN_FS_TYPE_FILE,
                    "and a regular file must still be PROVEN_FS_TYPE_FILE", "");
            } else if (n.size == 5 && memcmp(n.ptr, "good1", 5) == 0) {
                saw_good_link = true;
                PROVEN_TEST_ASSERT(entry.type == PROVEN_FS_TYPE_FILE,
                    "a symlink to a regular file must be PROVEN_FS_TYPE_FILE, like stat says it is",
                    "The walk used to stat with AT_SYMLINK_NOFOLLOW, so it said OTHER while proven_fs_stat on the same path said FILE. A caller filtering a listing on type == FILE skipped files it could open and read.");
            }
        }
        proven_fs_dir_close(&dir);

        PROVEN_TEST_ASSERT(saw_dangling && saw_fifo && saw_regular && saw_good_link,
            "the walk must have seen all four entries", "");

        /* And stat must agree with the listing, which is the whole point. */
        proven_fs_stat_t st = {0};
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_stat(heap, PROVEN_LIT("test_fs_perms.d/good1"), &st)) &&
                           st.type == PROVEN_FS_TYPE_FILE,
            "and proven_fs_stat must say the same thing about that symlink",
            "Two answers to the same question is worse than either answer.");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a private mapping cannot be synced, and says so",
        "msync on a copy-on-write mapping writes nothing back. It used to return PROVEN_OK.",
        "A caller could write through a private mapping, sync it, be told it worked, and find the file unchanged.");
    // ---------------------------------------------------------------
    {
        proven_u8str_view_t path = PROVEN_LIT("test_fs_perms.d/mapped.bin");
        proven_byte_t data[64];
        memset(data, 'A', sizeof data);
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_write_file(heap, path,
            (proven_mem_view_t){ .ptr = data, .size = sizeof data })), "setup: a file to map", "");

        proven_result_file_t f = proven_fs_open(heap, path, PROVEN_FS_READ | PROVEN_FS_WRITE);
        PROVEN_TEST_ASSERT(proven_is_ok(f.err), "setup: open it", "");

        proven_result_mmap_t m = proven_mmap_create(f.value, 0, 0,
            (proven_mmap_prot_t)(PROVEN_MMAP_READ | PROVEN_MMAP_WRITE), PROVEN_MMAP_PRIVATE);
        PROVEN_TEST_ASSERT(proven_is_ok(m.err), "a private mapping must be creatable", "");

        ((unsigned char *)m.value.ptr)[0] = 'Z';   /* copy-on-write: this goes nowhere */

        PROVEN_TEST_ASSERT(proven_mmap_sync(&m.value) == PROVEN_ERR_UNSUPPORTED,
            "syncing a PRIVATE mapping must be PROVEN_ERR_UNSUPPORTED",
            "It used to return PROVEN_OK while persisting nothing - success reported for a persist that cannot happen.");

        PROVEN_TEST_ASSERT(proven_is_ok(proven_mmap_destroy(&m.value)), "the mapping must unmap", "");
        (void)proven_fs_close(f.value);

        /* And a SHARED mapping still works. */
        f = proven_fs_open(heap, path, PROVEN_FS_READ | PROVEN_FS_WRITE);
        PROVEN_TEST_ASSERT(proven_is_ok(f.err), "reopen", "");
        m = proven_mmap_create(f.value, 0, 0,
            (proven_mmap_prot_t)(PROVEN_MMAP_READ | PROVEN_MMAP_WRITE), PROVEN_MMAP_SHARED);
        PROVEN_TEST_ASSERT(proven_is_ok(m.err), "a shared mapping must be creatable", "");
        ((unsigned char *)m.value.ptr)[0] = 'Z';
        PROVEN_TEST_ASSERT(proven_is_ok(proven_mmap_sync(&m.value)),
            "syncing a SHARED mapping must still work", "");
        PROVEN_TEST_ASSERT(proven_is_ok(proven_mmap_destroy(&m.value)), "unmap", "");
        (void)proven_fs_close(f.value);

        proven_result_mem_mut_t back = proven_fs_read_all(heap, path);
        PROVEN_TEST_ASSERT(proven_is_ok(back.err) && ((unsigned char *)back.value.ptr)[0] == 'Z',
            "and the shared write must actually be on disk", "");
        heap.free_fn(heap.ctx, back.value.ptr);
    }

    (void)system("rm -rf test_fs_perms.d");

    PROVEN_TEST_PASS("permissions are carried, windows are closed, and types are honest.");
    return 0;
}
#endif
