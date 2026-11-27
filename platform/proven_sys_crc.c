#include "proven_sys_crc.h"

/* Folding by carry-less multiplication (Intel, "Fast CRC Computation for Generic Polynomials
 * Using PCLMULQDQ", 2009), in the reflected form: four 128-bit lanes while 64 bytes remain, then
 * one. Each 128-bit value A, followed by D bits of message, is replaced by
 * A.lo * (x^(D+64) mod P) ^ A.hi * (x^D mod P), which has the same remainder. The last 16 bytes
 * are left to the table-driven code: no Barrett reduction, and one routine for every width. */

#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__)) && !defined(PROVEN_SYS_CRC_DISABLE)

#include <immintrin.h>

#define CRC_TARGET __attribute__((target("pclmul,sse2")))

bool proven_sys_crc_clmul_available(void) {
    __builtin_cpu_init();
    return __builtin_cpu_supports("pclmul") && __builtin_cpu_supports("sse2");
}

CRC_TARGET static inline __m128i crc_fold(__m128i x, __m128i k, __m128i next) {
    return _mm_xor_si128(_mm_xor_si128(_mm_clmulepi64_si128(x, k, 0x00), _mm_clmulepi64_si128(x, k, 0x11)), next);
}

#define CRC_LOAD(p) _mm_loadu_si128((const __m128i *)(const void *)(p))

CRC_TARGET bool proven_sys_crc_fold(const uint64_t k[4], uint64_t crc, const uint8_t *p, size_t n, uint8_t out[16]) {
    const __m128i k4 = _mm_set_epi64x((long long)k[1], (long long)k[0]);
    const __m128i k1 = _mm_set_epi64x((long long)k[3], (long long)k[2]);
    __m128i x0 = _mm_xor_si128(CRC_LOAD(p), _mm_set_epi64x(0, (long long)crc));
    __m128i x1 = CRC_LOAD(p + 16), x2 = CRC_LOAD(p + 32), x3 = CRC_LOAD(p + 48);
    p += 64; n -= 64;
    while (n >= 64) {
        x0 = crc_fold(x0, k4, CRC_LOAD(p));
        x1 = crc_fold(x1, k4, CRC_LOAD(p + 16));
        x2 = crc_fold(x2, k4, CRC_LOAD(p + 32));
        x3 = crc_fold(x3, k4, CRC_LOAD(p + 48));
        p += 64; n -= 64;
    }
    x0 = crc_fold(crc_fold(crc_fold(x0, k1, x1), k1, x2), k1, x3);
    for (; n >= 16; p += 16, n -= 16) x0 = crc_fold(x0, k1, CRC_LOAD(p));
    _mm_storeu_si128((__m128i *)(void *)out, x0);
    return true;
}

#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__)) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ && \
    !defined(PROVEN_SYS_CRC_DISABLE)

#include <arm_neon.h>

#if defined(__linux__)
#include <sys/auxv.h>
bool proven_sys_crc_clmul_available(void) { return (getauxval(AT_HWCAP) & (1ul << 4)) != 0; }   /* HWCAP_PMULL */
#elif defined(__APPLE__)
bool proven_sys_crc_clmul_available(void) { return true; }                   /* every Apple AArch64 core has it */
#elif defined(_WIN32)
#include <windows.h>
bool proven_sys_crc_clmul_available(void) { return IsProcessorFeaturePresent(PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE) != 0; }
#else
bool proven_sys_crc_clmul_available(void) { return false; }
#endif

#if defined(__clang__)
#define CRC_TARGET __attribute__((target("aes")))
#else
#define CRC_TARGET __attribute__((target("+crypto")))
#endif

CRC_TARGET static inline uint64x2_t crc_fold(uint64x2_t x, uint64x2_t k, uint64x2_t next) {
    const uint64x2_t lo = vreinterpretq_u64_p128(vmull_p64((poly64_t)vgetq_lane_u64(x, 0), (poly64_t)vgetq_lane_u64(k, 0)));
    const uint64x2_t hi = vreinterpretq_u64_p128(vmull_p64((poly64_t)vgetq_lane_u64(x, 1), (poly64_t)vgetq_lane_u64(k, 1)));
    return veorq_u64(veorq_u64(lo, hi), next);
}

#define CRC_LOAD(p) vreinterpretq_u64_u8(vld1q_u8(p))

CRC_TARGET bool proven_sys_crc_fold(const uint64_t k[4], uint64_t crc, const uint8_t *p, size_t n, uint8_t out[16]) {
    const uint64x2_t k4 = vld1q_u64(k), k1 = vld1q_u64(k + 2);
    uint64x2_t x0 = veorq_u64(CRC_LOAD(p), vcombine_u64(vcreate_u64(crc), vcreate_u64(0)));
    uint64x2_t x1 = CRC_LOAD(p + 16), x2 = CRC_LOAD(p + 32), x3 = CRC_LOAD(p + 48);
    p += 64; n -= 64;
    while (n >= 64) {
        x0 = crc_fold(x0, k4, CRC_LOAD(p));
        x1 = crc_fold(x1, k4, CRC_LOAD(p + 16));
        x2 = crc_fold(x2, k4, CRC_LOAD(p + 32));
        x3 = crc_fold(x3, k4, CRC_LOAD(p + 48));
        p += 64; n -= 64;
    }
    x0 = crc_fold(crc_fold(crc_fold(x0, k1, x1), k1, x2), k1, x3);
    for (; n >= 16; p += 16, n -= 16) x0 = crc_fold(x0, k1, CRC_LOAD(p));
    vst1q_u8(out, vreinterpretq_u8_u64(x0));
    return true;
}

#else

bool proven_sys_crc_clmul_available(void) { return false; }

bool proven_sys_crc_fold(const uint64_t k[4], uint64_t crc, const uint8_t *p, size_t n, uint8_t out[16]) {
    (void)k; (void)crc; (void)p; (void)n; (void)out;
    return false;
}

#endif
