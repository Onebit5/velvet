// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/none/cpu.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * one core, which is not a core.
 */

#ifndef ARCH_NONE_CPU_H
#define ARCH_NONE_CPU_H

#include <stdint.h>
#include "arch/inline.h"

/*
 * one core, which is not a core. the number is 1 rather than something
 * roomier on purpose: a portable file that only works because the array
 * happens to be big enough is a portable file with a bug in it
 */
#define CPU_MAX 1

/*
 * how many times a second the clock this architecture installs will
 * interrupt. the scheduler needs it to say how long a quantum is in
 * milliseconds, and that is the only thing above the line that cares,
 * which is why it is a number here rather than the name of a timer chip
 * in the portable half
 */
#define CLOCK_TICK_HZ 100


ARCH_INLINE void cpu_relax(void)
{
}
ARCH_INLINE void cpu_idle(void)
{
}
ARCH_INLINE uint32_t cpu_id(void)
{
    return 0;
}

/*
 * zero rather than anything believable. a stub that handed back a
 * plausible stack pointer would hide the file that needed a real one
 */
ARCH_INLINE uint64_t cpu_frame_pointer(void)
{
    return 0;
}
ARCH_INLINE uint64_t cpu_stack_pointer(void)
{
    return 0;
}

void cpu_stop(void) __attribute__((noreturn));

#endif
