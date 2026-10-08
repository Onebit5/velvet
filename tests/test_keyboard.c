// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_keyboard.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the ps/2 scancode state machine, fed synthetic set-1 bytes.
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

/* kernel bits the driver links against but doesnt need here */
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


#include "drivers/keyboard.h"
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

/*
 * the tty gets first refusal on every key and takes the ones that mean
 * something to the machine rather than to whatever is running
 */
static int intercepted = -1;
bool tty_intercept(int key)
{
    intercepted = key;
    return false;       /* let everything through to the ring as well */
}


static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

static void feed(const uint8_t *bytes, int n)
{
    for (int i = 0; i < n; i++) keyboard_feed(bytes[i]);
}

/* drain as text, for the plain-character cases */
static void expect(const char *want, const char *what)
{
    char got[512];
    int n = 0, c;
    while ((c = input_getchar()) >= 0 && n < 511) got[n++] = (char)c;
    got[n] = 0;
    if (strcmp(got, want) != 0) {
        printf("FAIL %s: got \"%s\" want \"%s\"\n", what, got, want);
        failures++;
    }
}

/* drain as raw key codes, for ctrl and the arrows */
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
    feed((uint8_t[]){0x1e, 0x9e}, 2);
    expect("a", "plain letter");

    feed((uint8_t[]){0x2a, 0x1e, 0x9e, 0xaa, 0x1e, 0x9e}, 6);
    expect("Aa", "shift letter");

    feed((uint8_t[]){0x36, 0x02, 0x82, 0xb6, 0x02, 0x82}, 6);
    expect("!1", "shift digit");

    feed((uint8_t[]){0x3a, 0xba, 0x1e, 0x9e, 0x02, 0x82}, 6);
    expect("A1", "capslock lifts letters but not digits");

    feed((uint8_t[]){0x2a, 0x1e, 0x9e, 0xaa}, 4);
    expect("a", "caps and shift cancel out");

    feed((uint8_t[]){0x3a, 0xba, 0x1e, 0x9e}, 4);
    expect("a", "caps off again");

    feed((uint8_t[]){0x2a, 0xaa, 0x38, 0xb8}, 4);
    expect("", "modifiers alone are silent");

    feed((uint8_t[]){0x1c, 0x39, 0x0e, 0x0f}, 4);
    expect("\n \b\t", "enter, space, backspace, tab");

    feed((uint8_t[]){0x23,0xa3, 0x12,0x92, 0x26,0xa6, 0x26,0xa6, 0x18,0x98}, 10);
    expect("hello", "a word, typed like a person");


    feed((uint8_t[]){0x1d, 0x2e, 0xae, 0x9d}, 4);
    expect_keys((int[]){0x03}, 1, "ctrl+c is 0x03");

    feed((uint8_t[]){0x1d, 0x1e, 0x9e, 0x9d}, 4);
    expect_keys((int[]){0x01}, 1, "ctrl+a is 0x01");

    feed((uint8_t[]){0xe0, 0x1d, 0x2e, 0xae, 0xe0, 0x9d}, 6);
    expect_keys((int[]){0x03}, 1, "right ctrl works the same");

    feed((uint8_t[]){0x2e, 0xae}, 2);
    expect("c", "letters are plain again once ctrl is released");

    feed((uint8_t[]){0x1d, 0x02, 0x82, 0x9d}, 4);
    expect_keys(NULL, 0, "ctrl+digit is dropped, not mangled");


    feed((uint8_t[]){0xe0, 0x48}, 2);
    expect_keys((int[]){KEY_UP}, 1, "arrow up");

    feed((uint8_t[]){0xe0, 0x50}, 2);
    expect_keys((int[]){KEY_DOWN}, 1, "arrow down");

    feed((uint8_t[]){0xe0, 0x4b, 0xe0, 0x4d}, 4);
    expect_keys((int[]){KEY_LEFT, KEY_RIGHT}, 2, "arrow left then right");

    feed((uint8_t[]){0xe0, 0xc8, 0xe0, 0xd0}, 4);
    expect_keys(NULL, 0, "arrow releases are silent");

    feed((uint8_t[]){0x1e, 0x9e, 0xe0, 0x48, 0x30, 0xb0}, 6);
    expect_keys((int[]){'a', KEY_UP, 'b'}, 3, "arrows keep their place in the queue");

    feed((uint8_t[]){0xe0, 0x5b, 0xe0, 0xdb, 0x1e, 0x9e}, 6);
    expect("a", "an e0 key do not know is swallowed whole");


    for (int i = 0; i < 400; i++) feed((uint8_t[]){0x1e, 0x9e}, 2);
    int n = 0;
    while (input_getchar() >= 0) n++;
    if (n != 255) {
        printf("FAIL overflow: drained %d keys, want 255\n", n);
        failures++;
    }

    /*
     * this is the bug a single input ring had:
     * four shells all block on it and a keypress wakes every one of
     * them, whichever the scheduler happens to pick takes the
     * character, and it picks the same one every time, so every
     * keystroke went consistently to the wrong console.
     *
     * one ring per console is the only arrangement where the question
     * has one answer. gating a shared ring afterwards is a race with
     * extra steps: by then something has already been woken and
     * something has already been consumed
     */
    {
        shown_console_n = 0;

        /* typed at console 1 */
        keyboard_feed(0x1e);            /* 'a' */
        keyboard_feed(0x1e | 0x80);

        my_console_n = 1;
        CHECK(input_getchar() == -1,
              "a key typed at console 1 is not there for console 2");
        my_console_n = 2;
        CHECK(input_getchar() == -1, "nor for console 3");

        my_console_n = 0;
        CHECK(input_getchar() == 'a',
              "and console 1 has it, exactly one reader, which is the "
              "whole point of a ring each");
        CHECK(input_getchar() == -1, "and only once");

        /* the screen moves, and so do the keys */
        shown_console_n = 2;
        keyboard_feed(0x30);            /* 'b' */
        keyboard_feed(0x30 | 0x80);

        my_console_n = 0;
        CHECK(input_getchar() == -1,
              "a key typed after switching does not reach the console that "
              "was showing");
        my_console_n = 2;
        CHECK(input_getchar() == 'b', "it reaches the one that is");

        shown_console_n = 0;
        my_console_n = 0;
    }

    /* scancode 0x03 is the 2 key. */
    {
        while (input_getchar() >= 0) { }
        keyboard_feed(0x03);
        keyboard_feed(0x03 | 0x80);
        CHECK(input_getchar() == '2',
              "scancode 3 is the 2 key, which is also the device id a "
              "wheel mouse answers with, and is why a stray 2 appeared at "
              "the login prompt");
    }

    /*
     * alt+f1..f4 is what every unix uses and is frequently unavailable
     * on a machine running inside something else, the host takes it
     * first and switches its own console. so alt with the number row
     * does the same thing, which is nobody's traditional binding and
     * exactly why it survives
     */
    {
        intercepted = -1;
        keyboard_feed(0x38);            /* alt down */
        keyboard_feed(0x3d);            /* f3 */
        CHECK(intercepted == KEY_CONSOLE_1 + 2,
              "alt+f3 asks for console 3");

        intercepted = -1;
        keyboard_feed(0x04);            /* the digit 3 */
        CHECK(intercepted == KEY_CONSOLE_1 + 2,
              "and so does alt+3, for machines whose host eats the "
              "function keys");
        keyboard_feed(0x38 | 0x80);     /* alt up */

        /* without alt they are the keys they have always been */
        while (input_getchar() >= 0) { }
        keyboard_feed(0x04);
        keyboard_feed(0x04 | 0x80);
        CHECK(input_getchar() == '3', "and without alt, 3 is just a three");

        keyboard_feed(0x3d);
        CHECK(input_getchar() == -1, "while f3 alone still means nothing here");
    }

    if (failures == 0) printf("all good\n");
    return failures;
}
