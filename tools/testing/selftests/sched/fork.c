// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/fork.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a forked child is its parent's copy: the environment it was holding,
 * and what it had arranged to do with each signal.
 */

#include "../kselftest.h"

#define NAME  "SELFTEST_FORK"
#define VALUE "carried"

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
    ksft_set_plan(2);

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
    ksft_finished();
}
