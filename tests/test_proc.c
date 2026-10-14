// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_proc.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for /proc: registering a file, and reading what it says.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* the lock complains through this when something is wrong with it */
void panic(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    exit(1);
}

#include <velvet/kprintf.h>
#include <velvet/proc.h>

/* what kprintf.c and proc.c lean on, done the plainest way there is */
void serial_putchar(char c_ch)
{
    (void)c_ch;
}
bool console_ready(void)
{
    return false;
}
void console_putchar(char c_ch)
{
    (void)c_ch;
}
void *kmalloc(size_t sz_size)
{
    return malloc(sz_size);
}
void kfree(void *p_mem)
{
    free(p_mem);
}
uint64_t pit_uptime_ms(void)
{
    return 61234;
}

/* one thread, so the capture is a single pointer kprintf is told about. */
static struct kcapture *s_p_capture;

static struct kcapture *capturing(void)
{
    return s_p_capture;
}

void sched_capture(struct kcapture *p_cap)
{
    s_p_capture = p_cap;
}

static int s_i_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); s_i_failures++; } \
} while (0)

static void show_greeting(void)
{
    kprintf("hello from %s, %d lines\n", "the kernel", 1);
}

static void show_nothing(void)
{
}

int main(void)
{
    char arr_text[256];
    size_t sz_index = 99;

    kprintf_capture_source(capturing);
    proc_init();

    CHECK(proc_find("version", &sz_index) && sz_index == 0,
          "version is the first file /proc has");
    CHECK(proc_find("uptime", &sz_index) && sz_index == 1,
          "and uptime the second");
    CHECK(proc_register("greeting", PROC_PUBLIC, show_greeting),
          "a file registers");
    CHECK(!proc_register("greeting", PROC_PUBLIC, show_nothing),
          "but not twice under one name");
    CHECK(!proc_register("a name far too long for any proc file",
                         PROC_PUBLIC, show_nothing),
          "nor under a name that does not fit");
    CHECK(proc_register("empty", PROC_PRIVATE, show_nothing),
          "an empty one is fine, and so is root's own");
    CHECK(!proc_find("nothing", &sz_index), "a name nobody registered");

    CHECK(proc_find("cpuinfo", &sz_index) && sz_index == 2,
          "and cpuinfo, which the architecture writes, the third");
    CHECK(strcmp(proc_name(3), "greeting") == 0, "listed by its name");
    CHECK(proc_name(5) == NULL, "and the listing ends");
    CHECK(proc_mode(3) == 0444 && proc_mode(4) == 0400,
          "each keeps the mode it was registered with");
    CHECK(proc_mode(9) == 0, "and one that does not exist has none");

    int64_t i_len = proc_render(3, arr_text, sizeof arr_text);
    CHECK(i_len == 31 && memcmp(arr_text, "hello from the kernel, 1 lines\n",
                                31) == 0, "its text is what it printed");
    CHECK(s_p_capture == NULL, "and printing goes back to normal after");
    CHECK(proc_render(4, arr_text, sizeof arr_text) == 0,
          "a file that says nothing is empty");
    CHECK(proc_render(9, arr_text, sizeof arr_text) == -1,
          "a file that does not exist cannot be written");

    i_len = proc_render(1, arr_text, sizeof arr_text);
    CHECK(i_len == 6 && memcmp(arr_text, "61.23\n", 6) == 0,
          "uptime is seconds to the hundredth");
    i_len = proc_render(0, arr_text, sizeof arr_text);
    CHECK(i_len > 15 && memcmp(arr_text, "velvet version ", 15) == 0,
          "version says which kernel");
    arr_text[i_len] = '\0';
    CHECK(strstr(arr_text, " (gcc ") != NULL && arr_text[i_len - 1] == '\n',
          "and which compiler made it, on one line");

    /* read in pieces, it is the same file */
    char arr_piece[8];
    char arr_whole[64];
    size_t sz_at = 0;
    for (;;) {
        int64_t i_got = proc_read(3, sz_at, arr_piece, 5);
        if (i_got <= 0) {
            CHECK(i_got == 0, "the end is a read of nothing");
            break;
        }
        memcpy(arr_whole + sz_at, arr_piece, (size_t)i_got);
        sz_at += (size_t)i_got;
    }
    CHECK(sz_at == 31 && memcmp(arr_whole, "hello from the kernel, 1 lines\n",
                                31) == 0, "five bytes at a time adds up");
    CHECK(proc_read(3, 1000, arr_piece, 5) == 0, "past the end, nothing");
    CHECK(proc_read(9, 0, arr_piece, 5) == -1, "no such file, no read");

    if (s_i_failures == 0) {
        printf("all good\n");
    }
    return s_i_failures;
}
