// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/keyboard.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * scancode set 1 -> ascii, the kernel layout. 0 means "nothing printable here",
 * which covers modifiers, f-keys and everything else do not care about. 27
 * is escape, which will draw as a weird glyph if you print it.
 */

#include <velvet/keyboard.h>
#include <velvet/input.h>
#include <asm/io.h>
#include <asm/pic.h>
#include <asm/interrupts.h>
#include <stdbool.h>

#define KBD_DATA   0x60
#define KBD_STATUS 0x64
#define KBD_IRQ    1

/*
 * scancode set 1 -> ascii, the kernel layout. backspace is DEL and enter
 * is a carriage return, what a pc console has always sent: the terminal
 * makes the return a newline unless a program asked it not to
 */

static const char keymap[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=',0x7f,
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\r',
    0,  /* lctrl */
    'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  /* lshift */
    '\\','z','x','c','v','b','n','m',',','.','/',
    0,  /* rshift */
    '*',
    0,  /* lalt */
    ' ',
    0,  /* capslock, and nothing but zeroes past here */
};

static const char keymap_shift[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+',0x7f,
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\r',
    0,
    'A','S','D','F','G','H','J','K','L',':','"','~',
    0,
    '|','Z','X','C','V','B','N','M','<','>','?',
    0,
    '*',
    0,
    ' ',
    0,
};

static bool lshift, rshift, caps;
static bool lctrl, rctrl;
static bool lalt, ralt;
static bool e0_prefix;

static bool shift(void)
{
    return lshift || rshift;
}
static bool alt(void)
{
    return lalt || ralt;
}

/* the e0-prefixed keys the kernel cares about. */
static void feed_extended(uint8_t code, bool release)
{
    if (code == 0x1d) {         /* right ctrl is a modifier, not a key */
        rctrl = !release;
        return;
    }
    if (code == 0x38) {         /* and so is right alt */
        ralt = !release;
        return;
    }
    if (release) {
        return;
    }
    switch (code) {
    case 0x48: input_push(KEY_UP);     break;
    case 0x50: input_push(KEY_DOWN);   break;
    case 0x4b: input_push(KEY_LEFT);   break;
    case 0x4d: input_push(KEY_RIGHT);  break;
    case 0x53: input_push(KEY_DELETE); break;
    case 0x47: input_push(KEY_HOME);   break;
    case 0x4f: input_push(KEY_END);    break;
    case 0x49: input_push(shift() ? KEY_SCROLL_UP : KEY_PGUP);   break;
    case 0x51: input_push(shift() ? KEY_SCROLL_DOWN : KEY_PGDN); break;
    default: break;
    }
}

void keyboard_feed(uint8_t sc)
{
    if (sc == 0xe0) {
        e0_prefix = true;
        return;
    }

    bool release = sc & 0x80;
    uint8_t code = sc & 0x7f;

    if (e0_prefix) {
        e0_prefix = false;
        feed_extended(code, release);
        return;
    }

    switch (code) {
    case 0x2a: lshift = !release; return;
    case 0x36: rshift = !release; return;
    case 0x1d: lctrl  = !release; return;
    case 0x38: lalt   = !release; return;

    /* "show the kernel that console", two ways. */
    case 0x3b: case 0x3c: case 0x3d: case 0x3e:     /* f1..f5 */
    case 0x3f:
        if (!release && alt()) {
            input_push(KEY_CONSOLE_1 + (code - 0x3b));
        }
        return;
    case 0x02: case 0x03: case 0x04: case 0x05:     /* 1..5 */
    case 0x06:
        if (!release && alt()) {
            input_push(KEY_CONSOLE_1 + (code - 0x02));
            return;
        }
        break;      /* without alt they are just digits */
    case 0x3a:
        if (!release) {
            caps = !caps;
        }
        return;
    }

    if (release) {
        return;
    }

    char c = (lshift || rshift) ? keymap_shift[code] : keymap[code];
    if (c == 0) {
        return;
    }

    /*
     * ctrl+letter collapses to the matching control code, so ctrl+c
     * arrives as 3 and the shell can treat it like every terminal
     * has since forever. ctrl+anything-else the kernel simply drop
     */
    if (lctrl || rctrl) {
        if (c >= 'a' && c <= 'z') {
            input_push(c - 'a' + 1);
        } else if (c >= 'A' && c <= 'Z') {
            input_push(c - 'A' + 1);
        }
        return;
    }

    /* capslock flips letters only, and cancels against shift */
    if (caps) {
        if (c >= 'a' && c <= 'z') {
            c += 'A' - 'a';
        } else if (c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
    }

    input_push((uint16_t)c);
}

static void keyboard_irq(void)
{

    /* bit 5 says this byte came from the *mouse* rather than the keyboard. */
    if (inb(KBD_STATUS) & 0x20) {
        return;
    }
    keyboard_feed(inb(KBD_DATA));
}

void keyboard_init(void)
{
    /* drain whatever stale bytes the 8042 has been hoarding since boot */
    while (inb(KBD_STATUS) & 1) {
        inb(KBD_DATA);
    }
    irq_install(KBD_IRQ, keyboard_irq);
}
