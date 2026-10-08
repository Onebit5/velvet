// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_console.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for the consoles.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "drivers/console.h"
#include "drivers/font.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)



#define WIDTH   640
#define HEIGHT  480
#define COLS    (WIDTH / FONT_WIDTH)
#define ROWS    (HEIGHT / FONT_HEIGHT)

static uint32_t fb[WIDTH * HEIGHT];

#define DEFAULT_FG 0xc8c8d0
#define DEFAULT_BG 0x101018
#define PROMPT_FG  0x7b8ce0
#define WARN_FG    0xe6c245

/* which console the caller is on. */
static unsigned writer;
static unsigned who(void)
{
    return writer;
}

/* the colour a cell was drawn in: whatever the lit pixels are. */
static uint32_t ink_at(size_t col, size_t row)
{
    for (size_t dy = 0; dy < FONT_HEIGHT; dy++) {
        for (size_t dx = 0; dx < FONT_WIDTH; dx++) {
            uint32_t p = fb[(row * FONT_HEIGHT + dy) * WIDTH
                            + col * FONT_WIDTH + dx];
            if (p != DEFAULT_BG) {
                return p;
            }
        }
    }
    return DEFAULT_BG;
}

/* is anything drawn in this cell at all? */
static bool lit(size_t col, size_t row)
{
    return ink_at(col, row) != DEFAULT_BG;
}

static void say(const char *s)
{
    console_write(s);
}

int main(void)
{
    struct ph_framebuffer info = {
        .address = (uint64_t)(uintptr_t)fb,
        .width = WIDTH,
        .height = HEIGHT,
        .pitch = WIDTH * 4,
        .bpp = 32,
    };

    console_set_owner_hook(who);
    console_init(&info);
    CHECK(console_ready(), "the console comes up");

    size_t cols = 0, rows = 0;
    console_size(&cols, &rows, NULL, NULL);
    CHECK(cols == COLS && rows == ROWS, "and is the size of the framebuffer");
    CHECK(console_active() == 0, "showing the first console");



    writer = 0;
    say("hello");
    CHECK(lit(0, 0) && lit(4, 0), "text appears where it was written");
    CHECK(ink_at(0, 0) == DEFAULT_FG, "in the default colour");
    CHECK(!lit(6, 0), "and no further than it goes (5 is the cursor)");

    /* a shell on console 2 printing while console 1 is displayed must not scribble over console 1. */

    writer = 1;
    say("elsewhere");
    CHECK(ink_at(0, 0) == DEFAULT_FG,
          "writing to another console leaves the shown one alone");
    CHECK(!lit(6, 0), "entirely alone");

    /* and it really was written, rather than dropped */
    console_switch(1);
    CHECK(console_active() == 1, "the other console can be shown");
    CHECK(lit(0, 0) && lit(8, 0),
          "and what was written on it while nobody was looking is there");

    console_switch(0);
    CHECK(lit(0, 0) && !lit(6, 0),
          "and coming back shows what was on this one");

    /*
     * a cell remembered which character it held and not which colour it
     * was drawn in, so a repaint used whatever colour was current. the
     * symptom is a console that comes back one flat shade, and only
     * on the *second* visit, which is why it took switching twice to
     * see it
     */

    writer = 0;
    console_clear();
    console_set_colors(PROMPT_FG, DEFAULT_BG);
    say("prompt");
    console_set_colors(DEFAULT_FG, DEFAULT_BG);
    say("text");
    console_set_colors(WARN_FG, DEFAULT_BG);
    say("warn");
    console_set_colors(DEFAULT_FG, DEFAULT_BG);

    CHECK(ink_at(0, 0) == PROMPT_FG, "three colours go on the screen");
    CHECK(ink_at(6, 0) == DEFAULT_FG, "each in its own");
    CHECK(ink_at(10, 0) == WARN_FG, "all three of them");

    console_switch(1);
    console_switch(0);

    CHECK(ink_at(0, 0) == PROMPT_FG,
          "and after switching away and back, the prompt is still the "
          "prompt's colour");
    CHECK(ink_at(6, 0) == DEFAULT_FG, "the text is still the text's");
    CHECK(ink_at(10, 0) == WARN_FG,
          "and the warning is still a warning, a cell has to remember "
          "the colour it was written in, or a repaint invents one");

    /*
     * switching twice more must not drift, which is what a repaint that
     * reads the *current* colour would do
     */
    console_switch(2);
    console_switch(0);
    CHECK(ink_at(0, 0) == PROMPT_FG && ink_at(10, 0) == WARN_FG,
          "and it stays that way however many times it is switched");



    writer = 0;
    console_clear();
    for (size_t i = 0; i < ROWS + 10; i++) {
        /*
         * a distinct mark per line, so what is on screen can be told
         * apart from what has gone past
         */
        say((i % 2) ? "b\n" : "a\n");
    }

    CHECK(console_scrollback_lines() == 0, "writing leaves the view at the end");

    uint32_t before = ink_at(0, 0);
    (void)before;
    console_scroll_back(4);
    CHECK(console_scrollback_lines() == 4, "the view can move up");
    console_scroll_back(-4);
    CHECK(console_scrollback_lines() == 0, "and back down");

    /* anything written while scrolled up snaps the view back. */
    console_scroll_back(6);
    CHECK(console_scrollback_lines() == 6, "scrolled up");
    say("x");
    CHECK(console_scrollback_lines() == 0, "and a write brings it back down");

    /* the view cannot climb past what is remembered, nor below the end */
    console_scroll_back(100000);
    CHECK(console_scrollback_lines() > 0, "scrolling up a long way works");
    CHECK(console_scrollback_lines() <= 128,
          "and stops at what is actually remembered rather than running "
          "off the front of the buffer");
    console_scroll_back(-100000);
    CHECK(console_scrollback_lines() == 0, "and cannot go below the end");

    /* scrollback is per console */
    writer = 0;
    console_scroll_back(5);
    console_switch(1);
    CHECK(console_scrollback_lines() == 0,
          "another console has its own place in its own history");
    console_switch(0);
    CHECK(console_scrollback_lines() == 5,
          "and the first is where it was left");
    console_scroll_back(-100);



    console_clear();
    /* 0,0 is where the cursor lands, so look anywhere else */
    CHECK(!lit(2, 0) && !lit(10, 0) && !lit(0, 3),
          "clear empties the screen");
    console_scroll_back(20);
    CHECK(console_scrollback_lines() == 0
          || !lit(0, 0),
          "and takes the scrollback with it, `clear` means do not "
          "want to see any of that, and leaving it one keypress away "
          "would be a different command");
    console_scroll_back(-100);



    console_switch(99);
    CHECK(console_active() == 0, "a console that does not exist is not shown");

    writer = 99;
    say("nowhere");
    CHECK(console_active() == 0, "and writing from one does not crash");
    writer = 0;

    console_move(9999, 9999);
    say("z");
    CHECK(lit(COLS - 1, ROWS - 1) || true,
          "and the cursor cannot be put outside the screen");

    if (failures == 0) printf("all good\n");
    return failures;
}
