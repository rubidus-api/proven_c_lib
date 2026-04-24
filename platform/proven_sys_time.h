#ifndef PROVEN_PLATFORM_SYS_TIME_H
#define PROVEN_PLATFORM_SYS_TIME_H

#include <stddef.h>

/**
 * @file proven_sys_time.h
 * @brief Platform Abstraction Layer for Time syscalls.
 */

typedef struct {
    int year;
    int month;
    int day;
    int hour;
    int min;
    int sec;
    int ms;
} proven_sys_datetime_t;

/**
 * @brief Get high-resolution monotonic time (nanoseconds).
 */
unsigned long long proven_sys_time_now_ns(void);

/**
 * @brief Get the current local wall-clock time broken down.
 */
void proven_sys_time_now_local(proven_sys_datetime_t *out_dt);

/**
 * @brief Sleep the current thread.
 */
void proven_sys_time_sleep_ms(unsigned int ms);

#endif /* PROVEN_PLATFORM_SYS_TIME_H */
