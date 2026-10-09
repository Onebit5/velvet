// SPDX-License-Identifier: GPL-2.0-only
/*
 * lib/epoch.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * dates, and the leap-year arithmetic behind them.
 */

#include <velvet/epoch.h>
#include <velvet/pit.h>

#define SECONDS_PER_DAY 86400u

static const uint8_t month_days[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

bool epoch_is_leap(uint16_t year)
{
    /* every four years, except every hundred, except every four hundred. */
    if ((year % 400) == 0) { return true; }
    if ((year % 100) == 0) { return false; }
    return (year % 4) == 0;
}

uint8_t epoch_days_in_month(uint16_t year, uint8_t month)
{
    if (month < 1 || month > 12) {
        return 0;
    }
    if (month == 2 && epoch_is_leap(year)) {
        return 29;
    }
    return month_days[month - 1];
}

uint64_t epoch_from_date(const struct rtc_time *t)
{
    /* refused rather than corrected. */
    if (t->year < 1970 || t->month < 1 || t->month > 12) {
        return 0;
    }
    if (t->day < 1 || t->day > epoch_days_in_month(t->year, t->month)) {
        return 0;
    }
    if (t->hour > 23 || t->minute > 59 || t->second > 59) {
        /* 60 is a leap second and this machine does not have them. */
        return 0;
    }

    uint64_t days = 0;
    for (uint16_t y = 1970; y < t->year; y++) {
        days += epoch_is_leap(y) ? 366 : 365;
    }
    for (uint8_t m = 1; m < t->month; m++) {
        days += epoch_days_in_month(t->year, m);
    }
    days += (uint64_t)(t->day - 1);

    return days * SECONDS_PER_DAY
         + (uint64_t)t->hour * 3600
         + (uint64_t)t->minute * 60
         + t->second;
}

void epoch_to_date(uint64_t seconds, struct rtc_time *out)
{
    uint64_t days = seconds / SECONDS_PER_DAY;
    uint64_t rest = seconds % SECONDS_PER_DAY;

    out->hour   = (uint8_t)(rest / 3600);
    out->minute = (uint8_t)((rest % 3600) / 60);
    out->second = (uint8_t)(rest % 60);

    uint16_t year = 1970;
    for (;;) {
        uint64_t in_year = epoch_is_leap(year) ? 366 : 365;
        if (days < in_year) {
            break;
        }
        days -= in_year;
        year++;
    }
    out->year = year;

    uint8_t month = 1;
    for (;;) {
        uint8_t in_month = epoch_days_in_month(year, month);
        if (days < in_month) {
            break;
        }
        days -= in_month;
        month++;
    }
    out->month = month;
    out->day   = (uint8_t)(days + 1);
}



static uint64_t boot_epoch;

void epoch_start(uint64_t seconds_at_boot)
{
    boot_epoch = seconds_at_boot;
}

bool epoch_known(void)
{
    return boot_epoch != 0;
}

uint64_t epoch_now(void)
{
    if (boot_epoch == 0) {
        /* nobody has said when boot was. */
        return 0;
    }
    return boot_epoch + pit_uptime_ms() / 1000;
}
