/*
 * The freestanding runtime contract, as a link (B-034).
 *
 * A freestanding build of proven needs from its environment exactly what GCC and Clang require of
 * any freestanding program: memcpy, memmove, memset and memcmp - the compiler may emit calls to
 * them for copies and zeroing whatever the source says - plus the compiler's own support library
 * (libgcc: __aeabi_* division and soft-float helpers and the like). Nothing from a C runtime:
 * no stdio, no malloc, no strlen, no startup files.
 *
 * `./nob cross` links this file with every freestanding object of the library using -nostdlib
 * -nostartfiles and -lgcc, into a static executable with this entry point. A static link fails
 * on any unresolved symbol, so a successful link IS the evidence: the four functions below are
 * the only runtime services the library uses. It is never run - there is no board here - which
 * is why it is a link check and says so.
 */
#include "proven.h"

/* The four required runtime services, minimal and obviously correct. A real target supplies its
 * own (newlib, picolibc, a vendor HAL); these exist so the link proves nothing else is needed. */
void *memcpy(void *d, const void *s, __SIZE_TYPE__ n) {
    unsigned char *dp = d; const unsigned char *sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}
void *memmove(void *d, const void *s, __SIZE_TYPE__ n) {
    unsigned char *dp = d; const unsigned char *sp = s;
    if (dp < sp) { while (n--) *dp++ = *sp++; }
    else { dp += n; sp += n; while (n--) *--dp = *--sp; }
    return d;
}
void *memset(void *d, int c, __SIZE_TYPE__ n) {
    unsigned char *dp = d;
    while (n--) *dp++ = (unsigned char)c;
    return d;
}
int memcmp(const void *a, const void *b, __SIZE_TYPE__ n) {
    const unsigned char *x = a, *y = b;
    for (; n; --n, ++x, ++y) if (*x != *y) return *x - *y;
    return 0;
}

#if defined(__wasm__)
/* wasm32 only: the compiler lowers a 128-bit multiply to a call to __multi3, which lives in the
 * compiler's support library (compiler-rt builtins) - the counterpart of -lgcc on the other
 * freestanding targets. A host need not have that library installed for wasm32, so the link
 * supplies this one helper itself; a real wasm32 build links the builtins instead. It is written
 * with 64-bit arithmetic only, so that it cannot call itself. */
typedef union { unsigned __int128 all; struct { unsigned long long lo, hi; } part; } proven_nocrt_u128;

__int128 __multi3(__int128 a, __int128 b) {
    proven_nocrt_u128 x, y, r;
    x.all = (unsigned __int128)a;
    y.all = (unsigned __int128)b;
    const unsigned long long mask = 0xffffffffull;
    unsigned long long a0 = x.part.lo & mask, a1 = x.part.lo >> 32;
    unsigned long long b0 = y.part.lo & mask, b1 = y.part.lo >> 32;
    unsigned long long p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    unsigned long long mid = (p00 >> 32) + (p01 & mask) + (p10 & mask);
    r.part.lo = (p00 & mask) | (mid << 32);
    r.part.hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32) +
                x.part.lo * y.part.hi + x.part.hi * y.part.lo;
    return (__int128)r.all;
}
#endif

static proven_byte_t g_mem[4096];
volatile int g_result;

/* Touch the freestanding surface so the link has real references to follow: an arena, a string,
 * the formatter, the float formatter and parser, a hash, a view split. */
void proven_nocrt_entry(void) {
    proven_arena_t arena = proven_arena_create((proven_mem_mut_t){ g_mem, sizeof g_mem });
    proven_allocator_t a = proven_arena_as_allocator(&arena);
    proven_result_u8str_t s = proven_u8str_create(a, 64);
    if (proven_is_ok(s.err)) {
        (void)proven_u8str_append_fmt(&s.value, "{} {}", PROVEN_ARG(42), PROVEN_ARG((const char *)"x"));
        g_result += (int)s.value.internal.len;
    }
    char digits[64];
    proven_size_t w = 0;
    if (proven_is_ok(proven_float_format_f64_policy(digits, sizeof digits, 1.25, PROVEN_FLOAT_FORMAT_POLICY_DEFAULT,
                                                    proven_float_format_options_shortest(), &w))) g_result += (int)w;
    proven_u8str_view_split_t it = proven_u8str_view_split(PROVEN_LIT("a,b"), PROVEN_LIT(","));
    proven_u8str_view_t f;
    while (proven_u8str_view_split_next(&it, &f)) g_result += (int)f.size;
    g_result += (int)proven_crc32(proven_mem_view_from_u8(PROVEN_LIT("abc")));
    for (;;) { }
}
