// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/wait.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what a parent learns from wait about how its child ended.
 */

#include "../kselftest.h"

/*
 * the status wait hands back is what a shell turns into $?, and what
 * every program that starts another decides by: a child that exits is
 * reported with its own number, and a child a signal ends with 128 plus
 * the signal. programs already depend on both
 */

/* no child ends with this, so it means there was nothing to read */
#define NO_STATUS (-1000)

static int status_of(int i_pid)
{
    int i_status = 0;

    if (i_pid <= 0 || wait(i_pid, &i_status) != i_pid) {
        return NO_STATUS;
    }
    return i_status;
}

static void expect(int i_got, int i_want, const char *str_what)
{
    if (i_got != i_want) {
        ksft_print_msg("wanted %d, read %d\n", i_want, i_got);
    }
    ksft_test_result(i_got == i_want, "%s\n", str_what);
}

int main(int i_argc, char **pp_argv)
{
    (void)i_argc;
    (void)pp_argv;

    ksft_print_header();
    ksft_set_plan(3);

    int i_pid = fork();
    if (i_pid == 0) {
        exit(42);
    }
    expect(status_of(i_pid), 42, "exit(42) reaches the parent as 42");

    /* a child that never ends on its own, so only the signal can end it */
    i_pid = fork();
    if (i_pid == 0) {
        for (;;) {
            sleep_ms(1000);
        }
    }
    sigsend(i_pid, SIGTERM);
    expect(status_of(i_pid), 128 + SIGTERM,
           "a child ended by SIGTERM reaches the parent as 128 + 15");

    int i_status = 0;
    expect(wait(getpid(), &i_status), -1,
           "wait refuses a pid that is not the caller's child");

    ksft_finished();
}
