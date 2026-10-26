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

/* ctrl+backslash was pressed and the next byte says which console */
static bool want_console;

/*
 * a byte from the far end of the wire. everything goes to the terminal
 * as it came, escape sequences and all, since a program reads the same
 * sequences from a terminal on the wire as from the console's keys. the
 * one exception is switching consoles: a serial line has no alt key and
 * no function keys, so ctrl+\ and a digit stand in for them
 */
void serial_feed(uint8_t b)
{
    if (want_console) {
        want_console = false;
        if (b >= '1' && b <= '0' + VCONSOLE_COUNT) {
            input_push(KEY_CONSOLE_1 + (b - '1'));
        }
        return;
    }
    if (b == 0x1c) {
        want_console = true;
        return;
    }
    input_push(b);
}
