#ifndef PROVEN_TIME_H
#define PROVEN_TIME_H

#include "types.h"
#include "error.h"

/**
 * @brief High-precision timestamp in nanoseconds since UNIX epoch.
 */
typedef proven_i64 proven_time_t;

/**
 * @brief Human-readable broken-down time.
 */
typedef struct {
    proven_i32 year;
    proven_u8  month;
    proven_u8  day;
    proven_u8  hour;
    proven_u8  min;
    proven_u8  sec;
    proven_u32 ms;
} proven_datetime_t;

/**
 * @brief Get the current high-precision timestamp in nanoseconds.
 */
[[nodiscard]]
proven_time_t proven_time_now(void);

/**
 * @brief Convert nanoseconds since epoch to broken-down UTC time.
 */
[[nodiscard]]
proven_datetime_t proven_time_breakdown(proven_time_t time_ns);

/**
 * @brief Get the current local time as broken-down structure.
 */
[[nodiscard]]
proven_datetime_t proven_time_now_datetime(void);

/**
 * @brief Sleep for a specified duration in milliseconds.
 */
void proven_time_sleep(proven_u32 ms);

#endif /* PROVEN_TIME_H */
