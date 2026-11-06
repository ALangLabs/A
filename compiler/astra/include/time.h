#ifndef _ASTRA_TIME_H
#define _ASTRA_TIME_H

#include <stddef.h>

typedef long time_t;
typedef long clock_t;

#define CLOCKS_PER_SEC 1000000L
#define TIME_UTC 1

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};

time_t time(time_t *t);
clock_t clock(void);
double difftime(time_t t1, time_t t0);
time_t mktime(struct tm *tm);
struct tm *localtime(const time_t *t);
struct tm *gmtime(const time_t *t);
struct tm *localtime_r(const time_t *t, struct tm *result);
struct tm *gmtime_r(const time_t *t, struct tm *result);
size_t strftime(char *s, size_t max, const char *format, const struct tm *tm);

#endif /* _ASTRA_TIME_H */
