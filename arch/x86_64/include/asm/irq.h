// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/include/asm/irq.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * interrupt masking on x86_64. included as <asm/irq.h>, which every
 * architecture provides, so nothing outside arch/ names this one.
 */

#ifndef ARCH_X86_64_ASM_IRQ_H
#define ARCH_X86_64_ASM_IRQ_H

#include <stdint.h>

#include <velvet/compiler.h>

/*
 * interrupt masking on x86_64. included as <asm/irq.h>, which every
 * architecture provides, so nothing outside arch/ names this one.
 */

/*
 * hook a device's handler onto one of the 16 irq lines and unmask it.
 * defined in interrupts.c, next to the dispatch that calls it
 */
#ifdef VELVET_HOSTED
ARCH_INLINE void irq_install(uint8_t line, void (*handler)(void))
{
    (void)line; (void)handler;
}
#else
void irq_install(uint8_t line, void (*handler)(void));
#endif

#ifdef VELVET_HOSTED

/*
 * host tests run in userspace, where `cli` gets you shot on sight, and
 * there is nothing to lock out anyway
 */
static inline uint64_t irq_save(void)
{
    return 0;
}
static inline void irq_restore(uint64_t flags)
{
    (void)flags;
}
ARCH_INLINE void irq_enable(void)
{
}
ARCH_INLINE void irq_disable(void)
{
}

#else

/*
 * save/restore rather than a blind disable/enable pair, so these nest.
 * the blind version cannot: an inner enable puts interrupts back on
 * while the outer caller still needed them off, and the window it opens
 * is exactly the one it was protecting
 */
static inline uint64_t irq_save(void)
{
    uint64_t flags;
    asm volatile ("pushfq\n\tpopq %0\n\tcli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags)
{
    if (flags & (1 << 9)) {     /* IF was set, so put it back */
        asm volatile ("sti" : : : "memory");
    }
}

/*
 * these two replaced bare `sti` and `cli` written in three files, so
 * they are the instruction rather than a call to it. save/restore above
 * are left as they have always been, a plain inline, out of line at
 * -O0, because they were never anything else and this version is not
 * supposed to change what it did not touch
 */
ARCH_INLINE void irq_enable(void)
{
    asm volatile ("sti" : : : "memory");
}

ARCH_INLINE void irq_disable(void)
{
    asm volatile ("cli" : : : "memory");
}

#endif

#endif
