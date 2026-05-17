#ifndef PROVEN_PANIC_H
#define PROVEN_PANIC_H

/**
 * @file panic.h
 * @brief Global panic handler for deterministic failure paths.
 */

/**
 * @brief Handles terminal failures (e.g. OOM in _or_panic allocator variants).
 *
 * This function is weakly defined by default and calls `__builtin_trap()`.
 * Users can override it by defining `proven_panic_handler` in their own modules.
 *
 * @param msg A descriptive message of the failure.
 *
 * Production panic handlers should not return.
 * Test panic handlers may return only when intentionally verifying panic paths.
 * If a panic handler returns, *_or_panic result validity is not guaranteed.
 *
 * The default implementation does not return.
 */
void proven_panic_handler(const char *msg);

#endif // PROVEN_PANIC_H
