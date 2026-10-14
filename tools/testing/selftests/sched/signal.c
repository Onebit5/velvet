// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/signal.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * who a program may send a signal to.
 */

#include "../kselftest.h"

int main(int i_argc, char **pp_argv)
{
    int i_status = -1;

    (void)i_argc;
    (void)pp_argv;
    ksft_print_header();
    ksft_set_plan(3);

    /* init is root's, and a guest stopping it would stop the machine */
    ksft_test_result(sigsend(1, SIGTERM) == -1,
                     "a guest may not signal init\n");
    ksft_test_result(sigsend(30000, SIGTERM) == -1,
                     "nor a pid that is not there\n");

    int i_pid = fork();
    if (i_pid == 0) {
        for (;;) {
            sleep_ms(100);
        }
    }
    ksft_test_result(sigsend(i_pid, SIGTERM) == 0
                     && wait(i_pid, &i_status) == i_pid
                     && i_status == 128 + SIGTERM,
                     "but may signal its own child\n");
    ksft_finished();
}
