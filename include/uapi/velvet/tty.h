// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/tty.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what a program can ask of the terminal it is attached to.
 */

#ifndef UAPI_VELVET_TTY_H
#define UAPI_VELVET_TTY_H

/*
 * the mode SYS_TTYMODE sets and hands back, three flags in one number.
 * raw is a key at a time with no line editing, echo is the terminal
 * printing what was typed, and signals is ctrl+c and ctrl+z arriving as
 * SIGINT and SIGTSTP rather than as the bytes 0x03 and 0x1a
 */
#define TTY_RAW      1
#define TTY_ECHO     2
#define TTY_SIGNALS  4

/* what a terminal is when nobody has asked for anything else */
#define TTY_COOKED  (TTY_ECHO | TTY_SIGNALS)

#endif
