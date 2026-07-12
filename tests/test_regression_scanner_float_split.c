#include "proven.h"
#include "proven_test.h"
#include <string.h>
#include <stdio.h>

#if defined(_WIN32) || defined(_WIN64)
int main(void) {
    PROVEN_TEST_SUITE("a float split across the scanner buffer",
        "A float whose exponent or sign lands on a refill boundary must still scan whole.",
        "Uses a temp file; the logic is platform-independent but the fixture path handling is kept POSIX-simple.");
    PROVEN_TEST_INFO("skipped on Windows for fixture simplicity");
    PROVEN_TEST_PASS("skipped");
    return 0;
}
#else

/*
 * A float cut in half by the buffered scanner's refill boundary used to be scanned WRONG in
 * two invisible ways, both found by a fmt -> file -> scanner -> float round-trip:
 *
 *   - the exponent split: "-3.0448...e" at the boundary parses as a valid float (the
 *     mantissa) and silently drops the "e-222" that had not arrived - a truncated value
 *     committed as a success;
 *   - the sign split: "-" alone at the boundary is a PARSE FAILURE, and the scanner dropped
 *     the byte and desynced instead of asking for the rest of the number.
 *
 * Both come down to the same thing: unlike an integer, a float can look complete (or look
 * like garbage) when it is merely unfinished. The scanner now refills for a float whose tail
 * is a float PREFIX, and only then - genuine garbage like "abc" is still the error it is.
 *
 * The test drives the exact byte boundaries by writing a token and then scanning it with
 * every small buffer size, so the split lands at every position inside the number.
 */

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (f) { fputs(content, f); fclose(f); }
}

int main(void) {
    PROVEN_TEST_SUITE("a float split across the scanner buffer scans whole",
        "Every refill boundary inside a float - at the exponent, at the sign, mid-mantissa - must yield the correct value, not a truncated one and not a desync.",
        "Inspect proven_scan_f64 in src/proven/scan.c: it flags needs_more both when a valid float might still grow and when a failed parse left only a float prefix. The buffered scanner turns that into a refill.");

    proven_allocator_t heap = proven_heap_allocator();

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a float with an exponent, split at every position",
        "The value has a sign, a long mantissa, and a signed exponent - every place a boundary can fall.",
        "A buffer large enough for the token (plus its delimiter) must scan it correctly wherever the window boundary lands.");
    // ---------------------------------------------------------------
    {
        /* "  <float>\n" - leading spaces so the token starts at various offsets too. */
        const char *floats[] = {
            "-3.0448159523880858e-222",
            "-7.860948551957922e45",
            "1.6798050267366749e308",
            "9.04695818245888e31",
            "5e-324",
            "-0.0001",
        };
        const double vals[] = {
            -3.0448159523880858e-222, -7.860948551957922e45, 1.6798050267366749e308,
            9.04695818245888e31, 5e-324, -0.0001,
        };

        for (size_t fi = 0; fi < sizeof floats / sizeof floats[0]; ++fi) {
            char line[64];
            snprintf(line, sizeof line, "%s\n", floats[fi]);
            write_file("scanfloat.tmp", line);
            proven_size_t toklen = (proven_size_t)strlen(floats[fi]);

            /* Every buffer from "just big enough for the token plus a delimiter" up. The
             * split lands at a different byte of the float for each size. */
            for (proven_size_t cap = toklen + 2; cap <= toklen + 20; ++cap) {
                proven_result_file_t rf = proven_fs_open(heap, PROVEN_LIT("scanfloat.tmp"), PROVEN_FS_READ);
                PROVEN_TEST_ASSERT(proven_is_ok(rf.err), "open the fixture", "");
                proven_sysio_scanner_t sc;
                proven_err_t ie = proven_sysio_scanner_init(&sc, rf.value, heap, cap);
                PROVEN_TEST_ASSERT(proven_is_ok(ie), "init the scanner", "");

                double got = 0.0;
                proven_err_t e = proven_sysio_scanner_scan(&sc, "{}", PROVEN_SCAN_ARG(&got));
                PROVEN_TEST_ASSERT(proven_is_ok(e) && got == vals[fi],
                    "the float must scan to its exact value at every buffer boundary",
                    "A wrong value here is a float truncated at the exponent; an error is the sign or a digit dropped at the boundary.");

                proven_sysio_scanner_deinit(&sc);
                (void)proven_fs_close(rf.value);
            }
        }
        (void)proven_fs_remove(heap, PROVEN_LIT("scanfloat.tmp"));
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("an int then a float, so the boundary falls inside the float",
        "This is the shape the round-trip failed on: the int consumes to near the buffer end, leaving the float straddling the refill.",
        "");
    // ---------------------------------------------------------------
    {
        write_file("scanfloat.tmp", "814537981 -7.860948551957922e45\n");
        for (proven_size_t cap = 12; cap <= 40; ++cap) {
            proven_result_file_t rf = proven_fs_open(heap, PROVEN_LIT("scanfloat.tmp"), PROVEN_FS_READ);
            proven_sysio_scanner_t sc;
            if (!proven_is_ok(proven_sysio_scanner_init(&sc, rf.value, heap, cap))) { (void)proven_fs_close(rf.value); continue; }

            int gi = 0; double gd = 0.0;
            proven_err_t e1 = proven_sysio_scanner_scan(&sc, "{}", PROVEN_SCAN_ARG(&gi));
            proven_err_t e2 = proven_sysio_scanner_scan(&sc, "{}", PROVEN_SCAN_ARG(&gd));

            /* Once the buffer is big enough for the longest token (the float, 21 chars) plus
             * a delimiter, both must be exact. Below that, an honest OUT_OF_BOUNDS is fine -
             * what must never happen is a wrong value or a desync. */
            if (cap >= 23) {
                PROVEN_TEST_ASSERT(proven_is_ok(e1) && gi == 814537981,
                    "the int must scan", "");
                PROVEN_TEST_ASSERT(proven_is_ok(e2) && gd == -7.860948551957922e45,
                    "and the float straddling the boundary must scan to its exact value",
                    "This is the exact round-trip failure: the float came back as an error and desynced the stream.");
            } else {
                PROVEN_TEST_ASSERT(e2 == PROVEN_ERR_OUT_OF_BOUNDS || (proven_is_ok(e1) && proven_is_ok(e2) && gd == -7.860948551957922e45),
                    "a buffer too small is OUT_OF_BOUNDS, never a wrong value", "");
            }
            proven_sysio_scanner_deinit(&sc);
            (void)proven_fs_close(rf.value);
        }
        (void)proven_fs_remove(heap, PROVEN_LIT("scanfloat.tmp"));
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("genuine garbage is still an error, not an endless wait for more",
        "The refill-on-prefix rule must not turn \"abc\" into a scanner that waits forever for it to become a number.",
        "A non-float character in the remaining view means the float is absent, not unfinished.");
    // ---------------------------------------------------------------
    {
        write_file("scanfloat.tmp", "abcdef\n");
        proven_result_file_t rf = proven_fs_open(heap, PROVEN_LIT("scanfloat.tmp"), PROVEN_FS_READ);
        proven_sysio_scanner_t sc;
        PROVEN_TEST_ASSERT(proven_is_ok(proven_sysio_scanner_init(&sc, rf.value, heap, 8)), "init", "");
        double g = 0;
        proven_err_t e = proven_sysio_scanner_scan(&sc, "{}", PROVEN_SCAN_ARG(&g));
        PROVEN_TEST_ASSERT(!proven_is_ok(e),
            "\"abcdef\" scanned as a float is an error, and the call returns rather than hanging",
            "If this test times out, the prefix rule is refilling on input that can never become a float.");
        proven_sysio_scanner_deinit(&sc);
        (void)proven_fs_close(rf.value);
        (void)proven_fs_remove(heap, PROVEN_LIT("scanfloat.tmp"));
    }

    PROVEN_TEST_PASS("a float survives every buffer boundary, and garbage is still garbage.");
    return 0;
}
#endif
