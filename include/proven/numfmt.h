#ifndef PROVEN_NUMFMT_H
#define PROVEN_NUMFMT_H

#include "proven/types.h"
#include "proven/memory.h"
#include "proven/u8str.h"

/**
 * @file numfmt.h
 * @brief Numbers written and read the way a place writes them - C's locale for numbers, as a
 *        value instead of global state (RFC-0058).
 *
 * C's setlocale changes how every printf and strtod of the process writes and reads a decimal
 * point, for code that never asked. Here the conventions are a value - the decimal mark, the
 * group separator, the group sizes and the minus sign - passed to the call that needs them, and
 * nothing else changes: `{}`, proven_parse_double_ascii and the rest stay locale-independent.
 * The digits are correctly rounded by the same engine as `{}`. Dates have their own value,
 * proven_time_locale_t (time.h).
 *
 * Writing refuses rather than truncates: PROVEN_ERR_OUT_OF_BOUNDS when `out` is too small, and
 * then nothing in it is to be used. Reading takes the sign, digits with group separators between
 * them in the integer part, and one decimal mark; no whitespace is skipped.
 */

typedef struct {
    proven_u8str_view_t decimal;   /**< the decimal mark: "." or "," */
    proven_u8str_view_t group;     /**< the group separator: "," "." a no-break space ...; empty for none */
    /** Digits per group, from the right: {3} is 1,234,567; {3, 2} is India's 12,34,567. A 0 ends
     *  the list and the last size repeats; {0} groups nothing. */
    proven_u8 grouping[4];
    proven_u8str_view_t minus;     /**< the minus sign: "-", or U+2212 */
} proven_numfmt_t;

/** @brief No grouping, "." and "-": what `{}` writes. */
extern const proven_numfmt_t proven_numfmt_c;
/** @brief 1,234,567.89 (English; also Korean, Japanese, Chinese). */
extern const proven_numfmt_t proven_numfmt_en;
/** @brief 1.234.567,89 (German, Spanish, Italian, ...). */
extern const proven_numfmt_t proven_numfmt_de;
/** @brief 1 234 567,89 with a narrow no-break space (U+202F; French). */
extern const proven_numfmt_t proven_numfmt_fr;
/** @brief 1'234'567.89 with a right single quotation mark (U+2019; Swiss). */
extern const proven_numfmt_t proven_numfmt_ch;
/** @brief 12,34,567.89 (Indian grouping). */
extern const proven_numfmt_t proven_numfmt_in;

/** @brief `v` written with `f`'s group separators into `out`; its length in `*written`. */
[[nodiscard]] proven_err_t proven_numfmt_u64(const proven_numfmt_t *f, proven_u64 v, proven_mem_mut_t out, proven_size_t *written);
[[nodiscard]] proven_err_t proven_numfmt_i64(const proven_numfmt_t *f, proven_i64 v, proven_mem_mut_t out, proven_size_t *written);
/**
 * @brief `v` with `decimals` digits after the mark (0-340), correctly rounded, never in
 *        scientific notation; infinity and NaN as "inf", "nan" with `f`'s minus.
 */
[[nodiscard]] proven_err_t proven_numfmt_f64(const proven_numfmt_t *f, double v, int decimals, proven_mem_mut_t out, proven_size_t *written);

/**
 * @brief An integer written with `f`'s conventions: `f`'s minus or '-', or '+'; digits, with
 *        group separators only between two digits.
 * @param used NULL: the whole view must be the number; else the bytes taken.
 * @return PROVEN_OK; PROVEN_ERR_INVALID_ARG when there is no number (or more than it, `used`
 *         NULL); PROVEN_ERR_OVERFLOW when it does not fit.
 */
[[nodiscard]] proven_err_t proven_numfmt_parse_i64(const proven_numfmt_t *f, proven_u8str_view_t s, proven_size_t *used, proven_i64 *out);
/** @brief A decimal number written with `f`'s conventions, correctly rounded to binary64; the
 *         same rules, and one decimal mark. At most 1,000 bytes. */
[[nodiscard]] proven_err_t proven_numfmt_parse_f64(const proven_numfmt_t *f, proven_u8str_view_t s, proven_size_t *used, double *out);

#endif /* PROVEN_NUMFMT_H */
