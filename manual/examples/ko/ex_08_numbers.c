#include "example.h"
#include <string.h>

/*
 * 글에서 수를 읽어 내고, 다시 써 넣기.
 *
 * 여기 쓰인 글은 프로그램이 실제로 만나는 종류다. 폭과 부호가 뒤섞인 로그 줄, 그리고
 * 관심 있는 수가 산문 속에 묻혀 있는 측정값 파일. 그래서 앞의 8장 예제들이 다루지 않는
 * 세 가지 일이 나온다.
 *
 *   - int 보다 *넓은* 타입, 그리고 부호 없는 타입으로 파싱하기. 여기서는 목적지 타입이
 *     물음의 전부다 - 32비트에 들어가지 않는 바이트 수야말로 파일이 5 GB 가 될 때까지
 *     아무도 알아채지 못하는 고전적인 넘침이다.
 *   - 입력이 빡빡한 형식이 아닐 때 커서를 손으로 옮기기 - 공백을 건너뛰거나, 다음 수가
 *     시작하는 자리까지 건너뛰기.
 *   - 십진 문자열을 double 로, 그리고 다시 글로, C 라이브러리의 로케일에 매인 변환을
 *     거치지 않고 정확하게 옮기기.
 *
 * 부동소수점 쪽은 들리는 것보다 중요하다. strtod 는 어느 로케일에서는 "3,5" 를 3.5 로,
 * 다른 로케일에서는 3 으로 읽고, 그러면 같은 프로그램이 기계마다 서로 다른 소리를 한다.
 * 여기 쓰인 파서는 만들어질 때부터 로케일이 없다. 환경이 무어라 하든 쉼표가 소수점이
 * 되는 일은 없다.
 */

int main(void) {
    /* --- 1. 값이 필요로 하는 타입으로 파싱하기 ---------------------------- */

    /* 로그 줄 하나: 64비트가 필요한 요청 번호, 결코 음수가 아니고 4 GB 를 넘을 수 있는
     * 바이트 수, 작은 상태 코드, 그리고 음수 오프셋. */
    proven_scan_t scan = proven_scan_init(
        PROVEN_LIT("id=9007199254740993 bytes=5368709120 status=404 delta=-17"));

    proven_i64 id = 0;
    proven_u64 bytes = 0;
    proven_u32 status = 0;
    proven_i32 delta = 0;

    /* 인자 생성자마다 목적지 타입을 이름으로 밝히므로, 스캐너는 옳은 폭으로 쓰고 감기는
     * 대신 넘침을 알린다. 일반 매크로 PROVEN_SCAN_ARG() 는 포인터의 타입에서 이것들을
     * 대신 골라 준다. 이름 붙은 꼴은 그것이 고르는 것 자체이고, 그 선택을 눈에 보이게
     * 하고 싶을 때 여러분이 적는 것이다. */
    proven_err_t err = proven_scan_fmt_cursor(&scan, "id={} bytes={} status={} delta={}",
                                       proven_scan_arg_i64(&id),
                                       proven_scan_arg_u64(&bytes),
                                       proven_scan_arg_u32(&status),
                                       proven_scan_arg_i32(&delta));
    EXAMPLE_REQUIRE(proven_is_ok(err), "scanning the log line must succeed");
    EXAMPLE_REQUIRE(id == 9007199254740993LL, "a 64-bit id survives, which a 32-bit destination could not");
    EXAMPLE_REQUIRE(bytes == 5368709120ULL, "and so does a byte count larger than 4 GiB");
    EXAMPLE_REQUIRE(status == 404u, "the small unsigned value reads normally");
    EXAMPLE_REQUIRE(delta == -17, "and a signed destination accepts the minus sign");

    /* 목적지 타입은 힌트가 아니라 계약이다. 음수에는 부호 없는 표현이 없고, 스캐너는
     * 그것을 어마어마한 값으로 감아 버리는 대신 거부한다. */
    proven_scan_t neg = proven_scan_init(PROVEN_LIT("-17"));
    proven_u32 nowhere = 12345;
    err = proven_scan_fmt_cursor(&neg, "{}", proven_scan_arg_u32(&nowhere));
    EXAMPLE_REQUIRE(err != PROVEN_OK, "a negative value cannot be scanned into an unsigned destination");
    EXAMPLE_REQUIRE(nowhere == 12345, "and the destination is left as it was");

    /* --- 2. 입력이 빡빡하지 않을 때 커서 옮기기 --------------------------- */

    /* 자유로운 글: 수가 중요하고 그 사이의 낱말은 아니다. */
    proven_scan_t notes = proven_scan_init(
        PROVEN_LIT("   sample A measured 42 units; sample B measured -8 units"));

    /* skip_whitespace 는 공백과 탭과 줄바꿈을 지나 나아간다. 손으로 몰고 가는 형식의 필드
     * 사이에서 부르는 것이고, 실패하는 일이 없다. "공백이 없었다" 는 오류가 아니기
     * 때문이다. */
    proven_scan_skip_whitespace(&notes);
    EXAMPLE_REQUIRE(notes.cursor == 3, "the three leading spaces are consumed");

    /* skip_until_number 는 커서를 첫 숫자, 또는 곧바로 숫자가 따라오는 부호까지 몰고
     * 간다. "이 줄에서 수를 찾아라" 를 손으로 쓴 반복문에서 한 문장으로 바꿔 주는
     * 호출이다. */
    proven_scan_skip_until_number(&notes);
    proven_i32 first = 0;
    err = proven_scan_fmt_cursor(&notes, "{}", proven_scan_arg_i32(&first));
    EXAMPLE_REQUIRE(proven_is_ok(err) && first == 42, "the first number in the line is 42");

    proven_scan_skip_until_number(&notes);
    proven_i32 second = 0;
    err = proven_scan_fmt_cursor(&notes, "{}", proven_scan_arg_i32(&second));
    EXAMPLE_REQUIRE(proven_is_ok(err) && second == -8,
                    "and the next one keeps its sign, because the sign is part of the number");

    /* 끝에 이르면 더 찾을 것이 없고, 커서는 달려 나가는 대신 멈춘다. 파싱이 망가진 것이
     * 아니라 끝난 것이다. */
    proven_scan_skip_until_number(&notes);
    EXAMPLE_REQUIRE(notes.cursor == notes.view.size, "with no number left, the cursor lands at the end");

    /* --- 3. 십진 글을 double 로, 로케일 없이 ----------------------------- */

    proven_parse_double_result_t d = proven_parse_double_ascii(PROVEN_LIT("3.14159 rest"));
    EXAMPLE_REQUIRE(proven_is_ok(d.err), "parsing a decimal number must succeed");
    EXAMPLE_REQUIRE(d.val > 3.14158 && d.val < 3.14160, "and produce the value the digits spell");

    /* consumed 는 수가 어디서 끝났는지 알려 주므로 부르는 쪽이 거기서 이어 갈 수 있다 -
     * 목록을 토막으로 복사하지 않고 파싱하는 방법이 그것이다. */
    EXAMPLE_REQUIRE(d.consumed == 7, "consumed reports exactly the bytes the number used");

    /* 여기서 쉼표는 소수점이 아니고 앞으로도 아니다. 프로그램이 어느 로케일에서 돌든
     * 마찬가지다. 수는 쉼표에서 끝난다. */
    proven_parse_double_result_t comma = proven_parse_double_ascii(PROVEN_LIT("3,5"));
    EXAMPLE_REQUIRE(proven_is_ok(comma.err) && comma.consumed == 1 && comma.val == 3.0,
                    "a comma ends the number: the parser is locale-free by construction");

    /* proven_parse_f64_ascii 는 예전 이름을 단 같은 함수다. 지금 이름이 생기기 전에 쓰인
     * 코드를 위해 남겨 두었다. 새 코드는 proven_parse_double_ascii 를 쓸 것. 옛 호출
     * 자리도 그대로 읽히도록 둘 다 여기 있다. */
    proven_parse_f64_result_t same = proven_parse_f64_ascii(PROVEN_LIT("3.14159 rest"));
    EXAMPLE_REQUIRE(same.val == d.val && same.consumed == d.consumed,
                    "the compatibility name is the same parser");

    /* 아예 수가 아닌 글은 조용한 0 이 아니라 오류다 - atof() 였다면 진짜 "0" 과 구별할
     * 방법 없이 그것을 돌려주었을 것이다. */
    proven_parse_double_result_t junk = proven_parse_double_ascii(PROVEN_LIT("not a number"));
    EXAMPLE_REQUIRE(!proven_is_ok(junk.err), "unparsable text is reported, not turned into 0");
    EXAMPLE_REQUIRE(junk.consumed == 0, "and nothing was consumed");

    /* --- 4. float 를 다시 글로 ------------------------------------------- */

    /* proven_arg_f64 가 부동소수점 값이 형식화기로 들어가는 길이다. float 도 double 도
     * 그것을 지나므로, 프로그램이 찍는 자릿수가 그 값이 어쩌다 담겨 있던 변수의 폭에
     * 좌우되지 않는다. */
    proven_allocator_t alloc = proven_heap_allocator();
    proven_result_u8str_t line = proven_u8str_create(alloc, 64);
    EXAMPLE_REQUIRE(proven_is_ok(line.err), "creating the output string must succeed");

    proven_fmt_result_t out = proven_u8str_append_fmt(&line.value, "measured {} units",
                                                      proven_arg_f64(d.val));
    EXAMPLE_REQUIRE(proven_is_ok(out.err), "formatting the parsed value must succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_starts_with(proven_u8str_as_view(&line.value),
                                                  PROVEN_LIT("measured 3.14159")),
                    "and spell the number it was given");

    /* 정책을 주는 꼴이 명시적인 쪽이다. 기본값이 아니라 특정한 표기가 필요할 때 쓴다.
     * SHORTEST 는 다시 읽었을 때 정확히 이 값이 되는 가장 적은 자릿수를 청한다 -
     * 직렬화기가 원하는 성질이 그것이다. 필요 없는 수까지 열일곱 자리로 찍지 않고도
     * 왕복을 정확하게 만들어 주기 때문이다. */
    char shortest[64];
    proven_size_t wrote = 0;
    float measured = 0.1f;
    proven_err_t ferr = proven_float_format_f32_policy(shortest, sizeof shortest, measured,
                                                       PROVEN_FLOAT_FORMAT_POLICY_RYU,
                                                       proven_float_format_options_shortest(),
                                                       &wrote);
    EXAMPLE_REQUIRE(proven_is_ok(ferr), "formatting a float in shortest mode must succeed");
    EXAMPLE_REQUIRE(wrote > 0 && shortest[wrote] == '\0', "the result is written and terminated");
    EXAMPLE_REQUIRE(strcmp(shortest, "0.1") == 0,
                    "0.1f prints as 0.1: the shortest text that reads back as the same float");

    /* 그리고 왕복한다. 그 글은 자기가 나온 값으로 되읽힌다. 가장 짧은 방식이 존재하는
     * 이유가 그 보장이다. */
    proven_parse_double_result_t roundtrip = proven_parse_double_ascii(
        (proven_u8str_view_t){ .ptr = (const proven_byte_t *)shortest, .size = wrote });
    EXAMPLE_REQUIRE(proven_is_ok(roundtrip.err), "the shortest form parses back");
    EXAMPLE_REQUIRE((float)roundtrip.val == measured, "as exactly the float it was printed from");

    printf("scanned id=%lld bytes=%llu; formatted %s and %s\n",
           (long long)id, (unsigned long long)bytes, proven_u8str_as_cstr(&line.value), shortest);

    proven_u8str_destroy(alloc, &line.value);
    return EXAMPLE_OK();
}
