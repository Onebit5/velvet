// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/termios.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * terminal modes, and what a key means.
 */

#include "drivers/termios.h"

#define CTRL_C 0x03
#define CTRL_D 0x04
#define CTRL_Z 0x1a

enum term_action term_input(const struct term_mode *m, int key,
                            uint64_t so_far)
{
    /* the signals come first, and only when the mode asks for them. */
    if (m->signals) {
        if (key == CTRL_C) {
            return TERM_INTERRUPT;
        }
        if (key == CTRL_Z) {
            return TERM_SUSPEND;
        }
    }

    if (m->raw) {
        /*
         * every key is a key. no line editing, no end-of-line, and
         * emphatically no backspace handling: an editor wants to know
         * that backspace was pressed, and a terminal that quietly
         * removed a character from a buffer the editor is not using
         * would be answering a question nobody asked
         */
        return TERM_KEEP;
    }

    if (key == '\n' || key == '\r') {
        return TERM_DELIVER;
    }
    if (key == '\b' || key == 0x7f) {
        return TERM_ERASE;
    }
    if (key == CTRL_D) {
        /*
         * on an empty line it is end of input; with something typed it
         * delivers what there is *without* a newline. that second
         * behaviour is why `cat` with no trailing newline works, and it
         * surprises everybody once
         */
        return (so_far == 0) ? TERM_END : TERM_DELIVER;
    }

    /* control characters that mean nothing here. */
    if (key < 0x20) {
        return TERM_IGNORE;
    }
    return TERM_KEEP;
}

bool term_should_echo(const struct term_mode *m, int key)
{
    if (!m->echo) {
        return false;
    }
    /*
     * a control character has no width on the screen, so echoing it
     * moves the cursor by nothing and leaves the line looking wrong
     * next time something is erased
     */
    return key >= 0x20 || key == '\n' || key == '\b' || key == '\t';
}
