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

#include <velvet/kprintf.h>

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

/*
 * a capture holds what is printed instead of letting it out, keeps what
 * fits, and says when something did not. and when nobody is capturing,
 * printing goes where it always went
 */
static struct kcapture *s_p_capturing;

static struct kcapture *test_source(void)
{
    return s_p_capturing;
}

static void capture_tests(void)
{
    char arr_buf[8];
    struct kcapture cap = { arr_buf, sizeof arr_buf, 0, false };

    kprintf_capture_source(test_source);
    memset(out, 0, sizeof(out));
    pos = 0;

    s_p_capturing = &cap;
    kprintf("fsck %d", 42);
    s_p_capturing = NULL;
    if (cap.sz_len != 7 || memcmp(arr_buf, "fsck 42", 7) != 0
        || cap.b_overflowed) {
        printf("FAIL capture: got %zu bytes \"%.*s\"\n", cap.sz_len,
               (int)cap.sz_len, arr_buf);
        failures++;
    }
    if (pos != 0) {
        printf("FAIL capture: \"%s\" also went to serial\n", out);
        failures++;
    }

    s_p_capturing = &cap;
    kprintf("ok");
    s_p_capturing = NULL;
    if (cap.sz_len != 8 || !cap.b_overflowed || arr_buf[7] != 'o') {
        printf("FAIL capture overflow: len %zu, overflowed %d\n",
               cap.sz_len, cap.b_overflowed);
        failures++;
    }

    kprintf("out");
    if (strcmp(out, "out") != 0) {
        printf("FAIL after capture: serial got \"%s\"\n", out);
        failures++;
    }
    kprintf_capture_source(NULL);
}

/*
 * the log hands back its newest bytes, oldest first, before it has gone
 * round and after, and a copy takes nothing out of it. this has to run
 * first, while the log holds nothing but what it writes
 */
static char s_arr_log[KLOG_SIZE + 64];

static bool no_serial(void)
{
    return false;
}

static void fail_klog(const char *str_what)
{
    printf("FAIL klog: %s\n", str_what);
    failures++;
}

static void klog_tests(void)
{
    char arr_buf[64];

    /* the serial stub has room for a line, not for a whole log */
    kprintf_serial_filter(no_serial);

    kwrite("boot", 4);
    if (klog_copy(arr_buf, sizeof arr_buf) != 4
        || memcmp(arr_buf, "boot", 4) != 0) {
        fail_klog("a log that has not gone round gives what it holds");
    }
    if (klog_copy(arr_buf, 2) != 2 || memcmp(arr_buf, "ot", 2) != 0) {
        fail_klog("a short buffer gets the newest of it");
    }

    /* round once and six bytes past where "boot" began */
    for (size_t sz_i = 0; sz_i < KLOG_SIZE - 2; sz_i++) {
        kwrite("x", 1);
    }
    kwrite("tail", 4);

    for (int i_pass = 0; i_pass < 2; i_pass++) {
        if (klog_copy(s_arr_log, sizeof s_arr_log) != KLOG_SIZE) {
            fail_klog("a log that has gone round holds exactly its size");
        }
        if (s_arr_log[0] != 'x'
            || memcmp(s_arr_log + KLOG_SIZE - 4, "tail", 4) != 0) {
            fail_klog("oldest first, ending with the last thing written");
        }
    }

    kprintf_serial_filter(NULL);
}

int main(void)
{
    klog_tests();

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

    capture_tests();

    if (failures == 0) {
        printf("all good\n");
    }
    return failures;
}
