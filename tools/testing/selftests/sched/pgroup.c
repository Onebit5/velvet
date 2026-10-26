// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/pgroup.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * process groups: what a shell makes a job out of.
 */

#include "../kselftest.h"

static pid_t sleeper(void)
{
    pid_t i_pid = fork();

    if (i_pid == 0) {
        for (;;) {
            ksft_sleep_ms(100);
        }
    }
    return i_pid;
}

int main(void)
{
    int i_one;
    int i_two;

    ksft_print_header();
    ksft_set_plan(4);

    pid_t i_left = sleeper();
    pid_t i_right = sleeper();
    ksft_test_result(setpgid(i_left, 0) == 0
                     && setpgid(i_right, i_left) == 0
                     && getpgid(i_right) == i_left,
                     "two children are put in one group\n");

    ksft_test_result(kill(-i_left, SIGTERM) == 0
                     && waitpid(i_left, &i_one, 0) == i_left
                     && waitpid(i_right, &i_two, 0) == i_right
                     && ksft_killed_by(i_one, SIGTERM)
                     && ksft_killed_by(i_two, SIGTERM),
                     "and one signal to the group ends both, and only "
                     "them\n");

    ksft_test_result(setpgid(1, 0) == -1 && errno == ESRCH,
                     "a process that is not the caller's child is left be\n");
    ksft_test_result(kill(-30000, SIGTERM) == -1 && errno == ESRCH,
                     "and a group with nobody in it is ESRCH\n");
    ksft_finished();
}
