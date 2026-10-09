// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/epoch.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a date, as a number.
 */

/* the design notes for epoch.h are in docs/subsystems/mm.rst */

#ifndef VELVET_EPOCH_H
#define VELVET_EPOCH_H

#include <stdint.h>
#include <stdbool.h>
#include <velvet/rtc.h>

/*
 * a date as a number. the cmos clock says a date, and a comparison needs
 * seconds since the epoch, so everything above this converts once and
 * never looks at a month again.
 *
 * it is its own file because it is arithmetic with traps in it, and the
 * traps can be checked without a machine: months differ in length and
 * february has two, a leap year is every four except every hundred
 * except every four hundred, the epoch is not the start of a year, and
 * the conversion has to agree with itself both ways. that last one is
 * the property worth having, and it is what the test spends its time on
 */

uint64_t epoch_from_date(const struct rtc_time *t);

void epoch_to_date(uint64_t seconds, struct rtc_time *out);

/*
 * whether `year` is a leap year, by the rule rather than by the
 * approximation everybody remembers
 */
bool epoch_is_leap(uint16_t year);

uint8_t epoch_days_in_month(uint16_t year, uint8_t month);

/* the cmos chip is read *once*, at boot, and never again. */
void epoch_start(uint64_t seconds_at_boot);

uint64_t epoch_now(void);

bool epoch_known(void);

#endif
