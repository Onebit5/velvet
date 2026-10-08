// SPDX-License-Identifier: GPL-2.0-only
/*
 * lib/panic.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * game over. prints to serial + console (if up) and parks the cpu with
 * interrupts off.
 */

/* the design notes for panic.h are in docs/subsystems/mm.rst */

#ifndef LIB_PANIC_H
#define LIB_PANIC_H

void panic(const char *fmt, ...)
    __attribute__((format(printf, 1, 2), noreturn));

#endif
