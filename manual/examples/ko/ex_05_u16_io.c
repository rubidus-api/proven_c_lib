#include "example.h"
#include <string.h>

/*
 * UTF-16 텍스트의 입출력: 표준 스트림으로, 읽을 쪽이 기대하는 인코딩의 파일로, 그리고
 * 한 줄씩 다시 안으로.
 *
 * 출력은 포매터를 거치므로, u8 텍스트를 받는 모든 곳이 u16 텍스트도 받는다: PROVEN_ARG 는
 * proven_u16str_view_t 에 u16 렌더러를 골라 UTF-8 로 쓴다. 받는 쪽이 UTF-16 바이트를
 * 원하면 - UTF-16LE 파일을 읽는 윈도 도구 같은 - 쓰기 스트림이 지정한 바이트 순서로 쓴다.
 *
 * 입력은 세 인코딩 중 어느 것이든 내 u16 버퍼로 해독하는 읽기 스트림이다. 원본을 두 번
 * 읽는 사이에 잘린 문자를 이어 붙이고, 바이트 줄 리더의 규칙을 그대로 지킨다: "\r\n" 은
 * '\r' 을 잃고, 마지막 줄은 개행이 없어도 되며, 버퍼보다 긴 줄은 잘린 줄이 아니라 오류다.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* "안녕 🙂" - 한글과, 서로게이트 쌍인 이모지. */
    static const proven_u16 hello[] = { 0xC548, 0xB155, ' ', 0xD83D, 0xDE42 };
    proven_u16str_view_t text = { hello, 5 };

    /* --- 표준 스트림으로 ----------------------------------------------- */

    /* stdout 과 stderr 에는 UTF-8. 윈도 콘솔이면 sysio 계층이 UTF-16 으로 넘기므로,
     * 코드 페이지가 무엇이든 올바르게 보인다. */
    proven_err_t err = proven_println("stdout: {}", PROVEN_ARG(text));
    EXAMPLE_REQUIRE(proven_is_ok(err), "u16 text prints like u8 text");
    err = proven_eprintln("stderr: {}", proven_arg_u16(text));
    EXAMPLE_REQUIRE(proven_is_ok(err), "and to stderr");

    /* 버퍼 쓰기 스트림도 받는다. 플러시하라, 안 하면 없었던 일이다. */
    proven_sysio_out_t out;
    proven_byte_t buf[256];
    proven_writer_t w = proven_sysio_stdout_buffered(&out, (proven_mem_mut_t){ buf, sizeof buf });
    EXAMPLE_REQUIRE(proven_is_ok(proven_fprintln(w, "buffered: {}", PROVEN_ARG(text)).err), "fprintln takes u16");
    EXAMPLE_REQUIRE(proven_is_ok(proven_writer_flush(w)), "and the flush sends it");

    /* 엄격: 짝 없는 서로게이트는 쓰레기를 찍는 대신 줄 전체를 실패시킨다. */
    static const proven_u16 torn_units[] = { 'o', 'k', 0xD83D };
    proven_u16str_view_t torn = { torn_units, 3 };
    err = proven_println("{}", PROVEN_ARG(torn));
    EXAMPLE_REQUIRE(err == PROVEN_ERR_INVALID_ENCODING, "half a surrogate pair is refused");

    /* --- 파일로, BOM 을 붙인 UTF-16LE 로 ----------------------------------- */

    proven_u8str_view_t path = PROVEN_LIT("proven_example_u16.txt");
    proven_result_file_t f = proven_fs_open(alloc, path, PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
    EXAMPLE_REQUIRE(proven_is_ok(f.err), "creating the file must succeed");
    if (!proven_is_ok(f.err)) return EXAMPLE_OK();

    proven_sysio_out_t fout;
    proven_writer_t fw = proven_sysio_file_buffered(&fout, f.value, (proven_mem_mut_t){ buf, sizeof buf });
    /* BOM 은 요청했기 때문에만 쓰인다. */
    err = proven_writer_write_bom(fw, PROVEN_TEXT_UTF16LE);
    for (int i = 0; i < 2 && proven_is_ok(err); ++i) {
        err = proven_writer_write_u16(fw, text, PROVEN_TEXT_UTF16LE);
        if (proven_is_ok(err)) err = proven_writer_write_u16(fw, (proven_u16str_view_t){ u"\r\n", 2 }, PROVEN_TEXT_UTF16LE);
    }
    if (proven_is_ok(err)) err = proven_writer_flush(fw);
    EXAMPLE_REQUIRE(proven_is_ok(err), "two lines of UTF-16LE are written");
    (void)proven_fs_close(f.value);

    /* --- 다시 안으로, 한 줄씩 --------------------------------------------- */

    f = proven_fs_open(alloc, path, PROVEN_FS_READ);
    EXAMPLE_REQUIRE(proven_is_ok(f.err), "opening the file again must succeed");
    if (proven_is_ok(f.err)) {
        /* AUTO: BOM 이 UTF-16LE 라고 알려 주고, 소비된다. 버퍼는 코드 단위이고
         * 가장 긴 줄을 담아야 한다. */
        proven_u16 line_buf[64];
        proven_sysio_u16_lines_t lines;
        err = proven_sysio_u16_lines_open(&lines, f.value, PROVEN_TEXT_AUTO, line_buf, 64);
        EXAMPLE_REQUIRE(proven_is_ok(err), "the line reader opens");
        int n = 0;
        for (;;) {
            proven_result_u16str_view_t line = proven_sysio_read_u16_line(&lines);
            if (line.err == PROVEN_ERR_EOF) break;
            EXAMPLE_REQUIRE(proven_is_ok(line.err), "each line reads");
            if (!proven_is_ok(line.err)) break;
            /* 뷰는 다음 호출 전까지 line_buf 안을 가리킨다. */
            EXAMPLE_REQUIRE(line.val.size == 5 && memcmp(line.val.ptr, hello, sizeof hello) == 0,
                            "each line is the text, without its CR LF");
            ++n;
        }
        EXAMPLE_REQUIRE(n == 2 && lines.reader.enc == PROVEN_TEXT_UTF16LE, "two lines, read as UTF-16LE");
        (void)proven_fs_close(f.value);
    }
    (void)proven_fs_remove(alloc, path);

    /* --- 어떤 읽기 스트림이든, 어떤 인코딩이든 ------------------------------ */

    /* 이미 메모리에 있는 바이트 - 여기서는 UTF-8 - 위의 같은 리더. read() 는 어느 줄에
     * 속하든 코드 단위를 내주고, 쌍의 반쪽은 절대 내주지 않는다. */
    proven_reader_view_t src;
    proven_u16 rbuf[16];
    proven_u16_reader_t rd;
    err = proven_u16_reader_init(&rd, proven_reader_from_view(&src, PROVEN_LIT("A\xF0\x9F\x99\x82\nB")),
                                 PROVEN_TEXT_UTF8, rbuf, 16);
    EXAMPLE_REQUIRE(proven_is_ok(err), "a u16 reader over UTF-8 bytes");
    proven_result_u16str_view_t first = proven_u16_reader_read_line(&rd);
    EXAMPLE_REQUIRE(proven_is_ok(first.err) && first.val.size == 3, "'A' and the emoji's two units");
    proven_u16 rest[4];
    proven_result_size_t got = proven_u16_reader_read(&rd, rest, 4);
    EXAMPLE_REQUIRE(proven_is_ok(got.err) && got.value == 1 && rest[0] == 'B', "the last line, without a newline");
    EXAMPLE_REQUIRE(proven_u16_reader_read(&rd, rest, 4).err == PROVEN_ERR_EOF, "then the end");

    return EXAMPLE_OK();
}
