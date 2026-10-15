// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/reboot.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * stopping the machine is root's: a guest asking is refused, and the
 * machine is still here to say so.
 */

#include "../kselftest.h"

int main(int i_argc, char **pp_argv)
{
    (void)i_argc;
    (void)pp_argv;
    ksft_print_header();
    ksft_set_plan(2);

    ksft_test_result(ksft_syscall(SYS_REBOOT, REBOOT_POWER_OFF, 0, 0) == -1,
                     "a guest may not power the machine off\n");
    ksft_test_result(ksft_syscall(SYS_REBOOT, REBOOT_RESTART, 0, 0) == -1,
                     "nor restart it\n");
    ksft_finished();
}
