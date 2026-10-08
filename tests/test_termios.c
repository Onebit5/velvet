// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_termios.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for what a terminal does about a key.
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include "drivers/termios.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

int main(void)
{
    struct term_mode cooked = TERM_COOKED;
    struct term_mode raw    = TERM_RAW;



    CHECK(term_input(&cooked, 'a', 0) == TERM_KEEP, "a letter is kept");
    CHECK(term_input(&cooked, '\n', 5) == TERM_DELIVER, "enter delivers");
    CHECK(term_input(&cooked, '\r', 5) == TERM_DELIVER, "and so does return");
    CHECK(term_input(&cooked, '\b', 5) == TERM_ERASE, "backspace erases");
    CHECK(term_input(&cooked, 0x7f, 5) == TERM_ERASE,
          "and so does delete, which is what half the terminals in the "
          "world send for the same key");

    CHECK(term_input(&cooked, 0x03, 0) == TERM_INTERRUPT, "ctrl+c interrupts");
    CHECK(term_input(&cooked, 0x1a, 0) == TERM_SUSPEND, "ctrl+z suspends");

    CHECK(term_input(&cooked, 0x04, 0) == TERM_END,
          "ctrl+d on an empty line is end of input");
    CHECK(term_input(&cooked, 0x04, 3) == TERM_DELIVER,
          "and with something typed it delivers what there is, which "
          "is why `cat` with no trailing newline works, and surprises "
          "everybody once");

    CHECK(term_input(&cooked, 0x01, 0) == TERM_IGNORE,
          "a control character nobody uses is dropped rather than "
          "stored: a stray 0x01 in a line is a byte the program cannot "
          "see and cannot erase");



    CHECK(term_input(&raw, 'a', 0) == TERM_KEEP, "in raw, a letter is a key");
    CHECK(term_input(&raw, '\n', 5) == TERM_KEEP,
          "and so is enter, there are no lines in raw mode, which is "
          "the whole of what raw means");
    CHECK(term_input(&raw, '\b', 5) == TERM_KEEP,
          "and backspace is a key the editor wants to know about, not a "
          "character the terminal quietly removes from a buffer the "
          "editor is not using");
    CHECK(term_input(&raw, 0x01, 0) == TERM_KEEP,
          "and a control character is a key like any other");

    CHECK(term_input(&raw, 0x03, 0) == TERM_KEEP,
          "ctrl+c arrives as a byte, because TERM_RAW turns signals off "
          "-- an editor binding it to `cancel this command` is not "
          "asking to be killed by it");

    /*
     * the two are separate settings, and a program may ask for raw keys
     * while still wanting to be killable
     */
    struct term_mode raw_but_killable = { .raw = true, .echo = false,
                                          .signals = true };
    CHECK(term_input(&raw_but_killable, 0x03, 0) == TERM_INTERRUPT,
          "raw with signals still on delivers ctrl+c as a signal, which "
          "is why these are three flags rather than one mode word");
    CHECK(term_input(&raw_but_killable, 'a', 0) == TERM_KEEP,
          "while everything else is still a key");



    CHECK(term_should_echo(&cooked, 'a'), "cooked echoes what is typed");
    CHECK(!term_should_echo(&raw, 'a'),
          "raw does not, an editor draws the screen itself, and a "
          "terminal helping would draw over it");

    struct term_mode quiet = { .raw = false, .echo = false,
                               .signals = true };
    CHECK(term_input(&quiet, 'a', 0) == TERM_KEEP,
          "a password prompt keeps the character");
    CHECK(!term_should_echo(&quiet, 'a'),
          "and does not show it, which is a different question from "
          "whether to keep it, hence two functions");

    CHECK(!term_should_echo(&cooked, 0x01),
          "a control character is not echoed even in cooked mode: it has "
          "no width, so showing it moves the cursor by nothing and "
          "leaves the line wrong the next time something is erased");
    CHECK(term_should_echo(&cooked, '\n'), "but a newline is");
    CHECK(term_should_echo(&cooked, '\b'), "and so is backspace");

    if (failures == 0) printf("all good\n");
    return failures;
}
