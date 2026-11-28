#include "proven/bytes.h"
#include "proven/bit.h"

/**
 * @file bytes.c
 * @brief bytes.h's searches (RFC-0058).
 *
 * The single-byte searches look at eight bytes at a time where eight are left: a word whose
 * bytes equal to `b` become 0, and "has a zero byte" is the classic carry test. The word is
 * loaded byte by byte (proven_load_u64_le), so nothing past the view is read and no alignment
 * is assumed. The set searches test one byte at a time against the 256-bit set.
 */

#define ONES ((proven_u64)0x0101010101010101u)
#define HIGHS ((proven_u64)0x8080808080808080u)

/* The bits 0x80 of each byte of `x` that is zero; exact for the lowest such byte. */
static proven_u64 zero_bytes(proven_u64 x) { return (x - ONES) & ~x & HIGHS; }

proven_size_t proven_mem_find_byte(proven_mem_view_t v, proven_byte_t b) {
    if (!v.ptr) return PROVEN_INDEX_NOT_FOUND;
    const proven_u64 pat = ONES * b;
    proven_size_t i = 0;
    for (; v.size - i >= 8; i += 8) {
        const proven_u64 z = zero_bytes(proven_load_u64_le(v.ptr + i) ^ pat);
        if (z) return i + proven_bit_trailing_zeros_u64(z) / 8;
    }
    for (; i < v.size; i++)
        if (v.ptr[i] == b) return i;
    return PROVEN_INDEX_NOT_FOUND;
}

proven_size_t proven_mem_find_last_byte(proven_mem_view_t v, proven_byte_t b) {
    if (!v.ptr) return PROVEN_INDEX_NOT_FOUND;
    const proven_u64 pat = ONES * b;
    proven_size_t end = v.size;
    for (; end >= 8; end -= 8) {
        if (zero_bytes(proven_load_u64_le(v.ptr + end - 8) ^ pat)) break;   /* a match in these eight */
    }
    while (end > 0) {
        end--;
        if (v.ptr[end] == b) return end;
    }
    return PROVEN_INDEX_NOT_FOUND;
}

proven_byteset_t proven_byteset_of(proven_mem_view_t v) {
    proven_byteset_t set = { { 0, 0, 0, 0 } };
    if (v.ptr)
        for (proven_size_t i = 0; i < v.size; i++) proven_byteset_add(&set, v.ptr[i]);
    return set;
}

proven_size_t proven_mem_span(proven_mem_view_t v, const proven_byteset_t *set) {
    if (!v.ptr || !set) return 0;
    proven_size_t i = 0;
    while (i < v.size && proven_byteset_has(set, v.ptr[i])) i++;
    return i;
}

proven_size_t proven_mem_span_not(proven_mem_view_t v, const proven_byteset_t *set) {
    if (!v.ptr) return 0;
    if (!set) return v.size;
    proven_size_t i = 0;
    while (i < v.size && !proven_byteset_has(set, v.ptr[i])) i++;
    return i;
}

proven_size_t proven_mem_find_any(proven_mem_view_t v, const proven_byteset_t *set) {
    if (!v.ptr || !set) return PROVEN_INDEX_NOT_FOUND;
    const proven_size_t i = proven_mem_span_not(v, set);
    return i < v.size ? i : PROVEN_INDEX_NOT_FOUND;
}

proven_size_t proven_mem_find_last_any(proven_mem_view_t v, const proven_byteset_t *set) {
    if (!v.ptr || !set) return PROVEN_INDEX_NOT_FOUND;
    for (proven_size_t i = v.size; i > 0; i--)
        if (proven_byteset_has(set, v.ptr[i - 1])) return i - 1;
    return PROVEN_INDEX_NOT_FOUND;
}
