// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/x86_64/cpuinfo.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the processors, read from /proc by a program.
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
    ksft_set_plan(3);

    i_len = ksft_read_file("/proc/cpuinfo", arr_text, TEXT_MAX);
    ksft_test_result(ksft_contains(arr_text, i_len, "model name  "),
                     "/proc/cpuinfo says what the processor is\n");
    ksft_test_result(ksft_contains(arr_text, i_len, "cpu  apic"),
                     "and lists the ones the firmware knows of\n");
    ksft_test_result(ksft_contains(arr_text, i_len, "processors awake"),
                     "and how many are awake\n");
    ksft_finished();
}
