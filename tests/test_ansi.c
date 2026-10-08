// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_ansi.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for the escape-sequence parser.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "drivers/ansi.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

/*
 * feed a whole string and keep the last thing that was not NOTHING,
 * plus everything that got printed
 */
static struct ansi_event last;
static char printed[256];
static size_t printed_len;

static void feed(const char *s)
{
    struct ansi a;
    ansi_reset(&a);
    memset(&last, 0, sizeof last);
    printed_len = 0;
    printed[0] = '\0';

    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        struct ansi_event e = ansi_feed(&a, *p);
        if (e.what == ANSI_PRINT) {
            if (printed_len + 1 < sizeof printed) {
                printed[printed_len++] = e.ch;
                printed[printed_len] = '\0';
            }
        } else if (e.what != ANSI_NOTHING) {
            last = e;
        }
    }
}

int main(void)
{


    feed("hello");
    CHECK(strcmp(printed, "hello") == 0, "plain text is printed");
    CHECK(last.what == ANSI_NOTHING, "and nothing else happens");



    feed("\033[2J");
    CHECK(last.what == ANSI_CLEAR && last.a == 2, "clear the whole screen");
    CHECK(printed_len == 0,
          "and not one byte of the sequence reaches the screen, which "
          "is what the console did with an incomplete decoder");

    feed("\033[H");
    CHECK(last.what == ANSI_MOVE && last.a == 1 && last.b == 1,
          "a move with no parameters is the top left corner");

    feed("\033[10;5H");
    CHECK(last.what == ANSI_MOVE && last.a == 10 && last.b == 5,
          "and with them, the place they name");

    feed("\033[K");
    CHECK(last.what == ANSI_CLEAR_LINE && last.a == 0,
          "erase to the end of the line, which is the default");

    feed("\033[1;31m");
    CHECK(last.what == ANSI_COLOUR, "a colour");

    feed("\033[?25l");
    CHECK(last.what == ANSI_HIDE_CURSOR, "hide the cursor");
    feed("\033[?25h");
    CHECK(last.what == ANSI_SHOW_CURSOR, "and show it again");



    feed("\033[A");
    CHECK(last.what == ANSI_UP && last.a == 1,
          "up with no parameter is up *one*, an absent parameter takes "
          "the default the sequence defines, and treating it as zero "
          "makes `ESC [ A` move nowhere");

    feed("\033[0A");
    CHECK(last.what == ANSI_UP && last.a == 0,
          "while an explicit zero is a zero, which is a different "
          "statement about the same field");

    feed("\033[5A");
    CHECK(last.what == ANSI_UP && last.a == 5, "and a number is itself");

    feed("\033[1;H");
    CHECK(last.what == ANSI_MOVE && last.a == 1 && last.b == 1,
          "a parameter after a semicolon that was not given takes its "
          "default too, rather than inheriting the one before it");



    {
        struct ansi a;
        ansi_reset(&a);
        const char *seq = "\033[12;34H";
        struct ansi_event e;
        memset(&e, 0, sizeof e);
        for (const unsigned char *p = (const unsigned char *)seq; *p; p++) {
            struct ansi_event r = ansi_feed(&a, *p);
            if (r.what != ANSI_NOTHING) { e = r; }
        }
        CHECK(e.what == ANSI_MOVE && e.a == 12 && e.b == 34,
              "a sequence split across as many calls as it has bytes is "
              "the same sequence, which is what a program writing an "
              "escape and its body separately actually produces");
    }

    /* text either side of a sequence survives it */
    feed("ab\033[2Jcd");
    CHECK(strcmp(printed, "abcd") == 0,
          "text before and after a sequence is printed, and the sequence "
          "itself is not");



    feed("\033[99Z");
    CHECK(last.what == ANSI_NOTHING, "a sequence nobody implements does nothing");
    CHECK(printed_len == 0,
          "and is swallowed rather than printed, garbage on the screen "
          "is worse than a thing that did not happen");

    feed("\033X");
    CHECK(printed_len == 0, "and so is an escape followed by a stray letter");

    /* a runaway sequence must not eat the screen */
    {
        /* digits and semicolons, which are what a sequence is *made* of. */
        char big[64];
        big[0] = '\033'; big[1] = '[';
        for (int i = 0; i < 40; i++) {
            big[2 + i] = (i % 4 == 3) ? ';' : '9';
        }
        big[42] = 'X';
        big[43] = '\0';
        feed(big);
        CHECK(printed_len > 0,
              "an escape followed by a great deal of text gives up and "
              "starts printing again, without a bound it swallows "
              "everything and the screen stays blank while the program "
              "appears to be working");
    }

    /* a parameter nobody could mean is clamped, not wrapped */
    feed("\033[99999999999999H");
    CHECK(last.what == ANSI_MOVE && last.a <= 9999,
          "an absurd parameter is clamped rather than wrapped, a "
          "wrapped one becomes a small number that looks reasonable");

    /* an escape in the middle of a sequence restarts it */
    feed("\033[12\033[5A");
    CHECK(last.what == ANSI_UP && last.a == 5,
          "an escape part way through abandons what came before rather "
          "than joining the two");

    /* more parameters than there is room for */
    feed("\033[1;2;3;4;5;6;7m");
    CHECK(last.what == ANSI_COLOUR && last.a == 1,
          "a sequence with more parameters than fit keeps the first ones "
          "and drops the rest, rather than being abandoned, a colour "
          "with six parameters is a real thing");

    /* ESC 7 and ESC 8, which a vt100 program still sends */
    feed("\0337");
    CHECK(last.what == ANSI_SAVE, "ESC 7 saves the cursor");
    feed("\0338");
    CHECK(last.what == ANSI_RESTORE, "and ESC 8 puts it back");

    if (failures == 0) printf("all good\n");
    return failures;
}
