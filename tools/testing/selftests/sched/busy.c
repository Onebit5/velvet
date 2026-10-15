// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/busy.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a program busy in a loop, making no calls at all, can still be ended.
 */

#include "../kselftest.h"

static int spinner(void)
{
    int i_pid = fork();

    if (i_pid == 0) {
        for (;;) {
            __asm__ volatile ("" ::: "memory");
        }
    }
    return i_pid;
}

int main(int i_argc, char **pp_argv)
{
    int i_status = -1;

    (void)i_argc;
    (void)pp_argv;
    ksft_print_header();
    ksft_set_plan(2);

    int i_pid = spinner();
    ksft_test_result(sigsend(i_pid, SIGKILL) == 0
                     && wait(i_pid, &i_status) == i_pid
                     && i_status == 128 + SIGKILL,
                     "SIGKILL ends a child that never makes a call\n");

    i_pid = spinner();
    ksft_test_result(sigsend(i_pid, SIGTERM) == 0
                     && wait(i_pid, &i_status) == i_pid
                     && i_status == 128 + SIGTERM,
                     "and so does SIGTERM, left to its default\n");
    ksft_finished();
}
