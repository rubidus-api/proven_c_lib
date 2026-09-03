#include "example.h"

/*
 * 쓰는 쪽은 "바이트가 가는 어딘가" 이고 읽는 쪽은 "바이트가 오는 어딘가" 이며, 각각
 * 포인터 둘이다. 작은 함수 표 하나와 그 함수들이 다룰 상태 하나. 그것이 전부다. 이
 * 얼개의 값어치는, 쓰는 쪽에 기대어 쓴 코드가 그 바이트가 파일로 가는지 문자열로 가는지
 * 터미널로 가는지 시험용 버퍼로 가는지 알지도 못하고 신경 쓰지도 않는다는 것이다 -
 * 그리고 읽는 쪽에 기대어 쓴 코드는 디스크의 파일 대신 기억 속 문자열에 대고 시험할 수
 * 있다.
 *
 * 이 프로그램은 둘 다, 그리고 거기 매달린 표준 스트림과 파일 배관을 보인다.
 *
 *   - 뷰(기억 속 문자열) 위의 읽는 쪽. 파일 시스템을 건드리지 않고 파서를 시험하는
 *     방법이다,
 *   - 버퍼 없는 stdout 과 stderr 쓰는 쪽, 그리고 "이걸 다 써라" 와 "쓸 수 있는 만큼
 *     써라" 의 차이,
 *   - 파일 위의 버퍼 둔 쓰는 쪽. 작은 쓰기 여럿을 큰 쓰기 몇으로 바꿔 준다,
 *   - 그 같은 파일 위의 줄 단위 읽는 쪽,
 *   - 파일 손잡이에서 곧장 형식화된 값 읽기.
 *
 * 사람들이 반나절을 잃게 만드는 주의 하나: 아래의 상태 구조체들은 선언된 자리에 그대로
 * 있어야 한다. 쓰는 쪽은 자기 상태 구조체 *안*을 가리키는 포인터를 쥐므로, 구조체를
 * 복사하면 사본은 죽은 것이 되고 주소는 원본을 가리킨 채로 남는다.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- 1. 이미 가진 바이트 위의 읽는 쪽 --------------------------------- */

    /* 아래 파서는 이것이 문자열이라는 것을 모른다. 파일이든 파이프든 소켓이든 똑같이
     * 돌 것이다 - 그래서 시험이 값싼 쪽을 쓸 수 있다. */
    proven_reader_view_t src_state;
    proven_reader_t src = proven_reader_from_view(&src_state, PROVEN_LIT("id=41\nid=42\n"));

    /* is_valid 는 그 읽는 쪽이 실제로 만들어졌는지 묻는다 - 0 으로 초기화된 손잡이는
     * 읽는 쪽이 아니고, 그것을 첫 읽기가 아니라 경계에서 말해 주는 검사가 이것이다. */
    EXAMPLE_REQUIRE(proven_reader_is_valid(src), "a reader made from a view must be usable");
    proven_reader_t never_made = {0};
    EXAMPLE_REQUIRE(!proven_reader_is_valid(never_made), "a zero-initialised reader handle is not");

    /* read 는 dest.size 바이트까지 채우고 실제로 얼마나 받았는지 알려 준다. 짧은 읽기는
     * 정상이다 - 파일의 끝이 아니고, 그것을 끝으로 여기는 것이 입력의 꼬리를 잃는
     * 고전적인 방법이다. 파일의 끝에는 제 코드가 따로 있다, PROVEN_ERR_EOF. */
    proven_byte_t chunk[5];
    proven_mem_mut_t into = { .ptr = chunk, .size = sizeof chunk };
    proven_result_size_t got = proven_reader_read(src, into);
    EXAMPLE_REQUIRE(proven_is_ok(got.err), "the first read must succeed");
    EXAMPLE_REQUIRE(got.value == 5, "and fill the buffer from the view");

    proven_size_t total = got.value;
    for (;;) {
        got = proven_reader_read(src, into);
        if (got.err == PROVEN_ERR_EOF) {
            break;
        }
        EXAMPLE_REQUIRE(proven_is_ok(got.err), "reads before end of file must succeed");
        total += got.value;
    }
    EXAMPLE_REQUIRE(total == 12, "the loop must consume the whole view, however it was chunked");

    /* --- 2. 표준 스트림 ---------------------------------------------------- */

    /* 상태 구조체가 쓰는 쪽이 가리키는 손잡이를 쥐고 있으므로, 쓰는 쪽보다 오래 살아야
     * 한다. 둘을 나란히 선언하는 습관이 그것을 지켜 준다. */
    proven_sysio_std_t out_state;
    proven_writer_t out = proven_sysio_stdout_writer(&out_state);
    EXAMPLE_REQUIRE(proven_writer_is_valid(out), "the stdout writer must be usable");

    /* write 는 "전부, 아니면 오류" 라는 뜻이다. 안에서 반복한다. 시스템 수준의 쓰기 한
     * 번은 청한 것보다 적은 바이트를 옮길 수 있기 때문이다. */
    proven_err_t err = proven_writer_write(out, proven_mem_view_from_u8(PROVEN_LIT("stream example: start\n")));
    EXAMPLE_REQUIRE(proven_is_ok(err), "writing a whole line to stdout must succeed");

    /* write_partial 은 정직한 저수준 쌍둥이다. 옮길 수 있는 만큼 옮기고 그 개수를 알려
     * 준다. 재시도나 역압을 여러분이 다룰 때 쓰고, 그냥 바이트를 내보내고 싶으면 write 를
     * 쓸 것. */
    proven_result_size_t part = proven_writer_write_partial(out, proven_mem_view_from_u8(PROVEN_LIT("partial write\n")));
    EXAMPLE_REQUIRE(proven_is_ok(part.err), "a partial write to a terminal or pipe must succeed");
    EXAMPLE_REQUIRE(part.value > 0, "and report how many bytes it moved");

    /* stderr 에 버퍼를 두지 않은 것은 일부러다. 진단은 다음 줄의 코드가 돌기 전에 나가야
     * 하고, 그 다음 줄이 바로 죽는 줄일 때 그것이 필요하다. */
    proven_sysio_std_t err_state;
    proven_writer_t diag = proven_sysio_stderr_writer(&err_state);
    EXAMPLE_REQUIRE(proven_writer_is_valid(diag), "the stderr writer must be usable");
    err = proven_writer_write(diag, proven_mem_view_from_u8(PROVEN_LIT("stream example: diagnostics go here\n")));
    EXAMPLE_REQUIRE(proven_is_ok(err), "writing to stderr must succeed");

    /* stdin 위의 읽는 쪽도 같은 방식으로 만든다. 이 예제는 거기서 읽지 않는다 - 시험
     * 실행에는 타이핑하는 사람이 없다 - 다만 만드는 모양을 보이고, 필터 프로그램이 돌
     * 손잡이가 이것이다. */
    proven_sysio_std_t in_state;
    proven_reader_t stdin_reader = proven_sysio_stdin_reader(&in_state);
    EXAMPLE_REQUIRE(proven_reader_is_valid(stdin_reader), "the stdin reader must be usable");

    /* --- 3. 파일로 나가는 버퍼 둔 출력 ------------------------------------ */

    proven_u8str_view_t path = PROVEN_LIT("proven_example_streams.txt");
    proven_result_file_t f = proven_fs_open(alloc, path, PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
    EXAMPLE_REQUIRE(proven_is_ok(f.err), "creating the output file must succeed");
    if (!proven_is_ok(f.err)) return 1;

    /* 버퍼는 여러분의 것이다. 라이브러리가 등 뒤에서 하나 할당하지 않으므로, 버퍼링의
     * 기억 비용은 여러분이 고른, 눈에 보이는 수다. 256바이트 버퍼를 지나는 예순 줄은
     * 예순 번이 아니라 몇 번의 쓰기다. */
    proven_byte_t outbuf[256];
    proven_sysio_out_t file_out;
    proven_writer_t w = proven_sysio_file_buffered(&file_out, f.value,
                                                   (proven_mem_mut_t){ .ptr = outbuf, .size = sizeof outbuf });
    EXAMPLE_REQUIRE(proven_writer_is_valid(w), "the buffered file writer must be usable");

    for (int i = 0; i < 20; ++i) {
        proven_fmt_result_t line_out = proven_fprintln(w, "reading {} = {}",
                                                       proven_arg_i32(i), proven_arg_i32(i * i));
        EXAMPLE_REQUIRE(proven_is_ok(line_out.err), "writing a formatted line must succeed");
    }

    /* 흘려 보내지 않은 버퍼 출력은 일어나지 않은 출력이다. 여기 어느 것도 여러분 대신
     * 종료 시점에 흘려 주지 않는다. */
    err = proven_writer_flush(w);
    EXAMPLE_REQUIRE(proven_is_ok(err), "the flush is what actually writes the file");
    err = proven_fs_close(f.value);
    EXAMPLE_REQUIRE(proven_is_ok(err), "closing the file must succeed");

    /* --- 4. 한 줄씩 되읽기 ------------------------------------------------ */

    proven_result_file_t rf = proven_fs_open(alloc, path, PROVEN_FS_READ);
    EXAMPLE_REQUIRE(proven_is_ok(rf.err), "reopening for reading must succeed");
    if (!proven_is_ok(rf.err)) return 1;

    /* 버퍼는 예상되는 가장 긴 *줄*에 맞춰 잡을 것. 더 긴 줄은 OUT_OF_BOUNDS 로 보고된다 -
     * 조용히 반으로 잘리는 일은 없다. 그 자르기가 나쁜 레코드 하나를 그럴듯한 레코드
     * 둘로 만드는 실패다. */
    proven_byte_t linebuf[128];
    proven_sysio_lines_t lines;
    err = proven_sysio_lines_open(&lines, rf.value, (proven_mem_mut_t){ .ptr = linebuf, .size = sizeof linebuf });
    EXAMPLE_REQUIRE(proven_is_ok(err), "opening a line reader over the file must succeed");

    proven_size_t count = 0;
    for (;;) {
        proven_result_u8str_view_t line = proven_sysio_read_line(&lines);
        if (line.err == PROVEN_ERR_EOF) {
            break;
        }
        EXAMPLE_REQUIRE(proven_is_ok(line.err), "reading a line must succeed until end of file");
        /* 그 뷰는 linebuf 안을 가리키고 다음 호출 전까지만 쓸 수 있다. 그래서 백만 줄이
         * 백만 번의 할당이 아니라 버퍼 하나만큼의 값이 든다 - 그리고 남겨 둘 줄은 복사해야
         * 하는 이유이기도 하다. */
        if (count == 0) {
            EXAMPLE_REQUIRE(proven_u8str_view_eq(line.val, PROVEN_LIT("reading 0 = 0")),
                            "the first line reads back exactly as it was written");
        }
        ++count;
    }
    EXAMPLE_REQUIRE(count == 20, "every line written must be read back");
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_close(rf.value)), "closing the read handle must succeed");

    /* --- 5. 파일에서 줄이 아니라 값을 읽기 -------------------------------- */

    /* 입력이 자유로운 글이 아니라 정해진 모양일 때는, 곧장 파싱하는 편이 자르고 변환하는
     * 반복문을 손으로 쓰는 수고를 던다. 이것은 파일 손잡이에서 한 토막을 읽어 그 안에서
     * 수 둘을 뽑아낸다. */
    proven_result_file_t sf = proven_fs_open(alloc, path, PROVEN_FS_READ);
    EXAMPLE_REQUIRE(proven_is_ok(sf.err), "opening the file for scanning must succeed");

    proven_i32 index = -1, square = -1;
    err = proven_scan_fmt_from_file(sf.value, "reading {} = {}",
                                    proven_scan_arg_i32(&index), proven_scan_arg_i32(&square));
    EXAMPLE_REQUIRE(proven_is_ok(err), "scanning the first record must succeed");
    EXAMPLE_REQUIRE(index == 0 && square == 0, "and produce the values that were written");
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_close(sf.value)), "closing the scan handle must succeed");

    printf("streams: %zu byte(s) read from the view, %zu line(s) round-tripped\n",
           (size_t)total, (size_t)count);

    (void)proven_fs_remove(alloc, path);
    return EXAMPLE_OK();
}
