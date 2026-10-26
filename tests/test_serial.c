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

/* a reader, and whether its process has been interrupted */
#include <velvet/thread.h>
static struct thread s_reader = { .pid = 7 };
static bool s_b_interrupted;
struct thread *sched_current(void)
{
    return &s_reader;
}
bool process_interrupt_pending(int i_pid)
{
    return i_pid == s_reader.pid && s_b_interrupted;
}

/*
 * input_push offers every key to the tty first, so ctrl+c aimed at a
 * program becomes an interrupt rather than a character. nothing is in
 * the foreground here, so nothing is intercepted
 */
static int s_i_seen = -1;
bool tty_intercept(int key)
{
    s_i_seen = key;
    return key >= 0x100;        /* the console switches, and nothing else */
}

#include <velvet/serial.h>
#include <velvet/input.h>

/* which console is being looked at, which is the one typing reaches */
static unsigned shown_console_n;
unsigned console_active(void)
{
    return shown_console_n;
}


static int failures = 0;

static void feed(const char *bytes)
{
    for (const char *p = bytes; *p; p++) serial_feed((uint8_t)*p);
}

/* what came through, byte by byte */
static void expect_keys(const int *want, int wantn, const char *what)
{
    int got[64], n = 0, c;
    while ((c = input_take(shown_console_n)) >= 0 && n < 64) got[n++] = c;
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

/* what the terminal was last offered, the console switch among them */
#define CHECK_ASKED(want, what) do { \
    if (s_i_seen != (want)) { printf("FAIL %s\n", what); failures++; } \
} while (0)

int main(void)
{
    feed("ps");
    expect_keys((int[]){'p', 's'}, 2, "plain letters pass through");

    feed("\r");
    expect_keys((int[]){'\r'}, 1,
                "CR from a terminal arrives as itself, for the terminal's "
                "termios to make a newline of, or not");

    serial_feed(0x7f);
    expect_keys((int[]){0x7f}, 1, "and DEL as itself, the erase character");

    serial_feed(0x03);
    expect_keys((int[]){0x03}, 1, "ctrl+c arrives as 3 already");

    /*
     * a terminal's escape sequences pass through whole, since a program
     * reads the same ones from the console's keys: nothing in between
     * decodes them, so nothing can lose the byte after an escape
     */
    feed("\x1b[A\x1b[5~");
    expect_keys((int[]){0x1b, '[', 'A', 0x1b, '[', '5', '~'}, 7,
                "escape sequences go through as they came");
    feed("\x1b" "x");
    expect_keys((int[]){0x1b, 'x'}, 2,
                "and a lone escape keeps what comes after it");

    /*
     * a serial line has no alt key and no function keys, so it needs a
     * sequence of ordinary bytes. octal rather than \x1c, which is not
     * the same thing: a hex escape keeps eating hex digits, so "\x1c3"
     * is one character numbered 0x1c3 rather than two. octal stops at
     * three digits and cannot
     */
    s_i_seen = -1;
    feed("\0343");
    CHECK_ASKED(KEY_CONSOLE_1 + 2, "ctrl+\\ then 3 asks for console 3");
    expect_keys(NULL, 0, "and leaves nothing in the ring");

    s_i_seen = -1;
    feed("\0349");
    CHECK_ASKED(-1, "a console that does not exist asks for nothing");
    expect_keys(NULL, 0, "and the digit is swallowed, not left in a shell");

    feed("3");
    expect_keys((int[]){'3'}, 1, "while a digit on its own is still a digit");

    /* a whole command line, as the CI script types it */
    feed("mem\r");
    expect_keys((int[]){'m', 'e', 'm', '\r'}, 4, "a full command line");

    if (failures == 0) printf("all good\n");
    return failures;
}
