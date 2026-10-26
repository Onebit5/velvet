// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/reboot.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * stopping the machine is root's: a guest asking is refused, and the
 * machine is still here to say so.
 */

#include "../kselftest.h"

#include <sys/reboot.h>

int main(void)
{
    ksft_print_header();
    ksft_set_plan(2);

    ksft_test_result(reboot(RB_POWER_OFF) == -1 && errno == EPERM,
                     "a guest may not power the machine off\n");
    ksft_test_result(reboot(RB_AUTOBOOT) == -1 && errno == EPERM,
                     "nor restart it\n");
    ksft_finished();
}
