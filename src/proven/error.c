#include "proven/error.h"

/**
 * @file error.c
 * @brief proven_err_name and proven_err_message (RFC-0058): C's strerror for proven_err_t.
 *
 * One table, in the order of the enum. Its size is checked against PROVEN_ERR_LAST at compile
 * time, so a code added to types.h without a row here does not build.
 */

static const struct { const char *name, *message; } ERRS[] = {
    { "PROVEN_OK", "no error" },
    { "PROVEN_ERR_NOMEM", "out of memory" },
    { "PROVEN_ERR_OUT_OF_BOUNDS", "out of bounds: it does not fit" },
    { "PROVEN_ERR_INVALID_ENCODING", "invalid encoding" },
    { "PROVEN_ERR_INVALID_ARG", "invalid argument" },
    { "PROVEN_ERR_IO", "input or output failed" },
    { "PROVEN_ERR_NOT_FOUND", "not found" },
    { "PROVEN_ERR_INVALID_STATE", "not allowed in this state" },
    { "PROVEN_ERR_NEED_MORE", "more input is needed" },
    { "PROVEN_ERR_OVERFLOW", "the value does not fit" },
    { "PROVEN_ERR_UNSUPPORTED", "not supported" },
    { "PROVEN_ERR_AGAIN", "not now: try again" },
    { "PROVEN_ERR_EOF", "end of input" },
    { "PROVEN_ERR_BUSY", "busy" },
    { "PROVEN_ERR_PERMISSION", "permission denied" },
    { "PROVEN_ERR_INVALID_FORMAT", "invalid format" },
    { "PROVEN_ERR_EXISTS", "already exists" },
    { "PROVEN_ERR_TIMEOUT", "timed out" },
    { "PROVEN_ERR_REFUSED", "connection refused" },
    { "PROVEN_ERR_RESET", "connection reset" },
    { "PROVEN_ERR_UNREACHABLE", "unreachable" },
    { "PROVEN_ERR_UNTRUSTED", "not trusted" },
    { "PROVEN_ERR_EXPIRED", "expired" },
    { "PROVEN_ERR_NOT_YET_VALID", "not yet valid" },
    { "PROVEN_ERR_NAME_MISMATCH", "the name does not match" },
    { "PROVEN_ERR_PROTOCOL", "protocol error" },
    { "PROVEN_ERR_CANCELLED", "cancelled" },
};

_Static_assert(sizeof ERRS / sizeof ERRS[0] == (unsigned)PROVEN_ERR_LAST + 1,
               "every proven_err_t code needs a row in error.c's table");

const char *proven_err_name(proven_err_t err) {
    const unsigned i = (unsigned)err;
    return i <= (unsigned)PROVEN_ERR_LAST ? ERRS[i].name : "PROVEN_ERR_UNKNOWN";
}

const char *proven_err_message(proven_err_t err) {
    const unsigned i = (unsigned)err;
    return i <= (unsigned)PROVEN_ERR_LAST ? ERRS[i].message : "unknown error";
}
