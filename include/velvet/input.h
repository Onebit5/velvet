// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/input.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * where every source of typing meets. the ps/2 keyboard pushes here, and
 * so does the serial port, so whoever reads neither knows nor cares
 * whether you are sitting at the machine or telnetted into its soul.
 */

#ifndef VELVET_INPUT_H
#define VELVET_INPUT_H

#include <stdint.h>
#include <stdbool.h>

/*
 * a key is a byte, or one of these, above any byte so the two cannot be
 * confused. the first few reach a program as the sequences a terminal
 * sends for them; the rest are the terminal's own and reach nobody
 */
#define KEY_UP     0x100
#define KEY_DOWN   0x101
#define KEY_LEFT   0x102
#define KEY_RIGHT  0x103
#define KEY_DELETE 0x104
#define KEY_HOME   0x105
#define KEY_END    0x106
#define KEY_PGUP   0x107
#define KEY_PGDN   0x108

/* alt+f1 through f5, and shift with the page keys. */
#define KEY_CONSOLE_1  0x110    /* .. 0x114 */
#define KEY_SCROLL_UP  0x120
#define KEY_SCROLL_DOWN 0x121

/*
 * called from irq handlers: a key typed at the console on the screen,
 * offered to the terminal first. wakes whoever is waiting
 */
void input_push(int key);

/* the next byte typed at a console, or -1 when there is none */
int input_take(unsigned u_console);

/* whether a byte is waiting there, without taking it */
bool input_waiting(unsigned u_console);

/* everything typed there and not yet read, thrown away */
void input_flush(unsigned u_console);

/*
 * sleep until a byte is waiting there, which costs nothing until an irq
 * wakes the thread: true, or false when a signal for the caller's
 * process ended the wait first
 */
bool input_wait(unsigned u_console);

#endif
