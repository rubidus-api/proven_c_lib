#include "proven/fs.h"
#include "proven/heap.h"
#include "proven_test.h"
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/*
 * seek, tell, truncate, pread, pwrite, sync.
 *
 * None of these existed. The library could not move a file position, could not
 * shorten a file without reading it all and rewriting it, and could not get a byte
 * onto the disk at any price - it imported no fsync at all.
 *
 * The contracts that are easy to get wrong, and are checked here:
 *   - a pipe is not seekable, and that is PROVEN_ERR_UNSUPPORTED, not an I/O error;
 *   - pread and pwrite do not move the file position;
 *   - truncate does not move the file position either.
 */

static bool bytes_eq(const proven_byte_t *a, const char *b, proven_size_t n) {
    for (proven_size_t i = 0; i < n; ++i) {
        if (a[i] != (proven_byte_t)b[i]) return false;
    }
    return true;
}

int main(void) {
    PROVEN_TEST_SUITE("file position, positional I/O, and durability",
        "seek/tell/truncate/pread/pwrite/sync must behave, and must say UNSUPPORTED rather than IO on a thing that cannot seek.",
        "Inspect proven_fs_seek and friends in src/proven/fs.c and the libc calls behind them in platform/proven_sys_io.c.");

    proven_allocator_t heap = proven_heap_allocator();
    proven_u8str_view_t path = PROVEN_LIT("test_fs_position.bin");
    proven_u8str_view_t content = PROVEN_LIT("0123456789");

    proven_err_t w = proven_fs_write_file(heap, path, proven_mem_view_from_u8(content));
    PROVEN_TEST_ASSERT(proven_is_ok(w), "failed to write the fixture", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("seek and tell",
        "Seeking from the start, the current position and the end must all land where arithmetic says.",
        "Inspect the whence mapping in proven_fs_seek; SEEK_END with a negative offset counts backwards from the end.");
    // ---------------------------------------------------------------
    proven_result_file_t f = proven_fs_open(heap, path, PROVEN_FS_READ);
    PROVEN_TEST_ASSERT(proven_is_ok(f.err), "open failed", "");

    proven_result_u64_t pos = proven_fs_tell(f.value);
    PROVEN_TEST_ASSERT(proven_is_ok(pos.err) && pos.val == 0, "a freshly opened file starts at 0", "");

    pos = proven_fs_seek(f.value, 4, PROVEN_FS_SEEK_SET);
    PROVEN_TEST_ASSERT(proven_is_ok(pos.err) && pos.val == 4, "seek to absolute 4", "");

    proven_byte_t one[1];
    proven_result_size_t r = proven_fs_read(f.value, (proven_mem_mut_t){ .ptr = one, .size = 1 });
    PROVEN_TEST_ASSERT(proven_is_ok(r.err) && one[0] == (proven_byte_t)'4',
        "reading at offset 4 yields '4'",
        "If this is '0', the seek did not take effect.");

    pos = proven_fs_seek(f.value, -2, PROVEN_FS_SEEK_END);
    PROVEN_TEST_ASSERT(proven_is_ok(pos.err) && pos.val == 8, "SEEK_END with -2 on a 10-byte file lands at 8", "");

    pos = proven_fs_seek(f.value, -3, PROVEN_FS_SEEK_CUR);
    PROVEN_TEST_ASSERT(proven_is_ok(pos.err) && pos.val == 5, "SEEK_CUR moves relative to where we are", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("pread does not move the position",
        "That is the whole point of positional I/O: two readers of one handle cannot race on a cursor neither of them moves.",
        "If the position moved, pread is implemented as seek+read rather than as pread(2).");
    // ---------------------------------------------------------------
    proven_result_u64_t before = proven_fs_tell(f.value);
    proven_byte_t three[3] = {0};
    proven_result_size_t pr = proven_fs_pread(f.value, (proven_mem_mut_t){ .ptr = three, .size = 3 }, 1);
    PROVEN_TEST_ASSERT(proven_is_ok(pr.err) && pr.value == 3, "pread of 3 bytes at offset 1", "");
    PROVEN_TEST_ASSERT(bytes_eq(three, "123", 3), "pread read the right bytes", "");

    proven_result_u64_t after = proven_fs_tell(f.value);
    PROVEN_TEST_ASSERT(proven_is_ok(after.err) && after.val == before.val,
        "pread left the file position exactly where it was",
        "A pread that moves the cursor is not a pread.");

    /* Reading past the end is EOF, not a zero-byte success. */
    proven_result_size_t eof = proven_fs_pread(f.value, (proven_mem_mut_t){ .ptr = three, .size = 3 }, 100);
    PROVEN_TEST_ASSERT(eof.err == PROVEN_ERR_EOF, "pread past the end is PROVEN_ERR_EOF", "");

    (void)proven_fs_close(f.value);

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("pwrite writes where told, without moving the position",
        "Same contract as pread, in the other direction.",
        "Inspect proven_fs_pwrite; it must reach pwrite(2), not seek+write.");
    // ---------------------------------------------------------------
    f = proven_fs_open(heap, path, (proven_fs_mode_t)(PROVEN_FS_READ | PROVEN_FS_WRITE));
    PROVEN_TEST_ASSERT(proven_is_ok(f.err), "open read-write failed", "");

    before = proven_fs_tell(f.value);
    proven_result_size_t pw = proven_fs_pwrite(f.value, proven_mem_view_from_u8(PROVEN_LIT("XY")), 3);
    PROVEN_TEST_ASSERT(proven_is_ok(pw.err) && pw.value == 2, "pwrite of 2 bytes at offset 3", "");

    after = proven_fs_tell(f.value);
    PROVEN_TEST_ASSERT(after.val == before.val, "pwrite left the position alone", "");
    (void)proven_fs_close(f.value);

    proven_result_mem_mut_t all = proven_fs_read_all(heap, path);
    PROVEN_TEST_ASSERT(proven_is_ok(all.err) && all.value.size == 10, "the file is still 10 bytes", "");
    PROVEN_TEST_ASSERT(bytes_eq(all.value.ptr, "012XY56789", 10),
        "pwrite overwrote exactly bytes 3 and 4",
        "");
    heap.free_fn(heap.ctx, all.value.ptr);

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("truncate is O(1) and leaves the position alone",
        "Shortening a file used to mean reading all of it and rewriting the part you kept.",
        "Inspect proven_fs_truncate; on POSIX it is ftruncate, and on Windows the saved position must be restored.");
    // ---------------------------------------------------------------
    f = proven_fs_open(heap, path, (proven_fs_mode_t)(PROVEN_FS_READ | PROVEN_FS_WRITE));
    PROVEN_TEST_ASSERT(proven_is_ok(f.err), "open failed", "");

    proven_result_u64_t saved = proven_fs_seek(f.value, 2, PROVEN_FS_SEEK_SET);
    PROVEN_TEST_ASSERT(proven_is_ok(saved.err), "seek failed", "");

    PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_truncate(f.value, 5)), "truncate to 5 failed", "");

    proven_result_u64_t still = proven_fs_tell(f.value);
    PROVEN_TEST_ASSERT(still.val == 2, "truncate did not move the file position", "");

    proven_result_size_t sz = proven_fs_size(f.value);
    PROVEN_TEST_ASSERT(proven_is_ok(sz.err) && sz.value == 5, "the file is now 5 bytes", "");

    /* Growing works too, and the new bytes are zeros. */
    PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_truncate(f.value, 8)), "grow to 8 failed", "");
    sz = proven_fs_size(f.value);
    PROVEN_TEST_ASSERT(sz.value == 8, "the file is now 8 bytes", "");
    (void)proven_fs_close(f.value);

    all = proven_fs_read_all(heap, path);
    PROVEN_TEST_ASSERT(proven_is_ok(all.err) && all.value.size == 8, "read back 8 bytes", "");
    PROVEN_TEST_ASSERT(bytes_eq(all.value.ptr, "012XY", 5), "the kept prefix survived", "");
    PROVEN_TEST_ASSERT(all.value.ptr[5] == 0 && all.value.ptr[6] == 0 && all.value.ptr[7] == 0,
        "growing a file fills the new space with zeros", "");
    heap.free_fn(heap.ctx, all.value.ptr);

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("sync reaches the disk, and says so",
        "The library could not force a byte to storage at any price before this.",
        "Inspect proven_fs_sync; on POSIX it is fsync, on Windows FlushFileBuffers.");
    // ---------------------------------------------------------------
    f = proven_fs_open(heap, path, (proven_fs_mode_t)(PROVEN_FS_READ | PROVEN_FS_WRITE));
    PROVEN_TEST_ASSERT(proven_is_ok(f.err), "open failed", "");
    PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_sync(f.value)),
        "syncing a writable file must succeed",
        "If this fails, fsync is not reachable - which was the whole gap.");
    (void)proven_fs_close(f.value);

    /* The directory sync is what makes a rename durable. On Windows it is honestly
     * unsupported rather than a lie. */
    proven_err_t dsync = proven_fs_sync_dir(heap, PROVEN_LIT("."));
    PROVEN_TEST_ASSERT(proven_is_ok(dsync) || dsync == PROVEN_ERR_UNSUPPORTED,
        "sync_dir either works or admits it cannot",
        "It must never silently return OK on a platform where it does nothing.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a thing that cannot seek says UNSUPPORTED, not IO",
        "Not being seekable is a property of a pipe, not a failure of the call. Code that adapts to it must be able to tell them apart.",
        "Inspect the ESPIPE branch in platform/proven_sys_io.c.");
    // ---------------------------------------------------------------
    {
        /* /proc/self/status reports size 0 and is not seekable the way a file is;
         * a FIFO is the clean case, so make one. */
        proven_u8str_view_t fifo = PROVEN_LIT("test_fs_position.fifo");
        (void)proven_fs_remove(heap, fifo);
        if (mkfifo("test_fs_position.fifo", 0600) == 0) {
            /* Open non-blocking for read so the open does not wait for a writer. */
            int fd = open("test_fs_position.fifo", O_RDONLY | O_NONBLOCK);
            if (fd >= 0) {
                proven_file_t pipe_file = {0};
                pipe_file.internal.fd = fd;

                proven_result_u64_t s = proven_fs_seek(pipe_file, 0, PROVEN_FS_SEEK_CUR);
                PROVEN_TEST_ASSERT(s.err == PROVEN_ERR_UNSUPPORTED,
                    "seeking a FIFO must be PROVEN_ERR_UNSUPPORTED",
                    "PROVEN_ERR_IO here would tell the caller the pipe is broken when it is merely a pipe.");

                proven_byte_t tmp[4];
                proven_result_size_t pp = proven_fs_pread(pipe_file, (proven_mem_mut_t){ .ptr = tmp, .size = 4 }, 0);
                PROVEN_TEST_ASSERT(pp.err == PROVEN_ERR_UNSUPPORTED || pp.err == PROVEN_ERR_EOF || pp.err == PROVEN_ERR_IO,
                    "pread on a FIFO must fail rather than invent data", "");

                close(fd);
            }
            (void)proven_fs_remove(heap, fifo);
        } else {
            PROVEN_TEST_INFO("mkfifo unavailable; skipping the non-seekable check");
        }
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a durable atomic write round-trips",
        "write_file_durable must be atomic AND on the disk: fsync the data, rename, fsync the directory.",
        "Inspect internal_write_file_atomic in src/proven/fs.c. The order matters: renaming before the data is synced leaves a crash window where the name points at contents that never arrived.");
    // ---------------------------------------------------------------
    {
        proven_u8str_view_t dpath = PROVEN_LIT("test_fs_durable.txt");
        proven_u8str_view_t body = PROVEN_LIT("this had better survive");
        proven_err_t d = proven_fs_write_file_durable(heap, dpath, proven_mem_view_from_u8(body));
        PROVEN_TEST_ASSERT(proven_is_ok(d), "a durable write must succeed", "");

        proven_result_mem_mut_t back = proven_fs_read_all(heap, dpath);
        PROVEN_TEST_ASSERT(proven_is_ok(back.err) && back.value.size == body.size,
            "the durable write round-trips", "");
        heap.free_fn(heap.ctx, back.value.ptr);

        /* And it still preserves the target's permissions, like the atomic one. */
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_chmod(heap, dpath, PROVEN_FS_PERM_OWNER_R | PROVEN_FS_PERM_OWNER_W)),
            "chmod 0600 failed", "");
        d = proven_fs_write_file_durable(heap, dpath, proven_mem_view_from_u8(PROVEN_LIT("second")));
        PROVEN_TEST_ASSERT(proven_is_ok(d), "a durable rewrite must succeed", "");

        proven_fs_stat_t st = {0};
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_stat(heap, dpath, &st)), "stat failed", "");
        PROVEN_TEST_ASSERT(st.perms == (PROVEN_FS_PERM_OWNER_R | PROVEN_FS_PERM_OWNER_W),
            "a durable rewrite must not widen permissions either", "");

        /* No temp debris. */
        proven_result_file_t leftover = proven_fs_open(heap, PROVEN_LIT("test_fs_durable.txt.pvtmp00"), PROVEN_FS_READ);
        PROVEN_TEST_ASSERT(!proven_is_ok(leftover.err), "the durable write left its temp file behind", "");

        (void)proven_fs_remove(heap, dpath);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("streaming directory iteration",
        "proven_fs_list reads the WHOLE directory before the caller sees any of it. The iterator hands over entries one at a time and allocates nothing per entry.",
        "Inspect proven_fs_dir_open/_next/_close in src/proven/fs.c. The entry name is BORROWED - it points into the iterator's storage and is valid only until the next call.");
    // ---------------------------------------------------------------
    {
        proven_u8str_view_t dir = PROVEN_LIT("test_fs_dir");
        (void)proven_fs_rmdir(heap, dir);
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_mkdir(heap, dir)), "mkdir failed", "");

        proven_u8str_view_t files[3] = {
            PROVEN_LIT("test_fs_dir/a.txt"),
            PROVEN_LIT("test_fs_dir/b.txt"),
            PROVEN_LIT("test_fs_dir/c.txt"),
        };
        for (int i = 0; i < 3; ++i) {
            PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_write_file(heap, files[i], proven_mem_view_from_u8(PROVEN_LIT("x")))),
                "writing a fixture file failed", "");
        }

        proven_result_dir_t d = proven_fs_dir_open(heap, dir);
        PROVEN_TEST_ASSERT(proven_is_ok(d.err), "dir_open failed", "");

        int seen = 0;
        bool saw_a = false, saw_b = false, saw_c = false;
        proven_fs_dir_entry_t e;
        while (proven_is_ok(proven_fs_dir_next(&d.value, &e))) {
            if (proven_u8str_view_eq(e.name, PROVEN_LIT("."))) continue;
            if (proven_u8str_view_eq(e.name, PROVEN_LIT(".."))) continue;
            if (proven_u8str_view_eq(e.name, PROVEN_LIT("a.txt"))) saw_a = true;
            if (proven_u8str_view_eq(e.name, PROVEN_LIT("b.txt"))) saw_b = true;
            if (proven_u8str_view_eq(e.name, PROVEN_LIT("c.txt"))) saw_c = true;
            ++seen;
        }
        proven_fs_dir_close(&d.value);

        PROVEN_TEST_ASSERT(seen == 3, "the iterator must yield exactly the three files", "");
        PROVEN_TEST_ASSERT(saw_a && saw_b && saw_c, "and all three by name", "");

        /* Opening something that is not there fails rather than yielding nothing. */
        proven_result_dir_t missing = proven_fs_dir_open(heap, PROVEN_LIT("test_fs_dir_nope"));
        PROVEN_TEST_ASSERT(!proven_is_ok(missing.err),
            "opening a directory that does not exist must fail",
            "Silently iterating zero entries would look exactly like an empty directory.");

        for (int i = 0; i < 3; ++i) (void)proven_fs_remove(heap, files[i]);
        (void)proven_fs_rmdir(heap, dir);
    }

    (void)proven_fs_remove(heap, path);
    PROVEN_TEST_PASS("position, positional I/O, and durability behave.");
    return 0;
}
