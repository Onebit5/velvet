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

/* where every source of typing meets. */

/* the ones a program can be handed by getkey are the abi */
#include <uapi/velvet/input.h>

/* and these never leave the kernel */
#define KEY_CTRL_C 0x03
#define KEY_CTRL_Z 0x1a

/* alt+f1 through f4, and shift with the page keys. */
#define KEY_CONSOLE_1  0x110    /* .. 0x113 */
#define KEY_SCROLL_UP  0x120
#define KEY_SCROLL_DOWN 0x121

/* called from irq handlers. wakes whoever is waiting */
void input_push(int key);

int  input_getchar(void);           /* next key, or -1 if nothing waiting */

/*
 * wait for a key. the calling thread sleeps on a waitq and costs
 * nothing until an irq wakes it, which is how a program can sit at a
 * prompt all day without burning a single cycle. -1 instead when the
 * caller's process is interrupted with no key waiting
 */
int  input_getchar_blocking(void);

#endif
