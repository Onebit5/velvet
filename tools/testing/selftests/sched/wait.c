// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/wait.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what a parent learns from wait about how its child ended.
 */

#include "../kselftest.h"

#include <sys/resource.h>

/*
 * the status wait hands back is what a shell turns into $?, and what
 * every program that starts another decides by: an exit code, or the
 * signal that ended it, each where the macros every libc has look
 */

static void caught(int i_sig)
{
    (void)i_sig;
}

int main(void)
{
    int i_status = 0;
    struct rusage usage;

    ksft_print_header();
    ksft_set_plan(7);

    pid_t i_pid = fork();
    if (i_pid == 0) {
        _exit(42);
    }
    ksft_test_result(waitpid(i_pid, &i_status, 0) == i_pid
                     && ksft_exited(i_status, 42),
                     "exit(42) reaches the parent as an exit with 42\n");

    /* a child that never ends on its own, so only the signal can end it */
    i_pid = fork();
    if (i_pid == 0) {
        for (;;) {
            ksft_sleep_ms(1000);
        }
    }
    ksft_test_result(waitpid(i_pid, &i_status, WNOHANG) == 0,
                     "WNOHANG says nothing has happened yet\n");
    kill(i_pid, SIGTERM);
    ksft_test_result(wait4(i_pid, &i_status, 0, &usage) == i_pid
                     && ksft_killed_by(i_status, SIGTERM)
                     && !WIFEXITED(i_status),
                     "a child ended by SIGTERM reaches it as that signal\n");

    ksft_test_result(waitpid(getpid(), &i_status, 0) == -1
                     && errno == ECHILD,
                     "wait refuses a pid that is not the caller's child\n");

    /* a SIGCHLD handler is told, and a wait interrupted by one restarts */
    struct sigaction act = { .sa_handler = caught, .sa_flags = SA_RESTART };
    sigaction(SIGCHLD, &act, NULL);
    i_pid = fork();
    if (i_pid == 0) {
        ksft_sleep_ms(100);
        _exit(3);
    }
    ksft_test_result(waitpid(i_pid, &i_status, 0) == i_pid
                     && ksft_exited(i_status, 3),
                     "SIGCHLD under SA_RESTART leaves the wait whole\n");

    /* a parent ignoring SIGCHLD has no children to wait for, ever */
    signal(SIGCHLD, SIG_IGN);
    i_pid = fork();
    if (i_pid == 0) {
        _exit(0);
    }
    ksft_sleep_ms(200);
    ksft_test_result(waitpid(i_pid, &i_status, 0) == -1 && errno == ECHILD,
                     "a child of a parent ignoring SIGCHLD is collected "
                     "for it\n");
    signal(SIGCHLD, SIG_DFL);

    ksft_test_result(waitpid(-1, &i_status, WNOHANG) == -1
                     && errno == ECHILD,
                     "and with none left, any child is none\n");
    ksft_finished();
}
