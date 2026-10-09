// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/rtc.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the cmos real time clock.
 */

#include <velvet/rtc.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <velvet/spinlock.h>
#include <stdbool.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

#define RTC_SECOND  0x00
#define RTC_MINUTE  0x02
#define RTC_HOUR    0x04
#define RTC_DAY     0x07
#define RTC_MONTH   0x08
#define RTC_YEAR    0x09
#define RTC_STATUS_A 0x0a
#define RTC_STATUS_B 0x0b

uint8_t rtc_from_bcd(uint8_t v)
{
    return (uint8_t)((v & 0x0f) + ((v >> 4) * 10));
}

void rtc_decode(struct rtc_time *t, uint8_t status_b)
{
    bool is_binary = status_b & 0x04;
    bool is_24h    = status_b & 0x02;

    /*
     * the pm flag rides in the top bit of the hour, and it has to come
     * out before any bcd conversion or it corrupts the digits
     */
    bool pm = !is_24h && (t->hour & 0x80);
    t->hour &= 0x7f;

    if (!is_binary) {
        t->second = rtc_from_bcd(t->second);
        t->minute = rtc_from_bcd(t->minute);
        t->hour   = rtc_from_bcd(t->hour);
        t->day    = rtc_from_bcd(t->day);
        t->month  = rtc_from_bcd(t->month);
        t->year   = rtc_from_bcd((uint8_t)t->year);
    }

    if (!is_24h) {
        if (pm && t->hour != 12) {
            t->hour = (uint8_t)(t->hour + 12);
        } else if (!pm && t->hour == 12) {
            t->hour = 0;        /* 12am is hour zero, not hour twelve */
        }
    }

    /* two digits is all the chip stores, and the century register is not reliable. */
    t->year = (uint16_t)(t->year + 2000);
}

#ifndef VELVETOS_HOSTED

/*
 * the clock is read through an index port and a data port, and two
 * readers interleaving get a time neither of them asked for
 */
static struct spinlock rtc_lock = SPINLOCK("rtc", LOCK_RANK_CLOCK);

static uint8_t cmos_read(uint8_t reg)
{
    /* the top bit of the address port also gates NMIs. */
    outb(CMOS_ADDR, reg & 0x7f);
    return inb(CMOS_DATA);
}

static bool update_in_progress(void)
{
    return cmos_read(RTC_STATUS_A) & 0x80;
}

static void read_raw(struct rtc_time *t)
{
    t->second = cmos_read(RTC_SECOND);
    t->minute = cmos_read(RTC_MINUTE);
    t->hour   = cmos_read(RTC_HOUR);
    t->day    = cmos_read(RTC_DAY);
    t->month  = cmos_read(RTC_MONTH);
    t->year   = cmos_read(RTC_YEAR);
}

void rtc_read(struct rtc_time *out)
{
    uint64_t flags = spin_lock_irq(&rtc_lock);

    struct rtc_time a, b;

    /* the chip updates itself once a second and the registers are inconsistent while it does. */
    /*
     * FIXME: neither of these waits is bounded, and both run with
     * interrupts off, because rtc_lock is taken with spin_lock_irq. the
     * update bit is set for a couple of milliseconds in every second, so
     * one read can hold the boot core that long with the timer unable to
     * arrive, and a clock that never clears the bit does not slow this
     * machine down, it stops it. every other wait in the tree carries a
     * limit; do the same here and fall back on the comparison below,
     * which is the part that actually catches a torn read.
     */
    do {
        while (update_in_progress()) { }
        read_raw(&a);
        while (update_in_progress()) { }
        read_raw(&b);
    } while (a.second != b.second || a.minute != b.minute || a.hour != b.hour
             || a.day != b.day || a.month != b.month || a.year != b.year);

    rtc_decode(&a, cmos_read(RTC_STATUS_B));

    *out = a;
    spin_unlock_irq(&rtc_lock, flags);
}

#endif /* VELVETOS_HOSTED */
