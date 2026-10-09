// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/ansi.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the escape-sequence parser.
 */

#include "ansi.h"

/* how long a sequence may get before it is abandoned. */
#define ANSI_MAX 24

void ansi_reset(struct ansi *a)
{
    a->state     = ANSI_GROUND;
    a->count     = 0;
    a->any_digit = false;
    a->question  = false;
    a->length    = 0;
    for (int i = 0; i < ANSI_PARAMS; i++) {
        a->param[i] = 0;
    }
}

static struct ansi_event nothing(void)
{
    struct ansi_event e = { ANSI_NOTHING, 0, 0, 0 };
    return e;
}

static struct ansi_event print(char c)
{
    struct ansi_event e = { ANSI_PRINT, c, 0, 0 };
    return e;
}

static struct ansi_event action(struct ansi *a, enum ansi_what what,
                                int fallback_a, int fallback_b)
{
    struct ansi_event e;
    e.what = what;
    e.ch   = 0;

    /* a parameter that was not given is not zero. */
    e.a = (a->count >= 1 || a->any_digit) ? a->param[0] : fallback_a;
    e.b = (a->count >= 2) ? a->param[1] : fallback_b;

    ansi_reset(a);
    return e;
}

struct ansi_event ansi_feed(struct ansi *a, uint8_t byte)
{
    switch (a->state) {
    case ANSI_GROUND:
        if (byte == 0x1b) {
            ansi_reset(a);
            a->state = ANSI_ESC;
            return nothing();
        }
        return print((char)byte);

    case ANSI_ESC:
        if (byte == '[') {
            a->state = ANSI_CSI;
            return nothing();
        }
        if (byte == 0x1b) {
            return nothing();       /* another escape restarts it */
        }
        /* ESC followed by something else. */
        ansi_reset(a);
        if (byte == '7') {
            struct ansi_event e = { ANSI_SAVE, 0, 0, 0 };
            return e;
        }
        if (byte == '8') {
            struct ansi_event e = { ANSI_RESTORE, 0, 0, 0 };
            return e;
        }
        return nothing();

    case ANSI_CSI:
        if (++a->length > ANSI_MAX) {
            /* a sequence this long is not one. */
            ansi_reset(a);
            return print((char)byte);
        }

        if (byte == 0x1b) {
            /* an escape part way through a sequence abandons it and begins a new one. */
            ansi_reset(a);
            a->state = ANSI_ESC;
            return nothing();
        }

        if (byte == '?') {
            /*
             * a private sequence. the two worth having are the ones
             * that hide and show the cursor, which every editor sends
             */
            a->question = true;
            return nothing();
        }

        if (byte >= '0' && byte <= '9') {
            if (a->count < ANSI_PARAMS) {
                int v = a->param[a->count] * 10 + (byte - '0');
                /* a parameter nobody could mean. */
                a->param[a->count] = (v > 9999) ? 9999 : v;
            }
            a->any_digit = true;
            return nothing();
        }

        if (byte == ';') {
            if (a->count < ANSI_PARAMS) {
                a->count++;
            }
            /*
             * and a parameter that follows a semicolon starts empty
             * again, so `ESC [ 1 ; H` has a second parameter that was
             * not given rather than one that is one
             */
            a->any_digit = false;
            return nothing();
        }

        /* whatever this is, it ends the sequence */
        if (a->any_digit && a->count < ANSI_PARAMS) {
            a->count++;
        }

        if (a->question) {
            int which = a->param[0];
            ansi_reset(a);
            if (which == 25 && byte == 'l') {
                struct ansi_event e = { ANSI_HIDE_CURSOR, 0, 0, 0 };
                return e;
            }
            if (which == 25 && byte == 'h') {
                struct ansi_event e = { ANSI_SHOW_CURSOR, 0, 0, 0 };
                return e;
            }
            return nothing();
        }

        switch (byte) {
        case 'H':
        case 'f':   return action(a, ANSI_MOVE, 1, 1);
        case 'A':   return action(a, ANSI_UP, 1, 0);
        case 'B':   return action(a, ANSI_DOWN, 1, 0);
        case 'C':   return action(a, ANSI_RIGHT, 1, 0);
        case 'D':   return action(a, ANSI_LEFT, 1, 0);
        case 'G':   return action(a, ANSI_COLUMN, 1, 0);
        case 'J':   return action(a, ANSI_CLEAR, 0, 0);
        case 'K':   return action(a, ANSI_CLEAR_LINE, 0, 0);
        case 'm':   return action(a, ANSI_COLOUR, 0, 0);
        case 's':   return action(a, ANSI_SAVE, 0, 0);
        case 'u':   return action(a, ANSI_RESTORE, 0, 0);
        default:
            /* a sequence this console does not implement. */
            ansi_reset(a);
            return nothing();
        }
    }

    ansi_reset(a);
    return nothing();
}
