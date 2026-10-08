// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/mmu.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the three things the memory manager needs the *hardware* told.
 */

#ifndef ARCH_MMU_H
#define ARCH_MMU_H

#include <stdint.h>
#include <stdbool.h>

/* the three things the memory manager needs the hardware told. */

#if defined(VELVETOS_ARCH_NONE)
/* an architecture that does nothing, for `make portable-check`. */
#  include "arch/none/mmu.h"
#elif defined(VELVETOS_ARCH_X86_64) || defined(__x86_64__)
#  include "arch/x86_64/mmu.h"
#else
#  error "arch/mmu.h: no implementation for this architecture"
#endif

#endif
