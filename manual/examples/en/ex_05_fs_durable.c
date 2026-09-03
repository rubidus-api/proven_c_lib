#include "example.h"

/*
 * Updating a file so that a power cut cannot leave it half-written.
 *
 * The recipe is old and every part of it is load-bearing:
 *
 *   1. write the new contents to a TEMPORARY file beside the real one,
 *   2. proven_fs_sync   - the new file's bytes are now on the device,
 *   3. proven_fs_rename - the name flips to the new file in one step; a reader
 *                         sees either the whole old file or the whole new one,
 *   4. proven_fs_sync_dir - the rename itself is now on the device.
 *
 * Skip step 2 and the rename can publish a file whose contents never arrived.
 * Skip step 4 and the contents are safe under a name that may not be. Neither
 * failure shows up in testing; both show up in production, once.
 *
 * The same program also shows the record-level calls - seek/tell, pread/pwrite,
 * truncate - and the advisory lock that stops two copies of the program doing
 * all this at the same time.
 */

typedef struct {
    proven_u32 id;
    proven_u32 score;
} record_t;

static proven_err_t write_records(proven_file_t f, const record_t *recs, proven_size_t n) {
    proven_mem_view_t view = { .ptr = (const proven_byte_t *)recs, .size = n * sizeof recs[0] };
    return proven_fs_write_all(f, view);
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    proven_u8str_view_t live = PROVEN_LIT("proven_example_durable.dat");
    proven_u8str_view_t temp = PROVEN_LIT("proven_example_durable.dat.new");
    proven_u8str_view_t here = PROVEN_LIT(".");

    /* is_absolute answers a question worth asking before you join paths or
     * resolve one against a base directory: does this path already start from
     * the root? The rule differs per platform - a leading '/' here, a drive
     * letter or a UNC prefix on Windows - which is exactly why it is a call and
     * not a comparison against '/'. */
    EXAMPLE_REQUIRE(!proven_fs_is_absolute(live), "the working paths in this example are relative");
    EXAMPLE_REQUIRE(proven_fs_is_absolute(PROVEN_LIT("/etc/hosts")), "a leading slash is absolute on POSIX");

    static const record_t initial[] = {
        { .id = 1, .score = 10 }, { .id = 2, .score = 20 },
        { .id = 3, .score = 30 }, { .id = 4, .score = 40 },
    };

    /* --- an ordinary first write ------------------------------------------ */

    proven_result_file_t f = proven_fs_open(alloc, live, PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
    EXAMPLE_REQUIRE(proven_is_ok(f.err), "creating the data file must succeed");
    if (!proven_is_ok(f.err)) return 1;

    proven_err_t err = write_records(f.value, initial, 4);
    EXAMPLE_REQUIRE(proven_is_ok(err), "writing the initial records must succeed");
    err = proven_fs_close(f.value);
    EXAMPLE_REQUIRE(proven_is_ok(err), "closing after a write must succeed");

    /* --- an advisory lock, so two copies do not interleave ---------------- */

    proven_result_file_t rw = proven_fs_open(alloc, live, PROVEN_FS_READ | PROVEN_FS_WRITE);
    EXAMPLE_REQUIRE(proven_is_ok(rw.err), "reopening for update must succeed");
    if (!proven_is_ok(rw.err)) return 1;

    /* An EXCLUSIVE lock keeps every other process that also asks for one out.
     * "Advisory" means exactly that: it stops cooperating programs, and a
     * program that never asks for the lock is not affected. `wait = false`
     * returns immediately rather than blocking - the right choice when you have
     * something else to do, and the only safe choice when the other holder
     * might be waiting on you. */
    err = proven_fs_lock(rw.value, PROVEN_FS_LOCK_EXCLUSIVE, false);
    EXAMPLE_REQUIRE(proven_is_ok(err), "taking the exclusive lock must succeed when nobody holds it");

    /* --- reading and writing one record, by offset ------------------------ */

    /* pread reads at an absolute offset and does NOT move the file position.
     * That is what makes it safe to use from two threads sharing one handle:
     * there is no shared cursor for them to race on. */
    record_t third = {0};
    proven_mem_mut_t into = { .ptr = (proven_byte_t *)&third, .size = sizeof third };
    proven_result_size_t got = proven_fs_pread(rw.value, into, 2 * sizeof(record_t));
    EXAMPLE_REQUIRE(proven_is_ok(got.err) && got.value == sizeof third, "reading record 2 must succeed");
    EXAMPLE_REQUIRE(third.id == 3 && third.score == 30, "and yield the record that was written there");

    /* tell reports the position; after a pread it has not moved. */
    proven_result_u64_t pos = proven_fs_tell(rw.value);
    EXAMPLE_REQUIRE(proven_is_ok(pos.err) && pos.val == 0, "pread must not move the file position");

    /* pwrite updates that record in place, again without touching the cursor. */
    third.score = 99;
    proven_mem_view_t out_view = { .ptr = (const proven_byte_t *)&third, .size = sizeof third };
    proven_result_size_t put = proven_fs_pwrite(rw.value, out_view, 2 * sizeof(record_t));
    EXAMPLE_REQUIRE(proven_is_ok(put.err) && put.value == sizeof third, "writing record 2 back must succeed");

    /* seek is the cursor-moving alternative, and it returns the position it
     * arrived at. Seeking from the END with a negative offset is how you find
     * the last record without knowing the file length first. */
    proven_result_u64_t last = proven_fs_seek(rw.value, -(proven_i64)sizeof(record_t), PROVEN_FS_SEEK_END);
    EXAMPLE_REQUIRE(proven_is_ok(last.err), "seeking to the last record must succeed");
    EXAMPLE_REQUIRE(last.val == 3 * sizeof(record_t), "which is three records in");

    pos = proven_fs_tell(rw.value);
    EXAMPLE_REQUIRE(proven_is_ok(pos.err) && pos.val == last.val, "tell agrees with the seek result");

    /* truncate sets the length directly. Dropping the last record is one call
     * and O(1); the old way - read everything, write back the part you keep -
     * was an O(n) copy for an operation the filesystem does by adjusting a
     * number. */
    err = proven_fs_truncate(rw.value, 3 * sizeof(record_t));
    EXAMPLE_REQUIRE(proven_is_ok(err), "truncating to three records must succeed");
    proven_result_size_t size = proven_fs_size(rw.value);
    EXAMPLE_REQUIRE(proven_is_ok(size.err) && size.value == 3 * sizeof(record_t),
                    "the file is now exactly three records long");

    /* Release the lock explicitly. Closing the handle would also drop it, but
     * saying so keeps the critical section visible in the code. */
    err = proven_fs_lock(rw.value, PROVEN_FS_LOCK_UNLOCK, false);
    EXAMPLE_REQUIRE(proven_is_ok(err), "releasing the lock must succeed");
    err = proven_fs_close(rw.value);
    EXAMPLE_REQUIRE(proven_is_ok(err), "closing the update handle must succeed");

    /* --- the durable replace ---------------------------------------------- */

    static const record_t replacement[] = {
        { .id = 1, .score = 11 }, { .id = 2, .score = 22 },
    };

    /* 1. Write the new contents beside the old file. CREATE_NEW refuses if the
     *    temporary name already exists, which is how a leftover from a crashed
     *    run is noticed instead of silently reused. */
    proven_result_file_t tmp = proven_fs_open(alloc, temp,
                                              PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
    EXAMPLE_REQUIRE(proven_is_ok(tmp.err), "creating the temporary file must succeed");
    if (!proven_is_ok(tmp.err)) return 1;

    err = write_records(tmp.value, replacement, 2);
    EXAMPLE_REQUIRE(proven_is_ok(err), "writing the new contents must succeed");

    /* 2. Push those bytes all the way to the storage device. This is expensive
     *    and meant to be: you are buying the guarantee that the data exists
     *    after a power cut, and the price is a real trip to the device. */
    err = proven_fs_sync(tmp.value);
    EXAMPLE_REQUIRE(proven_is_ok(err), "syncing the new file's data must succeed");

    err = proven_fs_close(tmp.value);
    EXAMPLE_REQUIRE(proven_is_ok(err), "closing the temporary file must succeed");

    /* 3. Flip the name. A rename within one directory is atomic: any reader
     *    sees the old file or the new one, never a partial write. */
    err = proven_fs_rename(alloc, temp, live);
    EXAMPLE_REQUIRE(proven_is_ok(err), "renaming the temporary file over the live one must succeed");

    /* 4. Make the rename itself durable. Until the directory reaches the device,
     *    the new contents are safe under a name that might not be. */
    err = proven_fs_sync_dir(alloc, here);
    EXAMPLE_REQUIRE(proven_is_ok(err), "syncing the directory must succeed");

    proven_result_file_t check = proven_fs_open(alloc, live, PROVEN_FS_READ);
    EXAMPLE_REQUIRE(proven_is_ok(check.err), "the live path must now open");
    size = proven_fs_size(check.value);
    EXAMPLE_REQUIRE(proven_is_ok(size.err) && size.value == 2 * sizeof(record_t),
                    "and hold exactly the replacement records");
    err = proven_fs_close(check.value);
    EXAMPLE_REQUIRE(proven_is_ok(err), "closing the verification handle must succeed");

    /* --- copies, and the two kinds of link -------------------------------- */

    /* copy duplicates the bytes: two independent files from here on. The
     * allocator is for the temporary buffer the copy moves data through. */
    proven_u8str_view_t backup = PROVEN_LIT("proven_example_durable.bak");
    err = proven_fs_copy(alloc, live, backup);
    EXAMPLE_REQUIRE(proven_is_ok(err), "copying the file must succeed");

    /* A HARD link is a second name for the same file. There is no original: the
     * data lives until the last name is removed. Both names must be on the same
     * filesystem, because a name and its data cannot span two. */
    proven_u8str_view_t hard = PROVEN_LIT("proven_example_durable.hard");
    err = proven_fs_link(alloc, live, hard);
    EXAMPLE_REQUIRE(proven_is_ok(err), "creating a hard link must succeed");

    proven_fs_stat_t st_live = {0}, st_hard = {0};
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_stat(alloc, live, &st_live)), "stat of the live name");
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_stat(alloc, hard, &st_hard)), "stat of the hard link");
    EXAMPLE_REQUIRE(st_live.ino == st_hard.ino && st_live.dev == st_hard.dev,
                    "both names refer to the same file, which is what a hard link means");

    /* A SYMBOLIC link is a small file holding a path. It may point at something
     * on another filesystem, and it may point at nothing at all - following it
     * then fails, which a hard link can never do. */
    proven_u8str_view_t soft = PROVEN_LIT("proven_example_durable.link");
    err = proven_fs_symlink(alloc, live, soft);
    EXAMPLE_REQUIRE(proven_is_ok(err), "creating a symbolic link must succeed");

    proven_fs_stat_t st_soft = {0};
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_stat(alloc, soft, &st_soft)),
                    "stat follows the symbolic link to its target");
    EXAMPLE_REQUIRE(st_soft.size == st_live.size, "so it reports the target's size");

    /* --- directories, and cleaning up ------------------------------------- */

    proven_u8str_view_t dir = PROVEN_LIT("proven_example_durable_dir");
    err = proven_fs_mkdir(alloc, dir);
    EXAMPLE_REQUIRE(proven_is_ok(err), "creating a directory must succeed");

    /* rmdir removes an EMPTY directory only. That refusal is a feature: a
     * recursive delete is a decision the caller should have to make explicitly,
     * not something a stray path argument can trigger. */
    proven_u8str_view_t inside = PROVEN_LIT("proven_example_durable_dir/file.txt");
    proven_result_file_t child = proven_fs_open(alloc, inside, PROVEN_FS_WRITE | PROVEN_FS_CREATE);
    EXAMPLE_REQUIRE(proven_is_ok(child.err), "creating a file inside it must succeed");
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_close(child.value)), "closing it must succeed");

    err = proven_fs_rmdir(alloc, dir);
    EXAMPLE_REQUIRE(err != PROVEN_OK, "removing a non-empty directory must be refused");

    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_remove(alloc, inside)), "removing the file must succeed");
    err = proven_fs_rmdir(alloc, dir);
    EXAMPLE_REQUIRE(proven_is_ok(err), "and then the empty directory can be removed");

    printf("durable replace complete: %zu byte(s) live\n", (size_t)size.value);

    (void)proven_fs_remove(alloc, soft);
    (void)proven_fs_remove(alloc, hard);
    (void)proven_fs_remove(alloc, backup);
    (void)proven_fs_remove(alloc, live);
    return EXAMPLE_OK();
}
