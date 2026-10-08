// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_serial.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * serial input translation.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>

/* what the lock complains through */
void kprintf(const char *fmt, ...)
{
    (void)fmt;
}
void panic(const char *fmt, ...)
{
    printf("PANIC: ");
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    exit(1);
}

#include <stdbool.h>

struct interrupt_frame;
void irq_register(uint8_t irq, void (*h)(struct interrupt_frame *))
{
    (void)irq; (void)h;
}
void pic_unmask(uint8_t irq)
{
    (void)irq;
}
struct waitq;
void waitq_block(struct waitq *q)
{
    (void)q;
}
void waitq_enqueue(struct waitq *q)
{
    (void)q;
}
void waitq_sleep(void)
{
}
void waitq_wake_all(struct waitq *q)
{
    (void)q;
}

/*
 * input_push offers every key to the tty first, so ctrl+c aimed at a
 * program becomes an interrupt rather than a character. nothing is in
 * the foreground here, so nothing is intercepted
 */
bool tty_intercept(int key)
{
    (void)key; return false;
}

#include "drivers/serial.h"
#include "drivers/input.h"

/* which console the reader belongs to, and which is being looked at. */
static unsigned my_console_n, shown_console_n;
unsigned tty_my_console(void)
{
    return my_console_n;
}
unsigned console_active(void)
{
    return shown_console_n;
}


static int failures = 0;

static void feed(const char *bytes)
{
    for (const char *p = bytes; *p; p++) serial_feed((uint8_t)*p);
}

static void expect_keys(const int *want, int wantn, const char *what)
{
    int got[64], n = 0, c;
    while ((c = input_getchar()) >= 0 && n < 64) got[n++] = c;
    int bad = (n != wantn);
    for (int i = 0; !bad && i < n; i++) bad = (got[i] != want[i]);
    if (bad) {
        printf("FAIL %s: got [", what);
        for (int i = 0; i < n; i++) printf("%#x%s", got[i], i + 1 < n ? " " : "");
        printf("] want [");
        for (int i = 0; i < wantn; i++) printf("%#x%s", want[i], i + 1 < wantn ? " " : "");
        printf("]\n");
        failures++;
    }
}

int main(void)
{
    feed("ps");
    expect_keys((int[]){'p', 's'}, 2, "plain letters pass through");

    feed("\r");
    expect_keys((int[]){'\n'}, 1, "CR from a terminal becomes newline");

    feed("\n");
    expect_keys((int[]){'\n'}, 1, "LF is already a newline");

    serial_feed(0x7f);
    expect_keys((int[]){'\b'}, 1, "DEL becomes backspace");

    serial_feed(0x08);
    expect_keys((int[]){'\b'}, 1, "a real backspace stays one");

    serial_feed(0x03);
    expect_keys((int[]){0x03}, 1, "ctrl+c arrives as 3 already");

    /* arrows come in as ESC [ A and friends */
    feed("\x1b[A");
    expect_keys((int[]){KEY_UP}, 1, "ESC[A is up");

    feed("\x1b[B");
    expect_keys((int[]){KEY_DOWN}, 1, "ESC[B is down");

    feed("\x1b[C\x1b[D");
    expect_keys((int[]){KEY_RIGHT, KEY_LEFT}, 2, "ESC[C and ESC[D are right and left");

    /*
     * home and end come both ways depending on the terminal, and both
     * have to work, a key that only functions on half of them is
     * worse than one that does not exist
     */
    feed("\x1b[H\x1b[F");
    expect_keys((int[]){KEY_HOME, KEY_END}, 2, "ESC[H and ESC[F are home and end");

    /* the other shape: a number and a tilde. */
    feed("\x1b[5~\x1b[6~");
    expect_keys((int[]){KEY_PGUP, KEY_PGDN}, 2, "ESC[5~ and ESC[6~ are the pages");
    feed("\x1b[1~\x1b[4~\x1b[3~");
    expect_keys((int[]){KEY_HOME, KEY_END, KEY_DELETE}, 3,
                "and home, end and delete are sent that way by some");

    /* a serial line has no alt key and no function keys, so it needs a sequence of ordinary bytes. */
    /*
     * octal rather than \x1c, which is not the same thing: a hex escape
     * keeps eating hex digits, so "\x1c3" is one character numbered
     * 0x1c3 rather than two. octal stops at three digits and cannot
     */
    feed("\0343");
    expect_keys((int[]){KEY_CONSOLE_1 + 2}, 1, "ctrl+\\ then 3 asks for console 3");

    feed("\0341");
    expect_keys((int[]){KEY_CONSOLE_1}, 1, "and 1 for the first");

    /*
     * the digit is swallowed either way, a half-typed sequence must
     * not leave a stray character in somebody's shell
     */
    feed("\0349");
    expect_keys(NULL, 0, "a console that does not exist asks for nothing");

    feed("\034x");
    expect_keys(NULL, 0, "and neither does something that is not a digit");

    feed("3");
    expect_keys((int[]){'3'}, 1, "while a digit on its own is still a digit");

    /* a CSI sequence do not handle must vanish, not spray garbage */
    feed("\x1b[Z");
    expect_keys(NULL, 0, "an unknown escape sequence is dropped");
    feed("\x1b[99~");
    expect_keys(NULL, 0, "and so is an unknown numbered one");

    /*
     * the number cannot run away with itself either: a sequence that
     * never ends must give up rather than collecting for ever
     */
    feed("\x1b[123456789~a");
    expect_keys((int[]){'a'}, 1, "a runaway number is abandoned, and text after it survives");

    /* the state machine must not swallow real input around a sequence */
    feed("a\x1b[Ab");
    expect_keys((int[]){'a', KEY_UP, 'b'}, 3, "text either side of an arrow survives");

    /* a lone ESC that isnt followed by [ shouldnt eat the next key */
    feed("\x1b" "x");
    expect_keys(NULL, 0, "bare ESC consumes only the byte after it");

    feed("ok");
    expect_keys((int[]){'o', 'k'}, 2, "and recovers cleanly afterwards");

    /* a whole command line, as the CI script types it */
    feed("mem\r");
    expect_keys((int[]){'m','e','m','\n'}, 4, "a full command line");

    if (failures == 0) printf("all good\n");
    return failures;
}
