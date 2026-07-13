#include "proven_sys_time.h"

#if defined(_WIN32) || defined(_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

unsigned long long proven_sys_time_now_ns(void) {
    // Windows: Use FileTime to match Unix Epoch
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER ull;
    ull.LowPart = ft.dwLowDateTime;
    ull.HighPart = ft.dwHighDateTime;
    // 100-ns intervals since 1601 to ns since 1970
    return (ull.QuadPart - 116444736000000000ULL) * 100;
}

void proven_sys_time_now_local(proven_sys_datetime_t *out_dt) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    out_dt->year = st.wYear;
    out_dt->month = st.wMonth;
    out_dt->day = st.wDay;
    out_dt->hour = st.wHour;
    out_dt->min = st.wMinute;
    out_dt->sec = st.wSecond;
    out_dt->ms = st.wMilliseconds;
}

void proven_sys_time_sleep_ms(unsigned int ms) {
    Sleep(ms);
}

int proven_sys_time_format_int_u16(unsigned short *buf, int buf_cap, int val, int pad_zeros) {
    if (buf_cap < 16) return 0; // Prevent overflow
    wchar_t fmt_str[8] = L"%d";
    if (pad_zeros > 0 && pad_zeros < 10) {
        fmt_str[1] = L'0';
        fmt_str[2] = (wchar_t)(L'0' + pad_zeros);
        fmt_str[3] = L'd';
        fmt_str[4] = L'\0';
    }
    return wsprintfW((wchar_t*)buf, fmt_str, val);
}
#else
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

unsigned long long proven_sys_time_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec;
}

void proven_sys_time_now_local(proven_sys_datetime_t *out_dt) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_val;
    localtime_r(&tv.tv_sec, &tm_val);
    
    out_dt->year = tm_val.tm_year + 1900;
    out_dt->month = tm_val.tm_mon + 1;
    out_dt->day = tm_val.tm_mday;
    out_dt->hour = tm_val.tm_hour;
    out_dt->min = tm_val.tm_min;
    out_dt->sec = tm_val.tm_sec;
    out_dt->ms = (int)(tv.tv_usec / 1000);
}

void proven_sys_time_sleep_ms(unsigned int ms) {
    struct timespec req;
    req.tv_sec = (time_t)(ms / 1000);
    req.tv_nsec = (long)((ms % 1000) * 1000000UL);
    while (nanosleep(&req, &req) != 0) {
        /* retry if interrupted */
    }
}

int proven_sys_time_format_int_u16(unsigned short *buf, int buf_cap, int val, int pad_zeros) {
    // Basic manual conversion without locale dependencies for other OSes
    if (buf_cap < 16) return 0; // Prevent overflow safely
    
    unsigned short temp[32];
    int len = 0;
    int is_neg = 0;
    unsigned int uval;
    
    if (val < 0) {
        is_neg = 1;
        uval = (unsigned int)(-(long long)val); // Safe absolute value
    } else {
        uval = (unsigned int)val;
    }
    
    if (uval == 0) {
        temp[len++] = (unsigned short)'0';
    } else {
        while (uval > 0) {
            unsigned int digit = (unsigned int)'0' + (uval % 10);
            temp[len++] = (unsigned short)digit;
            uval /= 10;
        }
    }
    
    /* The sign counts toward the zero-padded width, exactly as printf's %0Nd and the u8
     * formatter do: "-044" for width 4, not "-0044". So the digit run is padded to one
     * fewer than pad_zeros when a sign will precede it, and the two encodings agree. */
    int digit_width = is_neg ? pad_zeros - 1 : pad_zeros;
    while (len < digit_width && len < 31) {
        temp[len++] = (unsigned short)'0';
    }

    if (is_neg && len < 31) {
        temp[len++] = (unsigned short)'-';
    }
    
    if (len >= buf_cap) return 0;
    
    int written = 0;
    while (written < len) {
        buf[written] = temp[len - 1 - written];
        written++;
    }
    
    // Null terminate optionally, but since wsprintfW does it, let's keep consistency but we just return length.
    return written;
}
#endif
