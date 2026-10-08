// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_rtc.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the cmos clock's decoding: bcd or binary, 12 or 24 hour, and a pm flag
 * that hides in the top bit of the hour byte.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

/* the lock complains through these when something is wrong with it */
void panic(const char *fmt, ...)
{
    printf("PANIC: ");
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    exit(1);
}
void kprintf(const char *fmt, ...)
{
    (void)fmt;
}

#include <stdint.h>

#include "drivers/rtc.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

/* status register B: bit 2 set = binary, bit 1 set = 24 hour */
#define BCD_12   0x00
#define BCD_24   0x02
#define BIN_12   0x04
#define BIN_24   0x06

static void check(uint8_t status, uint8_t h, uint8_t mi, uint8_t sec,
                  uint8_t d, uint8_t mo, uint8_t y,
                  uint8_t want_h, uint8_t want_mi, uint16_t want_year,
                  const char *what)
{
    struct rtc_time t = { .second = sec, .minute = mi, .hour = h,
                          .day = d, .month = mo, .year = y };
    rtc_decode(&t, status);
    if (t.hour != want_h || t.minute != want_mi || t.year != want_year) {
        printf("FAIL %s: got %02u:%02u year %u, wanted %02u:%02u year %u\n",
               what, t.hour, t.minute, t.year, want_h, want_mi, want_year);
        failures++;
    }
}

/*
 * epoch.c came along with rtc.c: it holds both the date
 * conversion and the clock built on it, and the clock wants a timer.
 * this suite is about the chip, so the timer stands still
 */
uint64_t pit_uptime_ms(void)
{
    return 0;
}

int main(void)
{
    /* the conversion on its own */
    CHECK(rtc_from_bcd(0x00) == 0,  "bcd 00");
    CHECK(rtc_from_bcd(0x09) == 9,  "bcd 09");
    CHECK(rtc_from_bcd(0x10) == 10, "bcd 10, where naive code says 16");
    CHECK(rtc_from_bcd(0x23) == 23, "bcd 23");
    CHECK(rtc_from_bcd(0x59) == 59, "bcd 59");
    CHECK(rtc_from_bcd(0x99) == 99, "bcd 99");

    /* binary, 24 hour: nothing to do but the year */
    check(BIN_24, 13, 45, 30, 4, 8, 26,  13, 45, 2026, "binary 24h");

    /* bcd, 24 hour: the common case on real hardware */
    check(BCD_24, 0x13, 0x45, 0x30, 0x04, 0x08, 0x26,  13, 45, 2026, "bcd 24h");

    /* 12 hour with the pm bit set in the top of the hour byte */
    check(BCD_12, 0x80 | 0x01, 0x30, 0, 1, 1, 0x26,  13, 30, 2026, "1pm is 13:00");
    check(BCD_12, 0x80 | 0x11, 0x00, 0, 1, 1, 0x26,  23, 0,  2026, "11pm is 23:00");

    /* the two that catch everyone */
    check(BCD_12, 0x80 | 0x12, 0x00, 0, 1, 1, 0x26,  12, 0, 2026,
          "12pm stays noon, not 24:00");
    check(BCD_12, 0x12, 0x00, 0, 1, 1, 0x26,        0, 0, 2026,
          "12am is hour zero, not hour twelve");

    /* am below noon passes through untouched */
    check(BCD_12, 0x09, 0x15, 0, 1, 1, 0x26,  9, 15, 2026, "9am is 09:00");

    /* binary 12 hour, because some chip somewhere does this */
    check(BIN_12, 0x80 | 3, 15, 0, 1, 1, 26,  15, 15, 2026, "binary 12h pm");

    /* the date fields survive too */
    {
        struct rtc_time t = { .second = 0, .minute = 0, .hour = 0,
                              .day = 0x31, .month = 0x12, .year = 0x99 };
        rtc_decode(&t, BCD_24);
        CHECK(t.day == 31 && t.month == 12 && t.year == 2099,
              "new year's eve 2099 decodes");
    }

    if (!failures) printf("all good\n");
    return failures;
}
