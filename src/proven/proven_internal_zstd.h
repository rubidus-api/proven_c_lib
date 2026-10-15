#ifndef PROVEN_INTERNAL_ZSTD_H
#define PROVEN_INTERNAL_ZSTD_H

#include "proven/types.h"

/*
 * Internal: what the Zstandard decoder (zstd.c) and encoder (zstd_encode.c) share - the frame
 * checksum, the code tables and the predefined distributions of RFC 8878. Not public API.
 * Everything here is static, so a program that links only one of the two carries only what
 * that one uses.
 */

#define ZSTD_MAGIC 0xFD2FB528u
#define ZSTD_BLOCK_MAX ((proven_size_t)128 * 1024)

static inline proven_u64 le64(const proven_byte_t *p) {
    return (proven_u64)p[0] | ((proven_u64)p[1] << 8) | ((proven_u64)p[2] << 16) | ((proven_u64)p[3] << 24) |
           ((proven_u64)p[4] << 32) | ((proven_u64)p[5] << 40) | ((proven_u64)p[6] << 48) | ((proven_u64)p[7] << 56);
}

static inline proven_u32 le32(const proven_byte_t *p) {
    return (proven_u32)p[0] | ((proven_u32)p[1] << 8) | ((proven_u32)p[2] << 16) | ((proven_u32)p[3] << 24);
}

/* Inlining that the hot loops need, and the bit scans the processor has, where the compiler
 * says how to ask for them; plain C otherwise. PROVEN_ZSTD_NO_BUILTINS takes the plain C bit
 * scans everywhere, so that a test can run the code other compilers get. */
#if defined(__GNUC__) || defined(__clang__)
#define ZSTD_HOT static inline __attribute__((always_inline))
#else
#define ZSTD_HOT static inline
#endif
#if (defined(__GNUC__) || defined(__clang__)) && !defined(PROVEN_ZSTD_NO_BUILTINS)
#define ZSTD_BUILTINS 1
#else
#define ZSTD_BUILTINS 0
#endif

/* The position of the highest set bit of a nonzero value. */
static inline unsigned highbit32(proven_u32 v) {
#if ZSTD_BUILTINS
    return 31u - (unsigned)__builtin_clz(v | 1u);
#else
    static const proven_u8 DEBRUIJN[32] = { 0, 9, 1, 10, 13, 21, 2, 29, 11, 14, 16, 18, 22, 25, 3, 30,
                                            8, 12, 20, 28, 15, 17, 24, 7, 19, 27, 23, 6, 26, 5, 4, 31 };
    v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
    return DEBRUIJN[(proven_u32)(v * 0x07C4ACDDu) >> 27];
#endif
}

typedef struct {
    proven_u64 v[4];
    proven_u64 total;
    proven_byte_t buf[32];
    unsigned held;
} xxh64_t;

// -----------------------------------------------------------------------------
// XXH64, for the frame checksum
// -----------------------------------------------------------------------------

#define P1 0x9E3779B185EBCA87ull
#define P2 0xC2B2AE3D27D4EB4Full
#define P3 0x165667B19E3779F9ull
#define P4 0x85EBCA77C2B2AE63ull
#define P5 0x27D4EB2F165667C5ull

static inline proven_u64 rotl64(proven_u64 x, unsigned r) { return (x << r) | (x >> (64 - r)); }

static inline proven_u64 xxh_round(proven_u64 acc, proven_u64 in) { return rotl64(acc + in * P2, 31) * P1; }

static inline void xxh_init(xxh64_t *x) {
    *x = (xxh64_t){ .v = { P1 + P2, P2, 0, (proven_u64)0 - P1 } };
}

static inline void xxh_update(xxh64_t *x, const proven_byte_t *p, proven_size_t n) {
    x->total += n;
    if (x->held > 0) {
        while (x->held < 32 && n > 0) { x->buf[x->held++] = *p++; n--; }
        if (x->held < 32) return;
        for (unsigned i = 0; i < 4; ++i) x->v[i] = xxh_round(x->v[i], le64(x->buf + 8 * i));
        x->held = 0;
    }
    for (; n >= 32; p += 32, n -= 32) {
        x->v[0] = xxh_round(x->v[0], le64(p));
        x->v[1] = xxh_round(x->v[1], le64(p + 8));
        x->v[2] = xxh_round(x->v[2], le64(p + 16));
        x->v[3] = xxh_round(x->v[3], le64(p + 24));
    }
    while (n > 0) { x->buf[x->held++] = *p++; n--; }
}

static inline proven_u64 xxh_digest(const xxh64_t *x) {
    proven_u64 h;
    if (x->total >= 32) {
        h = rotl64(x->v[0], 1) + rotl64(x->v[1], 7) + rotl64(x->v[2], 12) + rotl64(x->v[3], 18);
        for (unsigned i = 0; i < 4; ++i) h = (h ^ xxh_round(0, x->v[i])) * P1 + P4;
    } else {
        h = x->v[2] + P5;
    }
    h += x->total;
    const proven_byte_t *p = x->buf;
    unsigned n = x->held;
    for (; n >= 8; p += 8, n -= 8) h = rotl64(h ^ xxh_round(0, le64(p)), 27) * P1 + P4;
    if (n >= 4) { h = rotl64(h ^ ((proven_u64)le32(p) * P1), 23) * P2 + P3; p += 4; n -= 4; }
    for (; n > 0; ++p, --n) h = rotl64(h ^ ((proven_u64)*p * P5), 11) * P1;
    h ^= h >> 33; h *= P2;
    h ^= h >> 29; h *= P3;
    h ^= h >> 32;
    return h;
}

// -----------------------------------------------------------------------------
// Sequences: the predefined distributions (section 3.1.1.3.2.2) and the codes of lengths
// -----------------------------------------------------------------------------

static const proven_i16 LL_DEFAULT[36] = { 4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2,
                                           2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1 };
static const proven_i16 ML_DEFAULT[53] = { 1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                           1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1 };
static const proven_i16 OF_DEFAULT[29] = { 1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1 };

static const proven_u32 LL_BASE[36] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 32, 40,
                                        48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536 };
static const proven_u8 LL_BITS[36] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3,
                                       4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
static const proven_u32 ML_BASE[53] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26,
                                        27, 28, 29, 30, 31, 32, 33, 34, 35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515,
                                        1027, 2051, 4099, 8195, 16387, 32771, 65539 };
static const proven_u8 ML_BITS[53] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                       1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };

#endif
