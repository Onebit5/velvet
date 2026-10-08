// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/none/irq.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * interrupt masking, on a machine with no interrupts.
 */

#ifndef ARCH_NONE_IRQ_H
#define ARCH_NONE_IRQ_H

#include <stdint.h>
#include "arch/inline.h"

/*
 * interrupt masking, on a machine with no interrupts. see README.md,
 * this is an instrument rather than a port
 */

ARCH_INLINE uint64_t irq_save(void)
{
    return 0;
}
ARCH_INLINE void irq_restore(uint64_t flags)
{
    (void)flags;
}
ARCH_INLINE void irq_install(uint8_t line, void (*handler)(void))
{
    (void)line; (void)handler;
}
ARCH_INLINE void irq_enable(void)
{
}
ARCH_INLINE void irq_disable(void)
{
}

#endif
