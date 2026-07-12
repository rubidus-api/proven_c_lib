#include "example.h"

/*
 * Walking a tree.
 *
 * The three things a recursive walker gets wrong, and what this one does instead:
 *
 *   It loops.       A symlink pointing at an ancestor is a cycle. This walk never descends
 *                   THROUGH a symlink - the symlinked directory is still reported, it is
 *                   simply not entered - so a cycle is impossible, and so is walking out of
 *                   the tree you asked about and into the rest of the filesystem.
 *
 *   It lies.        A directory it cannot read gets skipped, and the walk reports success.
 *                   That is how a backup misses a subtree. Here the error comes back from
 *                   proven_fs_walk_next, with the entry naming the directory, and the walk
 *                   carries on from the next sibling. You decide what to do about it.
 *
 *   It bloats.      Reading a whole directory into memory before yielding anything makes a
 *                   walk of a big tree cost a big allocation. This one holds one open handle
 *                   per LEVEL of the current path and one reused path buffer - so its memory
 *                   is a function of depth, not of how many files there are.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* A small tree to walk: a file, a directory, a file inside it. */
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_mkdir(alloc, PROVEN_LIT("ex_walk"))), "mkdir");
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_mkdir(alloc, PROVEN_LIT("ex_walk/inner"))), "mkdir inner");
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_write_file(alloc, PROVEN_LIT("ex_walk/top.txt"),
        proven_mem_view_from_u8(PROVEN_LIT("top")))), "write top.txt");
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_write_file(alloc, PROVEN_LIT("ex_walk/inner/deep.txt"),
        proven_mem_view_from_u8(PROVEN_LIT("deep")))), "write deep.txt");

    proven_result_walk_t walk = proven_fs_walk_open(alloc, PROVEN_LIT("ex_walk"),
                                                    PROVEN_FS_WALK_UNLIMITED);
    EXAMPLE_REQUIRE(proven_is_ok(walk.err), "the walk should open");

    proven_size_t files = 0;
    proven_size_t dirs = 0;
    proven_size_t unreadable = 0;
    proven_size_t total_bytes = 0;

    for (;;) {
        proven_fs_walk_entry_t entry = {0};
        proven_err_t err = proven_fs_walk_next(&walk.value, &entry);

        if (err == PROVEN_ERR_EOF) break;

        if (!proven_is_ok(err)) {
            /* A directory that could not be read, or one deeper than the walk's stack. It is
             * REPORTED, not skipped - `entry.path` says which one - and the walk goes on. A
             * tool that copies a tree should fail here; one that reports on a tree should
             * count it and say so. What it must not do is pretend it did not happen. */
            unreadable++;
            continue;
        }

        /* `entry.path` and `entry.name` are borrowed: they point into the walk's one reused
         * buffer and are valid until the next call. Copy them if you need them longer. */
        if (entry.type == PROVEN_FS_TYPE_DIR) {
            dirs++;
        } else if (entry.type == PROVEN_FS_TYPE_FILE) {
            files++;
            total_bytes += entry.size;
        }
    }

    proven_fs_walk_close(&walk.value);

    EXAMPLE_REQUIRE(files == 2, "two files: top.txt and inner/deep.txt");
    EXAMPLE_REQUIRE(dirs == 1, "one directory: inner");
    EXAMPLE_REQUIRE(unreadable == 0, "and nothing in this tree is unreadable");
    EXAMPLE_REQUIRE(total_bytes == 7, "three bytes plus four");

    /* Depth-limited: max_depth 0 reports what is directly inside the root and descends
     * nowhere. The directory at the limit is still an entry, so it is still reported. */
    walk = proven_fs_walk_open(alloc, PROVEN_LIT("ex_walk"), 0);
    EXAMPLE_REQUIRE(proven_is_ok(walk.err), "the shallow walk should open");

    proven_size_t shallow = 0;
    for (;;) {
        proven_fs_walk_entry_t entry = {0};
        proven_err_t err = proven_fs_walk_next(&walk.value, &entry);
        if (err == PROVEN_ERR_EOF) break;
        if (proven_is_ok(err)) shallow++;
    }
    proven_fs_walk_close(&walk.value);

    EXAMPLE_REQUIRE(shallow == 2, "top.txt and inner - but nothing inside inner");

    (void)proven_fs_remove(alloc, PROVEN_LIT("ex_walk/inner/deep.txt"));
    (void)proven_fs_remove(alloc, PROVEN_LIT("ex_walk/top.txt"));
    (void)proven_fs_remove(alloc, PROVEN_LIT("ex_walk/inner"));
    (void)proven_fs_remove(alloc, PROVEN_LIT("ex_walk"));

    return EXAMPLE_OK();
}
