// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_tty.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the terminals: a line typed, a mode set, and who each belongs to.
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

/* what the lock complains through */
void panic(const char *fmt, ...)
{
    printf("PANIC: ");
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    exit(1);
}

void kprintf(const char *fmt, ...)
{
    (void)fmt;
}

#include <velvet/thread.h>
#include <velvet/console.h>
#include <velvet/sched.h>

/* the thread making the calls, and the process it is */
static struct thread this_thread;
struct thread *sched_current(void)
{
    return &this_thread;
}

/*
 * what reached each console's screen. the terminal writes through
 * kwrite, which prints to the console of the thread writing, so that is
 * the console it is filed under
 */
static char s_arr_screen[VCONSOLE_COUNT][512];
static size_t s_arr_shown[VCONSOLE_COUNT];
void kwrite(const char *s, size_t n)
{
    unsigned u_c = this_thread.console;
    for (size_t sz_i = 0; sz_i < n && s_arr_shown[u_c] < 511; sz_i++) {
        s_arr_screen[u_c][s_arr_shown[u_c]++] = s[sz_i];
    }
    s_arr_screen[u_c][s_arr_shown[u_c]] = '\0';
}

static void screens_clear(void)
{
    memset(s_arr_shown, 0, sizeof s_arr_shown);
    memset(s_arr_screen, 0, sizeof s_arr_screen);
}

static unsigned s_u_shown;
unsigned console_active(void)
{
    return s_u_shown;
}
void console_switch(unsigned n)
{
    s_u_shown = n;
}
static int s_i_scrolled;
void console_scroll_back(int lines)
{
    s_i_scrolled += lines;
}
void console_size(size_t *p_cols, size_t *p_rows, size_t *p_w, size_t *p_h)
{
    *p_cols = 100;
    *p_rows = 37;
    *p_w = 800;
    *p_h = 600;
}

/*
 * a keyboard per console, typed at by the test. nothing ever arrives
 * while a reader waits, so a wait with nothing typed is a signal's
 */
static uint8_t s_arr_keys[VCONSOLE_COUNT][256];
static size_t s_arr_head[VCONSOLE_COUNT], s_arr_tail[VCONSOLE_COUNT];
static void type(unsigned u_c, const char *str)
{
    for (; *str; str++) {
        s_arr_keys[u_c][s_arr_head[u_c]++] = (uint8_t)*str;
    }
}
int input_take(unsigned u_c)
{
    return (s_arr_tail[u_c] < s_arr_head[u_c])
           ? s_arr_keys[u_c][s_arr_tail[u_c]++] : -1;
}
bool input_waiting(unsigned u_c)
{
    return s_arr_tail[u_c] < s_arr_head[u_c];
}
void input_flush(unsigned u_c)
{
    s_arr_tail[u_c] = s_arr_head[u_c];
}
bool input_wait(unsigned u_c)
{
    return input_waiting(u_c);
}

/* a clock that moves only when a timed read sleeps on it */
static uint64_t s_u_now;
uint64_t pit_uptime_ms(void)
{
    return s_u_now;
}
void sleep_ms(uint64_t u_ms)
{
    s_u_now += u_ms;
}

/* what the process table does to threads, recorded rather than done */
static int s_i_stopped_id;
void sched_wake_thread(int id)
{
    (void)id;
}
void sched_set_stopped(int id, bool b_stop)
{
    s_i_stopped_id = b_stop ? id : 0;
}

/* a host has no mmu to fault, so every address the test hands is good */
bool host_uaccess_reachable(uint64_t u_addr, uint64_t u_len, bool b_write)
{
    (void)u_addr; (void)u_len; (void)b_write;
    return true;
}

#include <velvet/file.h>
struct file *file_alloc(const struct file_operations *p_ops, uint32_t u_mode,
                        enum file_type e_type)
{
    struct file *p_file = calloc(1, sizeof *p_file);
    p_file->p_ops = p_ops;
    p_file->u_refs = 1;
    p_file->u_mode = u_mode;
    p_file->e_type = e_type;
    return p_file;
}
void file_get(struct file *p_file)
{
    p_file->u_refs++;
}
void file_put(struct file *p_file)
{
    if (p_file != NULL && --p_file->u_refs == 0) {
        free(p_file);
    }
}

#include <velvet/process.h>
#include <velvet/tty.h>
#include <velvet/input.h>
#include <velvet/termios.h>
#include <velvet/errno.h>
#include <uapi/velvet/fcntl.h>

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

/* the calling thread becomes process i_pid's, on console u_console */
static void be(int i_pid, unsigned u_console)
{
    this_thread.pid = i_pid;
    this_thread.console = u_console;
}

static int64_t rd(struct file *p_file, char *p_buf, size_t sz_len)
{
    int64_t i_got = p_file->p_ops->read(p_file, p_buf, sz_len);
    if (i_got >= 0 && (size_t)i_got < sz_len) {
        p_buf[i_got] = '\0';
    }
    return i_got;
}

static int64_t ctl(struct file *p_file, uint32_t u_cmd, void *p_arg)
{
    return p_file->p_ops->ioctl(p_file, u_cmd, (uint64_t)(uintptr_t)p_arg);
}

int main(void)
{
    char arr_buf[64];
    /* pid 1 takes no signal it has no handler for, so it is not the shell */
    process_create("init", 0, 0, false, 0);
    int i_shell = process_create("sh", 0, 0, false, 0);
    struct file *p_tty = tty_open(1, FILE_READ | FILE_WRITE);
    be(i_shell, 0);

    CHECK(p_tty != NULL && p_tty->e_type == FILE_TYPE_TTY
          && p_tty->u_rdev == ((TTY_MAJOR << 8) | 2),
          "a console's terminal opens as tty2, major 4");
    CHECK(tty_open(VCONSOLE_COUNT, FILE_READ) == NULL,
          "and there is none past the last console");

    /* a line, typed at the terminal's console, echoed there */
    screens_clear();
    type(1, "ls -l\r");
    CHECK(rd(p_tty, arr_buf, sizeof arr_buf) == 6
          && strcmp(arr_buf, "ls -l\n") == 0,
          "a read is a line, return made a newline");
    CHECK(strcmp(s_arr_screen[1], "ls -l\n") == 0 && s_arr_shown[0] == 0,
          "echoed on the terminal's own console, whichever the reader is on");

    type(1, "abx\x7f" "c\r");
    CHECK(rd(p_tty, arr_buf, sizeof arr_buf) == 4
          && strcmp(arr_buf, "abc\n") == 0,
          "DEL takes back what was typed");
    type(1, "wrong words\x15right\r");
    CHECK(rd(p_tty, arr_buf, sizeof arr_buf) == 6
          && strcmp(arr_buf, "right\n") == 0,
          "ctrl+u the whole line");
    type(1, "one two\x17three\r");
    CHECK(rd(p_tty, arr_buf, sizeof arr_buf) == 10
          && strcmp(arr_buf, "one three\n") == 0,
          "and ctrl+w the word before");

    type(1, "first\rsecond\r");
    CHECK(rd(p_tty, arr_buf, sizeof arr_buf) == 6
          && strcmp(arr_buf, "first\n") == 0,
          "two lines typed ahead are two reads, not one");
    CHECK(rd(p_tty, arr_buf, 3) == 3 && memcmp(arr_buf, "sec", 3) == 0
          && rd(p_tty, arr_buf, sizeof arr_buf) == 4
          && strcmp(arr_buf, "ond\n") == 0,
          "and a line longer than the read is the rest of it next time");

    type(1, "tail\x04");
    CHECK(rd(p_tty, arr_buf, sizeof arr_buf) == 4
          && strcmp(arr_buf, "tail") == 0,
          "ctrl+d hands over what there is, with no newline");
    type(1, "\x04");
    CHECK(rd(p_tty, arr_buf, sizeof arr_buf) == 0,
          "and on an empty line is the end of input");

    screens_clear();
    type(1, "\x01\r");
    rd(p_tty, arr_buf, sizeof arr_buf);
    CHECK(strcmp(s_arr_screen[1], "^A\n") == 0,
          "a control character echoes as ^ and a letter");

    /* nothing typed, and a signal waiting: the read is made again later */
    CHECK(rd(p_tty, arr_buf, sizeof arr_buf) == -ERESTARTSYS,
          "a wait a signal ends is started again once it is dealt with");

    /* modes */
    struct termios mode;
    CHECK(ctl(p_tty, TCGETS, &mode) == 0 && (mode.c_lflag & ICANON)
          && (mode.c_lflag & ECHO),
          "TCGETS says how it is");
    struct termios raw = mode;
    raw.c_lflag &= ~(uint32_t)(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    CHECK(ctl(p_tty, TCSETS, &raw) == 0, "TCSETS changes it");
    screens_clear();
    type(1, "q\x1b[A");
    CHECK(rd(p_tty, arr_buf, 1) == 1 && arr_buf[0] == 'q',
          "raw, a key at a time");
    CHECK(rd(p_tty, arr_buf, sizeof arr_buf) == 3
          && memcmp(arr_buf, "\x1b[A", 3) == 0,
          "or as many as are there");
    CHECK(s_arr_shown[1] == 0, "and nothing echoed with ECHO off");

    raw.c_cc[VMIN] = 0;
    ctl(p_tty, TCSETS, &raw);
    CHECK(rd(p_tty, arr_buf, sizeof arr_buf) == 0,
          "VMIN 0 and VTIME 0 is whatever is there, even nothing");
    raw.c_cc[VTIME] = 3;
    ctl(p_tty, TCSETS, &raw);
    uint64_t u_was = s_u_now;
    CHECK(rd(p_tty, arr_buf, sizeof arr_buf) == 0 && s_u_now - u_was >= 300,
          "VTIME is how long to wait for one first, in tenths");

    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    ctl(p_tty, TCSETS, &raw);
    p_tty->u_flags |= O_NONBLOCK;
    CHECK(rd(p_tty, arr_buf, sizeof arr_buf) == -EAGAIN,
          "a terminal opened not to wait says EAGAIN");
    p_tty->u_flags &= ~(uint32_t)O_NONBLOCK;

    /* a line half typed when ICANON goes off is what a raw read gets */
    ctl(p_tty, TCSETS, &mode);
    type(1, "half");
    rd(p_tty, arr_buf, sizeof arr_buf);     /* takes it as far as a line */
    ctl(p_tty, TCSETS, &raw);
    CHECK(rd(p_tty, arr_buf, sizeof arr_buf) == 4
          && memcmp(arr_buf, "half", 4) == 0,
          "turning ICANON off hands over the line in progress");
    ctl(p_tty, TCSETS, &mode);

    int32_t i_count = -1;
    type(1, "x\r");
    CHECK(ctl(p_tty, FIONREAD, &i_count) == 0 && i_count == 2,
          "FIONREAD counts what a read would have");
    rd(p_tty, arr_buf, sizeof arr_buf);

    struct winsize size;
    CHECK(ctl(p_tty, TIOCGWINSZ, &size) == 0 && size.ws_col == 100
          && size.ws_row == 37,
          "TIOCGWINSZ is the screen's size");
    CHECK(ctl(p_tty, 0x7777, NULL) == -ENOTTY,
          "and a request it does not know is ENOTTY");

    /* sessions: a leader takes a terminal, and decides who is in front */
    int i_job = process_create("job", i_shell, 0, false, 0);
    process_inherit(i_job, i_shell);
    process_set_pgid(i_job, i_job);
    int i_other = process_create("other", 0, 0, false, 0);
    int32_t i_pgrp = 0;

    be(i_job, 1);
    CHECK(ctl(p_tty, TIOCSCTTY, NULL) == -EPERM,
          "a process that leads no session may not take a terminal");
    be(i_shell, 1);
    CHECK(ctl(p_tty, TIOCGPGRP, &i_pgrp) == -ENOTTY,
          "nor ask who is in front of one that is not its own");
    CHECK(ctl(p_tty, TIOCSCTTY, NULL) == 0 && tty_foreground(1) == i_shell,
          "a session leader takes it, its group in front");
    CHECK(ctl(p_tty, TIOCGPGRP, &i_pgrp) == 0 && i_pgrp == i_shell,
          "and is told so");
    struct file *p_own = tty_open_own(FILE_READ);
    CHECK(p_own != NULL && p_own->p_private == p_tty->p_private,
          "/dev/tty is it, to the session");
    be(i_other, 0);
    CHECK(tty_open_own(FILE_READ) == NULL,
          "and nothing to a process of no session with one");
    CHECK(ctl(p_tty, TIOCSCTTY, NULL) == -EPERM,
          "a terminal already a session's is not another's");

    be(i_shell, 1);
    i_pgrp = i_other;
    CHECK(ctl(p_tty, TIOCSPGRP, &i_pgrp) == -EPERM,
          "the group in front may not hand it to a group outside the "
          "session");
    raw.c_lflag &= ~(uint32_t)ISIG;
    ctl(p_tty, TCSETS, &raw);
    s_u_shown = 1;
    struct signal_state *p_ssig = process_signal_state(i_shell);
    CHECK(!tty_intercept(0x03) && !(p_ssig->pending & SIGNAL_BIT(SIGINT)),
          "with ISIG off ctrl+c is a byte like any other");
    ctl(p_tty, TCSETS, &mode);
    i_pgrp = i_job;
    CHECK(ctl(p_tty, TIOCSPGRP, &i_pgrp) == 0 && tty_foreground(1) == i_job,
          "and may hand it to another group of the session");

    /* ctrl+c is the terminal's to judge, typed at the console on screen */
    s_u_shown = 1;
    struct signal_state *p_jsig = process_signal_state(i_job);
    type(1, "unfinished");
    rd(p_tty, arr_buf, 0);
    CHECK(tty_intercept(0x03) && (p_jsig->pending & SIGNAL_BIT(SIGINT)),
          "ctrl+c is SIGINT to the group in front");
    CHECK(!input_waiting(1), "and throws away what was typed");
    p_jsig->pending = 0;
    s_u_shown = 0;
    CHECK(tty_intercept(0x03) && !(p_jsig->pending & SIGNAL_BIT(SIGINT)),
          "and typed at another console it is that one's terminal's, "
          "which no session has, so it reaches nobody");
    p_jsig->pending = 0;

    /* a group behind its terminal is stopped when it reads, or changes it */
    be(i_shell, 1);
    type(1, "x\r");
    process_set_thread(i_shell, 55);
    CHECK(rd(p_tty, arr_buf, sizeof arr_buf) == -ERESTARTSYS
          && s_i_stopped_id == 55,
          "a read from behind stops its group with SIGTTIN");
    process_signal(i_shell, SIGCONT);
    struct sigaction ignore = { .sa_handler = SIG_IGNORE };
    signal_set_action(process_signal_state(i_shell), SIGTTIN, &ignore);
    CHECK(rd(p_tty, arr_buf, sizeof arr_buf) == -EIO,
          "or, ignoring SIGTTIN, is told EIO");
    i_pgrp = i_shell;
    CHECK(ctl(p_tty, TIOCSPGRP, &i_pgrp) == -ERESTARTSYS
          && s_i_stopped_id == 55,
          "and changing who is in front from behind is SIGTTOU");
    process_signal(i_shell, SIGCONT);
    signal_set_action(process_signal_state(i_shell), SIGTTOU, &ignore);
    CHECK(ctl(p_tty, TIOCSPGRP, &i_pgrp) == 0 && tty_foreground(1) == i_shell,
          "unless it ignores that, as a shell taking it back does");

    /* a session ends with its leader, and its terminal is nobody's */
    process_exited(i_shell, 0, 10);
    CHECK(tty_foreground(1) == TTY_NOBODY,
          "the terminal of a session whose leader is gone is nobody's");

    /* the keys that were never characters stay the terminal's own */
    CHECK(tty_intercept(KEY_CONSOLE_1 + 2) && s_u_shown == 2,
          "alt+f3 switches to the third console");
    CHECK(tty_intercept(KEY_SCROLL_UP) && s_i_scrolled == 8,
          "and shift+pageup looks back");
    CHECK(!tty_intercept(KEY_UP),
          "while an arrow is for the program, as a sequence");

    if (failures == 0) printf("all good\n");
    return failures;
}
