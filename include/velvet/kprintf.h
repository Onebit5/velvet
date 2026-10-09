// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/kprintf.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * kernel printf. output goes to serial always, and to the framebuffer
 * console once its up. supported: %c %s %d %i %u %x %p %%, length mods
 * l/ll/z (all 64-bit here anyway), zero padding + width like %08x.
 */

/* the design notes for kprintf.h are in docs/subsystems/mm.rst */

#ifndef VELVET_KPRINTF_H
#define VELVET_KPRINTF_H

#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>

void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void kwrite(const char *s, size_t n);
void kvprintf(const char *fmt, va_list ap);

void kprintf_to_console(bool on);

void kprintf_serial_filter(bool (*fn)(void));

/* like kprintf, but never to the screen, the log and the serial line only. */
void klog_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void klog_dump(void);

#endif
