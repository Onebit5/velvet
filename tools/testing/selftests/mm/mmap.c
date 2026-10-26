// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/mm/mmap.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * memory a program asks for, from ring 3: mapped, protected, given back.
 */

#define _GNU_SOURCE     /* mremap */

#include "../kselftest.h"

#include <sys/mman.h>

#define PAGE 4096

/* what a child touching `p_at` ended with: the signal, or 0 */
static int touch(volatile char *p_at, bool b_write)
{
    int i_status = 0;
    pid_t i_pid = fork();

    if (i_pid == 0) {
        if (b_write) {
            *p_at = 1;
        } else {
            (void)*p_at;
        }
        _exit(0);
    }
    waitpid(i_pid, &i_status, 0);
    return WIFSIGNALED(i_status) ? WTERMSIG(i_status) : 0;
}

static bool zeroes(const char *p_at, size_t sz_len)
{
    for (size_t sz_i = 0; sz_i < sz_len; sz_i++) {
        if (p_at[sz_i] != 0) {
            return false;
        }
    }
    return true;
}

int main(int i_argc, char **pp_argv)
{
    (void)i_argc;
    ksft_print_header();
    ksft_set_plan(11);

    char *p_map = mmap(NULL, 4 * PAGE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ksft_test_result(p_map != MAP_FAILED && zeroes(p_map, 4 * PAGE),
                     "anonymous memory comes as zeroes\n");
    memset(p_map, 'a', 4 * PAGE);
    ksft_test_result(p_map[4 * PAGE - 1] == 'a',
                     "and is the program's to write\n");

    ksft_test_result(mprotect(p_map + PAGE, PAGE, PROT_READ) == 0
                     && p_map[PAGE] == 'a' && touch(p_map + PAGE, true)
                     == SIGSEGV && touch(p_map, true) == 0,
                     "mprotect makes one page of it read only\n");

    ksft_test_result(munmap(p_map + 2 * PAGE, PAGE) == 0
                     && touch(p_map + 2 * PAGE, false) == SIGSEGV
                     && p_map[3 * PAGE] == 'a',
                     "munmap takes a page out of the middle, keeping the "
                     "rest\n");

    ksft_test_result(madvise(p_map, PAGE, MADV_DONTNEED) == 0
                     && zeroes(p_map, PAGE),
                     "MADV_DONTNEED gives a page back, and it comes back "
                     "zeroes\n");

    char *p_small = mremap(p_map + 3 * PAGE, PAGE, PAGE / 2, 0);
    ksft_test_result(p_small == p_map + 3 * PAGE,
                     "mremap smaller stays where it is\n");

    char *p_fixed = mmap(p_map + 2 * PAGE, PAGE, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    ksft_test_result(p_fixed == p_map + 2 * PAGE && zeroes(p_fixed, PAGE),
                     "MAP_FIXED puts a mapping where it is asked to\n");
    ksft_test_result(mmap(p_map, PAGE, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS
                          | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED
                     && errno == EEXIST,
                     "and MAP_FIXED_NOREPLACE will not put one over another\n");
    munmap(p_map, 4 * PAGE);

    /* the call itself: musl's sbrk refuses to move the break at all */
    char *p_brk = (char *)syscall(SYS_brk, 0);
    ksft_test_result((char *)syscall(SYS_brk, p_brk + 3 * PAGE)
                     == p_brk + 3 * PAGE && (p_brk[3 * PAGE - 1] = 1)
                     && (char *)syscall(SYS_brk, p_brk + PAGE) == p_brk + PAGE
                     && touch(p_brk + 2 * PAGE, false) == SIGSEGV
                     && (char *)syscall(SYS_brk, p_brk) == p_brk,
                     "the break grows and comes back down\n");

    /* a file's bytes, copied in */
    int i_fd = open(pp_argv[0], O_RDONLY);
    char *p_file = mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE, i_fd, 0);
    ksft_test_result(p_file != MAP_FAILED
                     && memcmp(p_file, "\x7f" "ELF", 4) == 0,
                     "a file mapped privately reads as the file\n");
    ksft_test_result(mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED,
                          i_fd, 0) == MAP_FAILED && errno == EACCES,
                     "while a shared one is not written through a "
                     "descriptor opened only for reading\n");
    close(i_fd);
    ksft_finished();
}
