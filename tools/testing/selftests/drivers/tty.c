// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/drivers/tty.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the terminal: who is in front of it, and who may say so.
 *
 * a process that is not in front has what it prints dropped, so the
 * checks made while the child holds the terminal are kept and printed
 * once the child has handed it back
 */

#include "../kselftest.h"

int main(int i_argc, char **pp_argv)
{
    int i_status = -1;
    int i_me = getpid();

    (void)i_argc;
    (void)pp_argv;
    ksft_print_header();
    ksft_set_plan(6);

    /* run as a job of one, so its group is its pid */
    ksft_test_result(tcgetpgrp() == i_me,
                     "the job being run is in front of the terminal\n");
    ksft_test_result(ksft_syscall(SYS_TTYATTACH, 1, 0, 0) == -1,
                     "a guest may not take a console\n");
    ksft_test_result(tcsetpgrp(30000) == -1 && tcsetpgrp(1) == -1,
                     "nor hand it to nobody, or to root's group\n");

    int i_pid = fork();
    if (i_pid == 0) {
        sleep_ms(300);
        exit(tcsetpgrp(i_me) == 0 ? 0 : 1);
    }
    setpgid(i_pid, 0);
    bool b_given = tcsetpgrp(i_pid) == 0 && tcgetpgrp() == i_pid;
    bool b_refused = tcsetpgrp(i_me) == -1;
    wait(i_pid, &i_status);

    ksft_test_result(b_given,
                     "the group in front may give the terminal away\n");
    ksft_test_result(b_refused,
                     "and, no longer in front, may not take it back\n");
    ksft_test_result(i_status == 0 && tcgetpgrp() == i_me,
                     "but the group it went to may hand it back\n");
    ksft_finished();
}
