#ifndef PROVEN_ERROR_H
#define PROVEN_ERROR_H

/**
 * @file error.h
 * @brief Error handling primitives for the proven library.
 * Strictly avoids macros for control flow.
 */

/**
 * @brief Core error codes for the proven library.
 */
typedef enum {
    PROVEN_OK = 0,
    PROVEN_ERR_NOMEM,
    PROVEN_ERR_OUT_OF_BOUNDS,
    PROVEN_ERR_INVALID_ENCODING,
    PROVEN_ERR_INVALID_ARG,
    PROVEN_ERR_IO,
    PROVEN_ERR_NOT_FOUND
} proven_err_t;

/**
 * @brief Helper inline function to check if a result was successful.
 */
static inline int proven_is_ok(proven_err_t err) {
    return err == PROVEN_OK;
}

#define PROVEN_IS_OK(err) proven_is_ok(err)

#endif /* PROVEN_ERROR_H */
