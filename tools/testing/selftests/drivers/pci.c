// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/drivers/pci.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what is plugged into the machine, read from /proc by a program.
 */

#include "../kselftest.h"

#define TEXT_MAX 4096

int main(void)
{
    char arr_text[TEXT_MAX];
    int64_t i_len;

    ksft_print_header();
    ksft_set_plan(2);

    /* every machine the runner boots is a q35, which has a host bridge */
    i_len = ksft_read_file("/proc/pci", arr_text, TEXT_MAX);
    ksft_test_result(ksft_contains(arr_text, i_len, "host bridge"),
                     "/proc/pci lists the devices the scan found\n");
    ksft_test_result(ksft_contains(arr_text, i_len, "00:00.0"),
                     "each where it is on the bus\n");
    ksft_finished();
}
