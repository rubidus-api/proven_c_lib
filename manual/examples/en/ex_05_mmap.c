#include "example.h"
#include <string.h>

/*
 * A memory-mapped file is a file the processor reads as memory: the operating
 * system arranges for the pages to appear at an address, and reads happen
 * without a read call. It is the right tool for one shape of problem - random
 * access into a large file that several processes look at - and the wrong tool
 * for streaming, where a buffered reader is simpler and does not tie a file's
 * size to your address space.
 *
 * The part that is easy to get wrong is durability, and it is the reason this
 * example exists:
 *
 *   PROVEN_MMAP_SHARED  - writes go to the file. proven_mmap_sync pushes them
 *                         to the storage device.
 *   PROVEN_MMAP_PRIVATE - writes are copy-on-write: they exist in this process
 *                         and nowhere else. There is nothing to write back, and
 *                         asking to sync one says so instead of quietly doing
 *                         nothing.
 *
 * This example also shows the calendar formatter alongside it, because the
 * record it writes carries a timestamp - and a timestamp written for a Windows
 * API is the one place the UTF-16 formatter is the right call.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();
    proven_u8str_view_t path = PROVEN_LIT("proven_example_mmap.dat");

    /* A mapping cannot extend a file, so the file has to be the size you intend
     * to map before you map it. */
    static const char initial[] = "record 0: pending    \n";
    proven_result_file_t create = proven_fs_open(alloc, path,
                                                 PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
    EXAMPLE_REQUIRE(proven_is_ok(create.err), "creating the backing file must succeed");
    if (!proven_is_ok(create.err)) return 1;

    proven_mem_view_t seed = { .ptr = (const proven_byte_t *)initial, .size = sizeof initial - 1 };
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_write_all(create.value, seed)), "writing the record must succeed");
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_close(create.value)), "closing it must succeed");

    /* --- a shared mapping: writes reach the file -------------------------- */

    proven_result_file_t f = proven_fs_open(alloc, path, PROVEN_FS_READ | PROVEN_FS_WRITE);
    EXAMPLE_REQUIRE(proven_is_ok(f.err), "opening the file for mapping must succeed");
    if (!proven_is_ok(f.err)) return 1;

    /* size 0 means "to the end of the file". */
    proven_result_mmap_t m = proven_mmap_create(f.value, 0, 0,
                                                PROVEN_MMAP_READ | PROVEN_MMAP_WRITE,
                                                PROVEN_MMAP_SHARED);
    EXAMPLE_REQUIRE(proven_is_ok(m.err), "mapping the file must succeed");
    if (!proven_is_ok(m.err)) {
        (void)proven_fs_close(f.value);
        return 1;
    }
    proven_mmap_t map = m.value;

    /* Reading is just reading memory - no call, no copy. as_view hands back the
     * whole mapping as a byte view, so the ordinary view helpers apply. */
    proven_u8str_view_t contents = proven_mmap_as_view(map);
    EXAMPLE_REQUIRE(contents.size == sizeof initial - 1, "the mapping covers the whole file");
    EXAMPLE_REQUIRE(proven_u8str_view_starts_with(contents, PROVEN_LIT("record 0:")),
                    "and shows the bytes that were written");

    /* Writing is writing memory. The status field is a fixed width on purpose:
     * a mapping cannot make the file longer, so an in-place edit has to fit the
     * space that is already there. */
    proven_size_t at = proven_u8str_view_find(contents, 0, PROVEN_LIT("pending"));
    EXAMPLE_REQUIRE(at != PROVEN_SIZE_MAX, "the status field must be found");
    memcpy((proven_byte_t *)map.ptr + at, "done   ", 7);

    /* sync is the durability step: without it the change is in the page cache,
     * where it survives this program exiting but not the machine losing power. */
    EXAMPLE_REQUIRE(proven_is_ok(proven_mmap_sync(&map)), "syncing a shared mapping must succeed");
    EXAMPLE_REQUIRE(proven_is_ok(proven_mmap_destroy(&map)), "unmapping must succeed");
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_close(f.value)), "closing the mapped file must succeed");

    /* The edit is in the file, not merely in this process's memory. */
    proven_result_u8str_t back = proven_fs_read_all_u8str(alloc, path);
    EXAMPLE_REQUIRE(proven_is_ok(back.err), "reading the file back must succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_find(proven_u8str_as_view(&back.value), 0, PROVEN_LIT("done")) != PROVEN_SIZE_MAX,
                    "the mapped write reached the file");
    proven_u8str_destroy(alloc, &back.value);

    /* --- a private mapping: writes go nowhere ----------------------------- */

    proven_result_file_t pf = proven_fs_open(alloc, path, PROVEN_FS_READ);
    EXAMPLE_REQUIRE(proven_is_ok(pf.err), "reopening for a private mapping must succeed");

    proven_result_mmap_t pm = proven_mmap_create(pf.value, 0, 0, PROVEN_MMAP_READ, PROVEN_MMAP_PRIVATE);
    EXAMPLE_REQUIRE(proven_is_ok(pm.err), "a private read mapping must succeed");
    proven_mmap_t priv = pm.value;

    /* Asking to sync a private mapping is refused rather than accepted and
     * ignored. The refusal is the useful behaviour: a caller who believed the
     * mapping was shared finds out here, not when the data is missing. */
    EXAMPLE_REQUIRE(proven_mmap_sync(&priv) == PROVEN_ERR_UNSUPPORTED,
                    "a private mapping has nothing to write back, and says so");

    EXAMPLE_REQUIRE(proven_is_ok(proven_mmap_destroy(&priv)), "unmapping the private mapping must succeed");
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_close(pf.value)), "closing it must succeed");

    /* --- the timestamp, in both string types ------------------------------ */

    proven_datetime_t now = proven_time_now_datetime();

    proven_result_u8str_t stamp = proven_u8str_create(alloc, 64);
    EXAMPLE_REQUIRE(proven_is_ok(stamp.err), "creating the timestamp string must succeed");
    EXAMPLE_REQUIRE(proven_is_ok(proven_time_u8_fmt(alloc, &stamp.value, now, &proven_time_locale_en,
                                                    "{year}-{month:0>2}-{day:0>2}")),
                    "formatting the date as UTF-8 must succeed");
    EXAMPLE_REQUIRE(proven_u8str_as_view(&stamp.value).size == 10, "a YYYY-MM-DD date is ten characters");

    /* The UTF-16 form of the same call, for the one place it is the right one:
     * handing the text straight to a system call that takes wide strings. */
    proven_result_u16str_t wide = proven_u16str_create(alloc, 64);
    EXAMPLE_REQUIRE(proven_is_ok(wide.err), "creating the wide timestamp string must succeed");
    EXAMPLE_REQUIRE(proven_is_ok(proven_time_u16_fmt(alloc, &wide.value, now, &proven_time_locale_en,
                                                     "{year}-{month:0>2}-{day:0>2}")),
                    "formatting the same date as UTF-16 must succeed");
    EXAMPLE_REQUIRE(proven_u16str_len(&wide.value) == 10, "ten code units, one per character here");
    EXAMPLE_REQUIRE(proven_u16str_as_ptr(&wide.value)[4] == (proven_u16)'-',
                    "and the same layout as the UTF-8 form");

    printf("mapped record updated; stamped %s\n", proven_u8str_as_cstr(&stamp.value));

    proven_u16str_destroy(alloc, &wide.value);
    proven_u8str_destroy(alloc, &stamp.value);
    (void)proven_fs_remove(alloc, path);
    return EXAMPLE_OK();
}
