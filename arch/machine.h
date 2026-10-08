// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/machine.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the box, rather than the processor.
 */

#ifndef ARCH_MACHINE_H
#define ARCH_MACHINE_H

#include <stdbool.h>

/*
 * the box rather than the processor: the operations that are the board
 * around the instruction set and not the architecture at all. two
 * machines with the same instructions stop and start in entirely
 * different ways, and this split means a second architecture does not
 * have to pretend its power switch is part of its instruction set.
 */

#if defined(VELVETOS_ARCH_NONE)
/* an architecture that does nothing, for `make portable-check`. */
#  include "arch/none/machine.h"
#elif defined(VELVETOS_ARCH_X86_64) || defined(__x86_64__)
#  include "arch/x86_64/machine.h"
#else
#  error "arch/machine.h: no implementation for this architecture"
#endif

#endif
