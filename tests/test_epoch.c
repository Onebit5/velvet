// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_epoch.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for turning a date into a number.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include "lib/epoch.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

/*
 * the file holds two things: the conversion, which is pure arithmetic
 * and is what this suite is about, and the *clock*, which adds the
 * timer's uptime to a seed taken at boot. the second needs a timer and
 * the first does not, so the timer is supplied here, and set, so the
 * clock itself can be checked too
 */
static uint64_t fake_uptime;
uint64_t pit_uptime_ms(void)
{
    return fake_uptime;
}

static struct rtc_time date(int y, int mo, int d, int h, int mi, int s)
{
    struct rtc_time t;
    t.year = (uint16_t)y; t.month = (uint8_t)mo; t.day = (uint8_t)d;
    t.hour = (uint8_t)h; t.minute = (uint8_t)mi; t.second = (uint8_t)s;
    return t;
}

/* what the host makes of the same date. */
static uint64_t host_epoch(int y, int mo, int d, int h, int mi, int s)
{
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = mi;
    tm.tm_sec  = s;
    return (uint64_t)timegm(&tm);
}

int main(void)
{


    CHECK(epoch_is_leap(2024), "2024 is a leap year");
    CHECK(!epoch_is_leap(2023), "2023 is not");
    CHECK(!epoch_is_leap(1900),
          "1900 was *not* a leap year, divisible by a hundred, and a "
          "great deal of software written since believed otherwise");
    CHECK(epoch_is_leap(2000),
          "and 2000 was, being divisible by four hundred, which is the "
          "exception to the exception");
    CHECK(!epoch_is_leap(2100), "2100 will not be");

    CHECK(epoch_days_in_month(2024, 2) == 29, "february has 29 in a leap year");
    CHECK(epoch_days_in_month(2023, 2) == 28, "and 28 otherwise");
    CHECK(epoch_days_in_month(2023, 4) == 30, "april has 30");
    CHECK(epoch_days_in_month(2023, 12) == 31, "december has 31");
    CHECK(epoch_days_in_month(2023, 0) == 0, "and month zero is not a month");
    CHECK(epoch_days_in_month(2023, 13) == 0, "nor is the thirteenth");



    struct { int y, mo, d, h, mi, s; const char *what; } fixed[] = {
        { 1970, 1, 1, 0, 0, 0,   "the epoch itself" },
        { 1970, 1, 1, 0, 0, 1,   "one second after it" },
        { 1970, 1, 2, 0, 0, 0,   "the day after it" },
        { 2000, 1, 1, 0, 0, 0,   "the millennium" },
        { 2000, 2, 29, 12, 0, 0, "the leap day of 2000" },
        { 2024, 2, 29, 23, 59, 59, "and of 2024, at the last second" },
        { 2026, 8, 15, 10, 19, 0, "an ordinary afternoon" },
        { 2038, 1, 19, 3, 14, 7, "where a 32-bit time would have stopped" },
        { 2100, 3, 1, 0, 0, 0,   "the day after a february that had 28" },
    };

    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++) {
        struct rtc_time t = date(fixed[i].y, fixed[i].mo, fixed[i].d,
                                 fixed[i].h, fixed[i].mi, fixed[i].s);
        uint64_t mine = epoch_from_date(&t);
        uint64_t theirs = host_epoch(fixed[i].y, fixed[i].mo, fixed[i].d,
                                     fixed[i].h, fixed[i].mi, fixed[i].s);
        if (mine != theirs) {
            printf("FAIL: %s, %llu, and the host says %llu\n",
                   fixed[i].what, (unsigned long long)mine,
                   (unsigned long long)theirs);
            failures++;
        }
    }



    uint64_t checked = 0;
    for (uint16_t y = 1970; y <= 2030; y++) {
        for (uint8_t mo = 1; mo <= 12; mo++) {
            uint8_t last = epoch_days_in_month(y, mo);
            for (uint8_t d = 1; d <= last; d++) {
                struct rtc_time t = date(y, mo, d, 13, 45, 30);
                uint64_t secs = epoch_from_date(&t);

                struct rtc_time back;
                epoch_to_date(secs, &back);

                if (back.year != y || back.month != mo || back.day != d
                 || back.hour != 13 || back.minute != 45
                 || back.second != 30) {
                    printf("FAIL: %u-%02u-%02u does not survive the round "
                           "trip, came back as %u-%02u-%02u %02u:%02u:%02u\n",
                           y, mo, d, back.year, back.month, back.day,
                           back.hour, back.minute, back.second);
                    failures++;
                    goto done;
                }
                checked++;
            }
        }
    }
done:
    CHECK(checked > 22000,
          "every day of sixty years survives being turned into a number "
          "and back, which is the one property here that can be "
          "established by loop rather than argued about");

    /* and every day agrees with the host, too */
    for (uint16_t y = 1999; y <= 2001; y++) {
        for (uint8_t mo = 1; mo <= 12; mo++) {
            for (uint8_t d = 1; d <= epoch_days_in_month(y, mo); d++) {
                struct rtc_time t = date(y, mo, d, 0, 0, 0);
                if (epoch_from_date(&t) != host_epoch(y, mo, d, 0, 0, 0)) {
                    printf("FAIL: %u-%02u-%02u disagrees with the host\n",
                           y, mo, d);
                    failures++;
                    goto done2;
                }
            }
        }
    }
done2:



    {
        struct rtc_time t = date(2023, 2, 30, 0, 0, 0);
        CHECK(epoch_from_date(&t) == 0,
              "the 30th of february is refused rather than quietly "
              "becoming the 2nd of march, a file stamped with a date "
              "nobody chose is worse than one that fails to be stamped");
    }
    {
        struct rtc_time t = date(2023, 2, 29, 0, 0, 0);
        CHECK(epoch_from_date(&t) == 0, "and so is the 29th in a common year");
    }
    {
        struct rtc_time t = date(2024, 2, 29, 0, 0, 0);
        CHECK(epoch_from_date(&t) != 0, "while the 29th in a leap year is fine");
    }
    {
        struct rtc_time t = date(1969, 12, 31, 23, 59, 59);
        CHECK(epoch_from_date(&t) == 0,
              "a date before the epoch has no number here");
    }
    {
        struct rtc_time t = date(2023, 13, 1, 0, 0, 0);
        CHECK(epoch_from_date(&t) == 0, "month thirteen is not a month");
    }
    {
        struct rtc_time t = date(2023, 1, 1, 24, 0, 0);
        CHECK(epoch_from_date(&t) == 0, "and hour 24 is not an hour");
    }
    {
        struct rtc_time t = date(2023, 1, 1, 0, 0, 60);
        CHECK(epoch_from_date(&t) == 0,
              "nor is second 60, a leap second, which this machine does "
              "not have and the chip should never report");
    }

    /* the chip is read once at boot and the timer says how long ago that was. */
    CHECK(!epoch_known(), "before anything says so, the time is not known");
    CHECK(epoch_now() == 0,
          "and it answers 0 rather than a plausible number, one that "
          "cannot be told from a right one is worse than none");

    {
        struct rtc_time boot = date(2026, 8, 15, 12, 0, 0);
        uint64_t at_boot = epoch_from_date(&boot);

        fake_uptime = 0;
        epoch_start(at_boot);
        CHECK(epoch_known(), "once seeded it is known");
        CHECK(epoch_now() == at_boot, "and starts where the chip said");

        fake_uptime = 5000;
        CHECK(epoch_now() == at_boot + 5,
              "five seconds of running is five seconds later");

        fake_uptime = 3600 * 1000;
        CHECK(epoch_now() == at_boot + 3600, "and an hour is an hour");

        struct rtc_time later;
        epoch_to_date(epoch_now(), &later);
        CHECK(later.hour == 13 && later.day == 15,
              "which reads back as one o'clock on the same day");
    }

    if (failures == 0) printf("all good\n");
    return failures;
}
