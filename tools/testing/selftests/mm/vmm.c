// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/mm/vmm.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the kernel's page tables, which are root's to read and no one else's.
 */

#include "../kselftest.h"

#include <dirent.h>

int main(void)
{
    bool b_listed = false;

    ksft_print_header();
    ksft_set_plan(2);

    DIR *p_dir = opendir("/proc");
    for (struct dirent *p_ent; p_dir != NULL
                               && (p_ent = readdir(p_dir)) != NULL; ) {
        b_listed |= ksft_streq(p_ent->d_name, "vmm");
    }
    if (p_dir != NULL) {
        closedir(p_dir);
    }
    ksft_test_result(b_listed, "/proc/vmm is there to be seen\n");

    /* the tests run as guest, and where the kernel keeps things is root's */
    ksft_test_result(open("/proc/vmm", O_RDONLY) < 0 && errno == EACCES,
                     "but a guest may not read the page tables\n");
    ksft_finished();
}
