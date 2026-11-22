/*
 * Zratro OS - time.h
 * 时间函数头文件
 */

#ifndef _TIME_H
#define _TIME_H

#include "types.h"

/* 时间类型 */
typedef int64_t time_t;
typedef int64_t clock_t;

/* 时钟常量 */
#define CLOCKS_PER_SEC  100

/* 时间结构 */
struct tm {
    int tm_sec;     /* 秒 [0-60] */
    int tm_min;     /* 分 [0-59] */
    int tm_hour;    /* 时 [0-23] */
    int tm_mday;    /* 日 [1-31] */
    int tm_mon;     /* 月 [0-11] */
    int tm_year;    /* 年 - 1900 */
    int tm_wday;    /* 星期 [0-6] */
    int tm_yday;    /* 年内天数 [0-365] */
    int tm_isdst;   /* 夏令时标志 */
};

/* 时间函数 */
time_t time(time_t *tloc);
clock_t clock(void);

double difftime(time_t time1, time_t time0);
time_t mktime(struct tm *tm);
struct tm *gmtime(const time_t *timep);
struct tm *localtime(const time_t *timep);
char *asctime(const struct tm *tm);
char *ctime(const time_t *timep);
size_t strftime(char *s, size_t max, const char *format, const struct tm *tm);

/* 时钟休眠 */
int nanosleep(const void *req, void *rem);

#endif /* _TIME_H */
