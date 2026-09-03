#include "example.h"

/*
 * 열고-읽고-쓰고-닫는 길. 파일 통째 API(ex_05_fs_wholefile)로는 모자랄 때 쓴다.
 * 흘려 보내고 있거나, 버퍼를 여러분이 소유하고 싶을 때다.
 *
 * 여기서 반드시 맞춰야 할 하나: 읽기나 쓰기 한 번은 청한 바이트 수 *만큼까지*를 옮기지,
 * 정확히 그만큼을 옮기지 않는다. 짧은 읽기 한 번을 파일 끝으로 여기는 것이 파일의 꼬리를
 * 조용히 잃는 고전적인 방법이다.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    proven_u8str_view_t path = PROVEN_LIT("proven_example_stream.tmp");
    proven_u8str_view_t text = PROVEN_LIT("streamed bytes, read back in chunks\n");

    /* --- 쓰기 --------------------------------------------------------------- */
    /* CREATE 는 없으면 만들고, TRUNC 는 있으면 비운다. 할당자는 플랫폼 호출을 위해 경로를
     * 변환하는 데만 쓰인다. */
    proven_result_file_t out = proven_fs_open(alloc, path,
                                              PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
    EXAMPLE_REQUIRE(proven_is_ok(out.err), "opening the file for writing should succeed");
    if (!proven_is_ok(out.err)) return 1;

    /* write_all 이 우리 대신 반복해 준다. proven_fs_write 는 한 번만 시도하고 더 적게 쓸
     * 수도 있는데, 그것은 부르는 쪽이 뜻한 바가 거의 아니다. */
    proven_err_t err = proven_fs_write_all(out.value, proven_mem_view_from_u8(text));

    /* close 는 쓰기의 일부이고, 네트워크나 할당량이 걸린 파일 시스템에서는 실패가 드러나는
     * *유일한* 자리다. 바이트는 버퍼에 담겼고 write() 는 그렇다고 했으며, 디스크가 마침내
     * 아니라고 말하는 자리가 close() 다. 실패 경로에서도 닫을 것 - 손잡이는 어느 쪽이든
     * 우리 것이다 - 다만 그 답을 버리지는 말 것. */
    proven_err_t cerr = proven_fs_close(out.value);
    if (proven_is_ok(err)) err = cerr;

    EXAMPLE_REQUIRE(proven_is_ok(err), "writing the whole buffer should succeed");
    if (!proven_is_ok(err)) {
        (void)proven_fs_remove(alloc, path);
        return 1;
    }

    /* --- 읽기 --------------------------------------------------------------- */
    proven_result_file_t in = proven_fs_open(alloc, path, PROVEN_FS_READ);
    EXAMPLE_REQUIRE(proven_is_ok(in.err), "opening the file for reading should succeed");
    if (!proven_is_ok(in.err)) {
        (void)proven_fs_remove(alloc, path);
        return 1;
    }

    /* size 는 버퍼 크기를 잡는 힌트이지, 읽기 한 번이 몇 바이트를 건넬지에 대한 약속이
     * 아니다 - 그리고 보통 파일이 아닌 것(파이프, 장치, /proc 항목)에서는 0 이다. 아래
     * 반복문은 그것에 기대지 않는다. */
    proven_result_size_t sz = proven_fs_size(in.value);
    EXAMPLE_REQUIRE(proven_is_ok(sz.err), "querying the size of an open file should succeed");
    EXAMPLE_REQUIRE(sz.value == text.size, "the file should be as long as what we wrote");

    proven_byte_t buf[128];
    proven_size_t total = 0;

    /* 부분 읽기 반복문. 한 바퀴마다 버퍼에 남은 만큼을 청하고 실제로 온 만큼 나아간다.
     * 짧은 읽기는 파일의 끝이 아니라 정상이다. 파일의 끝은 따로 있는 상태 -
     * 바이트 0 과 함께 오는 PROVEN_ERR_EOF - 이므로 반복문은 그것으로만 끝난다. 원본이
     * 버퍼보다 커져도 반복문은 멈추는데, 그것을 알아채는 일은 부르는 쪽의 몫이다(여기서는
     * 일어날 수 없지만, 자라나는 파일이라면 그럴 수 있다). */
    for (;;) {
        if (total == sizeof buf) break;   /* 버퍼가 가득 찼다: 어떻게 할지는 부르는 쪽이 정한다 */

        proven_mem_mut_t dest = { .ptr = buf + total, .size = sizeof buf - total };
        proven_result_size_t r = proven_fs_read(in.value, dest);
        if (r.err == PROVEN_ERR_EOF) break;
        if (!proven_is_ok(r.err)) {
            (void)proven_fs_close(in.value);
            (void)proven_fs_remove(alloc, path);
            EXAMPLE_REQUIRE(false, "reading from the open file should not fail");
            return 1;
        }
        total += r.value;
    }

    (void)proven_fs_close(in.value);

    EXAMPLE_REQUIRE(total == text.size, "the loop should have read every byte in the file");
    proven_u8str_view_t got = { .ptr = buf, .size = total };
    EXAMPLE_REQUIRE(proven_u8str_view_eq(got, text), "the bytes should come back unchanged");

    printf("read %zu bytes in chunks: %.*s", (size_t)total, (int)total, (const char *)buf);

    /* --- 뒷정리 -------------------------------------------------------------- */
    err = proven_fs_remove(alloc, path);
    EXAMPLE_REQUIRE(proven_is_ok(err), "removing the temp file should succeed");

    return EXAMPLE_OK();
}
