#include "proven_test.h"
#include "proven/fs.h"
#include "proven/sysio.h"

#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#else
#include <unistd.h>
#endif

static proven_size_t drain_pipe_into_buffer(proven_file_t file, char *buffer, proven_size_t capacity) {
    proven_size_t total = 0;
#if defined(_WIN32) || defined(_WIN64)
    HANDLE handle = (HANDLE)file.internal.ptr;
    while (total < capacity) {
        DWORD read_count = 0;
        if (!ReadFile(handle, buffer + total, (DWORD)(capacity - total), &read_count, NULL)) {
            return (proven_size_t)-1;
        }
        if (read_count == 0) {
            break;
        }
        total += (proven_size_t)read_count;
    }
#else
    int fd = file.internal.fd;
    while (total < capacity) {
        ssize_t read_count = read(fd, buffer + total, capacity - total);
        if (read_count < 0) {
            return (proven_size_t)-1;
        }
        if (read_count == 0) {
            break;
        }
        total += (proven_size_t)read_count;
    }
#endif
    return total;
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_contract_sysio_scan_nonseekable",
        "Verify one-chunk sysio scanning rejects non-seekable inputs before consuming data.",
        "If this fails, inspect proven_sysio_scan_chunk_impl and make sure the helper does not rely on rewinding a pipe or stdin."
    );

    const char *payload = "123 456\n";
    char observed[32] = {0};
    proven_file_t read_file = {0};
#if defined(_WIN32) || defined(_WIN64)
    HANDLE read_handle = NULL;
    HANDLE write_handle = NULL;
    BOOL created = CreatePipe(&read_handle, &write_handle, NULL, 0);
    PROVEN_TEST_ASSERT(created, "create anonymous pipe", "Check Windows pipe availability and handle permissions.");
    DWORD written = 0;
    BOOL wrote = WriteFile(write_handle, payload, (DWORD)strlen(payload), &written, NULL);
    PROVEN_TEST_ASSERT(wrote && written == strlen(payload), "write pipe payload", "Check Windows pipe write behavior.");
    CloseHandle(write_handle);
    read_file.internal.ptr = read_handle;
#else
    int pipe_fds[2] = {-1, -1};
    PROVEN_TEST_ASSERT(pipe(pipe_fds) == 0, "create anonymous pipe", "Check POSIX pipe support and test permissions.");
    ssize_t written = write(pipe_fds[1], payload, strlen(payload));
    PROVEN_TEST_ASSERT(written == (ssize_t)strlen(payload), "write pipe payload", "Check POSIX pipe write behavior.");
    close(pipe_fds[1]);
    read_file.internal.fd = pipe_fds[0];
#endif

    proven_u64 number = 0;
    proven_scan_arg_t args[] = { proven_scan_arg_none(), PROVEN_SCAN_ARG(&number) };
    proven_err_t err = proven_sysio_scan_chunk_impl(read_file, "{}", args, 2u);
    PROVEN_TEST_ASSERT(err == PROVEN_ERR_UNSUPPORTED, "non-seekable scan_chunk is rejected explicitly", "Inspect the seekability probe in proven_sysio_scan_chunk_impl.");
    PROVEN_TEST_ASSERT(number == 0u, "failed probe leaves the destination untouched", "Inspect the early-reject path in proven_sysio_scan_chunk_impl.");

    proven_size_t remaining = drain_pipe_into_buffer(read_file, observed, sizeof(observed) - 1u);
    PROVEN_TEST_ASSERT(remaining == strlen(payload), "pipe payload remains available after rejection", "Inspect cursor handling; the scan helper should reject before consuming bytes.");
    PROVEN_TEST_ASSERT(memcmp(observed, payload, strlen(payload)) == 0, "pipe payload content is unchanged", "Inspect the early reject path and avoid reading from non-seekable streams.");

#if defined(_WIN32) || defined(_WIN64)
    CloseHandle(read_handle);
#else
    close(pipe_fds[0]);
#endif

    PROVEN_TEST_PASS("Sysio non-seekable scan rejection regression checks passed.");
    return 0;
}
