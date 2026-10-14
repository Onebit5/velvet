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

static int to_int(const char *str_num)
{
    int i_num = 0;

    while (*str_num >= '0' && *str_num <= '9') {
        i_num = i_num * 10 + (*str_num++ - '0');
    }
    return i_num;
}

static void to_text(int i_num, char *str_out)
{
    char arr_rev[12];
    int i_len = 0;

    do {
        arr_rev[i_len++] = (char)('0' + i_num % 10);
        i_num /= 10;
    } while (i_num > 0);
    while (i_len > 0) {
        *str_out++ = arr_rev[--i_len];
    }
    *str_out = '\0';
}

/* 0 when all is as it should be, else which part was not */
static int again(int i_argc, char **pp_argv)
{
    char arr_text[16];

    if (i_argc != 4) {
        return 1;
    }
    if (to_int(pp_argv[2]) != getpid()) {
        return 2;
    }
    if (read(to_int(pp_argv[3]), arr_text, 15) <= 0
        || !ksft_memeq(arr_text, "velvet", 6)) {
        return 3;
    }
    return 0;
}

int main(int i_argc, char **pp_argv)
{
    char arr_pid[12];
    char arr_fd[12];
    int i_status = -1;

    if (i_argc >= 2 && ksft_streq(pp_argv[1], AGAIN)) {
        exit(again(i_argc, pp_argv));
    }

    ksft_print_header();
    ksft_set_plan(5);

    int i_fd = open("/proc/version");
    int i_pid = fork();
    if (i_pid == 0) {
        char *arr_args[] = { pp_argv[0], AGAIN, arr_pid, arr_fd, NULL };

        to_text(getpid(), arr_pid);
        to_text(i_fd, arr_fd);
        execv(pp_argv[0], arr_args);
        exit(EXEC_FAILED);
    }
    wait(i_pid, &i_status);

    ksft_test_result(i_status != EXEC_FAILED && i_status != 1,
                     "exec starts the program, with its arguments\n");
    ksft_test_result(i_status == 0 || i_status == 3,
                     "in the same process, under the same pid\n");
    ksft_test_result(i_status == 0,
                     "holding the descriptors it held\n");

    char *arr_none[] = { "nothing", NULL };
    ksft_test_result(execv("/nothing/here", arr_none) == -1,
                     "a path with nothing there is -1, and the caller "
                     "carries on\n");
    ksft_test_result(execv(pp_argv[0],
                           (char *const *)0xffff800000000000ull) == -1,
                     "an argv in the kernel's memory is refused\n");
    ksft_finished();
}
