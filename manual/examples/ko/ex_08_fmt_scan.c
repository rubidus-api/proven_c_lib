#include "example.h"

/*
 * 형식화와 파싱은 같은 생각의 두 반쪽이다. `{}` 는 타입 있는 값을 글로 그려 내고,
 * 스캐너는 글을 타입 있는 목적지로 되읽는다. 둘 다 호출 자리에서 타입이 검사되므로
 * (_Generic 이 생성자를 고른다), 실행 중에 어긋날 서식 문자열/인자 짝이 없다.
 *
 * 중요한 선택은 *바이트가 어디로 가는가* 다.
 *
 *   append_fmt       - 용량 고정, 원자적. 너무 길면? 아무것도 쓰이지 않고
 *                      PROVEN_ERR_OUT_OF_BOUNDS 를 받는다. 할당자가 끼지 않으므로
 *                      스택 버퍼에서도 돈다.
 *   append_fmt_grow  - 할당자를 등에 업는다. 들어가도록 늘리고, 할당이 실패하면
 *                      문자열은 있던 그대로 남는다.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- 용량 고정: 할당자도, 할당도 없다 ---------------------------------- */
    /* borrow 는 부르는 쪽의 기억을 감싼다. 그래서 이 문자열은 통째로 스택에 산다. `cap`
     * 은 NUL 을 포함하므로 32 바이트는 내용 31 바이트를 담는다. 지울 것도 없다. */
    proven_byte_t stack_buf[32];
    proven_u8str_t fixed = proven_u8str_borrow(stack_buf, sizeof stack_buf);

    proven_fmt_result_t r = proven_u8str_append_fmt(&fixed, "port={}", PROVEN_ARG(8080));
    EXAMPLE_REQUIRE(PROVEN_FMT_IS_OK(r), "a short line should fit in 32 bytes");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&fixed), PROVEN_LIT("port=8080")),
                    "the fixed-capacity append should have rendered the port");

    /* 원자적이라는 말은 원자적이라는 뜻이다. 들어가지 않는 append 는 아무것도 바꾸지
     * 않는다. 문자열은 여전히 온전하고 전에 담고 있던 것을 그대로 담고 있다 - 치울 잘린
     * 꼬리가 없다. (잘린 꼬리가 원하는 것이라면 append_fmt_trunc 를 쓸 것.) */
    proven_fmt_result_t too_long = proven_u8str_append_fmt(
        &fixed, " and a great deal more text than will ever fit here {}", PROVEN_ARG(1));
    EXAMPLE_REQUIRE(too_long.err == PROVEN_ERR_OUT_OF_BOUNDS, "the overlong append must fail");
    EXAMPLE_REQUIRE(too_long.required > too_long.written, "it reports what it would have needed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&fixed), PROVEN_LIT("port=8080")),
                    "a failed atomic append must leave the string untouched");

    /* --- 지정자: 채움, 정렬, 폭, 16진 ------------------------------------- */
    proven_result_u8str_t created = proven_u8str_create(alloc, 8);   /* 일부러 작게 */
    EXAMPLE_REQUIRE(proven_is_ok(created.err), "creating the output string should succeed");
    if (!proven_is_ok(created.err)) return 1;
    proven_u8str_t out = created.value;

    /* grow 는 필요한 만큼 재할당하므로, 처음 용량은 한계가 아니라 힌트다.
     * `{:0>4}` = 채움 '0', 오른쪽 정렬, 폭 4. `{:x}` = 소문자 16진, 0x 없음. */
    r = proven_u8str_append_fmt_grow(alloc, &out, "id={:0>4} tag={:*^9} addr=0x{:x}",
                                     PROVEN_ARG(7),
                                     PROVEN_ARG(PROVEN_LIT("ok")),
                                     PROVEN_ARG(48879));
    EXAMPLE_REQUIRE(PROVEN_FMT_IS_OK(r), "the growing append should succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&out),
                                         PROVEN_LIT("id=0007 tag=***ok**** addr=0xbeef")),
                    "fill/align/width/hex should render exactly this");
    printf("%s\n", proven_u8str_as_cstr(&out));

    /* --- 믿을 수 없는 글은 경계가 있고, NUL 로 끝난다고 믿지 않는다 -------- */
    /* char* 에 PROVEN_ARG 를 쓰면 "NUL 이 나올 때까지 걸어라" 는 뜻이다 - 리터럴에는
     * 괜찮지만 소켓에서 온 것에는 버퍼 넘어 읽기다. 이 버퍼에는 NUL 이 아예 없다.
     * PROVEN_ARG_CSTR_N 은 대신 길이에서 멈추므로 실제로 있는 것만 읽는다. 여러분이 직접
     * 만들지 않은 것에는 이것을 쓸 것. */
    const char untrusted[4] = {'a', 'b', 'c', 'd'};   /* 일부러 종결자를 두지 않았다 */
    EXAMPLE_REQUIRE(proven_is_ok(proven_u8str_reset(&out)), "reset should keep the buffer");
    r = proven_u8str_append_fmt_grow(alloc, &out, "payload={}",
                                     PROVEN_ARG_CSTR_N(untrusted, sizeof untrusted));
    EXAMPLE_REQUIRE(PROVEN_FMT_IS_OK(r), "the bounded append should succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&out), PROVEN_LIT("payload=abcd")),
                    "the bounded argument should render its whole 4 bytes and stop");

    /* --- 레코드를 형식화하고, 도로 파싱하기 -------------------------------- */
    proven_i64 sensor_id = 42;
    double reading = 3.14159;

    EXAMPLE_REQUIRE(proven_is_ok(proven_u8str_reset(&out)), "reset should keep the buffer");
    r = proven_u8str_append_fmt_grow(alloc, &out, "{} {} {}",
                                     PROVEN_ARG(sensor_id),
                                     PROVEN_ARG(PROVEN_LIT("boiler")),
                                     PROVEN_ARG(reading));
    EXAMPLE_REQUIRE(PROVEN_FMT_IS_OK(r), "formatting the record should succeed");
    printf("record: %s\n", proven_u8str_as_cstr(&out));

    /* 뷰 하나 위의 스캐너 하나. 호출마다 커서를 자기가 삼킨 만큼 앞으로 옮기므로 호출이
     * 왼쪽에서 오른쪽으로 이어진다 - 그리고 하나하나가 따로 실패할 수 있는데, 그것이
     * 파서와 짐작을 가르는 차이다. */
    proven_scan_t sc = proven_scan_init(proven_u8str_as_view(&out));

    proven_result_i64_t id = proven_scan_i64(&sc);
    EXAMPLE_REQUIRE(proven_is_ok(id.err), "the first field should parse as an integer");
    EXAMPLE_REQUIRE(id.val == sensor_id, "the integer should round-trip");

    /* scan_str 은 *파싱 중인 문자열 안*을 가리키는 뷰를 돌려준다 - 복사도 소유도 하지
     * 않으므로 `out` 이 살아 있는 동안에만 쓸 수 있다. */
    proven_result_u8str_view_t name = proven_scan_str(&sc);
    EXAMPLE_REQUIRE(proven_is_ok(name.err), "the second field should parse as a word");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(name.val, PROVEN_LIT("boiler")), "the name should round-trip");

    proven_result_f64_t temp = proven_scan_f64(&sc);
    EXAMPLE_REQUIRE(proven_is_ok(temp.err), "the third field should parse as a float");

    /* 근사하게가 아니라 정확히 같다. 스캐너는 올바르게 반올림하므로 글자에 가장 가까운
     * double 을 돌려주고 - 형식화기가 내놓은 글자(소수 여섯 자리)는 이 값을 모호함 없이
     * 지목한다. 비트 하나까지 우리가 시작한 그 double 이다. 소수 여섯 자리보다 더 필요한
     * 값이라면 가장 짧은 정책(proven_float_format_options_shortest)으로 형식화하면 같은
     * 왕복이 성립한다. */
    EXAMPLE_REQUIRE(temp.val == reading, "the float must round-trip exactly, not approximately");

    /* 입력을 남김없이 삼켰다. 조용히 남겨 둔 것이 없다. */
    proven_result_i64_t extra = proven_scan_i64(&sc);
    EXAMPLE_REQUIRE(!proven_is_ok(extra.err), "there should be nothing left to scan");

    proven_u8str_destroy(alloc, &out);
    return EXAMPLE_OK();
}
