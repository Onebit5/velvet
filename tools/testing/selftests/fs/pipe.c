// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/pipe.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * pipes and dup2, from ring 3: what a shell builds a pipeline out of.
 */

#include "../kselftest.h"

#define TEXT_MAX 64

/* everything the pipe holds until every writer is gone, and how much */
static int64_t drain(int i_fd, char *str_buf, size_t sz_size)
{
    size_t sz_len = 0;

    for (;;) {
        int64_t i_got = read(i_fd, str_buf + sz_len, sz_size - sz_len);
        if (i_got <= 0) {
            return i_got < 0 ? -1 : (int64_t)sz_len;
        }
        sz_len += (size_t)i_got;
    }
}

int main(int i_argc, char **pp_argv)
{
    char arr_text[TEXT_MAX];
    int32_t arr_fds[2];
    int i_status;

    (void)i_argc;
    (void)pp_argv;
    ksft_print_header();
    ksft_set_plan(6);

    ksft_test_result(pipe(arr_fds) == 0 && arr_fds[0] >= 0
                     && arr_fds[1] >= 0 && arr_fds[0] != arr_fds[1],
                     "pipe gives two descriptors\n");
    write(arr_fds[1], "one way", 7);
    ksft_test_result(read(arr_fds[0], arr_text, TEXT_MAX) == 7
                     && ksft_memeq(arr_text, "one way", 7),
                     "what goes in one end comes out of the other\n");

    /* the child never closes its end: leaving has to do that for it */
    int i_pid = fork();
    if (i_pid == 0) {
        write(arr_fds[1], "from the child", 14);
        exit(0);
    }
    close(arr_fds[1]);
    ksft_test_result(drain(arr_fds[0], arr_text, TEXT_MAX) == 14
                     && ksft_memeq(arr_text, "from the child", 14),
                     "a writer that exits is a pipe at its end\n");
    wait(i_pid, &i_status);
    close(arr_fds[0]);

    pipe(arr_fds);
    i_pid = fork();
    if (i_pid == 0) {
        dup2(arr_fds[1], 1);
        close(arr_fds[1]);
        close(arr_fds[0]);
        write(1, "on stdout", 9);
        exit(0);
    }
    close(arr_fds[1]);
    ksft_test_result(drain(arr_fds[0], arr_text, TEXT_MAX) == 9
                     && ksft_memeq(arr_text, "on stdout", 9),
                     "dup2 puts a pipe where stdout was\n");
    wait(i_pid, &i_status);
    close(arr_fds[0]);

    ksft_test_result(dup2(arr_fds[0], 5) == -1,
                     "dup2 of a closed descriptor is -1\n");
    ksft_test_result(pipe((int32_t *)0xffff800000000000ull) == -1,
                     "and pipe writes nowhere but the caller's memory\n");
    ksft_finished();
}
