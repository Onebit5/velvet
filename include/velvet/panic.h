// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/panic.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * game over. prints to serial + console (if up) and parks the cpu with
 * interrupts off.
 */

/* the design notes for panic.h are in docs/subsystems/mm.rst */

#ifndef VELVET_PANIC_H
#define VELVET_PANIC_H

void panic(const char *fmt, ...)
    __attribute__((format(printf, 1, 2), noreturn));

#endif
