// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/input.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * where every source of typing meets. the ps/2 keyboard pushes here, and
 * so does the serial port, so the shell neither knows nor cares whether
 * you are sitting at the machine or telnetted into its soul.
 */

#ifndef DRIVERS_INPUT_H
#define DRIVERS_INPUT_H

#include <stdint.h>
#include <stdbool.h>

/* where every source of typing meets. */

/* keys that arent characters get values above 0xff so they cant be confused with one. */
#define KEY_UP     0x100
#define KEY_DOWN   0x101
#define KEY_LEFT   0x102
#define KEY_RIGHT  0x103
#define KEY_DELETE 0x104

/* the four an editor wants and a line editor never did. */
#define KEY_HOME   0x105
#define KEY_END    0x106
#define KEY_PGUP   0x107
#define KEY_PGDN   0x108

#define KEY_CTRL_C 0x03
#define KEY_CTRL_Z 0x1a

/* alt+f1 through f4, and shift with the page keys. */
#define KEY_CONSOLE_1  0x110    /* .. 0x113 */
#define KEY_SCROLL_UP  0x120
#define KEY_SCROLL_DOWN 0x121

/* called from irq handlers. wakes whoever is waiting */
void input_push(int key);

int  input_getchar(void);           /* next key, or -1 if nothing waiting */
bool input_haskey(void);

/* the next key without consuming it. */
int  input_peek(void);

/*
 * wait for a key. the calling thread sleeps on a waitq and costs
 * nothing until an irq wakes it, which is how the shell can sit at a
 * prompt all day without burning a single cycle
 */
int  input_getchar_blocking(void);

#endif
