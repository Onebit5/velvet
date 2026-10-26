// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/x86_64/fsbase.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * where %fs points is each program's own, set with arch_prctl.
 */

#include "../kselftest.h"

#include <velvet/prctl.h>

/* more children than a small machine has cores, so they take turns */
#define CHILDREN 6

/* what each child points %fs at: a word of its own, and the next */
static uint64_t s_arr_blocks[CHILDREN + 1][2];

/*
 * the libc keeps its thread pointer in %fs, so while %fs points anywhere
 * else nothing of the libc may run: these make the calls themselves
 */
static inline long raw2(long i_nr, long i_a0, long i_a1)
{
    long i_ret;

    __asm__ volatile ("syscall"
                      : "=a"(i_ret)
                      : "a"(i_nr), "D"(i_a0), "S"(i_a1)
                      : "rcx", "r11", "memory");
    return i_ret;
}

static inline long set_fs(uint64_t u_base)
{
    return raw2(SYS_arch_prctl, ARCH_SET_FS, (long)u_base);
}

/* the two words %fs points at */
static inline uint64_t fs_word(int i_at)
{
    uint64_t u_value;

    if (i_at == 0) {
        __asm__ volatile ("mov %%fs:0, %0" : "=r"(u_value));
    } else {
        __asm__ volatile ("mov %%fs:8, %0" : "=r"(u_value));
    }
    return u_value;
}

static uint64_t fs_base(void)
{
    uint64_t u_base = 1;

    raw2(SYS_arch_prctl, ARCH_GET_FS, (long)(uintptr_t)&u_base);
    return u_base;
}

static uint64_t now_ms(void)
{
    struct timespec ts;

    raw2(SYS_clock_gettime, CLOCK_MONOTONIC, (long)(uintptr_t)&ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* each child its own base, held across time spent and cores changed */
static int keep(int i_at, uint64_t u_libc)
{
    uint64_t *p_block = s_arr_blocks[i_at];
    p_block[0] = 0x1000 + (uint64_t)i_at;
    p_block[1] = 0x2000 + (uint64_t)i_at;

    if (set_fs((uint64_t)(uintptr_t)p_block) != 0) {
        return 2;
    }
    uint64_t u_until = now_ms() + 300;
    while (now_ms() < u_until) {
        for (volatile int i_spin = 0; i_spin < 100000; i_spin++) {
        }
    }
    struct timespec ts = { 0, 20 * 1000000L };
    raw2(SYS_nanosleep, (long)(uintptr_t)&ts, 0);
    bool b_kept = fs_word(0) == p_block[0] && fs_word(1) == p_block[1];
    set_fs(u_libc);
    return b_kept ? 0 : 1;
}

int main(void)
{
    uint64_t *p_mine = s_arr_blocks[CHILDREN];
    int i_status = -1;

    ksft_print_header();
    ksft_set_plan(6);

    /* the libc's thread pointer, which it keeps where %fs:0 says */
    uint64_t u_libc = fs_base();
    ksft_test_result(u_libc != 0 && fs_word(0) == u_libc,
                     "the libc's thread pointer is where ARCH_GET_FS "
                     "says\n");

    p_mine[0] = 0x5eed;
    p_mine[1] = 0xf00d;
    bool b_set = set_fs((uint64_t)(uintptr_t)p_mine) == 0;
    bool b_words = fs_word(0) == 0x5eed && fs_word(1) == 0xf00d;
    bool b_said = fs_base() == (uint64_t)(uintptr_t)p_mine;

    /* a fork made while it is set, which the child reports and leaves */
    long i_child = raw2(SYS_fork, 0, 0);
    if (i_child == 0) {
        raw2(SYS_exit_group, fs_word(1) == 0xf00d ? 0 : 1, 0);
    }
    set_fs(u_libc);
    ksft_test_result(b_set && b_words,
                     "ARCH_SET_FS points fs at the program's own data\n");
    ksft_test_result(b_said, "and ARCH_GET_FS says where\n");
    waitpid((pid_t)i_child, &i_status, 0);
    ksft_test_result(i_child > 0 && ksft_exited(i_status, 0),
                     "a forked child keeps it\n");

    pid_t arr_pids[CHILDREN];
    for (int i_at = 0; i_at < CHILDREN; i_at++) {
        arr_pids[i_at] = fork();
        if (arr_pids[i_at] == 0) {
            _exit(keep(i_at, u_libc));
        }
    }
    bool b_kept = true;
    for (int i_at = 0; i_at < CHILDREN; i_at++) {
        b_kept = waitpid(arr_pids[i_at], &i_status, 0) == arr_pids[i_at]
                 && ksft_exited(i_status, 0) && b_kept;
    }
    ksft_test_result(b_kept && fs_base() == u_libc,
                     "each program's outlasts the others running in "
                     "between\n");

    uint64_t u_gs = 0;
    ksft_test_result(raw2(SYS_arch_prctl, ARCH_SET_FS,
                          (long)0xffff800000000000ull) == -EPERM
                     && raw2(SYS_arch_prctl, ARCH_SET_FS,
                             (long)0x0000800000000000ull) == -EPERM
                     && raw2(SYS_arch_prctl, ARCH_GET_FS,
                             (long)0xffff800000001000ull) == -EFAULT
                     && raw2(SYS_arch_prctl, ARCH_SET_GS,
                             (long)(uintptr_t)p_mine) == -EINVAL
                     && raw2(SYS_arch_prctl, ARCH_GET_GS,
                             (long)(uintptr_t)&u_gs) == -EINVAL
                     && fs_base() == u_libc,
                     "an address not the program's is refused, and gs, "
                     "the kernel's, is not offered\n");
    ksft_finished();
}
