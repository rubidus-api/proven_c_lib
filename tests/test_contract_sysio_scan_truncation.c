#include "proven.h"
#include "proven/fs.h"
#include "proven/sysio.h"
#include "proven_test.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();
    const char *path = "test_contract_sysio_scan_truncation.tmp";
    const char *exact_path = "test_sysio_scan_exact_fit.tmp";
    char payload[4104];
    char exact_payload[4096];
    char long_fmt[4097];
    char exact_fmt[4098];

    memset(payload, 'a', 4096);
    payload[4096] = ' ';
    payload[4097] = '1';
    payload[4098] = '2';
    payload[4099] = '3';
    payload[4100] = '\n';
    payload[4101] = '\0';
    memset(exact_payload, 'b', sizeof(exact_payload));
    memset(long_fmt, 'a', sizeof(long_fmt) - 1u);
    long_fmt[sizeof(long_fmt) - 1u] = '\0';
    memset(exact_fmt, 'b', sizeof(exact_payload));
    exact_fmt[sizeof(exact_payload)] = '\0';

    PROVEN_TEST_SUITE(
        "test_contract_sysio_scan_truncation",
        "Verify one-chunk scanning rejects inputs that exceed the fixed chunk, refuses borrowed view outputs, and leaves the stream positioned for a retry.",
        "Inspect proven_sysio_scan_chunk_impl if long inputs are accepted, a stack-backed view escapes, or a failed scan consumes the stream."
    );

    proven_result_file_t opened = proven_fs_open(alloc, (proven_u8str_view_t){ .ptr = (const proven_byte_t*)path, .size = (proven_size_t)strlen(path) }, PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(opened.err), "open temp file", "Check file creation and write flags.");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_fs_write_all(opened.value, (proven_mem_view_t){ .ptr = (const proven_byte_t*)payload, .size = 4101u })), "write temp payload", "Check temp file write support.");
    (void)proven_fs_close(opened.value);

    opened = proven_fs_open(alloc, (proven_u8str_view_t){ .ptr = (const proven_byte_t*)path, .size = (proven_size_t)strlen(path) }, PROVEN_FS_READ);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(opened.err), "reopen temp file", "Check read access for the regression file.");

    proven_scan_arg_t no_args[] = { proven_scan_arg_none() };
    proven_err_t first = proven_sysio_scan_chunk_impl(opened.value, long_fmt, no_args, 1u);
    PROVEN_TEST_ASSERT(first == PROVEN_ERR_OUT_OF_BOUNDS, "chunk overflow reports an explicit bounds error", "Inspect the one-chunk scan path and its truncation policy.");

    proven_u64 number = 0;
    proven_scan_arg_t number_args[] = { proven_scan_arg_none(), PROVEN_SCAN_ARG(&number) };
    proven_err_t second = proven_sysio_scan_chunk_impl(opened.value, "{}", number_args, 2u);
    PROVEN_TEST_ASSERT(second != PROVEN_OK, "failed chunk scan keeps the stream at the original position", "Inspect cursor rewind after a truncated scan.");
    PROVEN_TEST_ASSERT(number == 0u, "failed chunk scan does not leak the trailing integer", "Inspect stream cursor restoration after truncation.");

    (void)proven_fs_close(opened.value);
    (void)remove(path);

    opened = proven_fs_open(alloc, (proven_u8str_view_t){ .ptr = (const proven_byte_t*)exact_path, .size = (proven_size_t)strlen(exact_path) }, PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(opened.err), "open exact-fit temp file", "Check file creation and write flags for the exact-fit regression.");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_fs_write_all(opened.value, (proven_mem_view_t){ .ptr = (const proven_byte_t*)exact_payload, .size = sizeof exact_payload })), "write exact-fit payload", "Check temp file write support for the exact-fit regression.");
    (void)proven_fs_close(opened.value);

    opened = proven_fs_open(alloc, (proven_u8str_view_t){ .ptr = (const proven_byte_t*)exact_path, .size = (proven_size_t)strlen(exact_path) }, PROVEN_FS_READ);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(opened.err), "reopen exact-fit temp file", "Check read access for the exact-fit regression file.");

    proven_err_t exact_err = proven_sysio_scan_chunk_impl(opened.value, exact_fmt, no_args, 1u);
    PROVEN_TEST_ASSERT(exact_err == PROVEN_OK, "exact chunk fit should succeed", "Inspect proven_sysio_scan_chunk_impl if a fully consumed 4096-byte token is treated as truncation.");

    proven_result_u64_t exact_pos = proven_fs_tell(opened.value);
    PROVEN_TEST_ASSERT(proven_is_ok(exact_pos.err) && exact_pos.val == sizeof exact_payload,
        "exact chunk fit should consume the full input", "Inspect the exact-fit probe path if the cursor stops at the wrong byte.");

    (void)proven_fs_close(opened.value);

    opened = proven_fs_open(alloc, (proven_u8str_view_t){ .ptr = (const proven_byte_t*)exact_path, .size = (proven_size_t)strlen(exact_path) }, PROVEN_FS_READ);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(opened.err), "reopen exact-fit temp file for delimiter regression", "Check read access for the delimiter regression file.");
    exact_fmt[sizeof(exact_payload)] = '!';
    exact_fmt[sizeof(exact_payload) + 1u] = '\0';
    exact_err = proven_sysio_scan_chunk_impl(opened.value, exact_fmt, no_args, 1u);
    PROVEN_TEST_ASSERT(exact_err == PROVEN_ERR_OUT_OF_BOUNDS, "missing delimiter at exact chunk fit should fail with out of bounds", "Inspect the full-buffer parse failure path in proven_sysio_scan_chunk_impl if truncation becomes a generic parse error.");

    (void)proven_fs_close(opened.value);
    (void)remove(exact_path);

    /* A view returned by the ordinary in-memory scanner borrows its input. The chunk
     * helper's input is a local stack array, so letting a STR_VIEW argument succeed
     * would return a dangling pointer the instant the function returns. Refuse that
     * argument before reading and leave both the destination and file cursor intact. */
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_fs_write_file(alloc, PROVEN_LIT("test_sysio_scan_view.tmp"),
        proven_mem_view_from_u8(PROVEN_LIT("42")))), "write view-lifetime fixture", "");
    opened = proven_fs_open(alloc, PROVEN_LIT("test_sysio_scan_view.tmp"), PROVEN_FS_READ);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(opened.err), "open view-lifetime fixture", "");

    proven_u8str_view_t token = PROVEN_LIT("unchanged");
    proven_scan_arg_t token_args[] = { proven_scan_arg_none(), PROVEN_SCAN_ARG(&token) };
    proven_err_t view_err = proven_sysio_scan_chunk_impl(opened.value, "{}", token_args, 2u);
    PROVEN_TEST_ASSERT(view_err == PROVEN_ERR_UNSUPPORTED,
        "chunk scanning must refuse a borrowed string view that would point into its stack",
        "Reject STR_VIEW arguments before the seek/read in proven_sysio_scan_chunk_impl.");
    PROVEN_TEST_ASSERT(proven_u8str_view_eq(token, PROVEN_LIT("unchanged")),
        "rejected view destination must stay unchanged", "Validate arguments before scanning.");

    proven_u64 after_reject = 0;
    proven_scan_arg_t number_after_reject[] = { proven_scan_arg_none(), PROVEN_SCAN_ARG(&after_reject) };
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_sysio_scan_chunk_impl(opened.value, "{}", number_after_reject, 2u)) &&
                       after_reject == 42u,
        "rejecting the unsafe view must not consume the file", "Move the view check before the first read.");
    (void)proven_fs_close(opened.value);
    (void)proven_fs_remove(alloc, PROVEN_LIT("test_sysio_scan_view.tmp"));

    PROVEN_TEST_PASS("Sysio scan truncation regression checks passed.");
    return 0;
}
