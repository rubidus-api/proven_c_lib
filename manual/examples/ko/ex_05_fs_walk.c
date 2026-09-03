#include "example.h"

/*
 * 나무 훑기.
 *
 * 재귀 훑기가 틀리는 세 가지, 그리고 이것이 대신 하는 일.
 *
 *   맴돈다.       조상을 가리키는 심링크는 순환이다. 이 훑기는 심링크를 *지나* 내려가지
 *                 않는다 - 심링크된 디렉터리도 보고는 되고, 다만 들어가지 않을 뿐이다 -
 *                 그래서 순환이 불가능하고, 물어본 나무를 벗어나 나머지 파일 시스템으로
 *                 걸어 나가는 일도 없다.
 *
 *   거짓말한다.   읽을 수 없는 디렉터리를 건너뛰고는 성공했다고 보고한다. 백업이 하위
 *                 나무 하나를 통째로 놓치는 방식이 그것이다. 여기서는 오류가
 *                 proven_fs_walk_next 에서 돌아오고, 그 항목이 어느 디렉터리인지 말해
 *                 주며, 훑기는 다음 형제부터 이어 간다. 어떻게 할지는 여러분이 정한다.
 *
 *   부푼다.       하나라도 내주기 전에 디렉터리 전체를 기억에 읽어 들이면, 큰 나무를
 *                 훑는 데 큰 할당이 든다. 이것은 지금 경로의 *층*마다 열린 손잡이 하나와
 *                 다시 쓰는 경로 버퍼 하나만 쥔다 - 그래서 기억 사용량은 파일이 몇
 *                 개인가가 아니라 깊이의 함수다.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* 훑어 볼 작은 나무: 파일 하나, 디렉터리 하나, 그 안의 파일 하나. */
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
            /* 읽을 수 없었던 디렉터리이거나, 훑기의 스택보다 깊은 것이다. 건너뛰는 것이
             * 아니라 *보고된다* - `entry.path` 가 어느 것인지 말해 준다 - 그리고 훑기는
             * 계속된다. 나무를 복사하는 도구라면 여기서 실패해야 하고, 나무를 보고하는
             * 도구라면 세어서 알려야 한다. 하지 말아야 할 것은 없던 일로 하는 것이다. */
            unreadable++;
            continue;
        }

        /* `entry.path` 와 `entry.name` 은 빌린 것이다. 훑기가 다시 쓰는 버퍼 하나를
         * 가리키고 다음 호출 전까지만 쓸 수 있다. 더 오래 필요하면 복사할 것. */
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

    /* 깊이 제한: max_depth 0 은 뿌리 바로 안에 있는 것을 보고하고 아무 데도 내려가지
     * 않는다. 한계에 걸린 디렉터리도 여전히 항목이므로 여전히 보고된다. */
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
