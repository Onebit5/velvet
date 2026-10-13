// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/mm/vmm.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the kernel's page tables, which are root's to read and no one else's.
 */

#include "../kselftest.h"

int main(int i_argc, char **pp_argv)
{
    char arr_name[64];
    bool b_listed = false;

    (void)i_argc;
    (void)pp_argv;
    ksft_print_header();
    ksft_set_plan(2);

    for (size_t sz_at = 0; readdir("/proc", sz_at, arr_name,
                                   sizeof arr_name) >= 0; sz_at++) {
        if (ksft_streq(arr_name, "vmm")) {
            b_listed = true;
        }
    }
    ksft_test_result(b_listed, "/proc/vmm is there to be seen\n");

    /*
     * the runner logs in as guest, and where the kernel keeps things is
     * root's
     */
    ksft_test_result(open("/proc/vmm") < 0,
                     "but a guest may not read the page tables\n");
    ksft_finished();
}
