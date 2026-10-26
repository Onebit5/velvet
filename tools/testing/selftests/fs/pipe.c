// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/pipe.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * pipes and dup2, from ring 3: what a shell builds a pipeline out of.
 */

#include "../kselftest.h"

#define TEXT_MAX 64

/* everything the pipe holds until every writer is gone, and how much */
static ssize_t drain(int i_fd, char *str_buf, size_t sz_size)
{
    size_t sz_len = 0;

    for (;;) {
        ssize_t i_got = read(i_fd, str_buf + sz_len, sz_size - sz_len);
        if (i_got <= 0) {
            return i_got < 0 ? -1 : (ssize_t)sz_len;
        }
        sz_len += (size_t)i_got;
    }
}

int main(void)
{
    char arr_text[TEXT_MAX];
    int arr_fds[2];
    int i_status;

    ksft_print_header();
    ksft_set_plan(9);

    ksft_test_result(pipe(arr_fds) == 0 && arr_fds[0] >= 0
                     && arr_fds[1] >= 0 && arr_fds[0] != arr_fds[1],
                     "pipe gives two descriptors\n");
    write(arr_fds[1], "one way", 7);
    ksft_test_result(read(arr_fds[0], arr_text, TEXT_MAX) == 7
                     && memcmp(arr_text, "one way", 7) == 0,
                     "what goes in one end comes out of the other\n");

    /* the child never closes its end: leaving has to do that for it */
    pid_t i_pid = fork();
    if (i_pid == 0) {
        write(arr_fds[1], "from the child", 14);
        _exit(0);
    }
    close(arr_fds[1]);
    ksft_test_result(drain(arr_fds[0], arr_text, TEXT_MAX) == 14
                     && memcmp(arr_text, "from the child", 14) == 0,
                     "a writer that exits is a pipe at its end\n");
    waitpid(i_pid, &i_status, 0);
    close(arr_fds[0]);

    pipe(arr_fds);
    i_pid = fork();
    if (i_pid == 0) {
        dup2(arr_fds[1], 1);
        close(arr_fds[1]);
        close(arr_fds[0]);
        write(1, "on stdout", 9);
        _exit(0);
    }
    close(arr_fds[1]);
    ksft_test_result(drain(arr_fds[0], arr_text, TEXT_MAX) == 9
                     && memcmp(arr_text, "on stdout", 9) == 0,
                     "dup2 puts a pipe where stdout was\n");
    waitpid(i_pid, &i_status, 0);
    close(arr_fds[0]);

    ksft_test_result(dup2(arr_fds[0], 5) == -1 && errno == EBADF,
                     "dup2 of a closed descriptor is EBADF\n");

    /* nobody to read it: SIGPIPE, and EPIPE where it is ignored */
    pipe(arr_fds);
    close(arr_fds[0]);
    i_pid = fork();
    if (i_pid == 0) {
        write(arr_fds[1], "x", 1);
        _exit(0);
    }
    waitpid(i_pid, &i_status, 0);
    ksft_test_result(ksft_killed_by(i_status, SIGPIPE),
                     "a write nobody will read ends the writer\n");
    signal(SIGPIPE, SIG_IGN);
    ksft_test_result(write(arr_fds[1], "x", 1) == -1 && errno == EPIPE,
                     "or, with SIGPIPE ignored, is EPIPE\n");
    close(arr_fds[1]);

    /* empty, and the descriptor asked not to wait */
    pipe2(arr_fds, O_NONBLOCK | O_CLOEXEC);
    ksft_test_result(read(arr_fds[0], arr_text, 1) == -1 && errno == EAGAIN
                     && (fcntl(arr_fds[0], F_GETFD) & FD_CLOEXEC),
                     "a pipe made not to wait says EAGAIN\n");

    ksft_test_result(syscall(SYS_pipe2, 0xffff800000000000ull, 0) == -1
                     && errno == EFAULT,
                     "and pipe writes nowhere but the caller's memory\n");
    ksft_finished();
}
