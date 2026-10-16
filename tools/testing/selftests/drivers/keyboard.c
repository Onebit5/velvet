// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/drivers/keyboard.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a program waiting for a key, and nobody typing: an interrupt has to
 * end the wait anyway, or a reader can only ever be woken by a key.
 */

#include "../kselftest.h"

/* a child that interrupts its parent once the parent has had time to wait */
static int interrupter(int i_parent)
{
    int i_pid = fork();

    if (i_pid == 0) {
        sleep_ms(300);
        sigsend(i_parent, SIGINT);
        exit(0);
    }
    return i_pid;
}

int main(int i_argc, char **pp_argv)
{
    char arr_line[16];
    int i_status;

    (void)i_argc;
    (void)pp_argv;
    ksft_print_header();
    ksft_set_plan(2);

    /* ignored, so it cuts the wait short and leaves the test alive */
    ksft_syscall(SYS_SIGNAL, SIGINT, SIG_IGNORE, 0);

    int i_pid = interrupter(getpid());
    ksft_test_result(read(0, arr_line, sizeof arr_line) == -1,
                     "an interrupt ends a read of the keyboard\n");
    wait(i_pid, &i_status);

    i_pid = interrupter(getpid());
    ksft_test_result(ksft_syscall(SYS_GETKEY, 0, 0, 0) == -1,
                     "and a wait for one key\n");
    wait(i_pid, &i_status);
    ksft_finished();
}
