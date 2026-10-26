// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/stop.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * stopping and continuing a process, and what wait says about it.
 */

#include "../kselftest.h"

/* a child that makes no calls at all, so nothing waits for one to end */
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
    ksft_set_plan(7);

    pid_t i_pid = spinner();
    ksft_test_result(kill(i_pid, SIGSTOP) == 0
                     && waitpid(i_pid, &i_status, WUNTRACED) == i_pid
                     && WIFSTOPPED(i_status) && WSTOPSIG(i_status) == SIGSTOP,
                     "a child busy in a loop stops when told to\n");
    ksft_test_result(waitpid(i_pid, &i_status, WUNTRACED | WNOHANG) == 0,
                     "and the stop is told once\n");
    ksft_test_result(kill(i_pid, SIGCONT) == 0
                     && waitpid(i_pid, &i_status, WCONTINUED) == i_pid
                     && WIFCONTINUED(i_status),
                     "SIGCONT sets it going again, and says so\n");
    ksft_test_result(waitpid(i_pid, &i_status, WNOHANG) == 0,
                     "still running\n");
    ksft_test_result(kill(i_pid, SIGKILL) == 0
                     && waitpid(i_pid, &i_status, 0) == i_pid
                     && ksft_killed_by(i_status, SIGKILL),
                     "and it can be killed\n");

    i_pid = spinner();
    kill(i_pid, SIGSTOP);
    waitpid(i_pid, &i_status, WUNTRACED);
    ksft_test_result(kill(i_pid, SIGKILL) == 0
                     && waitpid(i_pid, &i_status, 0) == i_pid
                     && ksft_killed_by(i_status, SIGKILL),
                     "SIGKILL ends a child that is stopped\n");

    i_pid = fork();
    if (i_pid == 0) {
        _exit(5);
    }
    ksft_test_result(waitpid(-1, &i_status, 0) == i_pid
                     && ksft_exited(i_status, 5),
                     "wait(-1) collects whichever child ended\n");
    ksft_finished();
}
