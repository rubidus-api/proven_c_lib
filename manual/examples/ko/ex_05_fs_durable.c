#include "example.h"

/*
 * 정전이 파일을 반쯤 쓰인 채로 남겨 두지 못하게 하며 파일을 고치기.
 *
 * 조리법은 오래되었고, 그 모든 걸음이 하중을 받는다.
 *
 *   1. 새 내용을 진짜 파일 옆의 *임시* 파일에 쓴다,
 *   2. proven_fs_sync   - 이제 새 파일의 바이트가 장치에 있다,
 *   3. proven_fs_rename - 이름이 한 걸음에 새 파일로 넘어간다. 읽는 쪽은 옛 파일 전체나
 *                         새 파일 전체를 보지, 그 사이를 보지 않는다,
 *   4. proven_fs_sync_dir - 이제 그 이름 바꾸기 자체가 장치에 있다.
 *
 * 2를 건너뛰면 내용이 도착한 적 없는 파일을 이름이 공표할 수 있다. 4를 건너뛰면 내용은
 * 안전한데 그 이름이 안전하지 않을 수 있다. 둘 다 시험에서는 드러나지 않는다. 둘 다
 * 운영에서, 한 번, 드러난다.
 *
 * 이 프로그램은 레코드 단위 호출들 - seek/tell, pread/pwrite, truncate - 과, 이 모든
 * 일을 두 벌의 프로그램이 동시에 하지 못하게 막는 권고 잠금도 함께 보인다.
 */

typedef struct {
    proven_u32 id;
    proven_u32 score;
} record_t;

static proven_err_t write_records(proven_file_t f, const record_t *recs, proven_size_t n) {
    proven_mem_view_t view = { .ptr = (const proven_byte_t *)recs, .size = n * sizeof recs[0] };
    return proven_fs_write_all(f, view);
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    proven_u8str_view_t live = PROVEN_LIT("proven_example_durable.dat");
    proven_u8str_view_t temp = PROVEN_LIT("proven_example_durable.dat.new");
    proven_u8str_view_t here = PROVEN_LIT(".");

    /* is_absolute 는 경로를 잇거나 기준 디렉터리에 대고 풀기 전에 물어 둘 값어치가 있는
     * 물음에 답한다 - 이 경로가 이미 뿌리에서 시작하는가? 규칙은 플랫폼마다 다르다 -
     * 여기서는 앞의 '/', 윈도에서는 드라이브 문자나 UNC 접두사 - 그리고 바로 그 때문에
     * 이것은 '/' 와의 비교가 아니라 호출이다. */
    EXAMPLE_REQUIRE(!proven_fs_is_absolute(live), "the working paths in this example are relative");
    EXAMPLE_REQUIRE(proven_fs_is_absolute(PROVEN_LIT("/etc/hosts")), "a leading slash is absolute on POSIX");

    static const record_t initial[] = {
        { .id = 1, .score = 10 }, { .id = 2, .score = 20 },
        { .id = 3, .score = 30 }, { .id = 4, .score = 40 },
    };

    /* --- 평범한 첫 쓰기 ---------------------------------------------------- */

    proven_result_file_t f = proven_fs_open(alloc, live, PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
    EXAMPLE_REQUIRE(proven_is_ok(f.err), "creating the data file must succeed");
    if (!proven_is_ok(f.err)) return 1;

    proven_err_t err = write_records(f.value, initial, 4);
    EXAMPLE_REQUIRE(proven_is_ok(err), "writing the initial records must succeed");
    err = proven_fs_close(f.value);
    EXAMPLE_REQUIRE(proven_is_ok(err), "closing after a write must succeed");

    /* --- 두 벌이 서로 끼어들지 못하게 하는 권고 잠금 ----------------------- */

    proven_result_file_t rw = proven_fs_open(alloc, live, PROVEN_FS_READ | PROVEN_FS_WRITE);
    EXAMPLE_REQUIRE(proven_is_ok(rw.err), "reopening for update must succeed");
    if (!proven_is_ok(rw.err)) return 1;

    /* *배타* 잠금은 같은 것을 청하는 다른 모든 프로세스를 밖에 세운다. "권고" 는 말
     * 그대로다. 협조하는 프로그램을 막을 뿐, 잠금을 아예 청하지 않는 프로그램에는 아무
     * 영향이 없다. `wait = false` 는 막지 않고 곧바로 돌아온다 - 달리 할 일이 있을 때
     * 옳은 선택이고, 잠금을 쥔 쪽이 여러분을 기다리고 있을 수 있을 때는 유일하게 안전한
     * 선택이다. */
    err = proven_fs_lock(rw.value, PROVEN_FS_LOCK_EXCLUSIVE, false);
    EXAMPLE_REQUIRE(proven_is_ok(err), "taking the exclusive lock must succeed when nobody holds it");

    /* --- 레코드 하나를, 위치를 지정해 읽고 쓰기 --------------------------- */

    /* pread 는 절대 위치에서 읽고 파일 위치를 옮기지 *않는다*. 그래서 손잡이 하나를
     * 나눠 쓰는 두 스레드에서도 안전하다 - 그들이 다툴 공유 커서가 없다. */
    record_t third = {0};
    proven_mem_mut_t into = { .ptr = (proven_byte_t *)&third, .size = sizeof third };
    proven_result_size_t got = proven_fs_pread(rw.value, into, 2 * sizeof(record_t));
    EXAMPLE_REQUIRE(proven_is_ok(got.err) && got.value == sizeof third, "reading record 2 must succeed");
    EXAMPLE_REQUIRE(third.id == 3 && third.score == 30, "and yield the record that was written there");

    /* tell 은 위치를 알려 준다. pread 뒤에도 그것은 움직이지 않았다. */
    proven_result_u64_t pos = proven_fs_tell(rw.value);
    EXAMPLE_REQUIRE(proven_is_ok(pos.err) && pos.val == 0, "pread must not move the file position");

    /* pwrite 는 그 레코드를 제자리에서 고친다. 이번에도 커서는 건드리지 않는다. */
    third.score = 99;
    proven_mem_view_t out_view = { .ptr = (const proven_byte_t *)&third, .size = sizeof third };
    proven_result_size_t put = proven_fs_pwrite(rw.value, out_view, 2 * sizeof(record_t));
    EXAMPLE_REQUIRE(proven_is_ok(put.err) && put.value == sizeof third, "writing record 2 back must succeed");

    /* seek 은 커서를 옮기는 쪽이고, 도착한 위치를 돌려준다. 음수 오프셋으로 *끝*에서
     * seek 하는 것이 파일 길이를 먼저 알지 않고 마지막 레코드를 찾는 방법이다. */
    proven_result_u64_t last = proven_fs_seek(rw.value, -(proven_i64)sizeof(record_t), PROVEN_FS_SEEK_END);
    EXAMPLE_REQUIRE(proven_is_ok(last.err), "seeking to the last record must succeed");
    EXAMPLE_REQUIRE(last.val == 3 * sizeof(record_t), "which is three records in");

    pos = proven_fs_tell(rw.value);
    EXAMPLE_REQUIRE(proven_is_ok(pos.err) && pos.val == last.val, "tell agrees with the seek result");

    /* truncate 는 길이를 곧장 정한다. 마지막 레코드를 버리는 것이 호출 하나에 O(1) 이다.
     * 옛날 방식 - 전부 읽고 남길 부분을 다시 쓰기 - 은 파일 시스템이 수 하나를 고쳐서
     * 하는 일에 O(n) 복사를 치르는 것이었다. */
    err = proven_fs_truncate(rw.value, 3 * sizeof(record_t));
    EXAMPLE_REQUIRE(proven_is_ok(err), "truncating to three records must succeed");
    proven_result_size_t size = proven_fs_size(rw.value);
    EXAMPLE_REQUIRE(proven_is_ok(size.err) && size.value == 3 * sizeof(record_t),
                    "the file is now exactly three records long");

    /* 잠금을 명시적으로 푼다. 손잡이를 닫아도 풀리지만, 그렇다고 적어 두면 임계 구역이
     * 코드에 눈에 보인 채로 남는다. */
    err = proven_fs_lock(rw.value, PROVEN_FS_LOCK_UNLOCK, false);
    EXAMPLE_REQUIRE(proven_is_ok(err), "releasing the lock must succeed");
    err = proven_fs_close(rw.value);
    EXAMPLE_REQUIRE(proven_is_ok(err), "closing the update handle must succeed");

    /* --- 견디는 바꿔치기 --------------------------------------------------- */

    static const record_t replacement[] = {
        { .id = 1, .score = 11 }, { .id = 2, .score = 22 },
    };

    /* 1. 새 내용을 옛 파일 옆에 쓴다. CREATE_NEW 는 임시 이름이 이미 있으면 거부하고,
     *    그것이 죽어 버린 실행이 남긴 찌꺼기를 조용히 재사용하지 않고 알아채는 방법이다. */
    proven_result_file_t tmp = proven_fs_open(alloc, temp,
                                              PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
    EXAMPLE_REQUIRE(proven_is_ok(tmp.err), "creating the temporary file must succeed");
    if (!proven_is_ok(tmp.err)) return 1;

    err = write_records(tmp.value, replacement, 2);
    EXAMPLE_REQUIRE(proven_is_ok(err), "writing the new contents must succeed");

    /* 2. 그 바이트를 저장 장치까지 끝까지 밀어 준다. 값이 비싸고, 비싸야 마땅하다.
     *    정전 뒤에도 자료가 있다는 보장을 사는 것이고, 그 값은 장치까지 실제로 다녀오는
     *    일이다. */
    err = proven_fs_sync(tmp.value);
    EXAMPLE_REQUIRE(proven_is_ok(err), "syncing the new file's data must succeed");

    err = proven_fs_close(tmp.value);
    EXAMPLE_REQUIRE(proven_is_ok(err), "closing the temporary file must succeed");

    /* 3. 이름을 넘긴다. 한 디렉터리 안의 rename 은 원자적이다. 읽는 쪽은 옛 파일이나 새
     *    파일을 보지, 반쯤 쓰인 것을 보지 않는다. */
    err = proven_fs_rename(alloc, temp, live);
    EXAMPLE_REQUIRE(proven_is_ok(err), "renaming the temporary file over the live one must succeed");

    /* 4. 그 이름 바꾸기 자체를 견디게 만든다. 디렉터리가 장치에 닿기 전까지는, 새 내용이
     *    안전하지 않을 수도 있는 이름 아래에 안전하게 있는 것이다. */
    err = proven_fs_sync_dir(alloc, here);
    EXAMPLE_REQUIRE(proven_is_ok(err), "syncing the directory must succeed");

    proven_result_file_t check = proven_fs_open(alloc, live, PROVEN_FS_READ);
    EXAMPLE_REQUIRE(proven_is_ok(check.err), "the live path must now open");
    size = proven_fs_size(check.value);
    EXAMPLE_REQUIRE(proven_is_ok(size.err) && size.value == 2 * sizeof(record_t),
                    "and hold exactly the replacement records");
    err = proven_fs_close(check.value);
    EXAMPLE_REQUIRE(proven_is_ok(err), "closing the verification handle must succeed");

    /* --- 복사, 그리고 두 가지 링크 ---------------------------------------- */

    /* copy 는 바이트를 복제한다. 이제부터는 서로 독립인 파일 둘이다. 할당자는 복사가
     * 자료를 옮겨 가는 임시 버퍼를 위한 것이다. */
    proven_u8str_view_t backup = PROVEN_LIT("proven_example_durable.bak");
    err = proven_fs_copy(alloc, live, backup);
    EXAMPLE_REQUIRE(proven_is_ok(err), "copying the file must succeed");

    /* *하드* 링크는 같은 파일의 두 번째 이름이다. 원본이라는 것이 없다. 자료는 마지막
     * 이름이 지워질 때까지 산다. 두 이름은 같은 파일 시스템에 있어야 한다. 이름과 그
     * 자료가 둘에 걸칠 수는 없기 때문이다. */
    proven_u8str_view_t hard = PROVEN_LIT("proven_example_durable.hard");
    err = proven_fs_link(alloc, live, hard);
    EXAMPLE_REQUIRE(proven_is_ok(err), "creating a hard link must succeed");

    proven_fs_stat_t st_live = {0}, st_hard = {0};
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_stat(alloc, live, &st_live)), "stat of the live name");
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_stat(alloc, hard, &st_hard)), "stat of the hard link");
    EXAMPLE_REQUIRE(st_live.ino == st_hard.ino && st_live.dev == st_hard.dev,
                    "both names refer to the same file, which is what a hard link means");

    /* *심볼릭* 링크는 경로를 담은 작은 파일이다. 다른 파일 시스템의 무언가를 가리킬 수도
     * 있고, 아무것도 아닌 것을 가리킬 수도 있다 - 그때 따라가기는 실패하는데, 하드 링크는
     * 결코 그럴 수 없다. */
    proven_u8str_view_t soft = PROVEN_LIT("proven_example_durable.link");
    err = proven_fs_symlink(alloc, live, soft);
    EXAMPLE_REQUIRE(proven_is_ok(err), "creating a symbolic link must succeed");

    proven_fs_stat_t st_soft = {0};
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_stat(alloc, soft, &st_soft)),
                    "stat follows the symbolic link to its target");
    EXAMPLE_REQUIRE(st_soft.size == st_live.size, "so it reports the target's size");

    /* --- 디렉터리, 그리고 뒷정리 ------------------------------------------ */

    proven_u8str_view_t dir = PROVEN_LIT("proven_example_durable_dir");
    err = proven_fs_mkdir(alloc, dir);
    EXAMPLE_REQUIRE(proven_is_ok(err), "creating a directory must succeed");

    /* rmdir 은 *빈* 디렉터리만 지운다. 그 거부가 기능이다. 재귀 삭제는 부르는 쪽이
     * 명시적으로 내려야 하는 결정이지, 잘못 들어온 경로 인자 하나가 일으킬 수 있는 일이
     * 아니다. */
    proven_u8str_view_t inside = PROVEN_LIT("proven_example_durable_dir/file.txt");
    proven_result_file_t child = proven_fs_open(alloc, inside, PROVEN_FS_WRITE | PROVEN_FS_CREATE);
    EXAMPLE_REQUIRE(proven_is_ok(child.err), "creating a file inside it must succeed");
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_close(child.value)), "closing it must succeed");

    err = proven_fs_rmdir(alloc, dir);
    EXAMPLE_REQUIRE(err != PROVEN_OK, "removing a non-empty directory must be refused");

    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_remove(alloc, inside)), "removing the file must succeed");
    err = proven_fs_rmdir(alloc, dir);
    EXAMPLE_REQUIRE(proven_is_ok(err), "and then the empty directory can be removed");

    printf("durable replace complete: %zu byte(s) live\n", (size_t)size.value);

    (void)proven_fs_remove(alloc, soft);
    (void)proven_fs_remove(alloc, hard);
    (void)proven_fs_remove(alloc, backup);
    (void)proven_fs_remove(alloc, live);
    return EXAMPLE_OK();
}
