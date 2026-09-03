#include "example.h"

/*
 * 파일 통째 API: 한 번 부르면 들어가고, 한 번 부르면 나온다. 이것이 있는 이유는 열고 -
 * 읽는 반복문 - 닫는 그 춤사위가 파일 다루기 버그 대부분이 사는 자리이기 때문이다.
 * 잊은 close, EOF 로 오해한 부분 읽기, 실패한 쓰기가 남긴 잘린 파일. 파일을 통째로 읽거나
 * 쓰는 것이라면 이것이 그 API 다.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* 지금 디렉터리의 상대 경로다. 이 예제가 쓸 수 있는 /tmp 에 기대면 안 되고, 만든
     * 것은 돌아가기 전에 지운다. */
    proven_u8str_view_t path = PROVEN_LIT("proven_example_wholefile.tmp");
    proven_u8str_view_t text = PROVEN_LIT("first line\nsecond line\n");

    /* --- 한 번의 호출로 쓰기 ------------------------------------------------ */
    /* 원자적이지 않다. 동시에 읽는 쪽은 이 파일이 반쯤 쓰인 것을 볼 수 있다. 여기서는
     * 괜찮다. 아직 아무도 이 파일을 보고 있지 않기 때문이다. */
    proven_err_t err = proven_fs_write_file(alloc, path, proven_mem_view_from_u8(text));
    EXAMPLE_REQUIRE(proven_is_ok(err), "writing the whole file should succeed");
    if (!proven_is_ok(err)) return 1;

    /* --- 날바이트로 되읽기 -------------------------------------------------- */
    /* proven_fs_read_all 은 미리 잰 크기까지가 아니라 EOF 까지 읽는다. 그래서 크기를
     * 미리 알 수 없는 파이프나 /proc 항목에서도 돈다. */
    proven_result_mem_mut_t raw = proven_fs_read_all(alloc, path);
    EXAMPLE_REQUIRE(proven_is_ok(raw.err), "reading the whole file should succeed");
    if (proven_is_ok(raw.err)) {
        EXAMPLE_REQUIRE(raw.value.size == text.size, "read_all should return every byte written");
        /* 그 덩이는 그냥 할당자의 기억이다 - 그것을 내준 할당자에게 돌려줄 것.
         * proven_fs_read_all_destroy 같은 것은 없다. */
        alloc.free_fn(alloc.ctx, raw.value.ptr);
    }

    /* --- 문자열로 되읽기 ---------------------------------------------------- */
    /* 대개의 부르는 쪽이 원하는 것이 이것이다. 결과가 NUL 로 끝나므로 뷰에도, as_cstr
     * 에도, 스캐너에도 두 번째 복사 없이 건넬 수 있다. 종결자 자리는 미리 잡아 두므로
     * 할당이 더 들지 않는다. */
    proven_result_u8str_t s = proven_fs_read_all_u8str(alloc, path);
    EXAMPLE_REQUIRE(proven_is_ok(s.err), "reading the whole file as a string should succeed");
    if (!proven_is_ok(s.err)) {
        (void)proven_fs_remove(alloc, path);
        return 1;
    }
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&s.value), text),
                    "the file's contents should come back unchanged");
    printf("read back %zu bytes: %s", (size_t)proven_u8str_as_view(&s.value).size,
           proven_u8str_as_cstr(&s.value));
    proven_u8str_destroy(alloc, &s.value);

    /* --- stat, 그리고 권한의 왕복 ------------------------------------------ */
    proven_fs_stat_t st = {0};
    err = proven_fs_stat(alloc, path, &st);
    EXAMPLE_REQUIRE(proven_is_ok(err), "stat on a file we just wrote should succeed");
    EXAMPLE_REQUIRE(st.type == PROVEN_FS_TYPE_FILE, "a regular file should stat as a FILE");
    EXAMPLE_REQUIRE(st.size == text.size, "stat should report the size we wrote");

    /* `perms` 는 권한 비트 아홉 개만 담고 그 밖의 것은 담지 않는다. 그래서 그대로
     * chmod 에 도로 먹일 수 있다. 이 필드의 요점이 그것이다 - 파일의 모드를 읽어 두었다가
     * 나중에 되돌리는 것. (예전에는 날 POSIX st_mode 를 담았는데, 그 파일 종류 비트를
     * chmod 가 거부해서 이 뻔한 왕복이 실패했다.) */
    err = proven_fs_chmod(alloc, path, st.perms);
    EXAMPLE_REQUIRE(proven_is_ok(err), "a stat's perms must be accepted back by chmod");

    /* 이제 파일을 소유자 전용으로 바꾼다. 그래야 다음 검사가 증명할 것이 생긴다. */
    proven_fs_perms_t private_perms = PROVEN_FS_PERM_OWNER_R | PROVEN_FS_PERM_OWNER_W;
    err = proven_fs_chmod(alloc, path, private_perms);
    EXAMPLE_REQUIRE(proven_is_ok(err), "restricting the file to its owner should succeed");

    /* --- 원자적으로 다시 쓰기 ----------------------------------------------- */
    /* 형제 임시 파일 하나에 rename 하나. 동시에 읽는 쪽은 옛 파일 전체이거나 새 파일
     * 전체를 보지, 반쯤 섞인 것을 보지 않는다. 읽는 쪽에게 원자적이지, 전원이 나가도
     * 견디는 것은 아니다. 그래야 할 때는 proven_fs_write_file_durable 이 그것을 청한다. */
    proven_u8str_view_t text2 = PROVEN_LIT("replacement\n");
    err = proven_fs_write_file_atomic(alloc, path, proven_mem_view_from_u8(text2));
    EXAMPLE_REQUIRE(proven_is_ok(err), "the atomic rewrite should succeed");

    proven_fs_stat_t st2 = {0};
    err = proven_fs_stat(alloc, path, &st2);
    EXAMPLE_REQUIRE(proven_is_ok(err), "stat after the atomic rewrite should succeed");
    EXAMPLE_REQUIRE(st2.size == text2.size, "the file should now hold the replacement text");
    /* rename 은 옛 이름 위에 *새* 아이노드를 씌우므로, 권한을 옮겨 주지 않으면 잃는다.
     * 옮겨 준다 - 0600 파일을 다시 써도 0644 로 다시 공개되지 않는다. */
    EXAMPLE_REQUIRE(st2.perms == private_perms,
                    "the atomic rewrite must preserve the target's permissions");

    /* --- 뒷정리 -------------------------------------------------------------- */
    err = proven_fs_remove(alloc, path);
    EXAMPLE_REQUIRE(proven_is_ok(err), "removing the temp file should succeed");

    return EXAMPLE_OK();
}
