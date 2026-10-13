// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/resize.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * resize, asked for by a program that is not root.
 */

#include "../kselftest.h"
#include <velvet/fsck.h>

static struct user_resize s_resize;
static struct user_fsck s_fsck;

static int64_t resize(const char *str_at, uint64_t u_blocks,
                      struct user_resize *p_out)
{
    return ksft_syscall5(SYS_RESIZE, (int64_t)str_at,
                         (int64_t)ksft_strlen(str_at), (int64_t)u_blocks,
                         (int64_t)p_out, 0);
}

/* how many blocks the filesystem at `str_at` has, as fsck counts them */
static uint32_t blocks_at(const char *str_at)
{
    if (ksft_syscall5(SYS_FSCK, (int64_t)str_at,
                      (int64_t)ksft_strlen(str_at), 0,
                      (int64_t)&s_fsck, 0) != 0) {
        return 0;
    }
    return s_fsck.u_blocks;
}

int main(int i_argc, char **pp_argv)
{
    uint32_t u_was;

    (void)i_argc;
    (void)pp_argv;
    ksft_print_header();
    ksft_set_plan(4);

    u_was = blocks_at("/work");
    ksft_test_result(resize("/work", 0, &s_resize) == -1
                     && s_resize.arr_why[0] != '\0',
                     "a guest may not resize, and is told why\n");
    ksft_test_result(u_was != 0 && blocks_at("/work") == u_was,
                     "and the filesystem is the size it was\n");
    ksft_test_result(resize("/selftests", 0, &s_resize) == -1,
                     "a path nothing is mounted at is refused\n");
    ksft_test_result(resize("/work", 0,
                            (struct user_resize *)0xffff800000000000ull)
                     == -1,
                     "and the answer goes nowhere but the caller's memory\n");
    ksft_finished();
}
