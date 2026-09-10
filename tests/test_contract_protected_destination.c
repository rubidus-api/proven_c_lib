/*
 * One rule, every door.
 *
 * A destination whose owner-write bit is clear is the caller saying: do not write this
 * file. Every whole-file replacement in the public API refuses one with
 * PROVEN_ERR_PERMISSION and leaves it exactly as it was.
 *
 * This test exists because the rule was true of some doors and not others, and nobody
 * could see that by reading. Measured before it was one rule: on ONE platform, the same
 * request gave three answers - proven_fs_write_file refused, proven_fs_write_file_atomic
 * succeeded, and proven_fs_copy succeeded AND left a 0444 file as 0664, so a protection
 * the caller had set was gone with nothing saying so. proven_fs_rename was worse than any
 * of them: it is what the atomic write is BUILT ON, so a caller refused by one got the
 * result from the other, and the rule was one line of caller code from being void.
 *
 * A table of doors, checked one by one, is the only form this can take. A rule that holds
 * for the functions somebody remembered is not a rule.
 */

#include "proven.h"
#include "proven_test.h"

#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#define PLATFORM_IS_POSIX 0
#else
#define PLATFORM_IS_POSIX 1
#include <unistd.h>
#include <sys/stat.h>
#endif

static proven_allocator_t heap;

static proven_u8str_view_t V(const char *s) {
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}

static proven_mem_view_t B(const char *s) {
    return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}

#define DIR   "test_protected.d"
#define PROT  DIR "/protected.txt"
#define SRC   DIR "/source.txt"

/* A 0444 file holding KEEP, fresh for every door. */
static void seed(void) {
    (void)proven_fs_chmod(heap, V(PROT), (proven_fs_perms_t)0600u);
    (void)proven_fs_remove(heap, V(PROT));
    (void)proven_fs_write_file(heap, V(PROT), B("KEEP"));
    (void)proven_fs_chmod(heap, V(PROT), (proven_fs_perms_t)0444u);
    (void)proven_fs_write_file(heap, V(SRC), B("SOURCE"));
}

static bool still_intact(void) {
    proven_fs_stat_t st = {0};
    if (!proven_is_ok(proven_fs_stat(heap, V(PROT), &st))) return false;
    if ((st.perms & 0777u) != 0444u) return false;
    proven_result_mem_mut_t r = proven_fs_read_all(heap, V(PROT));
    if (!proven_is_ok(r.err)) return false;
    bool same = r.value.size == 4 && memcmp(r.value.ptr, "KEEP", 4) == 0;
    heap.free_fn(heap.ctx, r.value.ptr);
    return same;
}

/* Every door gets the same two questions: did you refuse, and did you leave it alone. */
static void door(const char *name, proven_err_t got) {
    PROVEN_TEST_ASSERT(got == PROVEN_ERR_PERMISSION,
        "this door must refuse a protected destination with PROVEN_ERR_PERMISSION",
        "One door that does not refuse is the whole rule: a caller reaches for whichever function suits them, and which one they picked is not a decision about permissions.");
    PROVEN_TEST_ASSERT(still_intact(),
        "and it must leave the file exactly as it was - contents AND mode",
        "proven_fs_copy used to leave a 0444 file as 0664. The protection was gone and nothing said so; that is the defect this rule exists to prevent, and a refusal that half-does the work repeats it.");
    PROVEN_TEST_INFO("{} refused, file untouched", PROVEN_ARG((const char *)name));
}

int main(void) {
    PROVEN_TEST_SUITE("a protected destination is refused by every door",
        "Read-only means do not write this file. That has to be true of every function that replaces one, or it is true of none of them.",
        "A failure names the door. Either it stopped refusing, or it refused and damaged the file anyway.");

    heap = proven_heap_allocator();
    (void)proven_fs_mkdir(heap, V(DIR));

#if PLATFORM_IS_POSIX
    if (geteuid() == 0) {
        PROVEN_TEST_INFO("SKIP: running as root, which is not refused by file modes at all.");
        PROVEN_TEST_PASS("skipped: root ignores the permission bits this test is about.");
        return 0;
    }
#endif

    /* The filesystem must honour the mode, or none of this means anything. A share with
     * inherited ACLs can hand back a wider mode than was asked for. */
    seed();
    {
        proven_fs_stat_t st = {0};
        if (!proven_is_ok(proven_fs_stat(heap, V(PROT), &st)) || (st.perms & 0200u) != 0u) {
            PROVEN_TEST_INFO("SKIP: this filesystem did not honour a 0444 chmod, so a protected file cannot be made here.");
            (void)proven_fs_chmod(heap, V(PROT), (proven_fs_perms_t)0600u);
            (void)proven_fs_remove(heap, V(PROT));
            (void)proven_fs_remove(heap, V(SRC));
            (void)proven_fs_rmdir(heap, V(DIR));
            PROVEN_TEST_PASS("skipped: creation modes are not honoured on this filesystem.");
            return 0;
        }
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("every door that replaces a file refuses a protected one",
        "The doors are: opening it for writing, the three whole-file writes, the copy, and the rename the atomic write is built on.",
        "The list is the point. Add a door to the public API and add it here, or the rule quietly stops covering the API.");
    // ---------------------------------------------------------------
    {
        seed(); door("open(WRITE)",       proven_fs_open(heap, V(PROT), PROVEN_FS_WRITE).err);
        seed(); door("open(WRITE|TRUNC)", proven_fs_open(heap, V(PROT), (proven_fs_mode_t)(PROVEN_FS_WRITE | PROVEN_FS_TRUNC)).err);
        seed(); door("open(APPEND)",      proven_fs_open(heap, V(PROT), PROVEN_FS_APPEND).err);
        seed(); door("write_file",        proven_fs_write_file(heap, V(PROT), B("NEW")));
        seed(); door("write_file_atomic", proven_fs_write_file_atomic(heap, V(PROT), B("NEW")));
        seed(); door("write_file_durable",proven_fs_write_file_durable(heap, V(PROT), B("NEW")));
        seed(); door("copy (destination)",proven_fs_copy(heap, V(SRC), V(PROT)));
        seed(); door("rename (destination)", proven_fs_rename(heap, V(SRC), V(PROT)));
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("lifting the mark is the caller's answer, and it works",
        "A refusal that cannot be recovered from is a wall, not a rule.",
        "If this fails, the refusal is being decided somewhere other than the mode - find out where.");
    // ---------------------------------------------------------------
    {
        seed();
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_chmod(heap, V(PROT), (proven_fs_perms_t)0600u)),
            "the caller clears the mark", "");
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_write_file_atomic(heap, V(PROT), B("NEW"))),
            "and the write goes through", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("removing is NOT covered, on purpose, and says so when it cannot",
        "Deleting a name is a directory operation. POSIX has never let the file's own mode have a say in it, and refusing here would break ordinary cleanup of read-only files for a rule about writing.",
        "The platforms genuinely differ - Windows refuses to delete a read-only file - so what is pinned here is that the answer is one of two NAMED answers, never a bare I/O error.");
    // ---------------------------------------------------------------
    {
        seed();
        proven_err_t rm = proven_fs_remove(heap, V(PROT));
        PROVEN_TEST_ASSERT(rm == PROVEN_OK || rm == PROVEN_ERR_PERMISSION,
            "remove either deletes it (POSIX) or refuses with PROVEN_ERR_PERMISSION (Windows)",
            "A bare PROVEN_ERR_IO here is the thing being prevented: a caller cannot tell 'clear the mark and retry' from 'the disk is broken'.");
        PROVEN_TEST_INFO("remove on a protected file answered {}",
            PROVEN_ARG((const char *)(rm == PROVEN_OK ? "PROVEN_OK (deleted)" : "PROVEN_ERR_PERMISSION (refused)")));
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a refusal names ITSELF, so a caller can act on it",
        "Not there, may not, held by someone else, and broken are four problems with four answers. They used to be one.",
        "Inspect the open and rename mappings in src/proven/fs.c and the _checked functions in the platform layer.");
    // ---------------------------------------------------------------
    {
        proven_result_file_t missing = proven_fs_open(heap, V(DIR "/no-such-file"), PROVEN_FS_READ);
        PROVEN_TEST_ASSERT(missing.err == PROVEN_ERR_NOT_FOUND,
            "opening a name that is not there is PROVEN_ERR_NOT_FOUND",
            "It used to be PROVEN_ERR_IO, which a caller cannot tell from a failing disk.");

        seed();
        proven_result_file_t denied = proven_fs_open(heap, V(PROT), PROVEN_FS_WRITE);
        PROVEN_TEST_ASSERT(denied.err == PROVEN_ERR_PERMISSION,
            "opening a protected file for writing is PROVEN_ERR_PERMISSION", "");
    }

    /* Leave nothing behind. */
    (void)proven_fs_chmod(heap, V(PROT), (proven_fs_perms_t)0600u);
    (void)proven_fs_remove(heap, V(PROT));
    (void)proven_fs_remove(heap, V(SRC));
    (void)proven_fs_rmdir(heap, V(DIR));

    PROVEN_TEST_PASS("every door refuses a protected destination the same way, and says which refusal it is.");
    return 0;
}
