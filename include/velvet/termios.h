// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/termios.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * how a terminal treats what is typed at it.
 */

#ifndef VELVET_TERMIOS_H
#define VELVET_TERMIOS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/velvet/termios.h>

/*
 * the decisions of a terminal's line discipline, one byte at a time,
 * over the termios a program sets. nothing here knows about buffers,
 * screens or processes, so every rule is checked without a machine
 */

/*
 * what a terminal is when it is made, and what a shell expects: a line
 * at a time, echoed and editable, ctrl+c a signal, return a newline
 */
void term_default(struct termios *p_t);

enum term_action {
    TERM_KEEP,          /* into the line */
    TERM_ERASE,         /* the last one in the line taken back */
    TERM_WERASE,        /* and the word before the cursor */
    TERM_KILL,          /* and the whole line */
    TERM_DELIVER,       /* kept, and the line is complete */
    TERM_EOF,           /* the line complete as it is, nothing kept */
};

/* a byte as the input flags make it: \r as \n and so on, or -1 when dropped */
int term_translate(const struct termios *p_t, uint8_t u_c);

/*
 * the signal a byte is under ISIG, and 0 for one that is not: SIGINT,
 * SIGQUIT or SIGTSTP
 */
int term_signal(const struct termios *p_t, uint8_t u_c);

/* what a byte does to a line being typed, with ICANON on */
enum term_action term_input(const struct termios *p_t, uint8_t u_c);

/*
 * what appears for a byte as it is kept: itself, or a control character
 * as ^X, into p_out, at least two bytes. how many
 */
size_t term_echo(const struct termios *p_t, uint8_t u_c, char *p_out);

/* the columns one kept byte took, which is what an erase takes back */
size_t term_width(const struct termios *p_t, uint8_t u_c);

#endif
