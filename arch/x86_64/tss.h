// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/tss.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the task state segment.
 */

#ifndef ARCH_X86_64_TSS_H
#define ARCH_X86_64_TSS_H

#include <stdint.h>
#include <stdbool.h>

/*
 * the task state segment. long mode threw out hardware task switching
 * and kept the tss anyway, for two things the kernel actually want:
 *
 *  - the IST, a set of known-good stacks the cpu switches to when
 *    certain exceptions fire, whether or not the current stack is
 *    usable. this is what makes stack overflow survivable: the guard
 *    page fault happens with rsp already inside the unmapped page, so
 *    the cpu cant push an exception frame, and without an IST that
 *    second failure becomes a double fault with nowhere to land and
 *    then a triple fault, which on real hardware is just a reboot.
 *
 *  - rsp0, the stack the cpu switches to on a ring 3 -> ring 0 trap.
 *    nothing uses it yet, but usermode will
 */

/* IST slots are numbered 1..7 in the idt (0 means "dont switch") */
#define IST_DOUBLE_FAULT 1

/* the boot core's, which is cpu 0 */
void tss_init(void);

/*
 * and every other core's. each gets its own task state segment with its
 * own stacks, two cores trapping in from ring 3 at the same moment
 * would otherwise land on the same stack and eat each other
 */
bool tss_init_cpu(unsigned cpu);

/*
 * where the cpu lands on a trap from ring 3, on whichever core is
 * asking. it follows the thread, so it changes on every switch
 */
void tss_set_rsp0(uint64_t rsp0);

/*
 * which core is asking, read straight out of the task register.
 *
 * every core loads a different tss selector, so the register the cpu
 * already holds *is* a core's name. it costs a couple of cycles and,
 * unlike anything kept in memory, needs nothing to have been set up
 * before it can be asked, which is what makes it usable this early
 */
unsigned tss_current_cpu(void);

#endif
