// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/kselftest.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what a selftest is written with: a libc, as any program has, and
 * results printed as TAP.
 */

#ifndef TOOLS_TESTING_SELFTESTS_KSELFTEST_H
#define TOOLS_TESTING_SELFTESTS_KSELFTEST_H

/*
 * a selftest is a program, and it sees the kernel from where a program
 * does: through a libc that has never heard of this kernel, built for
 * the system calls every x86_64 program makes. so it can only depend on
 * what the kernel promises ring 3, never on how it is built inside, and
 * a libc that works against it is the first thing it checks.
 *
 * that is what makes one worth having. a change that turns a selftest
 * from ok to not ok has broken a program somewhere, and it is the change
 * that gets fixed, not the test
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* how a test ends: every result ok, something not ok, or unable to run */
#define KSFT_PASS 0
#define KSFT_FAIL 1
#define KSFT_SKIP 4

/* one line of output, built whole and written with one call */
#define KSFT_LINE 256

/*
 * what has been counted so far. a program is one test, so one instance
 * is all there will ever be
 */
static struct {
    unsigned int u_planned;
    unsigned int u_run;
    unsigned int u_passed;
    unsigned int u_failed;
} s_ksft;

/*
 * a line formatted and written in one write, never through stdio's
 * buffer: a test forks, and a buffer the child inherits would be printed
 * twice
 */
static inline void ksft_print(const char *str_prefix, const char *str_fmt,
                              va_list args)
{
    char arr_line[KSFT_LINE];
    int i_at = snprintf(arr_line, sizeof arr_line, "%s", str_prefix);
    int i_more = vsnprintf(arr_line + i_at, sizeof arr_line - (size_t)i_at,
                           str_fmt, args);
    size_t sz_len = (size_t)i_at + (size_t)(i_more < 0 ? 0 : i_more);
    if (sz_len > sizeof arr_line - 1) {
        sz_len = sizeof arr_line - 1;
    }
    write(1, arr_line, sz_len);
}

static inline void ksft_print_header(void)
{
    write(1, "TAP version 13\n", 15);
}

static inline void ksft_set_plan(unsigned int u_plan)
{
    char arr_line[32];
    int i_len = snprintf(arr_line, sizeof arr_line, "1..%u\n", u_plan);

    s_ksft.u_planned = u_plan;
    write(1, arr_line, (size_t)i_len);
}

/* a diagnostic, which TAP marks with a # so no reader mistakes it */
static inline __attribute__((format(printf, 1, 2)))
void ksft_print_msg(const char *str_fmt, ...)
{
    va_list args;

    va_start(args, str_fmt);
    ksft_print("# ", str_fmt, args);
    va_end(args);
}

/* one result. the format is the test's name and ends in a newline */
static inline __attribute__((format(printf, 2, 3)))
void ksft_test_result(bool b_ok, const char *str_fmt, ...)
{
    char arr_prefix[32];
    va_list args;

    s_ksft.u_run++;
    if (b_ok) {
        s_ksft.u_passed++;
    } else {
        s_ksft.u_failed++;
    }
    snprintf(arr_prefix, sizeof arr_prefix, "%s %u ",
             b_ok ? "ok" : "not ok", s_ksft.u_run);

    va_start(args, str_fmt);
    ksft_print(arr_prefix, str_fmt, args);
    va_end(args);
}

/*
 * the totals, and the exit code: a pass only when every planned test ran
 * and passed
 */
static inline __attribute__((noreturn)) void ksft_finished(void)
{
    if (s_ksft.u_run != s_ksft.u_planned) {
        ksft_print_msg("Planned tests != run tests (%u != %u)\n",
                       s_ksft.u_planned, s_ksft.u_run);
    }
    ksft_print_msg("Totals: pass:%u fail:%u xfail:0 xpass:0 skip:0 "
                   "error:0\n", s_ksft.u_passed, s_ksft.u_failed);
    _exit(s_ksft.u_failed == 0 && s_ksft.u_run == s_ksft.u_planned
          ? KSFT_PASS : KSFT_FAIL);
}

static inline bool ksft_streq(const char *str_a, const char *str_b)
{
    return strcmp(str_a, str_b) == 0;
}

/*
 * the whole of a file, up to `sz_size` bytes, into `str_buf`: its length,
 * or -1 when it would not open
 */
static inline int64_t ksft_read_file(const char *str_path, char *str_buf,
                                     size_t sz_size)
{
    int i_fd = open(str_path, O_RDONLY);
    size_t sz_len = 0;

    if (i_fd < 0) {
        return -1;
    }
    while (sz_len < sz_size) {
        ssize_t i_got = read(i_fd, str_buf + sz_len, sz_size - sz_len);
        if (i_got <= 0) {
            break;
        }
        sz_len += (size_t)i_got;
    }
    close(i_fd);
    return (int64_t)sz_len;
}

/* whether `str_needle` is somewhere in the first `i_len` bytes of `str_text` */
static inline bool ksft_contains(const char *str_text, int64_t i_len,
                                 const char *str_needle)
{
    size_t sz_need = strlen(str_needle);

    for (int64_t i_at = 0; i_at + (int64_t)sz_need <= i_len; i_at++) {
        if (memcmp(str_text + i_at, str_needle, sz_need) == 0) {
            return true;
        }
    }
    return false;
}

/* a pause, in ms, that a signal does not cut short */
static inline void ksft_sleep_ms(long i_ms)
{
    struct timespec ts = { i_ms / 1000, (i_ms % 1000) * 1000000L };

    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

/* whether a wait status says the child ended by signal `i_sig` */
static inline bool ksft_killed_by(int i_status, int i_sig)
{
    return WIFSIGNALED(i_status) && WTERMSIG(i_status) == i_sig;
}

/* and whether it says the child exited with `i_code` */
static inline bool ksft_exited(int i_status, int i_code)
{
    return WIFEXITED(i_status) && WEXITSTATUS(i_status) == i_code;
}

#endif
