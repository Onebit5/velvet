// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/pit.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the 8253/8254 programmable interval timer.
 */

#ifndef VELVET_PIT_H
#define VELVET_PIT_H

#include <stdint.h>

/* the 8253/8254 programmable interval timer. */

#define PIT_HZ 100      /* ticks per second, so one tick = 10ms */

void     pit_init(void);
uint64_t pit_ticks(void);
uint64_t pit_uptime_ms(void);

/* spin until n milliseconds have gone by, counting the ticks the timer interrupt delivers. */
void pit_busy_wait(uint64_t ms);

/*
 * the same wait, with no interrupt involved at all: channel 2 counts
 * down and says so through a bit on the keyboard controller's port,
 * which the kernel can simply read. this is how you measure one clock against
 * another before either of them is delivering anything.
 *
 * capped at 50ms, because channel 2 counts 16 bits at 1.193 MHz and
 * runs out a little past 54
 */
void pit_poll_wait(uint64_t ms);

/* stop the pit interrupting. */
void pit_stop(void);

/* the tick, from wherever it now comes. */
void pit_tick(void);

#endif
