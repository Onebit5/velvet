// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/irq.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * may interrupts happen right now.
 */

#ifndef ARCH_IRQ_H
#define ARCH_IRQ_H

/* may interrupts happen right now. */

#if defined(VELVETOS_ARCH_NONE)
/* an architecture that does nothing, for `make portable-check`. */
#  include "arch/none/irq.h"
#elif defined(VELVETOS_ARCH_X86_64) || defined(__x86_64__)
#  include "arch/x86_64/irq.h"
#else
#  error "arch/irq.h: no implementation for this architecture"
#endif

#endif
