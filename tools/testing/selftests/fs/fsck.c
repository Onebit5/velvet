// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/fsck.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a check of the disk's filesystems, asked for by a program.
 */

#include "../kselftest.h"
#include <velvet/fsck.h>

static struct user_fsck s_fsck;

static int64_t fsck(const char *str_at, int i_flags, struct user_fsck *p_out)
{
    return ksft_syscall5(SYS_FSCK, (int64_t)str_at,
                         (int64_t)ksft_strlen(str_at), i_flags,
                         (int64_t)p_out, 0);
}

/* the shape of a filesystem the check could actually walk */
static bool plausible(const struct user_fsck *p_fsck)
{
    return p_fsck->u_blocks > 0 && p_fsck->u_inodes > 0 && p_fsck->u_groups > 0
           && (p_fsck->u_block_size == 1024 || p_fsck->u_block_size == 2048
               || p_fsck->u_block_size == 4096);
}

int main(int i_argc, char **pp_argv)
{
    (void)i_argc;
    (void)pp_argv;
    ksft_print_header();
    ksft_set_plan(6);

    ksft_test_result(fsck("/", 0, &s_fsck) == 0 && plausible(&s_fsck),
                     "fsck checks the system at /\n");
    ksft_test_result(s_fsck.u_total_mended == 0,
                     "and without FSCK_MEND it mends nothing\n");
    ksft_test_result(fsck("/work", 0, &s_fsck) == 0 && plausible(&s_fsck),
                     "the work filesystem is checked by where it hangs\n");

    ksft_test_result(fsck("/", FSCK_MEND, &s_fsck) == -1
                     && s_fsck.arr_why[0] != '\0',
                     "a guest may not have it mended, and is told why\n");
    ksft_test_result(fsck("/selftests", 0, &s_fsck) == -1,
                     "a path nothing is mounted at is refused\n");
    ksft_test_result(fsck("/", 0, (struct user_fsck *)0xffff800000000000ull)
                     == -1,
                     "and the answer goes nowhere but the caller's memory\n");
    ksft_finished();
}
