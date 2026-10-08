// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/inline.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * an arch primitive has to *be* its instruction, where it is written.
 */

#ifndef ARCH_INLINE_H
#define ARCH_INLINE_H

/* an arch primitive has to *be* its instruction, where it is written. */
#define ARCH_INLINE static inline __attribute__((always_inline))

#endif
