// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/mm/uaccess.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a pointer handed to the kernel is refused or served, never a crash.
 */

#include "../kselftest.h"

/* nothing a program here is given is mapped this low */
#define UNMAPPED 0x10000000ull

/* the kernel's half, and an address that is in neither half */
#define KERNEL_HALF  0xffffffff80000000ull
#define NONCANONICAL 0x0000900000000000ull

static const char s_arr_rodata[64] = "read only";

int main(int i_argc, char **pp_argv)
{
    int32_t arr_fds[2];

    (void)i_argc;
    (void)pp_argv;
    ksft_print_header();
    ksft_set_plan(7);

    ksft_test_result(write(1, (const void *)UNMAPPED, 8) == -1
                     && write(1, (const void *)KERNEL_HALF, 8) == -1
                     && write(1, (const void *)NONCANONICAL, 8) == -1,
                     "a write from memory that is not the program's is "
                     "refused\n");

    char arr_cwd[8];
    ksft_test_result(ksft_syscall(SYS_GETCWD, (int64_t)KERNEL_HALF, 64, 0)
                     == -1
                     && ksft_syscall(SYS_GETCWD, (int64_t)(uintptr_t)arr_cwd,
                                     sizeof arr_cwd, 0) >= 0,
                     "and so is a buffer for an answer\n");

    ksft_test_result(pipe(arr_fds) == 0 && write(arr_fds[1], "abcd", 4) == 4
                     && read(arr_fds[0], (void *)(uintptr_t)s_arr_rodata, 4)
                        == -1
                     && s_arr_rodata[0] == 'r',
                     "the kernel does not write into what is read only\n");

    /*
     * a range nothing has touched yet: the pages are made by the copy,
     * as they would be by the program's own first store
     */
    int64_t i_map = ksft_syscall(SYS_MMAP, 3 * 4096, 0, 0);
    char *p_map = (char *)(uintptr_t)i_map;
    ksft_test_result(i_map > 0 && write(arr_fds[1], "fresh", 5) == 5
                     && read(arr_fds[0], p_map + 4096 + 100, 5) == 5
                     && ksft_memeq(p_map + 4096 + 100, "fresh", 5),
                     "a read into memory never touched makes the page\n");

    ksft_test_result(write(1, p_map + 3 * 4096 - 4, 8) == -1,
                     "a buffer running off the end of its mapping is "
                     "refused whole\n");

    char *arr_bad[] = { (char *)(uintptr_t)UNMAPPED, NULL };
    ksft_test_result(execv("/selftests/mm/uaccess", arr_bad) == -1
                     && ksft_syscall(SYS_EXECVE, (int64_t)UNMAPPED, 10, 0)
                        == -1,
                     "exec refuses arguments it cannot read\n");

    ksft_test_result(ksft_syscall(SYS_PIPE, (int64_t)KERNEL_HALF, 0, 0) == -1
                     && write(arr_fds[1], "x", 1) == 1,
                     "and the machine is still here to say so\n");
    ksft_finished();
}
