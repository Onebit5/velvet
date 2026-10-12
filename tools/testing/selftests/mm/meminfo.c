// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/mm/meminfo.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what memory says about itself, read from /proc by a program.
 */

#include "../kselftest.h"

#define TEXT_MAX 4096

int main(int i_argc, char **pp_argv)
{
    char arr_text[TEXT_MAX];
    int64_t i_len;

    (void)i_argc;
    (void)pp_argv;
    ksft_print_header();
    ksft_set_plan(4);

    i_len = ksft_read_file("/proc/meminfo", arr_text, TEXT_MAX);
    ksft_test_result(ksft_contains(arr_text, i_len, "physical frames")
                     && ksft_contains(arr_text, i_len, "ever in use at once"),
                     "/proc/meminfo counts the frames and their peak\n");
    ksft_test_result(ksft_contains(arr_text, i_len, "free blocks, by size")
                     && ksft_contains(arr_text, i_len, "kernel heap"),
                     "and the shape of what is free, and the heap\n");

    i_len = ksft_read_file("/proc/slabinfo", arr_text, TEXT_MAX);
    ksft_test_result(ksft_contains(arr_text, i_len, "kmalloc-"),
                     "/proc/slabinfo lists the object caches\n");
    ksft_test_result(ksft_contains(arr_text, i_len, "holding "),
                     "and what they hold against what they use\n");
    ksft_finished();
}
