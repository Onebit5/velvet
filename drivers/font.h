// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/font.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * 8x16 bitmap font, one byte per row, msb = leftmost pixel.
 */

#ifndef DRIVERS_FONT_H
#define DRIVERS_FONT_H

#include <stdint.h>

/* 8x16 bitmap font, one byte per row, msb = leftmost pixel. */

#define FONT_WIDTH  8
#define FONT_HEIGHT 16

extern const uint8_t console_font[256][16];

#endif
