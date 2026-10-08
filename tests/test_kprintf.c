// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_kprintf.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for the kernel's kprintf formatter.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

/* the lock complains through these when something is wrong with it */
void panic(const char *fmt, ...)
{
    printf("PANIC: ");
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    exit(1);
}

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

static char out[1024];
static int pos;

/* stubs for the kernel bits kprintf.c links against */
void serial_putchar(char c)
{
    out[pos++] = c;
}
bool console_ready(void)
{
    return false;
}
void console_putchar(char c)
{
    (void)c;
}

#include "lib/kprintf.h"

static int failures = 0;

static void check(const char *expect, const char *fmt, ...)
{
    memset(out, 0, sizeof(out));
    pos = 0;
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    if (strcmp(out, expect) != 0) {
        printf("FAIL fmt=\"%s\": got \"%s\" want \"%s\"\n", fmt, out, expect);
        failures++;
    }
}

int main(void)
{
    check("hello", "hello");
    check("42", "%d", 42);
    check("-42", "%d", -42);
    check("0", "%u", 0u);
    check("   42", "%5d", 42);
    check("00042", "%05d", 42);
    check("  -42", "%5d", -42);
    check("-0042", "%05d", -42);
    check("deadbeef", "%x", 0xdeadbeefu);
    check("00ff", "%04x", 0xffu);
    check("18446744073709551615", "%lu", UINT64_MAX);
    check("-9223372036854775808", "%ld", INT64_MIN);
    check("ffffffffffffffff", "%lx", UINT64_MAX);
    check("x", "%c", 'x');
    check("abc def", "%s %s", "abc", "def");
    check("(null)", "%s", (char *)NULL);
    check("100%", "100%%");
    check("0x00000000deadbeef", "%p", (void *)0xdeadbeefUL);
    check("0xffffffff80000000", "%p", (void *)0xffffffff80000000UL);
    check("a=1 b=02 c= 3", "a=%d b=%02d c=%2d", 1, 2, 3);
    check("4096", "%zu", (size_t)4096);

    /*
     * left justify. this is the flag whose absence once made the vmm
     * print its own machine code and then page fault
     */
    check("abc    |",  "%-7s|", "abc");
    check("    abc|",  "%7s|",  "abc");
    check("abc|",      "%-2s|", "abc");     /* too long for the field */
    check("kernel |",  "%-7s|", "kernel");
    check("42   |",    "%-5d|", 42);
    check("-42  |",    "%-5d|", -42);
    check("2a   |",    "%-5x|", 0x2au);
    check("x    |",    "%-5c|", 'x');
    check("    x|",    "%5c|",  'x');
    /* '-' beats '0': you cannot zero-pad the right of a number */
    check("42   |",    "%-05d|", 42);
    check("(null) |",  "%-7s|", (char *)NULL);
    check("a  b  |",   "%-3s%-3s|", "a", "b");

    if (failures == 0) {
        printf("all good\n");
    }
    return failures;
}
