// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/tty.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the terminals: one per console, and who each belongs to.
 */

#ifndef VELVET_TTY_H
#define VELVET_TTY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * a terminal per console, each with its own termios, its own line being
 * typed, and its own group in front. a session that takes one as its
 * controlling terminal decides who is in front of it, and ctrl+c reaches
 * that group and nobody else.
 *
 * group 0 in front means no program's: a terminal no session has taken
 */
#define TTY_NOBODY 0

/* how a terminal is numbered as a device: major 4, and the console + 1 */
#define TTY_MAJOR 4

struct file;

/*
 * a key arrived at the console on the screen. true when the terminal
 * took it rather than passing it on: switching or scrolling consoles,
 * or a signal, ctrl+c aimed at a program being a request, not a byte
 */
bool tty_intercept(int key);

/* console u_console's terminal opened for u_mode: one reference, or NULL */
struct file *tty_open(unsigned u_console, uint32_t u_mode);

/*
 * the caller's controlling terminal opened, as /dev/tty is: or NULL when
 * its session has none
 */
struct file *tty_open_own(uint32_t u_mode);

/* the group in front of a console's terminal, TTY_NOBODY for none */
int tty_foreground(unsigned u_console);

#endif
