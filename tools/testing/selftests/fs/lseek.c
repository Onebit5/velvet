// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/lseek.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * moving a file's position, which is how a write goes on the end.
 */

#include "../kselftest.h"

/* room for the whole of the file read below, the test's own */
#define TEXT_MAX (128 * 1024)

static char s_arr_whole[TEXT_MAX];
static char s_arr_again[TEXT_MAX];

int main(int i_argc, char **pp_argv)
{
    char arr_tail[5];

    (void)i_argc;
    ksft_print_header();
    ksft_set_plan(5);

    /*
     * the test's own file, by the path it was started by: a file every
     * medium that carries the test has, whatever else it carries
     */
    int i_fd = open(pp_argv[0], O_RDONLY);
    ssize_t i_len = read(i_fd, s_arr_whole, TEXT_MAX);
    ksft_test_result(i_len > 5 && lseek(i_fd, 0, SEEK_SET) == 0
                     && read(i_fd, s_arr_again, TEXT_MAX) == i_len
                     && memcmp(s_arr_whole, s_arr_again, (size_t)i_len) == 0,
                     "back to the start, the file reads again the same\n");
    ksft_test_result(lseek(i_fd, -5, SEEK_END) == i_len - 5
                     && read(i_fd, arr_tail, 5) == 5
                     && memcmp(arr_tail, s_arr_whole + i_len - 5, 5) == 0,
                     "five from the end is its last five bytes\n");
    ksft_test_result(lseek(i_fd, 1, SEEK_END) == i_len + 1
                     && read(i_fd, arr_tail, 5) == 0,
                     "past the end is somewhere, with nothing to read\n");
    ksft_test_result(lseek(i_fd, -1, SEEK_SET) == -1 && errno == EINVAL
                     && lseek(i_fd, 0, SEEK_CUR) == i_len + 1,
                     "before the start is EINVAL, and moves nothing\n");
    ksft_test_result(lseek(1, 0, SEEK_SET) == -1 && errno == ESPIPE,
                     "and the console has no position to move\n");
    close(i_fd);
    ksft_finished();
}
