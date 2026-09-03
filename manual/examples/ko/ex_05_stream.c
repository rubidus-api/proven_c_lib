#include "example.h"

/*
 * 쓰는 쪽과 읽는 쪽. "바이트가 어디로 가는가" 를 위한 인터페이스 하나와 "바이트가
 * 어디서 오는가" 를 위한 인터페이스 하나.
 *
 * 요점은 아래 코드 - render_row - 가 자기가 문자열로 쓰는지, 고정 버퍼로 쓰는지, 파일로
 * 쓰는지 모르고 신경도 쓰지 않는다는 것이다. 예전에는 그럴 수 없었다. 형식화기가 받는
 * 그릇은 proven_u8str_t 하나뿐이었다.
 */

/* 직렬화기 하나. 목적지가 아니라 그릇을 받는다. */
static proven_err_t render_row(proven_writer_t w, int id, const char *name) {
    proven_fmt_result_t r = proven_fprintln(w, "{:>4} | {}", PROVEN_ARG(id), PROVEN_ARG(name));
    return r.err;
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- 같은 코드로, 늘어나는 문자열에 ------------------------------------ */
    proven_result_u8str_t s = proven_u8str_create(alloc, 16);
    EXAMPLE_REQUIRE(proven_is_ok(s.err), "string create");

    proven_writer_u8str_t s_state;
    proven_writer_t to_string = proven_writer_from_u8str(&s_state, &s.value, alloc);
    EXAMPLE_REQUIRE(proven_is_ok(render_row(to_string, 7, "ada")), "render into a string");

    printf("into a string:\n%s", proven_u8str_as_cstr(&s.value));
    proven_u8str_destroy(alloc, &s.value);

    /* --- 같은 코드로, 여러분이 소유한 기억에: 할당은 0 -------------------- */
    proven_byte_t fixed[64];
    proven_writer_buf_t b_state = { .buf = { .ptr = fixed, .size = sizeof fixed } };
    proven_writer_t to_buffer = proven_writer_from_buffer(&b_state);
    EXAMPLE_REQUIRE(proven_is_ok(render_row(to_buffer, 8, "grace")), "render into a buffer");
    EXAMPLE_REQUIRE(b_state.len > 0, "the buffer received the row");

    /* 가득 찬 버퍼는 *거부한다*. 자르지 않는다. 여러분 자료의 끝을 조용히 버리는 그릇은
     * 받을 수 없다고 말하는 그릇보다 나쁘다. */
    proven_byte_t tiny[4];
    proven_writer_buf_t t_state = { .buf = { .ptr = tiny, .size = sizeof tiny } };
    proven_writer_t to_tiny = proven_writer_from_buffer(&t_state);
    EXAMPLE_REQUIRE(proven_writer_write_str(to_tiny, PROVEN_LIT("far too long")) == PROVEN_ERR_OUT_OF_BOUNDS,
                    "a full buffer refuses rather than truncating");
    EXAMPLE_REQUIRE(t_state.overflowed, "and it records that it did");

    /* --- 같은 코드로, 파일에, 버퍼를 두고 ---------------------------------- */
    /*
     * 버퍼는 아레나에서와 똑같이 *여러분이* 대는 기억이다. 이 라이브러리에는 숨은 전역
     * 상태가 없으므로 종료 시점에 대신 흘려 줄 수 없다 - 그래서 버퍼가 스코프를 벗어나기
     * 전에 반드시 flush 해야 한다. 그 대가로 로그 경로에서는 할당이 전혀 일어나지 않는다.
     * 여기서 만 줄은 malloc 0 번에 write 시스템 호출 스물 몇 번이지만, proven_println 을
     * 만 번 부르면 시스템 호출 10,000 번이다.
     */
    proven_u8str_view_t path = PROVEN_LIT("example_stream_rows.txt");
    proven_result_file_t f = proven_fs_open(alloc, path,
        (proven_fs_mode_t)(PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC));
    EXAMPLE_REQUIRE(proven_is_ok(f.err), "open the output file");
    proven_file_t file = f.value;

    proven_byte_t out_buf[4096];
    proven_writer_buffered_t w_state;
    proven_writer_t to_file = proven_writer_buffered(&w_state,
        proven_writer_from_file(&file),
        (proven_mem_mut_t){ .ptr = out_buf, .size = sizeof out_buf });

    for (int i = 0; i < 3; ++i) {
        EXAMPLE_REQUIRE(proven_is_ok(render_row(to_file, i, "row")), "render into the file");
    }
    EXAMPLE_REQUIRE(proven_is_ok(proven_writer_flush(to_file)),
                    "flush: nothing is written until you say so");
    /* 그리고 close - 쓰기가 도착하지 못했다고 알려 줄 수 있는 마지막 자리다. */
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_close(file)), "closing the written file");

    /* --- 한 줄씩 되읽기 ---------------------------------------------------- */
    /* 파일을 한 줄씩 읽는 것은 예전에는 아예 되지 않았다. 길은 파일 전체를 기억에 올려
     * 손으로 자르는 것뿐이었다. */
    proven_result_file_t rf = proven_fs_open(alloc, path, PROVEN_FS_READ);
    EXAMPLE_REQUIRE(proven_is_ok(rf.err), "reopen for reading");
    proven_file_t rfile = rf.value;

    proven_byte_t in_buf[128];
    proven_reader_buffered_t r_state;
    (void)proven_reader_buffered(&r_state, proven_reader_from_file(&rfile),
                                 (proven_mem_mut_t){ .ptr = in_buf, .size = sizeof in_buf });

    int lines = 0;
    for (;;) {
        proven_result_u8str_view_t line = proven_reader_read_line(&r_state);
        if (line.err == PROVEN_ERR_EOF) break;
        EXAMPLE_REQUIRE(proven_is_ok(line.err), "read a line");
        /* 그 뷰는 읽는 쪽의 버퍼 *안*을 가리키고, 다음 호출 전까지만 쓸 수 있다. 그보다
         * 오래 살아야 한다면 복사할 것. */
        printf("line %d: %.*s\n", lines, (int)line.val.size, (const char *)line.val.ptr);
        ++lines;
    }
    EXAMPLE_REQUIRE(lines == 3, "three rows in, three lines out");

    (void)proven_fs_close(rfile);
    (void)proven_fs_remove(alloc, path);

    return EXAMPLE_OK();
}
