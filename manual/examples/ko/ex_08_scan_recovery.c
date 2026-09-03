#include "example.h"

/*
 * 스캐너의 오류 코드들, 그리고 거기서 되살아나는 법.
 *
 * 이 스캐너는 scanf 가 아니다. 받지 않은 포인터를 통해 쓰는 일이 없고, 폭을 짐작하지
 * 않으며, 여러 가지 중 무엇이 잘못됐는지를 말해 준다. 마지막 것은 그 코드들이 무슨
 * 뜻인지 알아야 도움이 되므로, 이 프로그램은 하나하나를 일부러 일으켜 본다.
 */

static proven_u8str_view_t v(const char *s) {
    return proven_u8str_view_from_cstr(s);
}

int main(void) {
    /* --- 기본 호출들은 실패하면 커서를 되돌린다 ---------------------------- */
    /* 실패한 파싱은 없던 일이다. 커서는 있던 자리에 있으므로, 같은 자리를 다른 것으로
     * 읽어 볼 수 있다. */
    {
        proven_scan_t sc = proven_scan_init(v("abc"));
        proven_result_i64_t n = proven_scan_i64(&sc);
        EXAMPLE_REQUIRE(n.err == PROVEN_ERR_INVALID_ARG, "'abc' is not an integer");
        EXAMPLE_REQUIRE(sc.cursor == 0, "a failed integer scan leaves the cursor alone");

        /* 그래서 같은 자리를 낱말로 읽을 수 있다. */
        proven_result_u8str_view_t w = proven_scan_str(&sc);
        EXAMPLE_REQUIRE(proven_is_ok(w.err) && proven_u8str_view_eq(w.val, PROVEN_LIT("abc")),
                        "the same bytes parse fine as a word");
    }

    /* --- 들어가지 않는 수는 감긴 값이 아니라 OVERFLOW 다 ------------------ */
    {
        proven_scan_t sc = proven_scan_init(v("9223372036854775808"));   /* INT64_MAX + 1 */
        proven_result_i64_t n = proven_scan_i64(&sc);
        EXAMPLE_REQUIRE(n.err == PROVEN_ERR_OVERFLOW, "one past INT64_MAX must not wrap");
        EXAMPLE_REQUIRE(sc.cursor == 0, "the cursor is restored on overflow too");
    }

    /* --- 그러나 아래로 넘치는 실수는 오류가 *아니다* ---------------------- */
    /* 너무 크면 OVERFLOW 이고, 너무 작으면 부호를 지킨 0 이다. 그 비대칭은 일부러다.
     * 0 으로 내려앉는 것은 올바르게 반올림한 답이지만, 위로 넘치는 데는 올바른 유한한
     * 답이 아예 없다. */
    {
        proven_scan_t big = proven_scan_init(v("1e309"));
        proven_result_f64_t b = proven_scan_f64(&big);
        EXAMPLE_REQUIRE(b.err == PROVEN_ERR_OVERFLOW, "1e309 does not fit a double");

        proven_scan_t tiny = proven_scan_init(v("-1e-400"));
        proven_result_f64_t t = proven_scan_f64(&tiny);
        EXAMPLE_REQUIRE(proven_is_ok(t.err), "1e-400 underflows, which is not an error");
        EXAMPLE_REQUIRE(t.val == 0.0, "it rounds to zero");
    }

    /* --- 정수 스캐너는 십진만 읽는다 -------------------------------------- */
    /* "0x10" 은 열여섯이 아니다. 0 하나이고, 그 뒤는 스캐너가 보라고 하지 않은 글이다.
     * 사람들이 놀라는 자리라 알아 둘 값어치가 있다. */
    {
        proven_scan_t sc = proven_scan_init(v("0x10"));
        proven_result_i64_t n = proven_scan_i64(&sc);
        EXAMPLE_REQUIRE(proven_is_ok(n.err) && n.val == 0, "0x10 scans as the integer 0");
        EXAMPLE_REQUIRE(sc.cursor == 1, "and the cursor stops before the 'x'");
    }

    /* --- 파싱은 그 값에 속할 수 없는 첫 바이트에서 멈춘다 ----------------- */
    {
        proven_scan_t sc = proven_scan_init(v("12abc"));
        proven_result_i64_t n = proven_scan_i64(&sc);
        EXAMPLE_REQUIRE(proven_is_ok(n.err) && n.val == 12, "12abc yields 12");
        EXAMPLE_REQUIRE(sc.cursor == 2, "and leaves 'abc' for whoever asks next");
    }

    /* --- 부호 없음은 부호 없음이라는 뜻이다 ------------------------------- */
    {
        proven_scan_t sc = proven_scan_init(v("-1"));
        proven_result_u64_t n = proven_scan_u64(&sc);
        EXAMPLE_REQUIRE(n.err == PROVEN_ERR_INVALID_ARG,
                        "-1 is rejected rather than wrapping to a huge unsigned value");
    }

    /* --- 값까지 찾아가기: skip_until -------------------------------------- */
    /* skip_until 은 커서를 목표 *위*에 두지, 지나쳐 두지 않는다. 그것을 얼마나 삼킬지는
     * 여러분이 정한다. */
    {
        proven_scan_t sc = proven_scan_init(v("port=8080"));
        proven_err_t err = proven_scan_skip_until(&sc, PROVEN_LIT("="));
        EXAMPLE_REQUIRE(proven_is_ok(err), "the '=' is there");
        EXAMPLE_REQUIRE(sc.cursor == 4, "the cursor sits on the '=' itself");

        ++sc.cursor;                                  /* 그것을 넘어선다 */
        proven_result_i64_t port = proven_scan_i64(&sc);
        EXAMPLE_REQUIRE(proven_is_ok(port.err) && port.val == 8080, "the port parses");

        /* 못 찾으면 NOT_FOUND 이고 커서는 움직이지 않는다 - 스캐너는 찾아가지 못한
         * 입력을 삼키지 않는다. */
        proven_scan_t sc2 = proven_scan_init(v("port=8080"));
        proven_err_t missing = proven_scan_skip_until(&sc2, PROVEN_LIT("#"));
        EXAMPLE_REQUIRE(missing == PROVEN_ERR_NOT_FOUND, "there is no '#'");
        EXAMPLE_REQUIRE(sc2.cursor == 0, "and the cursor stayed put");
    }

    /* --- 구조를 읽는 스캐너 ------------------------------------------------ */
    {
        int id = 0;
        double ratio = 0.0;
        proven_u8str_view_t name = {0};

        proven_err_t err = proven_scan_fmt(v("id=7 ratio=0.5 name=ada"),
                                           "id={} ratio={} name={}",
                                           PROVEN_SCAN_ARG(&id),
                                           PROVEN_SCAN_ARG(&ratio),
                                           PROVEN_SCAN_ARG(&name));
        EXAMPLE_REQUIRE(proven_is_ok(err), "the line matches the shape");
        EXAMPLE_REQUIRE(id == 7 && ratio == 0.5, "the values land in the right places");
        EXAMPLE_REQUIRE(proven_u8str_view_eq(name, PROVEN_LIT("ada")), "including the word");
    }

    /* --- 구조를 읽는 스캐너는 트랜잭션이 *아니다* ------------------------- */
    /*
     * 이것이 사람을 무는 자리다. 리터럴이 맞지 않으면 파싱은 오류를 돌려주는데 - 그
     * 어긋남 *앞*의 자리표들은 이미 목적지에 쓰여 버렸다. 호출이 실패했는데도 `id` 는
     * 7 이다.
     *
     * 그러니 실패했을 때는 모든 목적지가 더럽혀졌다고 여길 것. 전부 아니면 전무가
     * 필요하면 지역 변수로 파싱하고 호출이 성공한 뒤에만 공표할 것. 아래 코드가 하는
     * 일이 그것이다.
     */
    {
        int id = -1;
        double ratio = -1.0;
        proven_err_t err = proven_scan_fmt(v("id=7 ratio=0.5"),
                                           "id={} XXX={}",       /* 리터럴이 틀렸다 */
                                           PROVEN_SCAN_ARG(&id),
                                           PROVEN_SCAN_ARG(&ratio));
        EXAMPLE_REQUIRE(err == PROVEN_ERR_NOT_FOUND, "the literal 'XXX=' is not in the input");
        EXAMPLE_REQUIRE(id == 7, "and yet id was already written: the scan is not atomic");

        /* 안전한 모양: 지역 변수로 파싱하고 성공했을 때 공표한다. */
        int good_id = 0;
        double good_ratio = 0.0;
        int published_id = -1;
        proven_err_t ok = proven_scan_fmt(v("id=7 ratio=0.5"), "id={} ratio={}",
                                          PROVEN_SCAN_ARG(&good_id), PROVEN_SCAN_ARG(&good_ratio));
        if (proven_is_ok(ok)) published_id = good_id;
        EXAMPLE_REQUIRE(published_id == 7, "publish only what a successful scan produced");
    }

    /* --- 입력이 모자랄 때, 그리고 남을 때 ---------------------------------- */
    {
        int a = 0, b = 0;
        proven_err_t short_input = proven_scan_fmt(v("5"), "{} {}",
                                                   PROVEN_SCAN_ARG(&a), PROVEN_SCAN_ARG(&b));
        EXAMPLE_REQUIRE(!proven_is_ok(short_input), "two placeholders, one value: that fails");

        /* 뒤에 남은 입력은 오류가 *아니다*. 스캐너는 청한 것을 맞추고 멈췄다. 청하지
         * 않은 것까지 단속하지는 않는다. 줄 전체를 삼켜야 한다면 그것은 여러분이
         * 확인할 일이다. */
        int only = 0;
        proven_scan_t sc = proven_scan_init(v("7 8"));
        proven_err_t err = proven_scan_fmt_cursor(&sc, "{}", PROVEN_SCAN_ARG(&only));
        EXAMPLE_REQUIRE(proven_is_ok(err) && only == 7, "the first value scans");
        EXAMPLE_REQUIRE(sc.cursor < sc.view.size, "and '8' is still sitting there, unconsumed");
    }

    /* --- 좁은 목적지는 범위가 검사된다 ------------------------------------ */
    {
        short small = 0;
        proven_err_t err = proven_scan_fmt(v("70000"), "{}", PROVEN_SCAN_ARG(&small));
        EXAMPLE_REQUIRE(err == PROVEN_ERR_OVERFLOW,
                        "70000 does not fit a short, and the scanner says so rather than truncating");
    }

    return EXAMPLE_OK();
}
