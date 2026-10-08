// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/context.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what a thread is, as far as the hardware is concerned.
 */

#ifndef ARCH_CONTEXT_H
#define ARCH_CONTEXT_H

#include <stdint.h>

/*
 * what a thread is as far as the hardware is concerned: the three moments
 * where the scheduler has to involve a processor rather than a structure,
 * which are the three things every machine spells differently.
 */

#if defined(VELVETOS_ARCH_NONE)
/* an architecture that does nothing, for `make portable-check`. */
#  include "arch/none/context.h"
#elif defined(VELVETOS_ARCH_X86_64) || defined(__x86_64__)
#  include "arch/x86_64/context.h"
#else
#  error "arch/context.h: no implementation for this architecture"
#endif

#endif
