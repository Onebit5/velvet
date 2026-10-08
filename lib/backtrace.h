// SPDX-License-Identifier: GPL-2.0-only
/*
 * lib/backtrace.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * walk the saved frame pointers and print who called whom.
 */

/* the design notes for backtrace.h are in docs/subsystems/mm.rst */

#ifndef LIB_BACKTRACE_H
#define LIB_BACKTRACE_H

#include <stdint.h>

/* walk the saved frame pointers and print who called whom. */
/*
 * named kbacktrace, not backtrace, because glibc has a backtrace() and
 * the host tests link against it, the kernel's was quietly being shadowed
 */
void kbacktrace(uint64_t rbp, uint64_t rip);

#endif
