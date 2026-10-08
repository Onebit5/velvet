// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/tty.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * who the keyboard belongs to.
 */

#ifndef DRIVERS_TTY_H
#define DRIVERS_TTY_H

#include <stdbool.h>
#include <stdint.h>
#include "drivers/termios.h"

/* who the keyboard belongs to. */

/*
 * group 0 means the kernel shell, which is where the terminal goes back
 * to whenever a program finishes
 */
#define TTY_SHELL 0

/* every question below used to have one answer for the machine. */

/* the front of the terminal is a *group*, not a process. */
void tty_set_foreground(int pgid);
int  tty_foreground(void);

/*
 * a key arrived. returns true if the tty consumed it rather than
 * passing it on as a character, ctrl+c and ctrl+z aimed at a program
 * are requests, not bytes, and must not end up in anybody's buffer
 */
bool tty_intercept(int key);

/* did ctrl+z stop the foreground since the kernel lasts asked? */
bool tty_take_stopped(int *pgid);

/* may this process read the keyboard at all? */
bool tty_is_current(int pid);

/* which console the calling thread belongs to. */
unsigned tty_my_console(void);

/* read a line on behalf of a process, echoing it as it is typed. */
int64_t tty_read_line(int pid, char *buf, uint64_t len);

/*
 * a mode rather than two syscalls, which is what the roadmap asked for
 * and is the part worth getting right: two calls would mean every
 * program choosing between them at every read, and one that chose
 * wrong once would hang. a mode is set once, asked about, and
 * *restored*, which an editor must do before it exits or it leaves
 * the terminal unusable for whatever runs next.
 *
 * the mode belongs to the console rather than to the process. that is
 * how a terminal works everywhere and it is worth saying why: a program
 * that dies without restoring cooked mode leaves the terminal raw, and
 * the shell that comes back has to be able to put it right. a
 * per-process mode would vanish with the process and leave nothing to
 * repair
 */
void tty_set_mode(const struct term_mode *m);
struct term_mode tty_get_mode(void);

/* put it back to what a shell wants. */
void tty_restore_mode(void);

#endif
