// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/threads.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the threads a program can see, and the one file about them it cannot.
 */

#include "../kselftest.h"

#define TEXT_MAX 8192

int main(int i_argc, char **pp_argv)
{
    char arr_text[TEXT_MAX];
    int64_t i_len;

    (void)i_argc;
    (void)pp_argv;
    ksft_print_header();
    ksft_set_plan(4);

    i_len = ksft_read_file("/proc/threads", arr_text, TEXT_MAX);
    ksft_test_result(ksft_contains(arr_text, i_len, "  id  name"),
                     "/proc/threads is the thread table\n");
    ksft_test_result(ksft_contains(arr_text, i_len, "idle0"),
                     "with the kernel's own threads in it\n");
    /*
     * a program's thread is named after the path it was started by, cut
     * to 15 characters: /selftests/sche once execve has resolved it, or
     * selftests/sched as a shell typed it
     */
    ksft_test_result(ksft_contains(arr_text, i_len, "selftests/sche"),
                     "and the program reading it\n");

    /* the tests run as guest, and a call trace is root's */
    ksft_test_result(open("/proc/stack") < 0,
                     "a guest may not read the kernel's call trace\n");
    ksft_finished();
}
