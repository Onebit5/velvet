// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/rtc.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the cmos real time clock.
 */

#ifndef DRIVERS_RTC_H
#define DRIVERS_RTC_H

#include <stdint.h>

/* the cmos real time clock. */

struct rtc_time {
    uint8_t  second, minute, hour;
    uint8_t  day, month;
    uint16_t year;
};

void rtc_read(struct rtc_time *out);

/*
 * bcd -> binary. the chip may report either, and which one is a bit in
 * a status register
 */
uint8_t rtc_from_bcd(uint8_t v);

/*
 * turn the raw register values sitting in *t into a real time,
 * according to status register B (bit 2 = binary, bit 1 = 24 hour).
 * split from the io so the fiddly parts, bcd, the pm bit hiding in
 * the top of the hour, midnight being 12am, can be tested
 */
void rtc_decode(struct rtc_time *t, uint8_t status_b);

#endif
