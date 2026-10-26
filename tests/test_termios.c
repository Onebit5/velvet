// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_termios.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for what a terminal does about a byte.
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <velvet/termios.h>
#include <uapi/velvet/signal.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

#define CTRL(c) ((c) & 0x1f)

int main(void)
{
    struct termios t;
    char arr_echo[2];

    term_default(&t);
    CHECK((t.c_lflag & (ICANON | ECHO | ISIG)) == (ICANON | ECHO | ISIG),
          "a terminal starts a line at a time, echoed, ctrl+c a signal");
    CHECK(t.c_cc[VERASE] == 0x7f && t.c_cc[VEOF] == CTRL('D')
          && t.c_cc[VINTR] == CTRL('C') && t.c_cc[VMIN] == 1,
          "with the control characters every terminal has");

    CHECK(term_translate(&t, '\r') == '\n',
          "return arrives as a newline, ICRNL being on");
    CHECK(term_translate(&t, 'a') == 'a', "and a letter as itself");
    struct termios plain = t;
    plain.c_iflag &= ~(uint32_t)ICRNL;
    CHECK(term_translate(&plain, '\r') == '\r',
          "a program that turned ICRNL off is handed the return itself");
    plain.c_iflag |= IGNCR;
    CHECK(term_translate(&plain, '\r') == -1, "or nothing, with IGNCR");

    CHECK(term_input(&t, 'a') == TERM_KEEP, "a letter is kept");
    CHECK(term_input(&t, '\n') == TERM_DELIVER, "a newline delivers");
    CHECK(term_input(&t, 0x7f) == TERM_ERASE, "DEL erases, as VERASE");
    CHECK(term_input(&t, CTRL('U')) == TERM_KILL, "ctrl+u takes the line");
    CHECK(term_input(&t, CTRL('W')) == TERM_WERASE, "and ctrl+w a word");
    CHECK(term_input(&t, CTRL('D')) == TERM_EOF,
          "ctrl+d ends the line as it is, and an empty one is the end of "
          "input, which is why `cat` with no trailing newline works, and "
          "surprises everybody once");
    CHECK(term_input(&t, 0x01) == TERM_KEEP,
          "a control character that means nothing is kept like any other "
          "byte, for the program to make of what it will");

    struct termios moved = t;
    moved.c_cc[VERASE] = '\b';
    CHECK(term_input(&moved, '\b') == TERM_ERASE
          && term_input(&moved, 0x7f) == TERM_KEEP,
          "the erase character is whatever the program set it to");
    moved.c_cc[VKILL] = 0;
    CHECK(term_input(&moved, 0) == TERM_KEEP,
          "and one set to 0 is turned off, not a NUL that kills lines");

    CHECK(term_signal(&t, CTRL('C')) == SIGINT, "ctrl+c is SIGINT");
    CHECK(term_signal(&t, CTRL('Z')) == SIGTSTP, "ctrl+z is SIGTSTP");
    CHECK(term_signal(&t, CTRL('\\')) == SIGQUIT, "ctrl+\\ is SIGQUIT");
    CHECK(term_signal(&t, 'a') == 0, "and a letter is none");
    struct termios raw = t;
    raw.c_lflag &= ~(uint32_t)(ISIG | ICANON | ECHO);
    CHECK(term_signal(&raw, CTRL('C')) == 0,
          "with ISIG off ctrl+c is a byte: an editor binding it to "
          "`cancel this command` is not asking to be killed by it");
    struct termios raw_but_killable = raw;
    raw_but_killable.c_lflag |= ISIG;
    CHECK(term_signal(&raw_but_killable, CTRL('C')) == SIGINT,
          "and ISIG on its own keeps it a signal in raw mode, which is "
          "why these are flags rather than one mode word");

    CHECK(term_echo(&t, 'a', arr_echo) == 1 && arr_echo[0] == 'a',
          "what is typed is echoed");
    CHECK(term_echo(&t, CTRL('A'), arr_echo) == 2
          && memcmp(arr_echo, "^A", 2) == 0,
          "a control character as ^ and its letter, ECHOCTL being on");
    CHECK(term_echo(&t, '\n', arr_echo) == 1 && arr_echo[0] == '\n',
          "but a newline as itself");
    CHECK(term_width(&t, CTRL('A')) == 2 && term_width(&t, 'a') == 1,
          "an erase takes back as many columns as the echo took");
    CHECK(term_echo(&raw, 'a', arr_echo) == 0,
          "with ECHO off nothing appears: an editor draws the screen "
          "itself, and a terminal helping would draw over it");
    struct termios quiet = t;
    quiet.c_lflag &= ~(uint32_t)ECHO;
    quiet.c_lflag |= ECHONL;
    CHECK(term_echo(&quiet, 'a', arr_echo) == 0
          && term_echo(&quiet, '\n', arr_echo) == 1,
          "a password prompt with ECHONL shows the end of the line only");

    if (failures == 0) printf("all good\n");
    return failures;
}
