#if !defined(_WIN32) && !defined(_WIN64)
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "proven_sys_random.h"

#if defined(_WIN32) || defined(_WIN64)

#include <windows.h>
#include <bcrypt.h>
#include <limits.h>
#include "proven_sys_random_chunk.h"
#if defined(_MSC_VER)
/* MSVC links the import library from the source. GCC-family Windows toolchains
 * (mingw-w64) do not understand this pragma and reject it under
 * -Werror=unknown-pragmas, so they must be given -lbcrypt at link time instead. */
#pragma comment(lib, "bcrypt.lib")
#endif

bool proven_sys_random_bytes(void *buf, size_t len) {
    if (len == 0) return true;
    if (!buf) return false;

    /*
     * The modern, DLL-free Windows CSPRNG. BCRYPT_USE_SYSTEM_PREFERRED_RNG means no
     * algorithm handle to open or close.
     *
     * Asked for in chunks of at most ULONG_MAX, because that is the type of the length
     * argument. This used to be one cast of the whole size_t to ULONG: on 64-bit Windows a
     * length above ULONG_MAX narrowed silently, only the low 32 bits were requested - a
     * length of exactly 2^32 requested ZERO bytes - and the success of that short request
     * was returned as success for the whole buffer. The caller then read bytes it had
     * never written as fresh entropy. Ordinary seeding never asks for enough to reach it,
     * which is exactly why it could sit here unnoticed.
     *
     * A failed chunk fails the whole call. There is no fallback to a PRNG: an entropy call
     * that quietly substitutes something weaker is worse than one that says no. Failure may
     * leave a filled prefix - the boolean API has nowhere to report how much - so a caller
     * must discard the entire buffer, not keep what looks filled.
     */
    unsigned char *p = (unsigned char *)buf;
    size_t remaining = len;
    while (remaining > 0) {
        size_t chunk = proven_sys_random_chunk(remaining, (size_t)ULONG_MAX);
        NTSTATUS s = BCryptGenRandom(NULL, (PUCHAR)p, (ULONG)chunk,
                                     BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (s != 0) return false; /* not STATUS_SUCCESS */
        /* Advanced only after the OS said it filled them. */
        p += chunk;
        remaining -= chunk;
    }
    return true;
}

#else

#include <stddef.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#if defined(__linux__)
/* <sys/random.h> declares getrandom() on glibc 2.25+ and defines this guard macro; older
 * systems without it fall through to the /dev/urandom path below. */
#include <sys/random.h>
#if defined(GRND_NONBLOCK)
#define PROVEN_HAVE_GETRANDOM 1
#endif
#endif

#if defined(__APPLE__) || defined(__OpenBSD__) || defined(__FreeBSD__) || defined(__NetBSD__)
/* getentropy(): the BSD/macOS spelling. This header used to CLAIM it and never call it - the
 * BSDs quietly fell through to /dev/urandom, which works but is not what the documentation
 * said, and needs an fd that an fd-exhausted process cannot open. */
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/random.h>
#endif
#define PROVEN_HAVE_GETENTROPY 1
#endif

bool proven_sys_random_bytes(void *buf, size_t len) {
    if (len == 0) return true;
    if (!buf) return false;

    unsigned char *p = (unsigned char *)buf;
    size_t got = 0;

#if defined(PROVEN_HAVE_GETRANDOM)
    /* getrandom() is the right call: it needs no fd, cannot be exhausted by an fd-leak, and
     * blocks only once, very early, until the pool is seeded - which is exactly when you
     * want it to wait rather than hand back weak bytes. Loop for short reads and retry EINTR. */
    while (got < len) {
        ssize_t n = getrandom(p + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;   /* fall through to the /dev/urandom fallback for old kernels */
        }
        got += (size_t)n;
    }
    if (got == len) return true;
#endif

#if defined(PROVEN_HAVE_GETENTROPY)
    /* getentropy() takes at most 256 bytes per call, so loop. Like getrandom() it needs no fd,
     * which matters: a process that has exhausted its descriptors cannot open /dev/urandom, and
     * that is not a moment at which a key derivation should start failing. */
    while (got < len) {
        size_t chunk = len - got;
        if (chunk > 256) chunk = 256;
        if (getentropy(p + got, chunk) != 0) break;   /* fall through to /dev/urandom */
        got += chunk;
    }
    if (got == len) return true;
#endif

    /* Last resort for systems with neither call: read /dev/urandom to completion. */
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        if (n == 0) { close(fd); return false; }   /* EOF from /dev/urandom: cannot happen, but do not spin */
        got += (size_t)n;
    }
    close(fd);
    return true;
}

#endif

/* ---- ChaCha20 keystream on vector lanes, for proven_chacha_rng_fill ----
 *
 * Vector j holds word j of four (SSE2) or eight (AVX2) consecutive blocks, so a quarter round is
 * the portable code's additions, XORs and rotations done on every block at once; transposes at
 * the end turn the words back into each block's 64 bytes. No table and no data-dependent branch.
 * The same construction as proven_crypto's (crypto/platform/proven_sys_chacha.c), which this base
 * layer cannot call; here the keystream is stored, not XORed into a text. */

#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__)) && !defined(PROVEN_SYS_CHACHA_DISABLE)

#include <immintrin.h>
#include <stdatomic.h>

#define KS_SSE __attribute__((target("sse2")))
#define KS_AVX __attribute__((target("avx2")))

int proven_sys_chacha_keystream_width(void) {
    static _Atomic int width = -1;   /* -1: not yet looked; the answer is the same on every thread */
    int w = atomic_load_explicit(&width, memory_order_relaxed);
    if (w < 0) {
        __builtin_cpu_init();
        w = __builtin_cpu_supports("avx2") ? 8 : __builtin_cpu_supports("sse2") ? 4 : 0;
        atomic_store_explicit(&width, w, memory_order_relaxed);
    }
    return w;
}

static void ks_forget(void *p, size_t n) {
    unsigned char *b = (unsigned char *)p;
    for (size_t i = 0; i < n; ++i) b[i] = 0;
    __asm__ __volatile__("" : : "r"(p) : "memory");
}

#define KS_R4_16(x) _mm_shufflehi_epi16(_mm_shufflelo_epi16((x), 0xb1), 0xb1)
#define KS_R4(x, n) _mm_or_si128(_mm_slli_epi32((x), (n)), _mm_srli_epi32((x), 32 - (n)))
#define KS_QR4(a, b, c, d)                                                           \
    do {                                                                             \
        a = _mm_add_epi32(a, b); d = _mm_xor_si128(d, a); d = KS_R4_16(d);           \
        c = _mm_add_epi32(c, d); b = _mm_xor_si128(b, c); b = KS_R4(b, 12);          \
        a = _mm_add_epi32(a, b); d = _mm_xor_si128(d, a); d = KS_R4(d, 8);           \
        c = _mm_add_epi32(c, d); b = _mm_xor_si128(b, c); b = KS_R4(b, 7);           \
    } while (0)
#define KS_T4(v0, v1, v2, v3)                                                        \
    do {                                                                             \
        __m128i t0 = _mm_unpacklo_epi32(v0, v1), t1 = _mm_unpacklo_epi32(v2, v3);    \
        __m128i t2 = _mm_unpackhi_epi32(v0, v1), t3 = _mm_unpackhi_epi32(v2, v3);    \
        v0 = _mm_unpacklo_epi64(t0, t1); v1 = _mm_unpackhi_epi64(t0, t1);            \
        v2 = _mm_unpacklo_epi64(t2, t3); v3 = _mm_unpackhi_epi64(t2, t3);            \
    } while (0)

KS_SSE static void ks4(uint32_t ctr, const uint32_t s[16], uint8_t *out) {
    __m128i in_[16], x[16];
    for (int j = 0; j < 16; ++j) in_[j] = _mm_set1_epi32((int)s[j]);
    in_[12] = _mm_add_epi32(_mm_set1_epi32((int)ctr), _mm_set_epi32(3, 2, 1, 0));
    for (int j = 0; j < 16; ++j) x[j] = in_[j];
    for (int i = 0; i < 10; ++i) {
        KS_QR4(x[0], x[4], x[8], x[12]); KS_QR4(x[1], x[5], x[9], x[13]);
        KS_QR4(x[2], x[6], x[10], x[14]); KS_QR4(x[3], x[7], x[11], x[15]);
        KS_QR4(x[0], x[5], x[10], x[15]); KS_QR4(x[1], x[6], x[11], x[12]);
        KS_QR4(x[2], x[7], x[8], x[13]); KS_QR4(x[3], x[4], x[9], x[14]);
    }
    for (int j = 0; j < 16; ++j) x[j] = _mm_add_epi32(x[j], in_[j]);
    KS_T4(x[0], x[1], x[2], x[3]); KS_T4(x[4], x[5], x[6], x[7]);
    KS_T4(x[8], x[9], x[10], x[11]); KS_T4(x[12], x[13], x[14], x[15]);
    /* x[4g + k] is bytes 16g..16g+15 of block k. */
    for (int k = 0; k < 4; ++k)
        for (int g = 0; g < 4; ++g) _mm_storeu_si128((__m128i *)(out + 64 * k + 16 * g), x[4 * g + k]);
    ks_forget(x, sizeof x); ks_forget(in_, sizeof in_);
}

#define KS_R8(x, n) _mm256_or_si256(_mm256_slli_epi32((x), (n)), _mm256_srli_epi32((x), 32 - (n)))
#define KS_QR8(a, b, c, d)                                                                          \
    do {                                                                                            \
        a = _mm256_add_epi32(a, b); d = _mm256_xor_si256(d, a); d = _mm256_shuffle_epi8(d, rot16);  \
        c = _mm256_add_epi32(c, d); b = _mm256_xor_si256(b, c); b = KS_R8(b, 12);                   \
        a = _mm256_add_epi32(a, b); d = _mm256_xor_si256(d, a); d = _mm256_shuffle_epi8(d, rot8);   \
        c = _mm256_add_epi32(c, d); b = _mm256_xor_si256(b, c); b = KS_R8(b, 7);                    \
    } while (0)
#define KS_T8(v0, v1, v2, v3)                                                                       \
    do {                                                                                            \
        __m256i t0 = _mm256_unpacklo_epi32(v0, v1), t1 = _mm256_unpacklo_epi32(v2, v3);             \
        __m256i t2 = _mm256_unpackhi_epi32(v0, v1), t3 = _mm256_unpackhi_epi32(v2, v3);             \
        v0 = _mm256_unpacklo_epi64(t0, t1); v1 = _mm256_unpackhi_epi64(t0, t1);                     \
        v2 = _mm256_unpacklo_epi64(t2, t3); v3 = _mm256_unpackhi_epi64(t2, t3);                     \
    } while (0)

KS_AVX static void ks8(uint32_t ctr, const uint32_t s[16], uint8_t *out) {
    const __m256i rot16 = _mm256_set_epi8(13, 12, 15, 14, 9, 8, 11, 10, 5, 4, 7, 6, 1, 0, 3, 2,
                                          13, 12, 15, 14, 9, 8, 11, 10, 5, 4, 7, 6, 1, 0, 3, 2);
    const __m256i rot8 = _mm256_set_epi8(14, 13, 12, 15, 10, 9, 8, 11, 6, 5, 4, 7, 2, 1, 0, 3,
                                         14, 13, 12, 15, 10, 9, 8, 11, 6, 5, 4, 7, 2, 1, 0, 3);
    __m256i in_[16], x[16];
    for (int j = 0; j < 16; ++j) in_[j] = _mm256_set1_epi32((int)s[j]);
    in_[12] = _mm256_add_epi32(_mm256_set1_epi32((int)ctr), _mm256_set_epi32(7, 6, 5, 4, 3, 2, 1, 0));
    for (int j = 0; j < 16; ++j) x[j] = in_[j];
    for (int i = 0; i < 10; ++i) {
        KS_QR8(x[0], x[4], x[8], x[12]); KS_QR8(x[1], x[5], x[9], x[13]);
        KS_QR8(x[2], x[6], x[10], x[14]); KS_QR8(x[3], x[7], x[11], x[15]);
        KS_QR8(x[0], x[5], x[10], x[15]); KS_QR8(x[1], x[6], x[11], x[12]);
        KS_QR8(x[2], x[7], x[8], x[13]); KS_QR8(x[3], x[4], x[9], x[14]);
    }
    for (int j = 0; j < 16; ++j) x[j] = _mm256_add_epi32(x[j], in_[j]);
    KS_T8(x[0], x[1], x[2], x[3]); KS_T8(x[4], x[5], x[6], x[7]);
    KS_T8(x[8], x[9], x[10], x[11]); KS_T8(x[12], x[13], x[14], x[15]);
    /* x[4g + k] holds bytes 16g..16g+15 of block k in its low half and of block k + 4 in its
     * high half: two of them side by side are 32 bytes of one block. */
    for (int k = 0; k < 4; ++k) {
        uint8_t *b = out + 64 * k;
        _mm256_storeu_si256((__m256i *)b, _mm256_permute2x128_si256(x[k], x[4 + k], 0x20));
        _mm256_storeu_si256((__m256i *)(b + 32), _mm256_permute2x128_si256(x[8 + k], x[12 + k], 0x20));
        _mm256_storeu_si256((__m256i *)(b + 256), _mm256_permute2x128_si256(x[k], x[4 + k], 0x31));
        _mm256_storeu_si256((__m256i *)(b + 288), _mm256_permute2x128_si256(x[8 + k], x[12 + k], 0x31));
    }
    ks_forget(x, sizeof x); ks_forget(in_, sizeof in_);
}

/* One block, its four rows in four vectors: a column round on the rows as they stand, then the
 * rows turned so that the diagonals line up as columns, and turned back. */
KS_SSE void proven_sys_chacha_keystream1(const uint32_t state[16], uint8_t *out) {
    const __m128i s0 = _mm_loadu_si128((const __m128i *)state), s1 = _mm_loadu_si128((const __m128i *)(state + 4));
    const __m128i s2 = _mm_loadu_si128((const __m128i *)(state + 8)), s3 = _mm_loadu_si128((const __m128i *)(state + 12));
    __m128i a = s0, b = s1, c = s2, d = s3;
    for (int i = 0; i < 10; ++i) {
        KS_QR4(a, b, c, d);
        b = _mm_shuffle_epi32(b, 0x39); c = _mm_shuffle_epi32(c, 0x4e); d = _mm_shuffle_epi32(d, 0x93);
        KS_QR4(a, b, c, d);
        b = _mm_shuffle_epi32(b, 0x93); c = _mm_shuffle_epi32(c, 0x4e); d = _mm_shuffle_epi32(d, 0x39);
    }
    _mm_storeu_si128((__m128i *)out, _mm_add_epi32(a, s0));
    _mm_storeu_si128((__m128i *)(out + 16), _mm_add_epi32(b, s1));
    _mm_storeu_si128((__m128i *)(out + 32), _mm_add_epi32(c, s2));
    _mm_storeu_si128((__m128i *)(out + 48), _mm_add_epi32(d, s3));
}

void proven_sys_chacha_keystream(const uint32_t state[16], uint8_t *out, size_t blocks) {
    uint32_t ctr = state[12];
    const int w = proven_sys_chacha_keystream_width();
    for (; w >= 8 && blocks >= 8; blocks -= 8, ctr += 8, out += 512) ks8(ctr, state, out);
    for (; blocks >= 4; blocks -= 4, ctr += 4, out += 256) ks4(ctr, state, out);
}

#else

int proven_sys_chacha_keystream_width(void) { return 0; }

void proven_sys_chacha_keystream(const uint32_t state[16], uint8_t *out, size_t blocks) {
    (void)state; (void)out; (void)blocks;
}

void proven_sys_chacha_keystream1(const uint32_t state[16], uint8_t *out) {
    (void)state; (void)out;
}

#endif
