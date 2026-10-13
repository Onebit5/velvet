// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/disk.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the drives, the partitions and the mounts, read from /proc by a program.
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

    i_len = ksft_read_file("/proc/mounts", arr_text, TEXT_MAX);
    ksft_test_result(ksft_contains(arr_text, i_len, "at         kind"),
                     "/proc/mounts is the mount table\n");
    ksft_test_result(ksft_contains(arr_text, i_len,
                                   "a module the bootloader handed the kernel"),
                     "with the ramdisk the machine booted with\n");

    /* a machine with or without a disk answers, in either case */
    i_len = ksft_read_file("/proc/disk", arr_text, TEXT_MAX);
    ksft_test_result(ksft_contains(arr_text, i_len, "drive      ")
                     || ksft_contains(arr_text, i_len, "no disk."),
                     "/proc/disk names the drive, or says there is none\n");

    i_len = ksft_read_file("/proc/partitions", arr_text, TEXT_MAX);
    ksft_test_result(i_len > 0, "/proc/partitions says what the drives hold\n");
    ksft_finished();
}
