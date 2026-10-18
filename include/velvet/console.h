// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/console.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * framebuffer text consoles. 8x16 font, scrolling, block cursor. handles
 * \n \r \b \t, everything else gets blitted as a glyph.
 */

#ifndef VELVET_CONSOLE_H
#define VELVET_CONSOLE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <velvet/philemon.h>

/*
 * framebuffer text consoles: four for whatever the system runs on them,
 * and a fifth the kernel keeps for its monitor, which no program may
 * take as its own
 */

#define VCONSOLE_COUNT   5
#define VCONSOLE_MONITOR 4

void console_init(const struct ph_framebuffer *fb);
bool console_ready(void);

/* write to whichever console the caller belongs to. */
void console_putchar(char c);
void console_write(const char *s);
void console_clear(void);
void console_set_colors(uint32_t fg, uint32_t bg);

/* how big the screen is, in characters and in pixels. */
void console_size(size_t *cols, size_t *rows, size_t *width, size_t *height);

/* put the cursor at a given cell, without writing anything. */
void console_move(size_t col, size_t row);

/* output goes to the console its *writer* belongs to, not to whichever is on the screen. */
void console_set_owner_hook(unsigned (*fn)(void));

unsigned console_active(void);

/* put a different one on the screen. */
void console_switch(unsigned n);

/* each console keeps more lines than fit on the screen. */
void console_scroll_back(int lines);
size_t console_scrollback_lines(void);

/*
 * the pointer lives here rather than in the mouse driver because what
 * it is *for* is the cells, and the cells are here. one mouse however
 * many consoles there are, so it follows whichever is on the screen.
 *
 * what it is good for on a text console is what it has been good for
 * since gpm in 1993: dragging over text to select it, and pressing the
 * middle button to have it typed back. that is the whole of it, and it
 * is genuinely useful, copying a path out of an `ls` and into a `cat`
 * without retyping it is the thing a pointer buys a terminal
 */
void console_pointer(size_t col, size_t row, uint8_t buttons,
                     uint8_t pressed, uint8_t released);

/* whatever was last selected, as text. */
size_t console_selection(char *out, size_t max);

bool console_pointer_visible(void);

#endif
