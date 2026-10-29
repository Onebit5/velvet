// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/exec.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * exec: a process becoming another program, and what it keeps.
 *
 * the program it becomes is this one, started with "again". that image
 * checks what it was handed and answers in its exit code, since the TAP
 * count lives in the memory exec throws away
 */

#include "../kselftest.h"

#define AGAIN       "again"
#define EXEC_FAILED 9

/* 0 when all is as it should be, else which part was not */
static int again(int i_argc, char **pp_argv)
{
    char arr_text[16];

    if (i_argc != 5) {
        return 1;
    }
    if (atoi(pp_argv[2]) != getpid()) {
        return 2;
    }
    if (read(atoi(pp_argv[3]), arr_text, 15) <= 0
        || memcmp(arr_text, "velvet", 6) != 0) {
        return 3;
    }
    if (fcntl(atoi(pp_argv[4]), F_GETFD) != -1 || errno != EBADF) {
        return 4;
    }
    return 0;
}

int main(int i_argc, char **pp_argv)
{
    char arr_pid[12];
    char arr_fd[12];
    char arr_gone[12];
    int i_status = -1;

    if (i_argc >= 2 && ksft_streq(pp_argv[1], AGAIN)) {
        return again(i_argc, pp_argv);
    }

    ksft_print_header();
    ksft_set_plan(7);

    int i_fd = open("/proc/version", O_RDONLY);
    int i_closing = open("/proc/version", O_RDONLY | O_CLOEXEC);
    pid_t i_pid = fork();
    if (i_pid == 0) {
        char *arr_args[] = { pp_argv[0], AGAIN, arr_pid, arr_fd, arr_gone,
                             NULL };

        snprintf(arr_pid, sizeof arr_pid, "%d", getpid());
        snprintf(arr_fd, sizeof arr_fd, "%d", i_fd);
        snprintf(arr_gone, sizeof arr_gone, "%d", i_closing);
        execv(pp_argv[0], arr_args);
        _exit(EXEC_FAILED);
    }
    waitpid(i_pid, &i_status, 0);
    int i_code = WIFEXITED(i_status) ? WEXITSTATUS(i_status) : -1;

    ksft_test_result(i_code != EXEC_FAILED && i_code != 1,
                     "exec starts the program, with its arguments\n");
    ksft_test_result(i_code == 0 || i_code >= 3,
                     "in the same process, under the same pid\n");
    ksft_test_result(i_code == 0 || i_code == 4,
                     "holding the descriptors it held\n");
    ksft_test_result(i_code == 0,
                     "except those marked to close on exec\n");

    char *arr_none[] = { "nothing", NULL };
    ksft_test_result(execv("/nothing/here", arr_none) == -1
                     && errno == ENOENT,
                     "a path with nothing there is ENOENT, and the caller "
                     "carries on\n");
    ksft_test_result(execv("/", arr_none) == -1 && errno == EACCES,
                     "a directory is not a program\n");
    ksft_test_result(execv(pp_argv[0], (char *const *)0xffff800000000000ull)
                     == -1 && errno == EFAULT,
                     "an argv in the kernel's memory is refused\n");
    ksft_finished();
}
