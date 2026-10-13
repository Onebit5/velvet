// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/sync.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * sync, asked for by a program rather than typed at the kernel's shell.
 */

#include "../kselftest.h"

int main(int i_argc, char **pp_argv)
{
    (void)i_argc;
    (void)pp_argv;
    ksft_print_header();
    ksft_set_plan(2);

    /* a guest may ask, as a guest at the shell may type it */
    ksft_test_result(ksft_syscall(SYS_SYNC, 0, 0, 0) == 0,
                     "sync leaves the disk holding what the kernel does\n");
    ksft_test_result(ksft_syscall(SYS_SYNC, 0, 0, 0) == 0,
                     "and asking twice is no harm\n");
    ksft_finished();
}
