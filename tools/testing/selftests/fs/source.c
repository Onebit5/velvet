// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/source.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the source tree on the medium, and whether it is the one this kernel
 * was built from, asked by a program.
 */

#include "../kselftest.h"

static struct user_source s_src;

static bool is_hex_digest(const char *str_digest)
{
    for (int i_at = 0; i_at < 40; i_at++) {
        char c_ch = str_digest[i_at];
        if (!((c_ch >= '0' && c_ch <= '9') || (c_ch >= 'a' && c_ch <= 'f'))) {
            return false;
        }
    }
    return str_digest[40] == '\0';
}

int main(int i_argc, char **pp_argv)
{
    (void)i_argc;
    (void)pp_argv;
    ksft_print_header();
    ksft_set_plan(5);

    ksft_test_result(ksft_syscall(SYS_SOURCE, 0, (int64_t)&s_src, 0) == 0
                     && s_src.u_files > 0 && s_src.u_bytes > 0,
                     "the medium carries a source tree\n");
    ksft_test_result(is_hex_digest(s_src.arr_built),
                     "and the kernel names the one it was built from\n");
    ksft_test_result(s_src.u_bytes == s_src.u_built_bytes,
                     "the archive is the size the build made it\n");

    /* every sector of it read and hashed, which is the slow one */
    ksft_test_result(ksft_syscall(SYS_SOURCE, SOURCE_HASH,
                                  (int64_t)&s_src, 0) == 0
                     && ksft_streq(s_src.arr_medium, s_src.arr_built),
                     "and hashes to what the build wrote down\n");
    ksft_test_result(ksft_syscall(SYS_SOURCE, 0, 0xffff800000000000ll, 0)
                     == -1,
                     "the answer goes nowhere but the caller's memory\n");
    ksft_finished();
}
