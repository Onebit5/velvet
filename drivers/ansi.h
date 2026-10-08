// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/ansi.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the control sequences a program written for a real terminal sends.
 */

#ifndef DRIVERS_ANSI_H
#define DRIVERS_ANSI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* the control sequences a program written for a real terminal sends. */

enum ansi_what {
    ANSI_NOTHING,       /* part of a sequence: nothing to do yet */
    ANSI_PRINT,         /* an ordinary character, in `ch` */

    ANSI_CLEAR,         /* erase the screen. `a`: 0 to end, 1 to start,
                         * 2 all of it */
    ANSI_CLEAR_LINE,    /* the same, for the line the cursor is on */
    ANSI_MOVE,          /* to row `a`, column `b`, counting from 1 */
    ANSI_UP,            /* by `a` rows, and the four that go with it */
    ANSI_DOWN,
    ANSI_RIGHT,
    ANSI_LEFT,
    ANSI_COLUMN,        /* to column `a` on this row */
    ANSI_COLOUR,        /* `a` is an sgr parameter: 0 resets, 30-37 set
                         * the foreground, 40-47 the background */
    ANSI_SAVE,          /* remember where the cursor is */
    ANSI_RESTORE,       /* and put it back */
    ANSI_HIDE_CURSOR,
    ANSI_SHOW_CURSOR
};

struct ansi_event {
    enum ansi_what what;
    char ch;            /* for ANSI_PRINT */
    int  a, b;          /* the parameters, already defaulted */
};

/* four is more than anything here uses. */
#define ANSI_PARAMS 4

struct ansi {
    enum { ANSI_GROUND, ANSI_ESC, ANSI_CSI } state;
    int    param[ANSI_PARAMS];
    int    count;       /* how many parameters have been seen */
    bool   any_digit;   /* whether the current parameter has any digits,
                         * which is how "0" is told from "not given" */
    bool   question;    /* a private sequence: ESC [ ? ... */
    size_t length;      /* bytes into the current sequence, so a
                         * runaway one can be abandoned */
};

void ansi_reset(struct ansi *a);

/* one byte in, one thing to do out */
struct ansi_event ansi_feed(struct ansi *a, uint8_t byte);

#endif
