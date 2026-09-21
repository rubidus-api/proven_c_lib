#include "example.h"

/*
 * 9과 - 바깥 세상도 실패하고, 같은 방식으로 그렇다고 말한다.
 *
 * 지금까지는 프로그램 안의 이유로만 실패할 수 있었다. 자리가 모자라거나 인자가
 * 틀렸거나. 파일은 없을 수도, 읽을 수 없을 수도, 디스크가 가득 찼을 수도 있고,
 * 그 어느 것도 여러분 코드의 버그가 아니다. 라이브러리는 그런 실패도 앞에서와
 * 같은 오류 값으로 알린다. 그러니 이미 배운 검사가 검사의 전부다.
 *
 * 여기 쓰는 것은 통파일 호출이다. 호출 하나로 파일을 쓰고, 하나로 되읽는다.
 * open 도, 읽기 루프도, 잊어버릴 close 도 없다.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* 현재 디렉터리의 이름 하나. 프로그램은 끝나기 전에 이것을 지운다. */
    proven_u8str_view_t path = PROVEN_LIT("proven_tutorial_notes.tmp");
    proven_u8str_view_t text = PROVEN_LIT("buy milk\ncall home\n");

    /* 원자적이다. 읽는 쪽은 옛 파일이나 새 파일을 보지, 반쪽을 보지 않는다.
     * 할당자는 호출이 경로를 다루며 쓸 수 있는 임시 작업 공간이다. */
    proven_err_t err = proven_fs_write_file_atomic(alloc, path, proven_mem_view_from_u8(text));
    EXAMPLE_REQUIRE(proven_is_ok(err), "writing a small file in the current directory should work");
    if (!proven_is_ok(err)) return 1;

    /* 읽기는 소유하는 문자열을 돌려준다. 4과의 모양, 5과의 규칙 그대로다.
     * `alloc` 으로 만들어졌으니 `alloc` 으로 없앤다. */
    proven_result_u8str_t back = proven_fs_read_all_u8str(alloc, path);
    EXAMPLE_REQUIRE(proven_is_ok(back.err), "the file we just wrote can be read");
    if (proven_is_ok(back.err)) {
        EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&back.value), text),
                        "the bytes come back exactly as written");
        proven_println("read {} bytes back", PROVEN_ARG(proven_u8str_as_view(&back.value).size));
        proven_u8str_destroy(alloc, &back.value);
    }

    err = proven_fs_remove(alloc, path);
    EXAMPLE_REQUIRE(proven_is_ok(err), "removing the file we created should work");

    /* 이제 파일은 없고, 그것을 읽는 것은 프로그램 바깥에서 온 실패다.
     * 그 실패도 보통 값으로 오고, 어떤 실패인지 말해 준다. */
    proven_result_u8str_t missing = proven_fs_read_all_u8str(alloc, path);
    EXAMPLE_REQUIRE(missing.err == PROVEN_ERR_NOT_FOUND, "a removed file reads as NOT_FOUND");
    if (proven_is_ok(missing.err)) proven_u8str_destroy(alloc, &missing.value);
    proven_println("reading it again: not found, as expected");

    return EXAMPLE_OK();
}
