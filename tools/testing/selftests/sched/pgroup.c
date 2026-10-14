// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/pgroup.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * process groups: what a shell makes a job out of.
 */

#include "../kselftest.h"

static int sleeper(void)
{
    int i_pid = fork();

    if (i_pid == 0) {
        for (;;) {
            sleep_ms(100);
        }
    }
    return i_pid;
}

int main(int i_argc, char **pp_argv)
{
    int i_one;
    int i_two;

    (void)i_argc;
    (void)pp_argv;
    ksft_print_header();
    ksft_set_plan(4);

    int i_left = sleeper();
    int i_right = sleeper();
    ksft_test_result(setpgid(i_left, 0) == 0
                     && setpgid(i_right, i_left) == 0,
                     "two children are put in one group\n");

    ksft_test_result(sigsend(-i_left, SIGTERM) == 0
                     && wait(i_left, &i_one) == i_left
                     && wait(i_right, &i_two) == i_right
                     && i_one == 128 + SIGTERM && i_two == 128 + SIGTERM,
                     "and one signal to the group ends both, and only "
                     "them\n");

    ksft_test_result(setpgid(1, 0) == -1,
                     "a process that is not the caller's child is left be\n");
    ksft_test_result(sigsend(-30000, SIGTERM) == -1,
                     "and a group with nobody in it is -1\n");
    ksft_finished();
}
