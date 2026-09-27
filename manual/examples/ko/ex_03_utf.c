#include "example.h"
#include <string.h>

/*
 * 프로그램은 UTF-8 로 생각하고, 윈도의 "와이드" 호출은 UTF-16 을 원한다. utf.h 가
 * 양방향의 건널목이고, 엄격하다 - 올바른 UTF-8 이나 UTF-16 이 아닌 텍스트는 고쳐지지
 * 않고 거부된다. 잘못된 바이트를 슬쩍 물음표로 바꾸는 변환기는, 막 열려던 파일 이름을
 * 바꿔 버린 것이다.
 *
 * 이 라이브러리 어디서나처럼 세 가지 모양이 있다: 재고 나서 내 버퍼로 변환(전부 아니면
 * 전무), 소유 문자열을 늘리며 덧붙이기(전부 아니면 전무), 조각으로 도착하는 텍스트를
 * 위한 부분 변환.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* "보고서.txt" - 프로그램의 나머지가 들고 있는 모양 그대로의 파일 이름. */
    proven_u8str_view_t name = PROVEN_LIT("\xEB\xB3\xB4\xEA\xB3\xA0\xEC\x84\x9C.txt");

    /* --- 재고 나서, 내 버퍼로 변환 ------------------------------------------ */

    /* 먼저 묻는다: 답은 코드 단위이고, 검증도 겸한다. 한글 아홉 바이트는 여기서
     * 코드 단위 아홉 개가 아니라 세 개다. */
    proven_result_size_t need = proven_utf8_to_utf16_size(name);
    EXAMPLE_REQUIRE(proven_is_ok(need.err) && need.value == 7, "3 syllables + \".txt\" are 7 code units");

    /* 와이드 API 가 기대하는 NUL 자리로 하나 더; 변환기는 NUL 을 쓰지 않는다. */
    proven_u16 wide[16];
    proven_size_t units = 0;
    proven_err_t err = proven_utf8_to_utf16(name, wide, 15, &units);
    EXAMPLE_REQUIRE(proven_is_ok(err) && units == 7, "the name converts whole");
    wide[units] = 0;
    EXAMPLE_REQUIRE(wide[0] == 0xBCF4, "the first code unit is the syllable, not its first byte");
    /* 윈도라면: CreateFileW((LPCWSTR)wide, ...). 어디서나 돌도록 여기서는 부르지 않는다. */

    /* 그리고 되돌리기: 와이드 API 가 돌려준 이름을, 나머지 모두를 위해 UTF-8 로. */
    proven_result_size_t back = proven_utf16_to_utf8_size(wide, units);
    EXAMPLE_REQUIRE(proven_is_ok(back.err) && back.value == name.size, "the reverse size is the original byte count");
    proven_byte_t bytes[32];
    proven_size_t nbytes = 0;
    err = proven_utf16_to_utf8(wide, units, bytes, sizeof bytes, &nbytes);
    EXAMPLE_REQUIRE(proven_is_ok(err) && nbytes == name.size && memcmp(bytes, name.ptr, nbytes) == 0,
                    "the round trip gives the same bytes");

    /* --- 엄격: 잘못된 텍스트는 거부되고, 아무것도 쓰이지 않는다 -------- */

    /* 잘렸거나 잘못 해독된 이름에 있을 법한, 홀로 남은 연속 바이트. */
    proven_u8str_view_t broken = PROVEN_LIT("bad\x80.txt");
    wide[0] = 0x1234;
    err = proven_utf8_to_utf16(broken, wide, 15, &units);
    EXAMPLE_REQUIRE(err == PROVEN_ERR_INVALID_ENCODING, "a stray continuation byte is refused");
    EXAMPLE_REQUIRE(units == 0 && wide[0] == 0x1234, "and the output is untouched");

    /* --- 소유 문자열 늘리기 ------------------------------------------------ */

    proven_result_u16str_t r = proven_u16str_create(alloc, 4);
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "creating the wide path must succeed");
    proven_u16str_t path = r.value;
    err = proven_utf8_append_to_u16str(alloc, &path, PROVEN_LIT("C:\\reports\\"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "the directory appends");
    err = proven_utf8_append_to_u16str(alloc, &path, name);
    EXAMPLE_REQUIRE(proven_is_ok(err) && proven_u16str_len(&path) == 11 + 7, "and the name, growing the string");
    EXAMPLE_REQUIRE(proven_u16str_as_ptr(&path)[18] == 0, "still NUL-terminated for the system call");

    /* 실패한 덧붙이기는 아무것도 바꾸지 않는다: 반쯤 변환된 꼬리가 없다. */
    err = proven_utf8_append_to_u16str(alloc, &path, broken);
    EXAMPLE_REQUIRE(err == PROVEN_ERR_INVALID_ENCODING && proven_u16str_len(&path) == 18, "a refused append leaves the path as it was");

    proven_result_u8str_t r8 = proven_u8str_create(alloc, 8);
    EXAMPLE_REQUIRE(proven_is_ok(r8.err), "creating the UTF-8 copy must succeed");
    proven_u8str_t copy = r8.value;
    err = proven_utf16_append_to_u8str(alloc, &copy, proven_u16str_as_ptr(&path), proven_u16str_len(&path));
    EXAMPLE_REQUIRE(proven_is_ok(err) && copy.internal.len == 11 + name.size, "the whole path comes back as UTF-8");

    /* --- 부분 변환: 조각으로 도착하는 텍스트 ------------------------------- */

    /* 읽기 스트림이 4 바이트씩 넘겨 주고, "고" 는 첫 바이트 뒤에서 잘린다. 부분 변환은
     * 온전한 것만 변환하고 NEED_MORE 로 멈추며, 얼마나 썼는지 알려 준다. 호출자는
     * 나머지를 간직했다가 다음 조각을 붙인다. */
    proven_u16 out[8];
    proven_utf_step_t st = proven_utf8_to_utf16_partial((proven_u8str_view_t){ name.ptr, 4 }, out, 8);
    EXAMPLE_REQUIRE(st.err == PROVEN_ERR_NEED_MORE && st.consumed == 3 && st.written == 1,
                    "one whole syllable, and the start of the next kept back");
    st = proven_utf8_to_utf16_partial((proven_u8str_view_t){ name.ptr + st.consumed, 6 }, out + 1, 7);
    EXAMPLE_REQUIRE(proven_is_ok(st.err) && st.written == 2 && out[1] == 0xACE0, "with the next piece it completes");

    /* 반대 방향에서도 같은 구분이다: 끝의 상위 서로게이트는, 나머지 반쪽이 다음
     * 조각에 있는 쌍의 반쪽일 수 있다. */
    const proven_u16 emoji_half[] = { 'o', 'k', 0xD83D };
    proven_byte_t u8out[16];
    st = proven_utf16_to_utf8_partial(emoji_half, 3, u8out, sizeof u8out);
    EXAMPLE_REQUIRE(st.err == PROVEN_ERR_NEED_MORE && st.consumed == 2, "a trailing high surrogate waits");

    printf("converted a %zu-byte name to %zu code units and back\n", (size_t)name.size, (size_t)need.value);

    proven_u8str_destroy(alloc, &copy);
    proven_u16str_destroy(alloc, &path);
    return EXAMPLE_OK();
}
