#ifndef PROVEN_ERROR_H
#define PROVEN_ERROR_H

#include "types.h"

/**
 * @file error.h
 * @brief Error handling primitives for the proven library.
 * Strictly avoids macros for control flow.
 */

/**
 * @brief Helper inline function to check if a result was successful.
 */
static inline int proven_is_ok(proven_err_t err) {
    return err == PROVEN_OK;
}

#define PROVEN_IS_OK(err) proven_is_ok(err)

/**
 * @brief C's strerror for proven_err_t (RFC-0058): the enum's own spelling ("PROVEN_ERR_TIMEOUT"),
 *        and a short English phrase for a reader ("timed out"). Static text: nothing to free, no
 *        locale. A code this version does not know is "PROVEN_ERR_UNKNOWN" / "unknown error".
 */
[[nodiscard]] const char *proven_err_name(proven_err_t err);
[[nodiscard]] const char *proven_err_message(proven_err_t err);

#endif /* PROVEN_ERROR_H */
