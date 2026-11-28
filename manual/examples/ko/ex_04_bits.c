#include "example.h"

/*
 * bit.h: 모든 입력에 답이 있는 C23 의 <stdbit.h>, 그리고 htonl 없는 바이트 순서.
 *
 * 1부는 프로토콜이 하듯이 패킷 머리말을 쓴다 - 매직 넘버는 빅엔디언, 길이는 리틀엔디언, 정렬은
 * 아무렇게나 - 그리고 다시 읽는다. 2부는 너비마다 값 하나에 비트에 관한 질문을 한다. C23 은
 * 가장자리를 정의하지 않지만 이 라이브러리는 정의한다. 0 의 앞쪽 0 개수는 너비, 담기지 않는
 * ceiling 은 0, 너비보다 큰 회전은 한 바퀴 돈다.
 */

int main(void) {
    /* 1부: 바이트 순서. 버퍼는 일부러 홀수 위치에서 시작한다. */
    proven_byte_t wire[1 + 2 + 4 + 8] = { 0 };
    proven_byte_t *p = wire + 1;
    proven_store_u16_be(p, 0xCAFE);                       /* 매직 넘버, 네트워크 순서 */
    proven_store_u32_le(p + 2, 1500);                     /* 길이, 리틀엔디언 */
    proven_store_u64_be(p + 6, 0x0102030405060708u);      /* 순번 */
    EXAMPLE_REQUIRE(p[0] == 0xCA && p[1] == 0xFE && p[2] == 0xDC && p[3] == 0x05, "the bytes in the order written");
    EXAMPLE_REQUIRE(proven_load_u16_be(p) == 0xCAFE && proven_load_u16_le(p) == 0xFECA, "the same two bytes, read both ways");
    EXAMPLE_REQUIRE(proven_load_u32_le(p + 2) == 1500 && proven_load_u32_be(p + 2) == proven_bit_byteswap_u32(1500), "a swap is the other order");
    EXAMPLE_REQUIRE(proven_load_u64_be(p + 6) == 0x0102030405060708u && proven_load_u64_le(p + 6) == proven_bit_byteswap_u64(0x0102030405060708u), "64 bits");
    proven_store_u16_le(p, 0xCAFE);
    proven_store_u32_be(p + 2, 1500);
    proven_store_u64_le(p + 6, 1);
    EXAMPLE_REQUIRE(proven_load_u16_be(p) == proven_bit_byteswap_u16(0xCAFE) && p[6] == 1, "and back");

    /* 2부: 너비마다 같은 질문. 0x50 은 0101 0000. */
    const proven_u8 a = 0x50;
    EXAMPLE_REQUIRE(proven_bit_count_ones_u8(a) == 2 && proven_bit_count_zeros_u8(a) == 6, "two ones, six zeros");
    EXAMPLE_REQUIRE(proven_bit_leading_zeros_u8(a) == 1 && proven_bit_trailing_zeros_u8(a) == 4, "one zero above, four below");
    EXAMPLE_REQUIRE(proven_bit_leading_ones_u8(0xE0) == 3 && proven_bit_trailing_ones_u8(0x07) == 3, "runs of ones");
    EXAMPLE_REQUIRE(proven_bit_width_u8(a) == 7 && proven_bit_floor_u8(a) == 64 && proven_bit_ceil_u8(a) == 128, "width, floor, ceiling");
    EXAMPLE_REQUIRE(!proven_bit_has_single_u8(a) && proven_bit_has_single_u8(64), "a power of two has one bit");
    EXAMPLE_REQUIRE(proven_bit_rotl_u8(0x81, 1) == 0x03 && proven_bit_rotr_u8(0x03, 9) == 0x81, "rotations wrap; 9 is 1 at this width");
    EXAMPLE_REQUIRE(proven_bit_ceil_u8(129) == 0 && proven_bit_leading_zeros_u8(0) == 8, "the edges: no ceiling fits, all zeros");

    const proven_u16 b = 0x0500;                          /* 0000 0101 0000 0000 */
    EXAMPLE_REQUIRE(proven_bit_count_ones_u16(b) == 2 && proven_bit_count_zeros_u16(b) == 14, "16 bits: counts");
    EXAMPLE_REQUIRE(proven_bit_leading_zeros_u16(b) == 5 && proven_bit_trailing_zeros_u16(b) == 8, "16 bits: zeros");
    EXAMPLE_REQUIRE(proven_bit_leading_ones_u16(0xF000) == 4 && proven_bit_trailing_ones_u16(0x00FF) == 8, "16 bits: ones");
    EXAMPLE_REQUIRE(proven_bit_width_u16(b) == 11 && proven_bit_floor_u16(b) == 0x0400 && proven_bit_ceil_u16(b) == 0x0800, "16 bits: powers");
    EXAMPLE_REQUIRE(!proven_bit_has_single_u16(b) && proven_bit_rotl_u16(0x8001, 4) == 0x0018 && proven_bit_rotr_u16(0x0018, 4) == 0x8001, "16 bits: rotations");

    const proven_u32 c = 4096 + 1;                        /* 2 의 거듭제곱을 원하는 해시 표 */
    EXAMPLE_REQUIRE(proven_bit_ceil_u32(c) == 8192 && proven_bit_floor_u32(c) == 4096 && proven_bit_width_u32(c) == 13, "32 bits: buckets for 4097 keys");
    EXAMPLE_REQUIRE(proven_bit_count_ones_u32(c) == 2 && proven_bit_count_zeros_u32(c) == 30 && proven_bit_has_single_u32(4096), "32 bits: counts");
    EXAMPLE_REQUIRE(proven_bit_leading_zeros_u32(c) == 19 && proven_bit_trailing_zeros_u32(4096) == 12, "32 bits: zeros");
    EXAMPLE_REQUIRE(proven_bit_leading_ones_u32(0xFFFF0000u) == 16 && proven_bit_trailing_ones_u32(0x0000FFFFu) == 16, "32 bits: ones");
    EXAMPLE_REQUIRE(proven_bit_rotl_u32(0x80000001u, 1) == 3 && proven_bit_rotr_u32(3, 1) == 0x80000001u, "32 bits: rotations");

    const proven_u64 d = 0;                               /* C23 이 정의하지 않는 경우 */
    EXAMPLE_REQUIRE(proven_bit_leading_zeros_u64(d) == 64 && proven_bit_trailing_zeros_u64(d) == 64 && proven_bit_width_u64(d) == 0, "0: the width, and no bits needed");
    EXAMPLE_REQUIRE(proven_bit_floor_u64(d) == 0 && proven_bit_ceil_u64(d) == 1 && proven_bit_ceil_u64(((proven_u64)1 << 63) + 1) == 0, "0's floor and ceiling; a ceiling past 2^63");
    EXAMPLE_REQUIRE(proven_bit_count_ones_u64(~d) == 64 && proven_bit_count_zeros_u64(d) == 64 && !proven_bit_has_single_u64(d), "64 bits: counts");
    EXAMPLE_REQUIRE(proven_bit_leading_ones_u64(~d) == 64 && proven_bit_trailing_ones_u64(7) == 3, "64 bits: ones");
    EXAMPLE_REQUIRE(proven_bit_rotl_u64(1, 64) == 1 && proven_bit_rotr_u64(1, 1) == (proven_u64)1 << 63, "64 bits: rotations");

    /* 매크로는 인수의 타입으로 너비를 고른다. */
    EXAMPLE_REQUIRE(PROVEN_BIT_COUNT_ONES(a) == 2 && PROVEN_BIT_WIDTH(b) == 11 && PROVEN_BIT_CEIL(c) == 8192, "type-generic forms");

    printf("magic %04x, length %u, 4097 keys -> %u buckets\n", 0xCAFE, 1500u, (unsigned)proven_bit_ceil_u32(c));
    return EXAMPLE_OK();
}
