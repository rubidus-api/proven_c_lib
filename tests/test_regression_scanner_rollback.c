#include "proven/sysio.h"
#include "proven/fs.h"
#include "proven/heap.h"
#include "proven_test.h"
#include <stdbool.h>

/*
 * sysio.h documents that when a token cannot fit the scanner's buffer, the scan
 * "returns PROVEN_ERR_OUT_OF_BOUNDS and restores the scanner state and file
 * position ... instead of accepting a truncated token."
 *
 * scanner_fill compacts the buffer (it memmoves the unconsumed bytes to the
 * front and resets the cursor to 0). The rollback used to restore the cursor and
 * length captured *before* that compaction, against the buffer that existed
 * *after* it - so the indices no longer described the same bytes. The observable
 * result was a corrupted stream: a byte dropped from the front, and the bytes
 * pulled in by the failed call re-read a second time.
 *
 * This test pins the contract that matters to a caller: a scan that fails must
 * leave the stream exactly where the previous successful scan left it.
 */

int main(void) {
    PROVEN_TEST_SUITE("scanner rollback after a failed scan",
        "A scan that fails on an oversized token must leave the byte stream untouched, not shifted or duplicated.",
        "Inspect scanner_fill's compaction and the rollback in proven_sysio_scanner_scan_impl; the snapshot must be reconciled with how far the buffer moved.");

    proven_allocator_t heap = proven_heap_allocator();
    proven_u8str_view_t path = PROVEN_LIT("test_scanner_rollback.txt");

    /* A small first token, then a token far too big for the scanner buffer,
     * then a token we must still be able to read afterwards. */
    proven_u8str_view_t content = PROVEN_LIT("1 2222222222222 7");
    proven_err_t w = proven_fs_write_file(heap, path, proven_mem_view_from_u8(content));
    PROVEN_TEST_ASSERT(proven_is_ok(w), "failed to write the fixture file", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a failed oversized-token scan does not corrupt the stream",
        "After the failure the next byte read must be the one right after the last committed token.",
        "If the recovered value is wrong, the rollback is restoring pre-compaction indices into a compacted buffer.");
    // ---------------------------------------------------------------
    proven_result_file_t f = proven_fs_open(heap, path, PROVEN_FS_READ);
    PROVEN_TEST_ASSERT(proven_is_ok(f.err), "failed to open the fixture file", "");

    proven_sysio_scanner_t sc;
    /* Capacity deliberately smaller than the middle token. */
    proven_err_t e = proven_sysio_scanner_init(&sc, f.value, heap, 8);
    PROVEN_TEST_ASSERT(proven_is_ok(e), "scanner_init failed", "");

    int first = 0;
    e = proven_sysio_scanner_scan(&sc, "{}", PROVEN_SCAN_ARG(&first));
    PROVEN_TEST_ASSERT(proven_is_ok(e), "the first token must scan", "");
    PROVEN_TEST_ASSERT(first == 1, "the first token must be 1", "");

    /* Snapshot the bytes the scanner holds but has not consumed. After a failed
     * scan these exact bytes - same count, same content - must still be the ones
     * waiting. */
    proven_byte_t before[64];
    proven_size_t before_len = sc.length - sc.cursor;
    PROVEN_TEST_ASSERT(before_len > 0 && before_len <= sizeof before,
        "the fixture must leave unconsumed bytes buffered", "");
    for (proven_size_t i = 0; i < before_len; ++i) {
        before[i] = ((const proven_byte_t*)sc.buffer)[sc.cursor + i];
    }

    /* This token cannot fit the 8-byte buffer: it must fail, and it must undo
     * itself completely. */
    unsigned long long big = 0;
    e = proven_sysio_scanner_scan(&sc, "{}", PROVEN_SCAN_ARG(&big));
    PROVEN_TEST_ASSERT(!proven_is_ok(e),
        "an oversized token must not be accepted",
        "the scanner must refuse a truncated token rather than return a partial value");

    /* The contract, checked where it actually broke. scanner_fill compacts the
     * buffer; the rollback used to write back the cursor and length captured
     * before that compaction. The scanner came out believing its unconsumed
     * bytes began one byte further in - dropping the separator - and ran one
     * byte longer, duplicating a byte the file rewind had already put back. */
    proven_size_t after_len = sc.length - sc.cursor;
    PROVEN_TEST_ASSERT(after_len == before_len,
        "a failed scan changed how many bytes the scanner holds unconsumed",
        "the rollback is restoring indices that no longer match the compacted buffer");
    bool same = true;
    for (proven_size_t i = 0; i < before_len; ++i) {
        if (((const proven_byte_t*)sc.buffer)[sc.cursor + i] != before[i]) same = false;
    }
    PROVEN_TEST_ASSERT(same,
        "a failed scan changed which bytes the scanner holds unconsumed",
        "the rollback must account for how far scanner_fill moved the buffer contents");

    /* And the stream really is back where it was: retrying the oversized token
     * must fail the same way, not succeed with a value stitched from stale
     * bytes. */
    unsigned long long again = 0;
    e = proven_sysio_scanner_scan(&sc, "{}", PROVEN_SCAN_ARG(&again));
    PROVEN_TEST_ASSERT(!proven_is_ok(e),
        "a restored scanner must fail the same way on a retry",
        "if this now succeeds, the failed scan consumed or shifted bytes it promised to restore");

    proven_sysio_scanner_deinit(&sc);
    proven_fs_close(f.value);

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a buffer large enough reads every token in order",
        "With a buffer that fits the tokens, the same file must scan cleanly - proving the fixture itself is sound.",
        "If this fails, the fixture or the scanner's normal path is broken, not the rollback.");
    // ---------------------------------------------------------------
    f = proven_fs_open(heap, path, PROVEN_FS_READ);
    PROVEN_TEST_ASSERT(proven_is_ok(f.err), "failed to reopen the fixture file", "");
    e = proven_sysio_scanner_init(&sc, f.value, heap, 64);
    PROVEN_TEST_ASSERT(proven_is_ok(e), "scanner_init failed", "");

    int a = 0;
    unsigned long long b = 0;
    int c = 0;
    e = proven_sysio_scanner_scan(&sc, "{}", PROVEN_SCAN_ARG(&a));
    PROVEN_TEST_ASSERT(proven_is_ok(e) && a == 1, "token 1 must scan as 1", "");
    e = proven_sysio_scanner_scan(&sc, "{}", PROVEN_SCAN_ARG(&b));
    PROVEN_TEST_ASSERT(proven_is_ok(e) && b == 2222222222222ULL, "token 2 must scan in full", "");
    e = proven_sysio_scanner_scan(&sc, "{}", PROVEN_SCAN_ARG(&c));
    PROVEN_TEST_ASSERT(proven_is_ok(e) && c == 7,
        "token 3 must scan as 7",
        "a wrong value here means bytes were dropped or duplicated between tokens");

    proven_sysio_scanner_deinit(&sc);
    proven_fs_close(f.value);

    (void)proven_fs_remove(heap, path);

    PROVEN_TEST_PASS("scanner rollback leaves the stream intact.");
    return 0;
}
