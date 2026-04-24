#include "proven/time.h"
#include "../../platform/proven_sys_time.h"

proven_time_t proven_time_now(void) {
    return (proven_time_t)proven_sys_time_now_ns();
}

proven_datetime_t proven_time_breakdown(proven_time_t time_ns) {
    proven_i64 seconds = time_ns / 1000000000ULL;
    proven_u32 ms = (proven_u32)((time_ns % 1000000000ULL) / 1000000ULL);

    // Simple Civil Date algorithm (Howard Hinnant) 
    proven_i64 days = seconds / 86400;
    proven_i64 rem = seconds % 86400;

    proven_datetime_t dt;
    dt.hour = (proven_u8)(rem / 3600);
    dt.min = (proven_u8)((rem % 3600) / 60);
    dt.sec = (proven_u8)(rem % 60);
    dt.ms = ms;

    // Days since UNIX epoch (1970-01-01)
    // 719468 is days from 0000-03-01 to 1970-01-01
    proven_i64 z = days + 719468;
    proven_i64 era = (z >= 0 ? z : z - 146096) / 146097;
    proven_i64 doe = (z - era * 146097);
    proven_i64 yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    proven_i64 y = yoe + era * 400;
    proven_i64 doy = doe - (365*yoe + yoe/4 - yoe/100);
    proven_i64 mp = (5*doy + 2)/153;
    proven_i64 d = doy - (153*mp + 2)/5 + 1;
    proven_i64 m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);

    dt.year = (proven_i32)y;
    dt.month = (proven_u8)m;
    dt.day = (proven_u8)d;
    
    return dt;
}

proven_datetime_t proven_time_now_datetime(void) {
    proven_sys_datetime_t sdt;
    proven_sys_time_now_local(&sdt);
    
    proven_datetime_t dt;
    dt.year = (proven_i32)sdt.year;
    dt.month = (proven_u8)sdt.month;
    dt.day = (proven_u8)sdt.day;
    dt.hour = (proven_u8)sdt.hour;
    dt.min = (proven_u8)sdt.min;
    dt.sec = (proven_u8)sdt.sec;
    dt.ms = (proven_u32)sdt.ms;
    return dt;
}

void proven_time_sleep(proven_u32 ms) {
    proven_sys_time_sleep_ms((unsigned int)ms);
}
