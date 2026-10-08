// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/cpu.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what this core can be told to do, and where it currently is.
 */

#ifndef ARCH_CPU_H
#define ARCH_CPU_H

#include <stdint.h>

/* what this core can be told to do, and where it currently is. */

#if defined(VELVETOS_ARCH_NONE)
/* an architecture that does nothing, for `make portable-check`. */
#  include "arch/none/cpu.h"
#elif defined(VELVETOS_ARCH_X86_64) || defined(__x86_64__)
#  include "arch/x86_64/cpu.h"
#else
#  error "arch/cpu.h: no implementation for this architecture"
#endif

#endif
