// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/serial.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the 16550 serial console.
 */

#include <velvet/serial.h>
#include <velvet/input.h>
#include <velvet/console.h>

/* the terminal, which is not a chip. */



/*
 * where the kernel is in an escape sequence: 0 = nowhere, 1 = saw ESC,
 * 2 = saw ESC[ and the next byte says which key, 3 = collecting the
 * digits of a `ESC [ n ~` sequence.
 *
 * terminals send special keys in two different shapes and there is no
 * getting away with knowing only one: the arrows are a single letter,
 * while page up and page down are a number followed by a tilde. home
 * and end are sent both ways depending on the terminal, so both are
 * accepted
 */
static int esc_state;
static int esc_number;

/* ctrl+backslash was pressed and the next byte says which console */
static bool want_console;

void serial_feed(uint8_t b)
{
    if (want_console) {
        want_console = false;
        if (b >= '1' && b <= '0' + VCONSOLE_COUNT) {
            input_push(KEY_CONSOLE_1 + (b - '1'));
        }
        return;
    }
    if (esc_state == 1) {
        /*
         * FIXME: the byte that is not a `[` is dropped on the floor.
         * pressing escape at a program over the wire, which is how
         * anybody leaves insert mode, costs the next character too: the
         * escape clears the state and the character after it is eaten by
         * this return. a terminal cannot tell a lone escape from the
         * start of a sequence, but it can refuse to lose whatever follows
         * one, so pass the byte on as an ordinary one, or time the
         * sequence out.
         */
        esc_state = (b == '[') ? 2 : 0;
        esc_number = 0;
        return;
    }
    if (esc_state == 2 || esc_state == 3) {
        if (b >= '0' && b <= '9') {
            esc_state = 3;
            if (esc_number >= 0) {
                esc_number = esc_number * 10 + (b - '0');
                if (esc_number > 99) {
                    /*
                     * nonsense, but the kernel is still inside a sequence, so
                     * keep swallowing until it ends rather than letting
                     * the rest of the digits out as text
                     */
                    esc_number = -1;
                }
            }
            return;
        }
        int was = esc_state;
        esc_state = 0;

        if (was == 3) {
            if (b != '~' || esc_number < 0) {
                return;             /* some other CSI sequence, not the kernel's */
            }
            switch (esc_number) {
            case 1: input_push(KEY_HOME);   return;
            case 3: input_push(KEY_DELETE); return;
            case 4: input_push(KEY_END);    return;
            case 5: input_push(KEY_PGUP);   return;
            case 6: input_push(KEY_PGDN);   return;
            default: return;
            }
        }

        switch (b) {
        case 'A': input_push(KEY_UP);    return;
        case 'B': input_push(KEY_DOWN);  return;
        case 'C': input_push(KEY_RIGHT); return;
        case 'D': input_push(KEY_LEFT);  return;
        case 'H': input_push(KEY_HOME);  return;
        case 'F': input_push(KEY_END);   return;
        default:  return;   /* some other CSI sequence, not the kernel's */
        }
    }

    switch (b) {
    case 0x1c:              /* ctrl+backslash: the next digit picks a console */
        /*
         * a serial line has no alt key and no function keys, so
         * switching needs a sequence of ordinary bytes. ctrl+\ is
         * chosen because nothing else here uses it and no shell binds
         * it, and `chvt` does the same thing for anyone who would
         * rather type a word
         */
        want_console = true;
        return;
    case 0x1b:              /* ESC: might be an arrow, wait and see */
        esc_state = 1;
        return;
    case '\r':              /* terminals send CR for enter, the kernel wants LF */
        input_push('\n');
        return;
    case 0x7f:              /* DEL is what most terminals send for backspace */
        input_push('\b');
        return;
    default:
        input_push(b);      /* ctrl codes included, ctrl+c is already 3 */
        return;
    }
}
