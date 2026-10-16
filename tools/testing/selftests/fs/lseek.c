// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/lseek.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * moving a file's position, which is how a write goes on the end.
 */

#include "../kselftest.h"

#define FILE     "/boot/motd.txt"
#define TEXT_MAX 4096

static char s_arr_whole[TEXT_MAX];
static char s_arr_again[TEXT_MAX];

static int64_t lseek(int i_fd, int64_t i_offset, int i_whence)
{
    return ksft_syscall(SYS_LSEEK, i_fd, i_offset, i_whence);
}

int main(int i_argc, char **pp_argv)
{
    char arr_tail[5];

    (void)i_argc;
    (void)pp_argv;
    ksft_print_header();
    ksft_set_plan(4);

    int i_fd = open(FILE);
    int64_t i_len = read(i_fd, s_arr_whole, TEXT_MAX);
    ksft_test_result(i_len > 5 && lseek(i_fd, 0, SEEK_SET) == 0
                     && read(i_fd, s_arr_again, TEXT_MAX) == i_len
                     && ksft_memeq(s_arr_whole, s_arr_again, (size_t)i_len),
                     "back to the start, the file reads again the same\n");
    ksft_test_result(lseek(i_fd, -5, SEEK_END) == i_len - 5
                     && read(i_fd, arr_tail, 5) == 5
                     && ksft_memeq(arr_tail, s_arr_whole + i_len - 5, 5),
                     "five from the end is its last five bytes\n");
    ksft_test_result(lseek(i_fd, 1, SEEK_END) == -1
                     && lseek(i_fd, 0, SEEK_CUR) == i_len,
                     "past the end is -1, and moves nothing\n");
    ksft_test_result(lseek(1, 0, SEEK_SET) == -1,
                     "and the console has no position to move\n");
    close(i_fd);
    ksft_finished();
}
