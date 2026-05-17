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

#ifdef PROVEN_FREESTANDING

static inline unsigned long long proven_sys_time_now_ns(void) {
    return 0;
}

static inline void proven_sys_time_now_local(proven_sys_datetime_t *out_dt) {
    if (out_dt) {
        out_dt->year = 1970;
        out_dt->month = 1;
        out_dt->day = 1;
        out_dt->hour = 0;
        out_dt->min = 0;
        out_dt->sec = 0;
        out_dt->ms = 0;
    }
}

static inline void proven_sys_time_sleep_ms(unsigned int ms) {
    (void)ms;
}

static inline int proven_sys_time_format_int_u16(unsigned short *buf, int buf_cap, int val, int pad_zeros) {
    (void)buf; (void)buf_cap; (void)val; (void)pad_zeros;
    return 0;
}

#else

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

/**
 * @brief Formats an integer to a u16 buffer (zero-padded). Returns characters written.
 */
int proven_sys_time_format_int_u16(unsigned short *buf, int buf_cap, int val, int pad_zeros);

#endif /* PROVEN_FREESTANDING */

#endif /* PROVEN_PLATFORM_SYS_TIME_H */
