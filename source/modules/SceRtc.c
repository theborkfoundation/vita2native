/*
 * SceRtcUser: real-time clock.
 *
 * The Vita counts microseconds since 0001-01-01 00:00:00 UTC. Host time is
 * converted through the same epoch so that guest tick arithmetic (which is
 * plain 64-bit addition on our side) stays consistent.
 */

#include <psp2/rtc.h>
#include <string.h>
#include <time.h>

#include "module.h"

/* Microseconds between 0001-01-01 and 1970-01-01. */
#define VITA_EPOCH_OFFSET 62135596800000000LL
#define VITA_TICKS_PER_SECOND 1000000LL

static int is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static const int days_per_month[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

static int days_in_month(int year, int month)
{
    if (month < 1 || month > 12)
        return 0;
    if (month == 2 && is_leap_year(year))
        return 29;
    return days_per_month[month - 1];
}

static int date_time_to_tm(const SceDateTime *date_time, struct tm *tm)
{
    memset(tm, 0, sizeof(*tm));
    tm->tm_year = (int)date_time->year - 1900;
    tm->tm_mon  = (int)date_time->month - 1;
    tm->tm_mday = (int)date_time->day;
    tm->tm_hour = (int)date_time->hour;
    tm->tm_min  = (int)date_time->minute;
    tm->tm_sec  = (int)date_time->second;
    return 0;
}

static void tm_to_date_time(const struct tm *tm, SceDateTime *date_time)
{
    date_time->year      = (unsigned short)(tm->tm_year + 1900);
    date_time->month     = (unsigned short)(tm->tm_mon + 1);
    date_time->day       = (unsigned short)tm->tm_mday;
    date_time->hour      = (unsigned short)tm->tm_hour;
    date_time->minute    = (unsigned short)tm->tm_min;
    date_time->second    = (unsigned short)tm->tm_sec;
    date_time->microsecond = 0;
}

static int date_time_is_valid(const SceDateTime *t)
{
    if (t->year < 1 || t->month < 1 || t->month > 12)
        return 0;
    if (t->day < 1 || t->day > (unsigned short)days_in_month(t->year, t->month))
        return 0;
    if (t->hour > 23 || t->minute > 59 || t->second > 59)
        return 0;
    if (t->microsecond > 999999)
        return 0;
    return 1;
}

static SceInt64 date_time_to_tick(const SceDateTime *t)
{
    struct tm tm;
    time_t seconds;

    date_time_to_tm(t, &tm);
    seconds = timegm(&tm);
    return (SceInt64)seconds * VITA_TICKS_PER_SECOND + (SceInt64)t->microsecond +
           VITA_EPOCH_OFFSET;
}

static int tick_to_date_time(SceInt64 tick, SceDateTime *t)
{
    SceInt64 relative = tick - VITA_EPOCH_OFFSET;
    SceInt64 seconds = relative / VITA_TICKS_PER_SECOND;
    SceInt64 micros = relative % VITA_TICKS_PER_SECOND;
    struct tm tm;
    time_t host_seconds;

    /* C division truncates towards zero; normalise so the remainder is
       always in [0, 1000000). */
    if (micros < 0) {
        micros += VITA_TICKS_PER_SECOND;
        seconds -= 1;
    }

    host_seconds = (time_t)seconds;
    if (!gmtime_r(&host_seconds, &tm))
        return SCE_RTC_ERROR_INVALID_VALUE;

    tm_to_date_time(&tm, t);
    t->microsecond = (unsigned int)micros;
    return 0;
}

static SceInt64 host_now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return (SceInt64)ts.tv_sec * VITA_TICKS_PER_SECOND + ts.tv_nsec / 1000;
}

EXPORT(SceRtcUser, 0x811313B3, unsigned int, sceRtcGetTickResolution, void)
{
    return (unsigned int)VITA_TICKS_PER_SECOND;
}

EXPORT(SceRtcUser, 0x23F79274, int, sceRtcGetCurrentTick, SceRtcTick *tick)
{
    if (!tick)
        return SCE_RTC_ERROR_INVALID_POINTER;

    tick->tick = (SceUInt64)(host_now_us() + VITA_EPOCH_OFFSET);
    return 0;
}

EXPORT(SceRtcUser, 0xCDDD25FE, int, sceRtcGetCurrentNetworkTick, SceRtcTick *tick)
{
    return sceRtcGetCurrentTick(tick);
}

EXPORT(SceRtcUser, 0x70FDE8F1, int, sceRtcGetCurrentClock, SceDateTime *time, int time_zone)
{
    if (!time)
        return SCE_RTC_ERROR_INVALID_POINTER;

    if (time_zone != 0) {
        SceInt64 tick = host_now_us() + VITA_EPOCH_OFFSET;

        tick += (SceInt64)time_zone * 60 * VITA_TICKS_PER_SECOND;
        return tick_to_date_time(tick, time);
    }

    return tick_to_date_time(host_now_us() + VITA_EPOCH_OFFSET, time);
}

EXPORT(SceRtcUser, 0x0572EDDC, int, sceRtcGetCurrentClockLocalTime, SceDateTime *date_time)
{
    time_t now;
    struct tm tm;

    if (!date_time)
        return SCE_RTC_ERROR_INVALID_POINTER;

    now = time(NULL);
    localtime_r(&now, &tm);
    tm_to_date_time(&tm, date_time);
    return 0;
}

EXPORT(SceRtcUser, 0xF2B238E2, int, sceRtcGetTick, const SceDateTime *time, SceRtcTick *tick)
{
    if (!time || !tick)
        return SCE_RTC_ERROR_INVALID_POINTER;
    if (!date_time_is_valid(time))
        return SCE_RTC_ERROR_INVALID_VALUE;

    tick->tick = (SceUInt64)date_time_to_tick(time);
    return 0;
}

EXPORT(SceRtcUser, 0xCD89F464, int, sceRtcSetTick, SceDateTime *time, const SceRtcTick *tick)
{
    if (!time || !tick)
        return SCE_RTC_ERROR_INVALID_POINTER;

    return tick_to_date_time((SceInt64)tick->tick, time);
}

EXPORT(SceRtcUser, 0x8DE6FEB7, int, sceRtcGetTime_t, const SceDateTime *date_time, time_t *time)
{
    struct tm tm;

    if (!date_time || !time)
        return SCE_RTC_ERROR_INVALID_POINTER;

    date_time_to_tm(date_time, &tm);
    *time = timegm(&tm);
    return 0;
}

EXPORT(SceRtcUser, 0xC995DE02, int, sceRtcGetTime64_t, const SceDateTime *date_time,
       SceUInt64 *time)
{
    time_t seconds;

    if (!date_time || !time)
        return SCE_RTC_ERROR_INVALID_POINTER;

    sceRtcGetTime_t(date_time, &seconds);
    *time = (SceUInt64)seconds;
    return 0;
}

EXPORT(SceRtcUser, 0x3A332F81, int, sceRtcSetTime_t, SceDateTime *date_time, time_t time)
{
    struct tm tm;

    if (!date_time)
        return SCE_RTC_ERROR_INVALID_POINTER;

    if (!gmtime_r(&time, &tm))
        return SCE_RTC_ERROR_INVALID_VALUE;

    tm_to_date_time(&tm, date_time);
    return 0;
}

EXPORT(SceRtcUser, 0xA6C36B6A, int, sceRtcSetTime64_t, SceDateTime *date_time, SceUInt64 time)
{
    return sceRtcSetTime_t(date_time, (time_t)time);
}

EXPORT(SceRtcUser, 0xF17FD8B5, int, sceRtcIsLeapYear, int year)
{
    return is_leap_year(year);
}

EXPORT(SceRtcUser, 0x49EB4556, int, sceRtcGetDaysInMonth, int year, int month)
{
    if (month < 1 || month > 12)
        return SCE_RTC_ERROR_INVALID_MONTH;

    return days_in_month(year, month);
}

EXPORT(SceRtcUser, 0x2F3531EB, int, sceRtcGetDayOfWeek, int year, int month, int day)
{
    struct tm tm;

    if (month < 1 || month > 12)
        return SCE_RTC_ERROR_INVALID_MONTH;
    if (day < 1 || day > days_in_month(year, month))
        return SCE_RTC_ERROR_INVALID_DAY;

    memset(&tm, 0, sizeof(tm));
    tm.tm_year = year - 1900;
    tm.tm_mon  = month - 1;
    tm.tm_mday = day;
    mktime(&tm); /* normalises tm_wday, interpreted as local time */
    return tm.tm_wday;
}

EXPORT(SceRtcUser, 0xD7622935, int, sceRtcCheckValid, const SceDateTime *time)
{
    if (!time)
        return SCE_RTC_ERROR_INVALID_POINTER;
    if (time->year < 1 || time->year > 9999)
        return SCE_RTC_ERROR_INVALID_YEAR;
    if (time->month < 1 || time->month > 12)
        return SCE_RTC_ERROR_INVALID_MONTH;
    if (time->day < 1 || time->day > (unsigned short)days_in_month(time->year, time->month))
        return SCE_RTC_ERROR_INVALID_DAY;
    if (time->hour > 23)
        return SCE_RTC_ERROR_INVALID_HOUR;
    if (time->minute > 59)
        return SCE_RTC_ERROR_INVALID_MINUTE;
    if (time->second > 59)
        return SCE_RTC_ERROR_INVALID_SECOND;
    if (time->microsecond > 999999)
        return SCE_RTC_ERROR_INVALID_MICROSECOND;

    return 0;
}

EXPORT(SceRtcUser, 0xC7385158, int, sceRtcCompareTick, const SceRtcTick *tick1,
       const SceRtcTick *tick2)
{
    if (!tick1 || !tick2)
        return SCE_RTC_ERROR_INVALID_POINTER;

    if (tick1->tick < tick2->tick)
        return -1;
    if (tick1->tick > tick2->tick)
        return 1;
    return 0;
}

EXPORT(SceRtcUser, 0x4559E2DB, int, sceRtcTickAddTicks, SceRtcTick *dst, const SceRtcTick *src,
       SceLong64 count)
{
    if (!dst || !src)
        return SCE_RTC_ERROR_INVALID_POINTER;

    dst->tick = src->tick + (SceUInt64)count;
    return 0;
}

#define TICK_ADD_FACTOR(name, nid, type, factor)                                                   \
    EXPORT(SceRtcUser, nid, int, name, SceRtcTick *dst, const SceRtcTick *src, type count)        \
    {                                                                                              \
        if (!dst || !src)                                                                          \
            return SCE_RTC_ERROR_INVALID_POINTER;                                                  \
        dst->tick = src->tick + (SceUInt64)((SceInt64)count * (factor));                           \
        return 0;                                                                                  \
    }

TICK_ADD_FACTOR(sceRtcTickAddMicroseconds, 0xAE26D920, SceLong64, 1LL)
TICK_ADD_FACTOR(sceRtcTickAddSeconds, 0x979AFD79, SceLong64, 1000000LL)
TICK_ADD_FACTOR(sceRtcTickAddMinutes, 0x4C358871, SceLong64, 60LL * 1000000LL)
TICK_ADD_FACTOR(sceRtcTickAddHours, 0x6F193F55, int, 3600LL * 1000000LL)
TICK_ADD_FACTOR(sceRtcTickAddDays, 0x58DE3C70, int, 86400LL * 1000000LL)
TICK_ADD_FACTOR(sceRtcTickAddWeeks, 0xE713C640, int, 7LL * 86400LL * 1000000LL)

EXPORT(SceRtcUser, 0x6321B4AA, int, sceRtcTickAddMonths, SceRtcTick *dst, const SceRtcTick *src,
       int count)
{
    SceDateTime dt;
    int total_months;
    int status;

    if (!dst || !src)
        return SCE_RTC_ERROR_INVALID_POINTER;

    status = tick_to_date_time((SceInt64)src->tick, &dt);
    if (status < 0)
        return status;

    total_months = (int)dt.year * 12 + ((int)dt.month - 1) + count;
    dt.year  = (unsigned short)(total_months / 12);
    dt.month = (unsigned short)(total_months % 12 + 1);
    if (dt.year < 1)
        return SCE_RTC_ERROR_INVALID_YEAR;

    if (dt.day > days_in_month(dt.year, dt.month))
        dt.day = (unsigned short)days_in_month(dt.year, dt.month);

    dst->tick = (SceUInt64)date_time_to_tick(&dt);
    return 0;
}

EXPORT(SceRtcUser, 0xDF6C3E1B, int, sceRtcTickAddYears, SceRtcTick *dst, const SceRtcTick *src,
       int count)
{
    return sceRtcTickAddMonths(dst, src, count * 12);
}

EXPORT(SceRtcUser, 0x1282C436, int, sceRtcConvertUtcToLocalTime, const SceRtcTick *utc,
       SceRtcTick *local_time)
{
    SceDateTime dt;
    struct tm tm;
    time_t seconds;
    int status;

    if (!utc || !local_time)
        return SCE_RTC_ERROR_INVALID_POINTER;

    status = tick_to_date_time((SceInt64)utc->tick, &dt);
    if (status < 0)
        return status;

    date_time_to_tm(&dt, &tm);
    seconds = timegm(&tm);
    localtime_r(&seconds, &tm);
    tm_to_date_time(&tm, &dt);
    dt.microsecond = (unsigned int)((utc->tick - VITA_EPOCH_OFFSET) % VITA_TICKS_PER_SECOND);

    local_time->tick = (SceUInt64)date_time_to_tick(&dt);
    return 0;
}

EXPORT(SceRtcUser, 0x0A05E201, int, sceRtcConvertLocalTimeToUtc, const SceRtcTick *local_time,
       SceRtcTick *utc)
{
    SceDateTime dt;
    struct tm tm;
    time_t seconds;
    int status;

    if (!local_time || !utc)
        return SCE_RTC_ERROR_INVALID_POINTER;

    status = tick_to_date_time((SceInt64)local_time->tick, &dt);
    if (status < 0)
        return status;

    date_time_to_tm(&dt, &tm);
    seconds = mktime(&tm);
    gmtime_r(&seconds, &tm);
    tm_to_date_time(&tm, &dt);
    dt.microsecond = (unsigned int)((local_time->tick - VITA_EPOCH_OFFSET) % VITA_TICKS_PER_SECOND);

    utc->tick = (SceUInt64)date_time_to_tick(&dt);
    return 0;
}

DECLARE_LIBRARY(SceRtcUser, 0x6704d3c6);
