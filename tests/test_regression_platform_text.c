#if !defined(_WIN32) && !defined(_WIN64)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "proven.h"
#include "proven_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

/*
 * RFC-0009 D-002 and D-003: text that comes from the platform.
 *
 *   D-002  A variable set to the empty string read as PROVEN_ERR_IO on Windows: the second
 *          GetEnvironmentVariableW call stores 0 characters and returns 0, which is also its
 *          failure value.
 *   D-003  Environment values and directory names were lossy on Windows (a lone surrogate
 *          became U+FFFD) and unchecked on POSIX (any bytes came back as PROVEN_OK). The rule
 *          is now strict everywhere: PROVEN_ERR_INVALID_ENCODING, nothing substituted, and a
 *          listing reports the entry and goes on.
 */

#define DIR_NAME "test_platform_text.d"

static bool set_env(const char *name, const char *value) {
#if defined(_WIN32) || defined(_WIN64)
    /* Not _putenv_s: it treats "" as "remove". SetEnvironmentVariableA keeps an empty value. */
    return SetEnvironmentVariableA(name, value) != 0;
#else
    return setenv(name, value, 1) == 0;
#endif
}

static void unset_env(const char *name) {
#if defined(_WIN32) || defined(_WIN64)
    (void)SetEnvironmentVariableA(name, NULL);
#else
    (void)unsetenv(name);
#endif
}

/* Plants a name that is not valid text: a lone surrogate on Windows, "bad\xFF" on POSIX.
 * Returns false where the filesystem refuses it (some POSIX filesystems insist on UTF-8). */
static bool plant_bad_name(void) {
#if defined(_WIN32) || defined(_WIN64)
    wchar_t wname[] = { L't', L'e', L's', L't', L'_', L'p', L'l', L'a', L't', L'f', L'o', L'r', L'm',
                        L'_', L't', L'e', L'x', L't', L'.', L'd', L'\\', L'b', L'a', L'd', 0xD800, 0 };
    HANDLE h = CreateFileW(wname, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
#else
    int fd = open(DIR_NAME "/bad\xFF", O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return false;
    close(fd);
    return true;
#endif
}

static void remove_bad_name(void) {
#if defined(_WIN32) || defined(_WIN64)
    wchar_t wname[] = { L't', L'e', L's', L't', L'_', L'p', L'l', L'a', L't', L'f', L'o', L'r', L'm',
                        L'_', L't', L'e', L'x', L't', L'.', L'd', L'\\', L'b', L'a', L'd', 0xD800, 0 };
    (void)DeleteFileW(wname);
#else
    (void)unlink(DIR_NAME "/bad\xFF");
#endif
}

int main(void) {
    PROVEN_TEST_SUITE("platform text: environment values and directory names (RFC-0009 D-002, D-003)",
        "An empty variable reads as empty; text that is not valid is PROVEN_ERR_INVALID_ENCODING, never substituted; a listing reports such an entry and goes on.",
        "Inspect platform/proven_sys_env.c, proven_env_get in src/proven/sysio.c, proven_sys_fs_dir_step and internal_dir_step_text in src/proven/fs.c.");
    proven_allocator_t heap = proven_heap_allocator();

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("an empty variable is an empty string",
        "D-002: set to \"\", the variable reads as PROVEN_OK with length 0 - on Windows it read as PROVEN_ERR_IO.",
        "On Windows, a 0 from the second GetEnvironmentVariableW call is told apart by GetLastError.");
    // ---------------------------------------------------------------
    PROVEN_TEST_ASSERT(set_env("PROVEN_TEXT_EMPTY", ""), "the empty variable is installed", "");
    proven_result_u8str_t r = proven_env_get(heap, PROVEN_LIT("PROVEN_TEXT_EMPTY"));
    PROVEN_TEST_ASSERT(r.err == PROVEN_OK, "an empty value is PROVEN_OK", "");
    PROVEN_TEST_ASSERT(proven_u8str_as_view(&r.value).size == 0, "and has length 0", "");
    proven_u8str_destroy(heap, &r.value);
    unset_env("PROVEN_TEXT_EMPTY");
    r = proven_env_get(heap, PROVEN_LIT("PROVEN_TEXT_EMPTY"));
    PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_NOT_FOUND, "an unset variable is still PROVEN_ERR_NOT_FOUND", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a value that is not text is refused",
        "D-003: a lone surrogate (Windows) or bytes that are not UTF-8 (POSIX) are PROVEN_ERR_INVALID_ENCODING.",
        "Windows: WC_ERR_INVALID_CHARS in proven_sys_env_get. POSIX: the UTF-8 check in proven_env_get.");
    // ---------------------------------------------------------------
#if defined(_WIN32) || defined(_WIN64)
    wchar_t lone[] = { L'a', 0xD800, L'b', 0 };
    PROVEN_TEST_ASSERT(SetEnvironmentVariableW(L"PROVEN_TEXT_BAD", lone) != 0, "the lone-surrogate value is installed", "");
#else
    PROVEN_TEST_ASSERT(set_env("PROVEN_TEXT_BAD", "a\xFF\xFE" "b"), "the non-UTF-8 value is installed", "");
#endif
    r = proven_env_get(heap, PROVEN_LIT("PROVEN_TEXT_BAD"));
    PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_INVALID_ENCODING, "the value is PROVEN_ERR_INVALID_ENCODING",
        "PROVEN_OK here means bytes were passed through, or a U+FFFD was substituted.");
    unset_env("PROVEN_TEXT_BAD");
#if defined(_WIN32) || defined(_WIN64)
    /* The W call: SetEnvironmentVariableA reads its bytes in the ANSI code page, not UTF-8. */
    wchar_t cafe[] = { L'c', L'a', L'f', 0x00E9, 0 };
    PROVEN_TEST_ASSERT(SetEnvironmentVariableW(L"PROVEN_TEXT_GOOD", cafe) != 0, "a valid value is installed", "");
#else
    PROVEN_TEST_ASSERT(set_env("PROVEN_TEXT_GOOD", "caf\xC3\xA9"), "a valid UTF-8 value is installed", "");
#endif
    r = proven_env_get(heap, PROVEN_LIT("PROVEN_TEXT_GOOD"));
    PROVEN_TEST_ASSERT(r.err == PROVEN_OK && proven_u8str_view_eq(proven_u8str_as_view(&r.value), PROVEN_LIT("caf\xC3\xA9")),
        "valid text still reads back exactly", "");
    proven_u8str_destroy(heap, &r.value);
    unset_env("PROVEN_TEXT_GOOD");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a directory entry whose name is not text",
        "D-003: dir_next reports it as PROVEN_ERR_INVALID_ENCODING with an empty name and goes on; walk does the same; fs_list refuses the whole listing.",
        "Inspect internal_dir_step_text and the INVALID_ENCODING branch of proven_fs_walk_next.");
    // ---------------------------------------------------------------
    remove_bad_name();
    (void)proven_fs_remove(heap, PROVEN_LIT(DIR_NAME "/good.txt"));
    (void)proven_fs_rmdir(heap, PROVEN_LIT(DIR_NAME));
    PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_mkdir(heap, PROVEN_LIT(DIR_NAME))), "the fixture directory is created", "");
    PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_write_file(heap, PROVEN_LIT(DIR_NAME "/good.txt"),
        proven_mem_view_from_u8(PROVEN_LIT("ok")))), "a valid entry is created", "");
    if (!plant_bad_name()) {
        PROVEN_TEST_INFO("SKIP: this filesystem refuses a name that is not valid text; the listing rule is unexercised here");
    } else {
        proven_result_dir_t d = proven_fs_dir_open(heap, PROVEN_LIT(DIR_NAME));
        PROVEN_TEST_ASSERT(proven_is_ok(d.err), "the directory opens", "");
        int good = 0, bad = 0, other = 0;
        proven_fs_dir_entry_t e;
        for (;;) {
            proven_err_t de = proven_fs_dir_next(&d.value, &e);
            if (de == PROVEN_ERR_EOF) break;
            if (de == PROVEN_ERR_INVALID_ENCODING) {
                PROVEN_TEST_ASSERT(e.name.size == 0, "the bad entry comes with an empty name", "");
                PROVEN_TEST_ASSERT(e.type == PROVEN_FS_TYPE_FILE, "and its other fields filled in", "");
                ++bad;
            } else if (de == PROVEN_OK && proven_u8str_view_eq(e.name, PROVEN_LIT("good.txt"))) {
                ++good;
            } else {
                ++other;
            }
        }
        proven_fs_dir_close(&d.value);
        PROVEN_TEST_ASSERT(bad == 1 && good == 1 && other == 0,
            "dir_next reports the bad entry once and still lists the good one",
            "If good == 0, the bad entry ended the listing instead of being reported and skipped.");

        proven_result_array_t l = proven_fs_list(heap, PROVEN_LIT(DIR_NAME));
        PROVEN_TEST_ASSERT(l.err == PROVEN_ERR_INVALID_ENCODING,
            "fs_list refuses the whole listing rather than leaving the entry out", "");

        proven_result_walk_t w = proven_fs_walk_open(heap, PROVEN_LIT(DIR_NAME), PROVEN_FS_WALK_UNLIMITED);
        PROVEN_TEST_ASSERT(proven_is_ok(w.err), "the walk opens", "");
        good = bad = other = 0;
        proven_fs_walk_entry_t we;
        for (;;) {
            proven_err_t de = proven_fs_walk_next(&w.value, &we);
            if (de == PROVEN_ERR_EOF) break;
            if (de == PROVEN_ERR_INVALID_ENCODING) {
                PROVEN_TEST_ASSERT(we.name.size == 0 && proven_u8str_view_eq(we.path, PROVEN_LIT(DIR_NAME)),
                    "the walk names the directory the bad entry is in", "");
                ++bad;
            } else if (de == PROVEN_OK && proven_u8str_view_eq(we.name, PROVEN_LIT("good.txt"))) {
                ++good;
            } else {
                ++other;
            }
        }
        proven_fs_walk_close(&w.value);
        PROVEN_TEST_ASSERT(bad == 1 && good == 1 && other == 0, "the walk reports it once and goes on", "");
        remove_bad_name();
    }
    (void)proven_fs_remove(heap, PROVEN_LIT(DIR_NAME "/good.txt"));
    PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_rmdir(heap, PROVEN_LIT(DIR_NAME))), "the fixture directory is empty at the end", "");

    PROVEN_TEST_PASS("platform text");
    return 0;
}
