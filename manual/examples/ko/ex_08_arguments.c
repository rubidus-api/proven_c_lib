#include "example.h"

/*
 * 형식화기에 건네는 모든 값은 proven_arg_t 로 도착하고, 스캐너가 써 넣는 모든 목적지는
 * proven_scan_arg_t 로 도착한다. 대개는 둘 다 볼 일이 없다. PROVEN_ARG(x) 와
 * PROVEN_SCAN_ARG(&x) 가 건넨 것의 타입에서 알맞은 생성자를 골라 주고, 그것이 그
 * 매크로들의 존재 이유다.
 *
 * 두 상황에서는 그 선택을 매크로에게서 도로 가져오게 되는데, 둘 다 평범한 일이다.
 *
 *   1. 매크로가 알아볼 타입이 없을 때. `proven_u8str_view_t`, 쪼개 놓은 날짜, 진단용으로
 *      찍는 날주소 - 이런 것들은 생성자를 이름으로 불러야 한다. 여러분이 뜻하는 바를
 *      뜻하는, 매크로가 갈래를 태울 수 있는 평범한 C 타입이 없기 때문이다.
 *
 *   2. 인자 목록을 *실행 중에* 짓고 있을 때. "부르는 쪽이 이미 모아 둔 무엇이든" 을 받는
 *      로그 도우미는 int 나 문자열이 아니라 proven_arg_t 값을 받는다 - 그리고 값을
 *      인자로 바꾸는 매크로에 이미 인자인 것을 건넬 수는 없다. 항등 생성자가 그것을 위한
 *      것이다. 매크로로 굴러가는 같은 코드 길이, 앞서 만들어진 인자도 받아들이게 해 준다.
 *
 * 그래서 이 예제는 양쪽을 다 이름으로 적는다. 목적지의 폭과 부호 유무는 장식이 아니다 -
 * 70000 이 70000 으로 도착할지 4464 로 도착할지를 정하는 것이 그것이다.
 */

/* 부르는 쪽이 이미 만들어 둔 인자를 받는 로그 도우미. 매개변수가 proven_arg_t 이므로 그
 * 안에서 PROVEN_ARG 는 틀린 도구다 - 그 값은 이미 인자이고, 그렇다고 말해 주는 것이
 * proven_arg_identity 다. */
static proven_fmt_result_t log_pair(proven_u8str_t *out, const char *fmt,
                                    proven_arg_t a, proven_arg_t b) {
    return proven_u8str_append_fmt(out, fmt, proven_arg_identity(a), proven_arg_identity(b));
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    proven_result_u8str_t line = proven_u8str_create(alloc, 256);
    EXAMPLE_REQUIRE(proven_is_ok(line.err), "creating the output string must succeed");
    if (!proven_is_ok(line.err)) {
        return 1;
    }
    proven_u8str_t out = line.value;

    /* --- 형식화 인자를 직접 이름으로 부르기 ------------------------------- */

    /* 문자 하나와 플래그 하나. bool 이 1/0 이 아니라 true/false 로 찍힌다는 것이 눈에
     * 보이도록 여기서는 이름으로 적었다. */
    proven_fmt_result_t r = proven_u8str_append_fmt(&out, "flag={} mark={}",
                                                    proven_arg_bool(true),
                                                    proven_arg_char('!'));
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "formatting a bool and a char must succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&out), PROVEN_LIT("flag=true mark=!")),
                    "a bool renders as a word, not as a digit");

    EXAMPLE_REQUIRE(proven_is_ok(proven_u8str_reset(&out)), "clearing the line must succeed");

    /* 부호 없는 폭들. 생성자가 그 값이 무엇인지에 대한 선언이다. 부호 없는 32비트 개수와
     * 부호 없는 64비트 바이트 합계는 서로 다른 사실이고, 그것을 적어 두면 자기가 가진
     * 것이 어느 쪽인지 말하게 된다. */
    r = proven_u8str_append_fmt(&out, "files={} bytes={}",
                                proven_arg_u32(1200u),
                                proven_arg_u64(5368709120ULL));
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "formatting unsigned values must succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&out), PROVEN_LIT("files=1200 bytes=5368709120")),
                    "a 64-bit total is printed in full, not truncated to 32 bits");

    EXAMPLE_REQUIRE(proven_is_ok(proven_u8str_reset(&out)), "clearing the line must succeed");

    /* 형식화기에 글을 주는 세 가지 방법. 부르는 쪽을 얼마나 믿는지 순서대로.
     *
     *   proven_arg_str_view  - 포인터 *와* 길이. 종결자를 찾아 훑는 일이 없으므로,
     *                          없어서 문제가 될 종결자도 없다. 이것을 고를 것.
     *   proven_arg_cstr      - NUL 로 끝나는 C 문자열. 형식화기가 종결자를 찾아 걸으므로
     *                          그것이 반드시 있어야 하고, 그 기억도 살아 있어야 한다.
     *   proven_arg_ucstr     - 같은 것인데 `unsigned char *` 용이다. 바이트 버퍼는 보통
     *                          그 타입으로 적힌다. 부르는 쪽이 진짜 경고를 입막음하는
     *                          형변환을 쓰지 않아도 되도록 있는 것이다.
     */
    const char *name = "report.txt";
    const unsigned char *tag = (const unsigned char *)"draft";
    proven_u8str_view_t note = PROVEN_LIT("first pass");

    r = proven_u8str_append_fmt(&out, "{} [{}] {}",
                                proven_arg_cstr(name),
                                proven_arg_ucstr(tag),
                                proven_arg_str_view(note));
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "formatting the three text forms must succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&out),
                                         PROVEN_LIT("report.txt [draft] first pass")),
                    "all three produce the same kind of output from different kinds of pointer");

    EXAMPLE_REQUIRE(proven_is_ok(proven_u8str_reset(&out)), "clearing the line must succeed");

    /* 날짜 하나와 주소 하나. 둘 다 자기 뜻을 뜻하는 평범한 C 타입이 없다. 날짜는
     * 구조체이고, 진단용으로 찍는 주소는 우연히 빠지는 것이 아니라 일부러 하는 일이다. */
    proven_datetime_t when = proven_time_breakdown(0);   /* 기점: 고정된, 확인할 수 있는 값 */
    int local = 0;

    r = proven_u8str_append_fmt(&out, "at {} object {}",
                                proven_arg_datetime(when),
                                proven_arg_ptr(&local));
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "formatting a date and a pointer must succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_starts_with(proven_u8str_as_view(&out), PROVEN_LIT("at 1970-01-01")),
                    "the epoch breaks down to the first of January 1970");
    EXAMPLE_REQUIRE(proven_u8str_view_find(proven_u8str_as_view(&out), 0, PROVEN_LIT("0x")) != PROVEN_SIZE_MAX,
                    "and an address is rendered in hexadecimal");

    EXAMPLE_REQUIRE(proven_is_ok(proven_u8str_reset(&out)), "clearing the line must succeed");

    /* 여기서 지은 인자를, 넘겨, 저기서 형식화한다. */
    r = log_pair(&out, "status={} retries={}", proven_arg_cstr("ok"), proven_arg_u32(3u));
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "formatting pre-built arguments must succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&out), PROVEN_LIT("status=ok retries=3")),
                    "an argument built by the caller formats exactly as one built in place");

    /* --- 파싱 목적지를 직접 이름으로 부르기 ------------------------------- */

    /* 들어오는 쪽에서는 생성자가 목적지를 이름으로 밝히고, 목적지가 "너무 크다" 의 뜻을
     * 정한다. 이것들은 평범한 C 타입들 - short, int, long, long long 과 그 부호 없는 꼴 -
     * 이고, 이미 가진 변수가 고정 폭 타입이 아니라 그중 하나인 아주 흔한 경우를 위한
     * 것이다. */
    proven_scan_t scan = proven_scan_init(
        PROVEN_LIT("h=-32000 uh=65000 i=-2000000 ui=4000000000 l=-9000000 ul=9000000 "
                   "ll=-9007199254740993 ull=18446744073709551615 f=2.5 word=alpha"));

    short h = 0;
    unsigned short uh = 0;
    int i = 0;
    unsigned int ui = 0;
    long l = 0;
    unsigned long ul = 0;
    long long ll = 0;
    unsigned long long ull = 0;
    double f = 0.0;
    proven_u8str_view_t word = {0};

    proven_err_t err = proven_scan_fmt_cursor(
        &scan, "h={} uh={} i={} ui={} l={} ul={} ll={} ull={} f={} word={}",
        proven_scan_arg_short(&h),
        proven_scan_arg_ushort(&uh),
        proven_scan_arg_int(&i),
        proven_scan_arg_uint(&ui),
        proven_scan_arg_long(&l),
        proven_scan_arg_ulong(&ul),
        proven_scan_arg_llong(&ll),
        proven_scan_arg_ullong(&ull),
        proven_scan_arg_f64(&f),
        proven_scan_arg_str_view(&word));
    EXAMPLE_REQUIRE(proven_is_ok(err), "scanning every plain integer width must succeed");
    EXAMPLE_REQUIRE(h == -32000 && uh == 65000u, "the short forms hold their values");
    EXAMPLE_REQUIRE(i == -2000000 && ui == 4000000000u, "and so do the int forms");
    EXAMPLE_REQUIRE(l == -9000000L && ul == 9000000UL, "and the long forms");
    EXAMPLE_REQUIRE(ll == -9007199254740993LL, "a value needing 64 bits arrives whole");
    EXAMPLE_REQUIRE(ull == 18446744073709551615ULL, "including the largest unsigned 64-bit value");
    EXAMPLE_REQUIRE(f > 2.4999 && f < 2.5001, "the floating-point destination reads a decimal");

    /* 파싱된 문자열 뷰는 파싱 중인 글 *안*을 가리킨다. 복사한 것도 할당한 것도 없으므로,
     * 그 글이 사는 동안 정확히 그만큼만 쓸 수 있다 - 파싱보다 오래 살아야 한다면
     * proven_u8str_t 로 복사할 것. */
    EXAMPLE_REQUIRE(proven_u8str_view_eq(word, PROVEN_LIT("alpha")), "the word is captured as a view");

    /* 목적지의 폭은 스캐너가 지키는 약속이다. 70000 은 short 에 들어가지 않으므로 4464 로
     * 감기는 대신 거부된다 - 형변환이었다면 아무도 다시 보지 않을 파일 안에서 조용히
     * 그렇게 만들어 냈을 값이다. */
    proven_scan_t narrow = proven_scan_init(PROVEN_LIT("70000"));
    short too_small = 7;
    err = proven_scan_fmt_cursor(&narrow, "{}", proven_scan_arg_short(&too_small));
    EXAMPLE_REQUIRE(err != PROVEN_OK, "a value that does not fit the destination is refused");
    EXAMPLE_REQUIRE(too_small == 7, "and the destination keeps the value it had");

    /* 파싱 쪽 항등 생성자. 이유는 형식화 쪽과 같다. 이미 만들어진 파싱 인자를 받는
     * 도우미는 그것을 다시 감쌀 수 없다. */
    proven_scan_t again = proven_scan_init(PROVEN_LIT("41"));
    proven_i32 answer = 0;
    proven_scan_arg_t prebuilt = proven_scan_arg_i32(&answer);
    err = proven_scan_fmt_cursor(&again, "{}", proven_scan_arg_identity(prebuilt));
    EXAMPLE_REQUIRE(proven_is_ok(err) && answer == 41, "a pre-built scan argument works unchanged");

    printf("arguments: %s\n", proven_u8str_as_cstr(&out));

    proven_u8str_destroy(alloc, &out);
    return EXAMPLE_OK();
}
