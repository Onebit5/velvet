// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/file.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * descriptors are numbers for open files, which they share.
 */

#include "../kselftest.h"

/* root's, on the disk, readable by everyone and writable by root alone */
#define ROOTS_FILE "/welcome.txt"

#define TEXT_MAX 256

static char s_arr_before[TEXT_MAX];
static char s_arr_after[TEXT_MAX];

static int child_status(pid_t i_pid)
{
    int i_status = -1;

    if (i_pid > 0) {
        waitpid(i_pid, &i_status, 0);
    }
    return i_status;
}

int main(int i_argc, char **pp_argv)
{
    char arr_got[16];

    (void)i_argc;
    ksft_print_header();
    ksft_set_plan(7);

    int i_fd = open(pp_argv[0], O_RDONLY);
    int i_copy = dup2(i_fd, 20);
    ksft_test_result(i_fd >= 0 && i_copy == 20
                     && read(i_fd, arr_got, 4) == 4
                     && lseek(i_copy, 0, SEEK_CUR) == 4
                     && read(i_copy, arr_got, 4) == 4
                     && lseek(i_fd, 0, SEEK_CUR) == 8,
                     "two descriptors on one file share its position\n");

    pid_t i_pid = fork();
    if (i_pid == 0) {
        _exit(read(i_fd, arr_got, 10) == 10 ? 0 : 1);
    }
    ksft_test_result(ksft_exited(child_status(i_pid), 0)
                     && lseek(i_fd, 0, SEEK_CUR) == 18,
                     "and a forked child reading moves its parent's\n");

    /*
     * opened to be read, so not written through, whoever owns the file:
     * the open checked reading and nothing else
     */
    int i_roots = open(ROOTS_FILE, O_RDONLY);
    ssize_t i_len = read(i_roots, s_arr_before, TEXT_MAX);
    bool b_refused = write(i_roots, "guest was here", 14) == -1
                     && errno == EBADF;
    close(i_roots);
    i_roots = open(ROOTS_FILE, O_RDONLY);
    ksft_test_result(i_len > 0 && b_refused
                     && read(i_roots, s_arr_after, TEXT_MAX) == i_len
                     && memcmp(s_arr_before, s_arr_after, (size_t)i_len) == 0,
                     "a file opened to be read is not written through\n");
    close(i_roots);
    ksft_test_result(open(ROOTS_FILE, O_WRONLY) == -1 && errno == EACCES,
                     "and a guest may not open root's to write\n");

    i_pid = fork();
    if (i_pid == 0) {
        close(0);
        _exit(open(pp_argv[0], O_RDONLY) == 0 ? 0 : 1);
    }
    ksft_test_result(ksft_exited(child_status(i_pid), 0),
                     "an open takes the lowest free descriptor\n");

    int arr_fds[2];
    ksft_test_result(pipe(arr_fds) == 0 && dup2(arr_fds[1], 21) == 21
                     && close(arr_fds[1]) == 0
                     && write(21, "x", 1) == 1
                     && read(arr_fds[0], arr_got, 1) == 1
                     && close(21) == 0
                     && read(arr_fds[0], arr_got, 1) == 0,
                     "a pipe's end closes with the last descriptor on it\n");

    ksft_test_result(close(i_fd) == 0 && close(i_fd) == -1 && errno == EBADF
                     && read(i_fd, arr_got, 1) == -1
                     && read(i_copy, arr_got, 1) == 1,
                     "a closed descriptor is closed for good, and its "
                     "copy is not\n");
    ksft_finished();
}
