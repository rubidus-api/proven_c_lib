#include "example.h"

/*
 * 라이브러리가 들어 본 적 없는 타입을 형식화하기.
 *
 * `PROVEN_ARG` 는 `_Generic` 위에 서 있고, 그것은 미리 알려 준 타입에만 갈래를 태울 수
 * 있다 - 그리고 여러분의 타입을 알려 줄 방법이 없다. 그래서 `PROVEN_ARG_OF` 가 생기기
 * 전에는 `rect_t` 를 찍을 방법이 아예 없었다. 임시 문자열에 미리 형식화해서 그것을
 * 건네거나(값마다 할당 한 번과 복사 한 번, 그것도 할당하면 안 되는 그 로그 경로에서),
 * 필드를 하나씩 찍고 열 맞추기는 포기하거나 둘 중 하나였다.
 *
 * 렌더러는 버퍼가 아니라 *그릇*을 받는다. 그것이 이 방식을 조립 가능하게 만든다. 렌더러는
 * 형식화기를 다시 부를 수 있고, 그 출력은 그저 어딘가로 가는 바이트다. 그리고 형식화기가
 * 내보내기 전에 렌더러의 출력을 재기 때문에 - 세는 그릇에 대고 한 번 돌려 본다 - 폭과
 * 채움과 정렬이 사용자 타입에서도 int 에서와 똑같이 먹는다.
 */

typedef struct { int w, h; } rect_t;

static proven_err_t render_rect(proven_fmt_sink_t out, const void *obj) {
    const rect_t *r = (const rect_t *)obj;

    /* 조립: 형식화기를 스택 버퍼로. 할당자는 어디에도 없다. */
    proven_byte_t tmp[64];
    proven_u8str_t s = proven_u8str_borrow(tmp, sizeof tmp);
    proven_fmt_result_t f = proven_u8str_append_fmt(&s, "{}x{}",
                                                    PROVEN_ARG(r->w), PROVEN_ARG(r->h));
    if (!PROVEN_FMT_IS_OK(f)) return f.err;

    return proven_fmt_put(out, proven_u8str_as_view(&s));
}

int main(void) {
    rect_t a = { .w = 1920, .h = 1080 };
    rect_t b = { .w = 640,  .h = 480  };

    proven_byte_t buf[128];
    proven_u8str_t line = proven_u8str_borrow(buf, sizeof buf);

    /* 다른 인자와 똑같다. */
    proven_fmt_result_t r = proven_u8str_append_fmt(&line, "mode={}", PROVEN_ARG_OF(&a, render_rect));
    EXAMPLE_REQUIRE(PROVEN_FMT_IS_OK(r), "a user type should format");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&line), PROVEN_LIT("mode=1920x1080")),
                    "the renderer's bytes should be what came out");

    /* 그리고 정렬된다. 형식화기가 먼저 재는 이유가 바로 이것이다 - 사용자 정의 값의 열이
     * 다른 무엇의 열과 똑같이 줄을 맞춘다. */
    (void)proven_u8str_reset(&line);
    r = proven_u8str_append_fmt(&line, "[{:>10}]\n[{:>10}]",
                                PROVEN_ARG_OF(&a, render_rect),
                                PROVEN_ARG_OF(&b, render_rect));
    EXAMPLE_REQUIRE(PROVEN_FMT_IS_OK(r), "two user types should format");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&line),
                                         PROVEN_LIT("[ 1920x1080]\n[   640x480]")),
                    "both rows should be right-aligned to the same width");

    /* 여러분의 타입에 대해 라이브러리가 해석할 수 없는 지정자는 추측하지 않고 거부된다.
     * 사각형에 `{:x}` 는 뜻이 없고, 성공했다고 알리면서 그럴듯한 무언가로 답하는 것이
     * 형식화기가 여러분에게 거짓말을 시작하는 방식이다. */
    (void)proven_u8str_reset(&line);
    r = proven_u8str_append_fmt(&line, "{:x}", PROVEN_ARG_OF(&a, render_rect));
    EXAMPLE_REQUIRE(r.err == PROVEN_ERR_INVALID_FORMAT,
                    "a type letter on a user type should be an error");

    return EXAMPLE_OK();
}
