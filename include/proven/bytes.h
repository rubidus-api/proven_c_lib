#ifndef PROVEN_BYTES_H
#define PROVEN_BYTES_H

#include "proven/types.h"
#include "proven/memory.h"

/**
 * @file bytes.h
 * @brief Searching bytes, sets of bytes, and ASCII classes - C's memchr, strspn, strcspn,
 *        strpbrk and <ctype.h>, over views and without a locale (RFC-0058).
 *
 * Every search takes a proven_mem_view_t and never reads past its `size`; a NULL `ptr` with
 * size 0 is an empty view. A position is an index into the view, or PROVEN_INDEX_NOT_FOUND.
 * The ASCII classes are those of the "C" locale for the 128 ASCII values, and false for every
 * byte from 0x80 up: they classify bytes, not characters, and are the same everywhere. No state,
 * no allocation; freestanding.
 */

// -----------------------------------------------------------------------------
// ASCII classes and case (static inline)
// -----------------------------------------------------------------------------

/** @brief 'A'-'Z' or 'a'-'z'. */
[[nodiscard]] static inline bool proven_ascii_is_alpha(proven_byte_t c) { return (proven_byte_t)((c | 0x20) - 'a') < 26; }
/** @brief '0'-'9'. */
[[nodiscard]] static inline bool proven_ascii_is_digit(proven_byte_t c) { return (proven_byte_t)(c - '0') < 10; }
/** @brief A letter or a digit. */
[[nodiscard]] static inline bool proven_ascii_is_alnum(proven_byte_t c) { return proven_ascii_is_alpha(c) || proven_ascii_is_digit(c); }
/** @brief '0'-'9', 'a'-'f', 'A'-'F'. */
[[nodiscard]] static inline bool proven_ascii_is_xdigit(proven_byte_t c) { return proven_ascii_is_digit(c) || (proven_byte_t)((c | 0x20) - 'a') < 6; }
/** @brief 'A'-'Z'. */
[[nodiscard]] static inline bool proven_ascii_is_upper(proven_byte_t c) { return (proven_byte_t)(c - 'A') < 26; }
/** @brief 'a'-'z'. */
[[nodiscard]] static inline bool proven_ascii_is_lower(proven_byte_t c) { return (proven_byte_t)(c - 'a') < 26; }
/** @brief Space, tab, newline, vertical tab, form feed, carriage return. */
[[nodiscard]] static inline bool proven_ascii_is_space(proven_byte_t c) { return c == ' ' || (proven_byte_t)(c - '\t') < 5; }
/** @brief Space or tab. */
[[nodiscard]] static inline bool proven_ascii_is_blank(proven_byte_t c) { return c == ' ' || c == '\t'; }
/** @brief 0x00-0x1F and 0x7F. */
[[nodiscard]] static inline bool proven_ascii_is_cntrl(proven_byte_t c) { return c < 0x20 || c == 0x7f; }
/** @brief 0x20-0x7E: space and every visible character. */
[[nodiscard]] static inline bool proven_ascii_is_print(proven_byte_t c) { return (proven_byte_t)(c - 0x20) < 0x5f; }
/** @brief 0x21-0x7E: every visible character. */
[[nodiscard]] static inline bool proven_ascii_is_graph(proven_byte_t c) { return (proven_byte_t)(c - 0x21) < 0x5e; }
/** @brief A visible character that is not a letter or a digit. */
[[nodiscard]] static inline bool proven_ascii_is_punct(proven_byte_t c) { return proven_ascii_is_graph(c) && !proven_ascii_is_alnum(c); }

/** @brief 'A'-'Z' made 'a'-'z'; every other byte unchanged. */
[[nodiscard]] static inline proven_byte_t proven_ascii_lower(proven_byte_t c) {
    return proven_ascii_is_upper(c) ? (proven_byte_t)(c | 0x20) : c;
}
/** @brief 'a'-'z' made 'A'-'Z'; every other byte unchanged. */
[[nodiscard]] static inline proven_byte_t proven_ascii_upper(proven_byte_t c) {
    return proven_ascii_is_lower(c) ? (proven_byte_t)(c & 0xdf) : c;
}
/** @brief The value of `c` as a digit: '0'-'9' are 0-9, 'a'-'z' and 'A'-'Z' are 10-35; 255 for
 *         any other byte. A digit of base `b` is one whose value is below `b`. */
[[nodiscard]] static inline proven_u8 proven_ascii_digit_value(proven_byte_t c) {
    if (proven_ascii_is_digit(c)) return (proven_u8)(c - '0');
    if (proven_ascii_is_alpha(c)) return (proven_u8)((c | 0x20) - 'a' + 10);
    return 255;
}

// -----------------------------------------------------------------------------
// Searching a view
// -----------------------------------------------------------------------------

/** @brief The first `b` in `v` (memchr), or PROVEN_INDEX_NOT_FOUND. */
[[nodiscard]] proven_size_t proven_mem_find_byte(proven_mem_view_t v, proven_byte_t b);
/** @brief The last `b` in `v`, or PROVEN_INDEX_NOT_FOUND. */
[[nodiscard]] proven_size_t proven_mem_find_last_byte(proven_mem_view_t v, proven_byte_t b);

/** @brief A set of bytes: 256 bits. Zero-initialise it for the empty set. */
typedef struct {
    proven_u64 bits[4];
} proven_byteset_t;

/** @brief The set of the bytes in `v` (each byte of the view is a member). */
[[nodiscard]] proven_byteset_t proven_byteset_of(proven_mem_view_t v);
/** @brief `b` added to `*set`. */
static inline void proven_byteset_add(proven_byteset_t *set, proven_byte_t b) {
    set->bits[b >> 6] |= (proven_u64)1 << (b & 63);
}
/** @brief Whether `b` is in `set`. */
[[nodiscard]] static inline bool proven_byteset_has(const proven_byteset_t *set, proven_byte_t b) {
    return (set->bits[b >> 6] >> (b & 63)) & 1;
}

/** @brief How many bytes at the front of `v` are in `set` (strspn). */
[[nodiscard]] proven_size_t proven_mem_span(proven_mem_view_t v, const proven_byteset_t *set);
/** @brief How many bytes at the front of `v` are not in `set` (strcspn). */
[[nodiscard]] proven_size_t proven_mem_span_not(proven_mem_view_t v, const proven_byteset_t *set);
/** @brief The first byte of `v` that is in `set` (strpbrk), or PROVEN_INDEX_NOT_FOUND. */
[[nodiscard]] proven_size_t proven_mem_find_any(proven_mem_view_t v, const proven_byteset_t *set);
/** @brief The last byte of `v` that is in `set`, or PROVEN_INDEX_NOT_FOUND. */
[[nodiscard]] proven_size_t proven_mem_find_last_any(proven_mem_view_t v, const proven_byteset_t *set);

#endif /* PROVEN_BYTES_H */
