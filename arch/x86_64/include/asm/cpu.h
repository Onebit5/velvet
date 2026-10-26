// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/include/asm/cpu.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * x86_64 cpu: the core count, the current core, and idling.
 */

#ifndef ARCH_X86_64_ASM_CPU_H
#define ARCH_X86_64_ASM_CPU_H

#include <stdint.h>
#include <stdbool.h>

#include <velvet/compiler.h>
#include <asm/smp.h>

/* what an x86_64 core can be told to do. included as <asm/cpu.h>. */

/*
 * how many cores this kernel will keep track of. a number rather than a
 * discovery, because the scheduler has an array of them and an array
 * wants a size at compile time.
 *
 * defined as the one smp.c already had rather than beside it: two
 * constants that must agree, with nothing making them, is a bug waiting
 * for somebody to raise one of them
 */
#define CPU_MAX SMP_MAX_CPUS

/* what uname calls the machine */
#define ARCH_MACHINE "x86_64"

/*
 * how many times a second the clock this architecture installs will
 * interrupt. the scheduler needs it to say how long a quantum is in
 * milliseconds, and that is the only thing above the line that cares,
 * which is why it is a number here rather than the name of a timer chip
 * in the portable half
 */
#define CLOCK_TICK_HZ SMP_TICK_HZ


#ifdef VELVET_HOSTED

ARCH_INLINE void cpu_relax(void)
{
}
ARCH_INLINE void cpu_idle(void)
{
}
ARCH_INLINE uint64_t cpu_frame_pointer(void)
{
    return (uint64_t)(uintptr_t)__builtin_frame_address(0);
}
ARCH_INLINE uint64_t cpu_stack_pointer(void)
{
    /*
     * near enough for a host test, which only ever wants an address that
     * is plausibly on this stack
     */
    return (uint64_t)(uintptr_t)__builtin_frame_address(0);
}

/*
 * on the host every pthread stands in for a core, and there is no apic
 * to ask which one this is. a thread-local counter is as good a name as
 * any, and unique for the same reason.
 *
 * this used to live inside spinlock.c behind its own #ifdef, which is
 * the shape of every arch leak: a portable file carrying two answers
 * because nowhere else was willing to hold them
 */
/* a host has no table of processors this kernel should describe. */
ARCH_INLINE void cpu_show(void)
{
}

ARCH_INLINE uint32_t cpu_id(void)
{
    static _Thread_local uint32_t id;
    static volatile uint32_t next;
    if (id == 0) {
        id = __atomic_add_fetch(&next, 1, __ATOMIC_SEQ_CST);
    }
    return id;
}

#else

/*
 * which core this is. asked on every lock and every context switch, so
 * it reads the task register rather than the local apic, the apic is
 * an uncached device access, thousands of times a second, to learn
 * something the processor already knows
 */
ARCH_INLINE uint32_t cpu_id(void)
{
    return smp_this_cpu();
}

/*
 * `pause`. it does not sleep and does not lower the clock, it tells
 * the core this is a spin, so it stops speculating a hundred iterations
 * ahead of a value that is about to change out from under it, and stops
 * fighting other cores for the cache line while it waits
 */
ARCH_INLINE void cpu_relax(void)
{
    asm volatile ("pause" : : : "memory");
}

/*
 * stop until something happens. an interrupt ends it, so interrupts had
 * better be on, with them off this is where the machine stops forever
 */
ARCH_INLINE void cpu_idle(void)
{
    asm volatile ("hlt" : : : "memory");
}

ARCH_INLINE uint64_t cpu_frame_pointer(void)
{
    uint64_t rbp;
    asm volatile ("mov %%rbp, %0" : "=r"(rbp));
    return rbp;
}

ARCH_INLINE uint64_t cpu_stack_pointer(void)
{
    uint64_t rsp;
    asm volatile ("mov %%rsp, %0" : "=r"(rsp));
    return rsp;
}

#endif

/*
 * and the deliberate end of the line. not inline and not hosted-away:
 * a host test that reached this really should stop too
 */
void cpu_stop(void) __attribute__((noreturn));

/*
 * the time stamp counter: cycles since the core came up, which nothing
 * outside the core can predict to the cycle. a source of disorder for the
 * random generator rather than a clock
 */
ARCH_INLINE uint64_t cpu_cycles(void)
{
    return __builtin_ia32_rdtsc();
}

#ifndef VELVET_HOSTED
/*
 * the processors: what they are, which the firmware listed, which woke
 * and which are taking work. what `cpus` prints and /proc/cpuinfo is
 */
void cpu_show(void);

/*
 * 64 bits from the core's own random number generator, rdrand: false
 * when it has none, or had none to give after a few tries
 */
bool cpu_random(uint64_t *p_out);
#else
ARCH_INLINE bool cpu_random(uint64_t *p_out)
{
    (void)p_out;
    return false;
}
#endif

#endif
