// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/setuid.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * setuid from a program that is not root.
 */

#include "../kselftest.h"

int main(void)
{
    uid_t u_uid = getuid();

    ksft_print_header();
    ksft_set_plan(2);

    ksft_test_result(setuid(0) == -1 && errno == EPERM && getuid() == u_uid,
                     "a guest may not become root\n");
    ksft_test_result(setuid(u_uid) == 0,
                     "but may stay who it is\n");
    ksft_finished();
}
