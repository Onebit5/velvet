// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/input.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the keys a program can be handed that are not characters.
 */

#ifndef UAPI_VELVET_INPUT_H
#define UAPI_VELVET_INPUT_H

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

#endif
