// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/mm/uaccess.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a pointer handed to the kernel is refused or served, never a crash.
 */

#include "../kselftest.h"

#include <sys/mman.h>

/* nothing a program here is given is mapped this low */
#define UNMAPPED 0x10000000ull

/* the kernel's half, and an address that is in neither half */
#define KERNEL_HALF  0xffffffff80000000ull
#define NONCANONICAL 0x0000900000000000ull

static const char s_arr_rodata[64] = "read only";

static bool efault(long i_result)
{
    return i_result == -1 && errno == EFAULT;
}

int main(void)
{
    int arr_fds[2];
    char arr_cwd[8];

    ksft_print_header();
    ksft_set_plan(7);

    ksft_test_result(efault(write(1, (const void *)UNMAPPED, 8))
                     && efault(write(1, (const void *)KERNEL_HALF, 8))
                     && efault(write(1, (const void *)NONCANONICAL, 8)),
                     "a write from memory that is not the program's is "
                     "EFAULT\n");

    ksft_test_result(efault(syscall(SYS_getcwd, KERNEL_HALF, 64))
                     && syscall(SYS_getcwd, arr_cwd, sizeof arr_cwd) > 0,
                     "and so is a buffer for an answer\n");

    ksft_test_result(pipe(arr_fds) == 0 && write(arr_fds[1], "abcd", 4) == 4
                     && efault(read(arr_fds[0], (void *)s_arr_rodata, 4))
                     && s_arr_rodata[0] == 'r',
                     "the kernel does not write into what is read only\n");

    /*
     * a range nothing has touched yet: the pages are made by the copy,
     * as they would be by the program's own first store
     */
    char *p_map = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ksft_test_result(p_map != MAP_FAILED && write(arr_fds[1], "fresh", 5) == 5
                     && read(arr_fds[0], p_map + 4096 + 100, 5) == 5
                     && memcmp(p_map + 4096 + 100, "fresh", 5) == 0,
                     "a read into memory never touched makes the page\n");

    ksft_test_result(efault(write(1, p_map + 3 * 4096 - 4, 8)),
                     "a buffer running off the end of its mapping is "
                     "refused whole\n");

    char *arr_bad[] = { (char *)UNMAPPED, NULL };
    ksft_test_result(efault(execv("/selftests/mm/uaccess", arr_bad))
                     && efault(syscall(SYS_execve, UNMAPPED, NULL, NULL)),
                     "exec refuses arguments it cannot read\n");

    ksft_test_result(efault(syscall(SYS_pipe2, KERNEL_HALF, 0))
                     && write(arr_fds[1], "x", 1) == 1,
                     "and the machine is still here to say so\n");
    ksft_finished();
}
