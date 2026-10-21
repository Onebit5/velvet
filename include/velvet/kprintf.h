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

/*
 * a ring of everything printed, so quieting the screen doesnt actually
 * throw anything away. 32k is a few hundred lines, which covers boot
 * and then some
 */
#define KLOG_SIZE (32 * 1024)

/*
 * the newest `sz_size` bytes of the log, oldest first, copied into
 * `str_buf`. answers how many that was, and leaves the log as it was
 */
size_t klog_copy(char *str_buf, size_t sz_size);

/*
 * a capture: what one thread prints, held in a buffer instead of going to
 * the screen, the serial line and the log. it is how a report the kernel
 * writes for a person, with kprintf, reaches a program as text it can
 * read, write to a file or pipe somewhere else. `sz_len` counts what was
 * kept and `b_overflowed` says some of it did not fit
 */
struct kcapture {
    char  *str_buf;
    size_t sz_size;
    size_t sz_len;
    bool   b_overflowed;
};

/*
 * who is capturing right now, asked on every character. the answer is the
 * scheduler's, since only it knows which thread is printing, and NULL
 * means nobody: print as usual
 */
void kprintf_capture_source(struct kcapture *(*fn)(void));

#endif
