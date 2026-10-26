// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/busy.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a program busy in a loop, making no calls at all, can still be ended.
 */

#include "../kselftest.h"

static pid_t spinner(void)
{
    pid_t i_pid = fork();

    if (i_pid == 0) {
        for (;;) {
            __asm__ volatile ("" ::: "memory");
        }
    }
    return i_pid;
}

int main(void)
{
    int i_status = -1;

    ksft_print_header();
    ksft_set_plan(2);

    pid_t i_pid = spinner();
    ksft_test_result(kill(i_pid, SIGKILL) == 0
                     && waitpid(i_pid, &i_status, 0) == i_pid
                     && ksft_killed_by(i_status, SIGKILL),
                     "SIGKILL ends a child that never makes a call\n");

    i_pid = spinner();
    ksft_test_result(kill(i_pid, SIGTERM) == 0
                     && waitpid(i_pid, &i_status, 0) == i_pid
                     && ksft_killed_by(i_status, SIGTERM),
                     "and so does SIGTERM, left to its default\n");
    ksft_finished();
}
