#ifndef PROVEN_BIT_H
#define PROVEN_BIT_H

#include "proven/types.h"

/**
 * @file bit.h
 * @brief Bit utilities - C23's <stdbit.h>, with every input defined - and byte order (RFC-0058).
 *
 * The same answers as C23's stdc_* functions where those are defined, and an answer where they
 * are not: a count of leading or trailing zeros of 0 is the width, the bit width of 0 is 0, the
 * floor of 0 is 0, and the ceiling of 0 is 1. A ceiling that does not fit the type is 0 (C23
 * leaves it undefined). Rotations take any count, reduced modulo the width. Byte order: loads
 * and stores of 16-, 32- and 64-bit unsigned values from and to bytes, big- or little-endian,
 * at any alignment; and byte swaps.
 *
 * All static inline; no state, no allocation; freestanding. On gcc and clang the counts are the
 * compilers' builtins (guarded against 0, where those are undefined); elsewhere a portable loop.
 * Each function has a form for each width (u8, u16, u32, u64); the PROVEN_BIT_* macros choose it
 * from the argument's type, which must be one of those four.
 */

#if defined(__GNUC__) || defined(__clang__)
#define PROVEN_BIT_BUILTINS_ 1
#else
#define PROVEN_BIT_BUILTINS_ 0
#endif

// -----------------------------------------------------------------------------
// The 64-bit core; the narrower forms widen to it
// -----------------------------------------------------------------------------

/** @brief The number of 1 bits. */
[[nodiscard]] static inline unsigned proven_bit_count_ones_u64(proven_u64 x) {
#if PROVEN_BIT_BUILTINS_
    return (unsigned)__builtin_popcountll(x);
#else
    unsigned n = 0;
    while (x) { x &= x - 1; n++; }
    return n;
#endif
}

/** @brief The number of 0 bits above the highest 1 bit; 64 for 0. */
[[nodiscard]] static inline unsigned proven_bit_leading_zeros_u64(proven_u64 x) {
    if (x == 0) return 64;
#if PROVEN_BIT_BUILTINS_
    return (unsigned)__builtin_clzll(x);
#else
    unsigned n = 0;
    while (!(x & ((proven_u64)1 << 63))) { x <<= 1; n++; }
    return n;
#endif
}

/** @brief The number of 0 bits below the lowest 1 bit; 64 for 0. */
[[nodiscard]] static inline unsigned proven_bit_trailing_zeros_u64(proven_u64 x) {
    if (x == 0) return 64;
#if PROVEN_BIT_BUILTINS_
    return (unsigned)__builtin_ctzll(x);
#else
    unsigned n = 0;
    while (!(x & 1)) { x >>= 1; n++; }
    return n;
#endif
}

/*
 * The narrower widths, each the 64-bit answer brought to its width. One line per function, the
 * same twelve for each width: count_ones, count_zeros, leading_zeros, trailing_zeros,
 * leading_ones, trailing_ones, width (bits needed; 0 for 0), has_single (a power of two), floor
 * (largest power of two not above; 0 for 0), ceil (smallest power of two not below; 1 for 0, 0
 * when it does not fit), rotl and rotr (the count modulo the width).
 */
/* 8 bits */
[[nodiscard]] static inline unsigned proven_bit_count_ones_u8(proven_u8 x) { return proven_bit_count_ones_u64(x); }
[[nodiscard]] static inline unsigned proven_bit_count_zeros_u8(proven_u8 x) { return 8u - proven_bit_count_ones_u64(x); }
[[nodiscard]] static inline unsigned proven_bit_leading_zeros_u8(proven_u8 x) { return proven_bit_leading_zeros_u64(x) - 56u; }
[[nodiscard]] static inline unsigned proven_bit_trailing_zeros_u8(proven_u8 x) { return x ? proven_bit_trailing_zeros_u64(x) : 8u; }
[[nodiscard]] static inline unsigned proven_bit_leading_ones_u8(proven_u8 x) { return proven_bit_leading_zeros_u8((proven_u8)~x); }
[[nodiscard]] static inline unsigned proven_bit_trailing_ones_u8(proven_u8 x) { return proven_bit_trailing_zeros_u8((proven_u8)~x); }
[[nodiscard]] static inline unsigned proven_bit_width_u8(proven_u8 x) { return 8u - proven_bit_leading_zeros_u8(x); }
[[nodiscard]] static inline bool proven_bit_has_single_u8(proven_u8 x) { return x != 0 && (x & (x - 1u)) == 0; }
[[nodiscard]] static inline proven_u8 proven_bit_floor_u8(proven_u8 x) { return (proven_u8)(x ? 1ull << (proven_bit_width_u8(x) - 1) : 0u); }
[[nodiscard]] static inline proven_u8 proven_bit_ceil_u8(proven_u8 x) {
    if (x <= 1) return 1;
    const unsigned w = proven_bit_width_u8((proven_u8)(x - 1));
    return (proven_u8)(w >= 8u ? 0u : 1ull << w);
}
[[nodiscard]] static inline proven_u8 proven_bit_rotl_u8(proven_u8 x, unsigned n) {
    const unsigned long long v = x; n %= 8u; return (proven_u8)(n ? (v << n) | (v >> (8u - n)) : v);
}
[[nodiscard]] static inline proven_u8 proven_bit_rotr_u8(proven_u8 x, unsigned n) {
    const unsigned long long v = x; n %= 8u; return (proven_u8)(n ? (v >> n) | (v << (8u - n)) : v);
}

/* 16 bits */
[[nodiscard]] static inline unsigned proven_bit_count_ones_u16(proven_u16 x) { return proven_bit_count_ones_u64(x); }
[[nodiscard]] static inline unsigned proven_bit_count_zeros_u16(proven_u16 x) { return 16u - proven_bit_count_ones_u64(x); }
[[nodiscard]] static inline unsigned proven_bit_leading_zeros_u16(proven_u16 x) { return proven_bit_leading_zeros_u64(x) - 48u; }
[[nodiscard]] static inline unsigned proven_bit_trailing_zeros_u16(proven_u16 x) { return x ? proven_bit_trailing_zeros_u64(x) : 16u; }
[[nodiscard]] static inline unsigned proven_bit_leading_ones_u16(proven_u16 x) { return proven_bit_leading_zeros_u16((proven_u16)~x); }
[[nodiscard]] static inline unsigned proven_bit_trailing_ones_u16(proven_u16 x) { return proven_bit_trailing_zeros_u16((proven_u16)~x); }
[[nodiscard]] static inline unsigned proven_bit_width_u16(proven_u16 x) { return 16u - proven_bit_leading_zeros_u16(x); }
[[nodiscard]] static inline bool proven_bit_has_single_u16(proven_u16 x) { return x != 0 && (x & (x - 1u)) == 0; }
[[nodiscard]] static inline proven_u16 proven_bit_floor_u16(proven_u16 x) { return (proven_u16)(x ? 1ull << (proven_bit_width_u16(x) - 1) : 0u); }
[[nodiscard]] static inline proven_u16 proven_bit_ceil_u16(proven_u16 x) {
    if (x <= 1) return 1;
    const unsigned w = proven_bit_width_u16((proven_u16)(x - 1));
    return (proven_u16)(w >= 16u ? 0u : 1ull << w);
}
[[nodiscard]] static inline proven_u16 proven_bit_rotl_u16(proven_u16 x, unsigned n) {
    const unsigned long long v = x; n %= 16u; return (proven_u16)(n ? (v << n) | (v >> (16u - n)) : v);
}
[[nodiscard]] static inline proven_u16 proven_bit_rotr_u16(proven_u16 x, unsigned n) {
    const unsigned long long v = x; n %= 16u; return (proven_u16)(n ? (v >> n) | (v << (16u - n)) : v);
}

/* 32 bits */
[[nodiscard]] static inline unsigned proven_bit_count_ones_u32(proven_u32 x) { return proven_bit_count_ones_u64(x); }
[[nodiscard]] static inline unsigned proven_bit_count_zeros_u32(proven_u32 x) { return 32u - proven_bit_count_ones_u64(x); }
[[nodiscard]] static inline unsigned proven_bit_leading_zeros_u32(proven_u32 x) { return proven_bit_leading_zeros_u64(x) - 32u; }
[[nodiscard]] static inline unsigned proven_bit_trailing_zeros_u32(proven_u32 x) { return x ? proven_bit_trailing_zeros_u64(x) : 32u; }
[[nodiscard]] static inline unsigned proven_bit_leading_ones_u32(proven_u32 x) { return proven_bit_leading_zeros_u32((proven_u32)~x); }
[[nodiscard]] static inline unsigned proven_bit_trailing_ones_u32(proven_u32 x) { return proven_bit_trailing_zeros_u32((proven_u32)~x); }
[[nodiscard]] static inline unsigned proven_bit_width_u32(proven_u32 x) { return 32u - proven_bit_leading_zeros_u32(x); }
[[nodiscard]] static inline bool proven_bit_has_single_u32(proven_u32 x) { return x != 0 && (x & (x - 1u)) == 0; }
[[nodiscard]] static inline proven_u32 proven_bit_floor_u32(proven_u32 x) { return (proven_u32)(x ? 1ull << (proven_bit_width_u32(x) - 1) : 0u); }
[[nodiscard]] static inline proven_u32 proven_bit_ceil_u32(proven_u32 x) {
    if (x <= 1) return 1;
    const unsigned w = proven_bit_width_u32((proven_u32)(x - 1));
    return (proven_u32)(w >= 32u ? 0u : 1ull << w);
}
[[nodiscard]] static inline proven_u32 proven_bit_rotl_u32(proven_u32 x, unsigned n) {
    const unsigned long long v = x; n %= 32u; return (proven_u32)(n ? (v << n) | (v >> (32u - n)) : v);
}
[[nodiscard]] static inline proven_u32 proven_bit_rotr_u32(proven_u32 x, unsigned n) {
    const unsigned long long v = x; n %= 32u; return (proven_u32)(n ? (v >> n) | (v << (32u - n)) : v);
}

/* 64 bits: the core above, and the rest. */
[[nodiscard]] static inline unsigned proven_bit_count_zeros_u64(proven_u64 x) { return 64u - proven_bit_count_ones_u64(x); }
[[nodiscard]] static inline unsigned proven_bit_leading_ones_u64(proven_u64 x) { return proven_bit_leading_zeros_u64(~x); }
[[nodiscard]] static inline unsigned proven_bit_trailing_ones_u64(proven_u64 x) { return proven_bit_trailing_zeros_u64(~x); }
[[nodiscard]] static inline unsigned proven_bit_width_u64(proven_u64 x) { return 64u - proven_bit_leading_zeros_u64(x); }
[[nodiscard]] static inline bool proven_bit_has_single_u64(proven_u64 x) { return x != 0 && (x & (x - 1)) == 0; }
[[nodiscard]] static inline proven_u64 proven_bit_floor_u64(proven_u64 x) { return x ? (proven_u64)1 << (proven_bit_width_u64(x) - 1) : 0; }
[[nodiscard]] static inline proven_u64 proven_bit_ceil_u64(proven_u64 x) {
    if (x <= 1) return 1;
    const unsigned w = proven_bit_width_u64(x - 1);
    return w >= 64u ? 0 : (proven_u64)1 << w;
}
[[nodiscard]] static inline proven_u64 proven_bit_rotl_u64(proven_u64 x, unsigned n) { n %= 64u; return n ? (x << n) | (x >> (64u - n)) : x; }
[[nodiscard]] static inline proven_u64 proven_bit_rotr_u64(proven_u64 x, unsigned n) { n %= 64u; return n ? (x >> n) | (x << (64u - n)) : x; }

/* The width chosen from the argument's type. */
#define PROVEN_BIT_GENERIC_(name, x) _Generic((x), \
    proven_u8: proven_bit_##name##_u8, proven_u16: proven_bit_##name##_u16, \
    proven_u32: proven_bit_##name##_u32, proven_u64: proven_bit_##name##_u64)
#define PROVEN_BIT_COUNT_ONES(x) PROVEN_BIT_GENERIC_(count_ones, x)(x)
#define PROVEN_BIT_COUNT_ZEROS(x) PROVEN_BIT_GENERIC_(count_zeros, x)(x)
#define PROVEN_BIT_LEADING_ZEROS(x) PROVEN_BIT_GENERIC_(leading_zeros, x)(x)
#define PROVEN_BIT_TRAILING_ZEROS(x) PROVEN_BIT_GENERIC_(trailing_zeros, x)(x)
#define PROVEN_BIT_LEADING_ONES(x) PROVEN_BIT_GENERIC_(leading_ones, x)(x)
#define PROVEN_BIT_TRAILING_ONES(x) PROVEN_BIT_GENERIC_(trailing_ones, x)(x)
#define PROVEN_BIT_WIDTH(x) PROVEN_BIT_GENERIC_(width, x)(x)
#define PROVEN_BIT_HAS_SINGLE(x) PROVEN_BIT_GENERIC_(has_single, x)(x)
#define PROVEN_BIT_FLOOR(x) PROVEN_BIT_GENERIC_(floor, x)(x)
#define PROVEN_BIT_CEIL(x) PROVEN_BIT_GENERIC_(ceil, x)(x)
#define PROVEN_BIT_ROTL(x, n) PROVEN_BIT_GENERIC_(rotl, x)((x), (n))
#define PROVEN_BIT_ROTR(x, n) PROVEN_BIT_GENERIC_(rotr, x)((x), (n))

// -----------------------------------------------------------------------------
// Byte order
// -----------------------------------------------------------------------------

/** @brief The bytes of `x` in the other order. */
[[nodiscard]] static inline proven_u16 proven_bit_byteswap_u16(proven_u16 x) { return (proven_u16)((x >> 8) | (x << 8)); }
[[nodiscard]] static inline proven_u32 proven_bit_byteswap_u32(proven_u32 x) {
    return (x >> 24) | ((x >> 8) & 0xff00u) | ((x << 8) & 0xff0000u) | (x << 24);
}
[[nodiscard]] static inline proven_u64 proven_bit_byteswap_u64(proven_u64 x) {
    return ((proven_u64)proven_bit_byteswap_u32((proven_u32)x) << 32) | proven_bit_byteswap_u32((proven_u32)(x >> 32));
}

/** @brief A value from bytes at `p`, at any alignment: big-endian (most significant first) or
 *         little-endian. `p` must have the value's size. */
[[nodiscard]] static inline proven_u16 proven_load_u16_be(const proven_byte_t *p) { return (proven_u16)(((proven_u16)p[0] << 8) | p[1]); }
[[nodiscard]] static inline proven_u16 proven_load_u16_le(const proven_byte_t *p) { return (proven_u16)(((proven_u16)p[1] << 8) | p[0]); }
[[nodiscard]] static inline proven_u32 proven_load_u32_be(const proven_byte_t *p) {
    return ((proven_u32)p[0] << 24) | ((proven_u32)p[1] << 16) | ((proven_u32)p[2] << 8) | p[3];
}
[[nodiscard]] static inline proven_u32 proven_load_u32_le(const proven_byte_t *p) {
    return ((proven_u32)p[3] << 24) | ((proven_u32)p[2] << 16) | ((proven_u32)p[1] << 8) | p[0];
}
[[nodiscard]] static inline proven_u64 proven_load_u64_be(const proven_byte_t *p) {
    return ((proven_u64)proven_load_u32_be(p) << 32) | proven_load_u32_be(p + 4);
}
[[nodiscard]] static inline proven_u64 proven_load_u64_le(const proven_byte_t *p) {
    return ((proven_u64)proven_load_u32_le(p + 4) << 32) | proven_load_u32_le(p);
}

/** @brief `x` into the bytes at `p`, at any alignment, big- or little-endian. */
static inline void proven_store_u16_be(proven_byte_t *p, proven_u16 x) { p[0] = (proven_byte_t)(x >> 8); p[1] = (proven_byte_t)x; }
static inline void proven_store_u16_le(proven_byte_t *p, proven_u16 x) { p[0] = (proven_byte_t)x; p[1] = (proven_byte_t)(x >> 8); }
static inline void proven_store_u32_be(proven_byte_t *p, proven_u32 x) {
    p[0] = (proven_byte_t)(x >> 24); p[1] = (proven_byte_t)(x >> 16); p[2] = (proven_byte_t)(x >> 8); p[3] = (proven_byte_t)x;
}
static inline void proven_store_u32_le(proven_byte_t *p, proven_u32 x) {
    p[0] = (proven_byte_t)x; p[1] = (proven_byte_t)(x >> 8); p[2] = (proven_byte_t)(x >> 16); p[3] = (proven_byte_t)(x >> 24);
}
static inline void proven_store_u64_be(proven_byte_t *p, proven_u64 x) { proven_store_u32_be(p, (proven_u32)(x >> 32)); proven_store_u32_be(p + 4, (proven_u32)x); }
static inline void proven_store_u64_le(proven_byte_t *p, proven_u64 x) { proven_store_u32_le(p, (proven_u32)x); proven_store_u32_le(p + 4, (proven_u32)(x >> 32)); }

#endif /* PROVEN_BIT_H */
