#ifndef PROVEN_PLATFORM_SYS_ENV_H
#define PROVEN_PLATFORM_SYS_ENV_H

#include <stdbool.h>
#include <stddef.h>

/**
 * @file proven_sys_env.h
 * @brief Platform Abstraction Layer for Environment Variables.
 */

/**
 * @brief Retrieves an environment variable.
 * @param name Null-terminated UTF-8 name of the environment variable.
 * @param out_buf Buffer to store the UTF-8 representation of the value.
 * @param buf_cap Capacity of the buffer.
 * @param out_len Pointer to store the number of bytes written (excluding null terminator).
 * @return true if the variable was found and fit into the buffer, false otherwise.
 */
[[nodiscard]]
bool proven_sys_env_get(const char *name, char *out_buf, size_t buf_cap, size_t *out_len);

#endif /* PROVEN_PLATFORM_SYS_ENV_H */
