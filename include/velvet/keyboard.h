// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/keyboard.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * ps/2 keyboard, scancode set 1, the kernel layout. interrupt driven.
 */

#ifndef VELVET_KEYBOARD_H
#define VELVET_KEYBOARD_H

#include <stdint.h>

/* ps/2 keyboard, scancode set 1, the kernel layout. */

void keyboard_init(void);

/*
 * the scancode state machine, split from the irq handler so it can be
 * fed synthetic bytes in host-side tests. the irq handler is just
 * keyboard_feed(inb(0x60))
 */
void keyboard_feed(uint8_t scancode);

#endif
