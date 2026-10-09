// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/termios.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * how a terminal treats what is typed at it.
 */

#ifndef VELVET_TERMIOS_H
#define VELVET_TERMIOS_H

#include <stdbool.h>
#include <stdint.h>

/* how a terminal treats what is typed at it. */

struct term_mode {
    /* keys arrive one at a time rather than a line at a time */
    bool raw;

    /* what is typed appears on the screen. */
    bool echo;

    /* ctrl+c and ctrl+z are signals rather than bytes. */
    bool signals;
};

/*
 * what a terminal does by default: everything on, which is what a shell
 * wants and what every program has had until now
 */
#define TERM_COOKED ((struct term_mode){ .raw = false, .echo = true, \
                                         .signals = true })

/*
 * and what an editor asks for: keys as they come, nothing echoed, and
 * ctrl+c delivered as a byte
 */
#define TERM_RAW    ((struct term_mode){ .raw = true, .echo = false, \
                                         .signals = false })

enum term_action {
    TERM_KEEP,          /* an ordinary character: put it in the buffer */
    TERM_ERASE,         /* backspace: take one back */
    TERM_DELIVER,       /* the read is complete */
    TERM_END,           /* end of input: ctrl+d on an empty line */
    TERM_INTERRUPT,     /* ctrl+c, as a signal */
    TERM_SUSPEND,       /* ctrl+z, likewise */
    TERM_IGNORE         /* nothing at all */
};

/* what to do about `key`, given the mode and how much has been typed so far. */
enum term_action term_input(const struct term_mode *m, int key,
                            uint64_t so_far);

/* whether this key should appear on the screen. */
bool term_should_echo(const struct term_mode *m, int key);

#endif
