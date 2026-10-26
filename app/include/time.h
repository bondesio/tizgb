/* glibc struct tm layout (identical on 32-bit ARM and x86-64 apart from the
   width of long, which the compiler supplies). time_t is long on both. */
#ifndef TIZGB_TIME_H
#define TIZGB_TIME_H
typedef long time_t;
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};
time_t time(time_t *);
struct tm *localtime_r(const time_t *, struct tm *);
#endif
