// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/kselftest.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what a selftest is written with: the kernel's calls, made the way ring
 * 3 makes them, and results printed as TAP.
 */

#ifndef TOOLS_TESTING_SELFTESTS_KSELFTEST_H
#define TOOLS_TESTING_SELFTESTS_KSELFTEST_H

/*
 * a selftest is a program, and it sees the kernel from where a program
 * does. the build hands it the uapi headers and the compiler's own
 * freestanding ones and nothing else, so it can only depend on what the
 * kernel promises ring 3, never on how the kernel is built inside.
 *
 * that is what makes one worth having. a change that turns a selftest
 * from ok to not ok has broken a program somewhere, and it is the change
 * that gets fixed, not the test
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <velvet/signal.h>
#include <velvet/syscall.h>

/* how a test ends: every result ok, something not ok, or unable to run */
#define KSFT_PASS 0
#define KSFT_FAIL 1
#define KSFT_SKIP 4

/* one line of output, built whole and written with one call */
#define KSFT_LINE 256

static inline int64_t ksft_syscall(int64_t i_nr, int64_t i_a0, int64_t i_a1,
                                   int64_t i_a2)
{
    int64_t i_ret;

    __asm__ volatile ("syscall"
                      : "=a"(i_ret)
                      : "a"(i_nr), "D"(i_a0), "S"(i_a1), "d"(i_a2)
                      : "rcx", "r11", "memory");
    return i_ret;
}

/* the same, for a call that takes five */
static inline int64_t ksft_syscall5(int64_t i_nr, int64_t i_a0, int64_t i_a1,
                                    int64_t i_a2, int64_t i_a3, int64_t i_a4)
{
    int64_t i_ret;
    register int64_t i_r10 __asm__("r10") = i_a3;
    register int64_t i_r8 __asm__("r8") = i_a4;

    __asm__ volatile ("syscall"
                      : "=a"(i_ret)
                      : "a"(i_nr), "D"(i_a0), "S"(i_a1), "d"(i_a2),
                        "r"(i_r10), "r"(i_r8)
                      : "rcx", "r11", "memory");
    return i_ret;
}

/*
 * the calls a test makes, under the names a program knows them by. each
 * is one syscall and nothing more, so what a test exercises is the
 * kernel and not a library in front of it
 */
static inline int64_t write(int i_fd, const void *p_buf, size_t sz_len)
{
    return ksft_syscall(SYS_WRITE, i_fd, (int64_t)p_buf, (int64_t)sz_len);
}

static inline __attribute__((noreturn)) void exit(int i_code)
{
    ksft_syscall(SYS_EXIT, i_code, 0, 0);
    __builtin_unreachable();
}

/* 0 in the child, the child's pid in the parent, -1 when there is none */
static inline int fork(void)
{
    return (int)ksft_syscall(SYS_FORK, 0, 0, 0);
}

/*
 * block until the child ends, and put how it ended in *p_status: what it
 * passed to exit, or 128 plus the signal that ended it. -1, and nothing
 * written, for a pid that is not the caller's child
 */
static inline int wait(int i_pid, int *p_status)
{
    return (int)ksft_syscall(SYS_WAIT, i_pid, (int64_t)p_status, 0);
}

static inline int getpid(void)
{
    return (int)ksft_syscall(SYS_GETPID, 0, 0, 0);
}

static inline int sigsend(int i_pid, int i_sig)
{
    return (int)ksft_syscall(SYS_SIGSEND, i_pid, i_sig, 0);
}

static inline int sleep_ms(int64_t i_ms)
{
    return (int)ksft_syscall(SYS_SLEEP, i_ms, 0, 0);
}

static inline size_t ksft_strlen(const char *str_text)
{
    size_t sz_len = 0;

    while (str_text[sz_len] != '\0') {
        sz_len++;
    }
    return sz_len;
}

/* whether `sz_len` bytes are the same, which tests ask often enough */
static inline bool ksft_memeq(const void *p_a, const void *p_b, size_t sz_len)
{
    const char *str_a = p_a;
    const char *str_b = p_b;

    for (size_t sz_at = 0; sz_at < sz_len; sz_at++) {
        if (str_a[sz_at] != str_b[sz_at]) {
            return false;
        }
    }
    return true;
}

static inline bool ksft_streq(const char *str_a, const char *str_b)
{
    size_t sz_len = ksft_strlen(str_a);

    return sz_len == ksft_strlen(str_b) && ksft_memeq(str_a, str_b, sz_len);
}

/* a file to read: its descriptor, or -1 */
static inline int open(const char *str_path)
{
    return (int)ksft_syscall(SYS_OPEN, (int64_t)str_path,
                             (int64_t)ksft_strlen(str_path), 0);
}

static inline int64_t read(int i_fd, void *p_buf, size_t sz_len)
{
    return ksft_syscall(SYS_READ, i_fd, (int64_t)p_buf, (int64_t)sz_len);
}

static inline int close(int i_fd)
{
    return (int)ksft_syscall(SYS_CLOSE, i_fd, 0, 0);
}

/*
 * the name of entry `sz_index` in the directory `str_path` into `str_buf`,
 * a directory's with a / after it: its length, or -1 past the last
 */
static inline int64_t readdir(const char *str_path, size_t sz_index,
                              char *str_buf, size_t sz_size)
{
    return ksft_syscall5(SYS_READDIR, (int64_t)sz_index, (int64_t)str_buf,
                         (int64_t)sz_size, (int64_t)str_path,
                         (int64_t)ksft_strlen(str_path));
}

/*
 * the whole of a file, up to `sz_size` bytes, into `str_buf`: its length,
 * or -1 when it would not open
 */
static inline int64_t ksft_read_file(const char *str_path, char *str_buf,
                                     size_t sz_size)
{
    int i_fd = open(str_path);
    size_t sz_len = 0;

    if (i_fd < 0) {
        return -1;
    }
    while (sz_len < sz_size) {
        int64_t i_got = read(i_fd, str_buf + sz_len, sz_size - sz_len);
        if (i_got <= 0) {
            break;
        }
        sz_len += (size_t)i_got;
    }
    close(i_fd);
    return (int64_t)sz_len;
}

/* process `i_pid` into group `i_pgid`, 0 meaning the caller and its own */
static inline int setpgid(int i_pid, int i_pgid)
{
    return (int)ksft_syscall(SYS_SETPGID, i_pid, i_pgid, 0);
}

/* a pipe: its reading end in arr_fds[0] and its writing end in arr_fds[1] */
static inline int pipe(int32_t arr_fds[2])
{
    return (int)ksft_syscall(SYS_PIPE, (int64_t)arr_fds, 0, 0);
}

/* descriptor `i_to` becomes another handle on `i_from`: `i_to`, or -1 */
static inline int dup2(int i_from, int i_to)
{
    return (int)ksft_syscall(SYS_DUP2, i_from, i_to, 0);
}

/*
 * become the program at `str_path`, with `pp_argv`, NULL-ended, as its
 * arguments and the caller's environment as its own. returns only when
 * it failed, with -1
 */
static inline int execv(const char *str_path, char *const pp_argv[])
{
    return (int)ksft_syscall(SYS_EXECVE, (int64_t)str_path,
                             (int64_t)ksft_strlen(str_path),
                             (int64_t)pp_argv);
}

/*
 * the kernel's log: SYSLOG_SIZE_BUFFER for how big all of it is, or
 * SYSLOG_READ_ALL for the newest `i_len` bytes of it in `str_buf`
 */
static inline int64_t klogctl(int i_type, char *str_buf, int64_t i_len)
{
    return ksft_syscall(SYS_SYSLOG, i_type, (int64_t)str_buf, i_len);
}

/* whether `str_needle` is somewhere in the first `i_len` bytes of `str_text` */
static inline bool ksft_contains(const char *str_text, int64_t i_len,
                                 const char *str_needle)
{
    size_t sz_need = ksft_strlen(str_needle);

    for (int64_t i_at = 0; i_at + (int64_t)sz_need <= i_len; i_at++) {
        if (ksft_memeq(str_text + i_at, str_needle, sz_need)) {
            return true;
        }
    }
    return false;
}

/*
 * enough of printf for a test to say what it saw: %s, %d, %u and %x, an
 * l in front for the 64-bit ones, and %%
 */
static inline size_t ksft_put(char *p_line, size_t sz_at, char c_ch)
{
    if (sz_at < KSFT_LINE) {
        p_line[sz_at++] = c_ch;
    }
    return sz_at;
}

static inline size_t ksft_put_str(char *p_line, size_t sz_at,
                                  const char *str_text)
{
    if (str_text == NULL) {
        str_text = "(null)";
    }
    while (*str_text != '\0') {
        sz_at = ksft_put(p_line, sz_at, *str_text++);
    }
    return sz_at;
}

static inline size_t ksft_put_num(char *p_line, size_t sz_at,
                                  uint64_t u_value, unsigned int u_base,
                                  bool b_negative)
{
    char arr_digits[24];
    size_t sz_count = 0;

    do {
        arr_digits[sz_count++] = "0123456789abcdef"[u_value % u_base];
        u_value /= u_base;
    } while (u_value != 0);
    if (b_negative) {
        sz_at = ksft_put(p_line, sz_at, '-');
    }
    while (sz_count > 0) {
        sz_at = ksft_put(p_line, sz_at, arr_digits[--sz_count]);
    }
    return sz_at;
}

static inline size_t ksft_format(char *p_line, size_t sz_at,
                                 const char *str_fmt, va_list args)
{
    for (const char *p_ch = str_fmt; *p_ch != '\0'; p_ch++) {
        if (*p_ch != '%') {
            sz_at = ksft_put(p_line, sz_at, *p_ch);
            continue;
        }
        bool b_long = p_ch[1] == 'l';
        p_ch += b_long ? 2 : 1;

        switch (*p_ch) {
        case 's':
            sz_at = ksft_put_str(p_line, sz_at, va_arg(args, const char *));
            break;
        case 'd': {
            int64_t i_value = b_long ? va_arg(args, int64_t)
                                     : va_arg(args, int);
            uint64_t u_mag = i_value < 0 ? -(uint64_t)i_value
                                         : (uint64_t)i_value;
            sz_at = ksft_put_num(p_line, sz_at, u_mag, 10, i_value < 0);
            break;
        }
        case 'u':
        case 'x': {
            uint64_t u_value = b_long ? va_arg(args, uint64_t)
                                      : va_arg(args, unsigned int);
            sz_at = ksft_put_num(p_line, sz_at, u_value,
                                 *p_ch == 'u' ? 10 : 16, false);
            break;
        }
        case '\0':
            return sz_at;
        default:
            sz_at = ksft_put(p_line, sz_at, *p_ch);
            break;
        }
    }
    return sz_at;
}

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

static inline void ksft_print(const char *str_prefix, const char *str_fmt,
                              va_list args)
{
    char arr_line[KSFT_LINE];
    size_t sz_at = ksft_put_str(arr_line, 0, str_prefix);

    sz_at = ksft_format(arr_line, sz_at, str_fmt, args);
    write(1, arr_line, sz_at);
}

static inline void ksft_print_header(void)
{
    write(1, "TAP version 13\n", 15);
}

static inline void ksft_set_plan(unsigned int u_plan)
{
    char arr_line[32];
    size_t sz_at = ksft_put_str(arr_line, 0, "1..");

    s_ksft.u_planned = u_plan;
    sz_at = ksft_put_num(arr_line, sz_at, u_plan, 10, false);
    sz_at = ksft_put(arr_line, sz_at, '\n');
    write(1, arr_line, sz_at);
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
    size_t sz_at = ksft_put_str(arr_prefix, 0, b_ok ? "ok " : "not ok ");
    va_list args;

    s_ksft.u_run++;
    if (b_ok) {
        s_ksft.u_passed++;
    } else {
        s_ksft.u_failed++;
    }
    sz_at = ksft_put_num(arr_prefix, sz_at, s_ksft.u_run, 10, false);
    sz_at = ksft_put(arr_prefix, sz_at, ' ');
    arr_prefix[sz_at] = '\0';      /* "not ok 4294967295 " is 18 */

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
    exit(s_ksft.u_failed == 0 && s_ksft.u_run == s_ksft.u_planned
         ? KSFT_PASS : KSFT_FAIL);
}

int main(int i_argc, char **pp_argv);

/*
 * where the kernel starts a program: argc in rdi and argv in rsi, which
 * is where a C function finds its first two arguments, so this is an
 * ordinary function. what main returns is the exit status
 */
__attribute__((used, noreturn)) void _start(int i_argc, char **pp_argv)
{
    exit(main(i_argc, pp_argv));
}

#endif
