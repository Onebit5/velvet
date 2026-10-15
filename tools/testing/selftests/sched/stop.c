// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/stop.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * stopping and continuing a process, and what wait says about it.
 */

#include "../kselftest.h"

/* a child that makes no calls at all, so nothing waits for one to end */
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
    ksft_set_plan(6);

    int i_pid = spinner();
    ksft_test_result(sigsend(i_pid, SIGSTOP) == 0
                     && waitpid(i_pid, &i_status, WUNTRACED) == i_pid
                     && i_status == (WAIT_STOPPED | SIGSTOP),
                     "a child busy in a loop stops when told to\n");
    ksft_test_result(waitpid(i_pid, &i_status, WUNTRACED | WNOHANG) == 0,
                     "and the stop is told once\n");
    ksft_test_result(sigsend(i_pid, SIGCONT) == 0
                     && waitpid(i_pid, &i_status, WNOHANG) == 0,
                     "SIGCONT sets it going again, still running\n");
    ksft_test_result(sigsend(i_pid, SIGKILL) == 0
                     && waitpid(i_pid, &i_status, 0) == i_pid
                     && i_status == 128 + SIGKILL,
                     "and it can be killed\n");

    i_pid = spinner();
    sigsend(i_pid, SIGSTOP);
    waitpid(i_pid, &i_status, WUNTRACED);
    ksft_test_result(sigsend(i_pid, SIGKILL) == 0
                     && waitpid(i_pid, &i_status, 0) == i_pid
                     && i_status == 128 + SIGKILL,
                     "SIGKILL ends a child that is stopped\n");

    i_pid = fork();
    if (i_pid == 0) {
        exit(5);
    }
    ksft_test_result(waitpid(-1, &i_status, 0) == i_pid && i_status == 5,
                     "wait(-1) collects whichever child ended\n");
    ksft_finished();
}
