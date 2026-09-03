#include "example.h"
#include <string.h>

/*
 * 기억에 사상된 파일은 프로세서가 기억처럼 읽는 파일이다. 운영체제가 그 쪽들을 어떤
 * 주소에 나타나게 해 주고, 읽기는 read 호출 없이 일어난다. 한 모양의 문제에는 옳은
 * 도구다 - 여러 프로세스가 함께 보는 큰 파일에 무작위로 접근하는 일 - 그리고 흘려 읽기에는
 * 틀린 도구다. 거기서는 버퍼를 둔 읽기 쪽이 더 단순하고, 파일 크기를 여러분의 주소 공간에
 * 묶지도 않는다.
 *
 * 틀리기 쉬운 자리는 지속성이고, 이 예제가 있는 이유가 그것이다.
 *
 *   PROVEN_MMAP_SHARED  - 쓰기가 파일로 간다. proven_mmap_sync 가 그것을 저장 장치까지
 *                         밀어 준다.
 *   PROVEN_MMAP_PRIVATE - 쓰기가 복사-후-쓰기다. 이 프로세스 안에만 있고 다른 어디에도
 *                         없다. 되쓸 것이 없으므로, sync 를 청하면 조용히 아무 일도 하지
 *                         않는 대신 그렇다고 말한다.
 *
 * 이 예제는 달력 형식화기도 나란히 보인다. 여기서 쓰는 레코드가 시각을 싣기 때문이고,
 * 윈도 API 를 위해 쓰는 시각이야말로 UTF-16 형식화기가 옳은 호출인 유일한 자리다.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();
    proven_u8str_view_t path = PROVEN_LIT("proven_example_mmap.dat");

    /* 사상은 파일을 늘릴 수 없다. 그러니 파일은 사상하기 전에 사상하려는 크기여야 한다. */
    static const char initial[] = "record 0: pending    \n";
    proven_result_file_t create = proven_fs_open(alloc, path,
                                                 PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
    EXAMPLE_REQUIRE(proven_is_ok(create.err), "creating the backing file must succeed");
    if (!proven_is_ok(create.err)) return 1;

    proven_mem_view_t seed = { .ptr = (const proven_byte_t *)initial, .size = sizeof initial - 1 };
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_write_all(create.value, seed)), "writing the record must succeed");
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_close(create.value)), "closing it must succeed");

    /* --- 공유 사상: 쓰기가 파일에 닿는다 ---------------------------------- */

    proven_result_file_t f = proven_fs_open(alloc, path, PROVEN_FS_READ | PROVEN_FS_WRITE);
    EXAMPLE_REQUIRE(proven_is_ok(f.err), "opening the file for mapping must succeed");
    if (!proven_is_ok(f.err)) return 1;

    /* size 0 은 "파일 끝까지" 라는 뜻이다. */
    proven_result_mmap_t m = proven_mmap_create(f.value, 0, 0,
                                                PROVEN_MMAP_READ | PROVEN_MMAP_WRITE,
                                                PROVEN_MMAP_SHARED);
    EXAMPLE_REQUIRE(proven_is_ok(m.err), "mapping the file must succeed");
    if (!proven_is_ok(m.err)) {
        (void)proven_fs_close(f.value);
        return 1;
    }
    proven_mmap_t map = m.value;

    /* 읽기는 그냥 기억을 읽는 것이다 - 호출도 복사도 없다. as_view 는 사상 전체를 바이트
     * 뷰로 돌려주므로, 보통의 뷰 도우미들이 그대로 먹는다. */
    proven_u8str_view_t contents = proven_mmap_as_view(map);
    EXAMPLE_REQUIRE(contents.size == sizeof initial - 1, "the mapping covers the whole file");
    EXAMPLE_REQUIRE(proven_u8str_view_starts_with(contents, PROVEN_LIT("record 0:")),
                    "and shows the bytes that were written");

    /* 쓰기는 기억에 쓰는 것이다. 상태 필드를 일부러 폭 고정으로 두었다. 사상은 파일을
     * 길게 만들 수 없으므로, 제자리 편집은 이미 있는 자리에 들어가야 한다. */
    proven_size_t at = proven_u8str_view_find(contents, 0, PROVEN_LIT("pending"));
    EXAMPLE_REQUIRE(at != PROVEN_SIZE_MAX, "the status field must be found");
    memcpy((proven_byte_t *)map.ptr + at, "done   ", 7);

    /* sync 가 지속성의 걸음이다. 그것 없이는 바뀐 내용이 페이지 캐시에 있고, 거기서는 이
     * 프로그램이 끝나는 것은 견디지만 기계가 전원을 잃는 것은 견디지 못한다. */
    EXAMPLE_REQUIRE(proven_is_ok(proven_mmap_sync(&map)), "syncing a shared mapping must succeed");
    EXAMPLE_REQUIRE(proven_is_ok(proven_mmap_destroy(&map)), "unmapping must succeed");
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_close(f.value)), "closing the mapped file must succeed");

    /* 편집이 이 프로세스의 기억에만이 아니라 파일에 들어가 있다. */
    proven_result_u8str_t back = proven_fs_read_all_u8str(alloc, path);
    EXAMPLE_REQUIRE(proven_is_ok(back.err), "reading the file back must succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_find(proven_u8str_as_view(&back.value), 0, PROVEN_LIT("done")) != PROVEN_SIZE_MAX,
                    "the mapped write reached the file");
    proven_u8str_destroy(alloc, &back.value);

    /* --- 사적 사상: 쓰기가 아무 데도 가지 않는다 -------------------------- */

    proven_result_file_t pf = proven_fs_open(alloc, path, PROVEN_FS_READ);
    EXAMPLE_REQUIRE(proven_is_ok(pf.err), "reopening for a private mapping must succeed");

    proven_result_mmap_t pm = proven_mmap_create(pf.value, 0, 0, PROVEN_MMAP_READ, PROVEN_MMAP_PRIVATE);
    EXAMPLE_REQUIRE(proven_is_ok(pm.err), "a private read mapping must succeed");
    proven_mmap_t priv = pm.value;

    /* 사적 사상에 sync 를 청하면 받아 놓고 무시하는 대신 거부된다. 그 거부가 쓸모 있는
     * 동작이다. 사상이 공유라고 믿고 있던 쪽이 자료가 사라진 뒤가 아니라 여기서 알게 된다. */
    EXAMPLE_REQUIRE(proven_mmap_sync(&priv) == PROVEN_ERR_UNSUPPORTED,
                    "a private mapping has nothing to write back, and says so");

    EXAMPLE_REQUIRE(proven_is_ok(proven_mmap_destroy(&priv)), "unmapping the private mapping must succeed");
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_close(pf.value)), "closing it must succeed");

    /* --- 시각을, 두 문자열 타입 모두로 ------------------------------------ */

    proven_datetime_t now = proven_time_now_datetime();

    proven_result_u8str_t stamp = proven_u8str_create(alloc, 64);
    EXAMPLE_REQUIRE(proven_is_ok(stamp.err), "creating the timestamp string must succeed");
    EXAMPLE_REQUIRE(proven_is_ok(proven_time_u8_fmt(alloc, &stamp.value, now, &proven_time_locale_en,
                                                    "{year}-{month:0>2}-{day:0>2}")),
                    "formatting the date as UTF-8 must succeed");
    EXAMPLE_REQUIRE(proven_u8str_as_view(&stamp.value).size == 10, "a YYYY-MM-DD date is ten characters");

    /* 같은 호출의 UTF-16 꼴이다. 그것이 옳은 유일한 자리를 위한 것 - 와이드 문자열을 받는
     * 시스템 호출에 글을 곧장 건네는 자리다. */
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
