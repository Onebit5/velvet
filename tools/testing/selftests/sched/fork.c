// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/fork.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a forked child is its parent's copy: the environment it was holding,
 * what it had arranged to do with each signal, and memory of its own.
 */

#include "../kselftest.h"

#define NAME  "SELFTEST_FORK"
#define VALUE "carried"

/*
 * a page the parent has written, so that after a fork it is the one
 * page both hold, until one of them writes to it
 */
static char s_arr_page[4096] __attribute__((aligned(4096))) = "parent";

/* how many children, enough that every core of a small machine runs one */
#define CHILDREN 8

static int64_t set_var(const char *str_name, const char *str_value)
{
    return ksft_syscall5(SYS_SETENV, (int64_t)str_name,
                         (int64_t)ksft_strlen(str_name), (int64_t)str_value,
                         (int64_t)ksft_strlen(str_value), 0);
}

static int64_t get_var(const char *str_name, char *str_out, size_t sz_size)
{
    return ksft_syscall5(SYS_GETENV, (int64_t)str_name,
                         (int64_t)ksft_strlen(str_name), (int64_t)str_out,
                         (int64_t)sz_size, 0);
}

int main(int i_argc, char **pp_argv)
{
    char arr_value[32];
    int i_status = -1;

    (void)i_argc;
    (void)pp_argv;
    ksft_print_header();
    ksft_set_plan(3);

    set_var(NAME, VALUE);
    int i_pid = fork();
    if (i_pid == 0) {
        int64_t i_len = get_var(NAME, arr_value, sizeof arr_value);
        exit(i_len == (int64_t)ksft_strlen(VALUE)
             && ksft_memeq(arr_value, VALUE, (size_t)i_len) ? 0 : 1);
    }
    wait(i_pid, &i_status);
    ksft_test_result(i_status == 0,
                     "a forked child holds its parent's environment\n");

    ksft_syscall(SYS_SIGNAL, SIGTERM, SIG_IGNORE, 0);
    i_pid = fork();
    if (i_pid == 0) {
        sleep_ms(300);
        exit(0);
    }
    sigsend(i_pid, SIGTERM);
    wait(i_pid, &i_status);
    ksft_test_result(i_status == 0,
                     "and ignores what its parent chose to ignore\n");

    /*
     * a read is the kernel writing into the child's memory, and that is
     * a write to the shared page like any other: the child gets a copy
     * of its own and the parent's stays as it was, whichever core the
     * child ran on
     */
    bool b_kept = true;
    for (int i_at = 0; i_at < CHILDREN && b_kept; i_at++) {
        int32_t arr_fds[2];
        if (pipe(arr_fds) != 0 || write(arr_fds[1], "child", 6) != 6) {
            b_kept = false;
            break;
        }
        s_arr_page[0] = 'p';
        i_pid = fork();
        if (i_pid == 0) {
            exit(read(arr_fds[0], s_arr_page, 6) == 6
                 && ksft_streq(s_arr_page, "child") ? 0 : 1);
        }
        wait(i_pid, &i_status);
        close(arr_fds[0]);
        close(arr_fds[1]);
        b_kept = i_status == 0 && ksft_streq(s_arr_page, "parent");
    }
    ksft_test_result(b_kept,
                     "what the kernel writes into a child is the child's\n");
    ksft_finished();
}
