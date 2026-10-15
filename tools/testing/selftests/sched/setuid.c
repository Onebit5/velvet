// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/setuid.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * setuid from a program that is not root.
 */

#include "../kselftest.h"

int main(int i_argc, char **pp_argv)
{
    int i_uid = (int)ksft_syscall(SYS_GETUID, 0, 0, 0);

    (void)i_argc;
    (void)pp_argv;
    ksft_print_header();
    ksft_set_plan(2);

    ksft_test_result(ksft_syscall(SYS_SETUID, 0, 0, 0) == -1
                     && ksft_syscall(SYS_GETUID, 0, 0, 0) == i_uid,
                     "a guest may not become root\n");
    ksft_test_result(ksft_syscall(SYS_SETUID, i_uid, 0, 0) == 0,
                     "but may stay who it is\n");
    ksft_finished();
}
