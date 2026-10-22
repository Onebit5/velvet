// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/x86_64/fsbase.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * where %fs points is each program's own, set with arch_prctl.
 */

#include "../kselftest.h"

/* more children than a small machine has cores, so they take turns */
#define CHILDREN 6

#define FRESH "--fresh"

/* what each child points %fs at: a word of its own, and the next */
static uint64_t s_arr_blocks[CHILDREN + 1][2];

static int64_t arch_prctl(int i_code, uint64_t u_addr)
{
    return ksft_syscall(SYS_ARCH_PRCTL, i_code, (int64_t)u_addr, 0);
}

/* the two words %fs points at */
static uint64_t fs_word(int i_at)
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

    arch_prctl(ARCH_GET_FS, (uint64_t)(uintptr_t)&u_base);
    return u_base;
}

static int child_status(int i_pid)
{
    int i_status = -1000;

    if (i_pid > 0) {
        wait(i_pid, &i_status);
    }
    return i_status;
}

/* each child its own base, held across time spent and cores changed */
static int keep(int i_at)
{
    uint64_t *p_block = s_arr_blocks[i_at];
    p_block[0] = 0x1000 + (uint64_t)i_at;
    p_block[1] = 0x2000 + (uint64_t)i_at;

    if (arch_prctl(ARCH_SET_FS, (uint64_t)(uintptr_t)p_block) != 0) {
        return 2;
    }
    int64_t i_until = ksft_syscall(SYS_UPTIME, 0, 0, 0) + 300;
    while (ksft_syscall(SYS_UPTIME, 0, 0, 0) < i_until) {
        for (volatile int i_spin = 0; i_spin < 100000; i_spin++) {
        }
    }
    sleep_ms(20);
    return fs_word(0) == p_block[0] && fs_word(1) == p_block[1] ? 0 : 1;
}

int main(int i_argc, char **pp_argv)
{
    int i_pid;
    uint64_t *p_mine = s_arr_blocks[CHILDREN];

    if (i_argc >= 2 && ksft_streq(pp_argv[1], FRESH)) {
        exit(fs_base() == 0 ? 0 : 1);
    }

    ksft_print_header();
    ksft_set_plan(7);

    ksft_test_result(fs_base() == 0, "a program starts with no fs base\n");

    p_mine[0] = 0x5eed;
    p_mine[1] = 0xf00d;
    ksft_test_result(arch_prctl(ARCH_SET_FS, (uint64_t)(uintptr_t)p_mine) == 0
                     && fs_word(0) == 0x5eed && fs_word(1) == 0xf00d,
                     "ARCH_SET_FS points fs at the program's own data\n");
    ksft_test_result(fs_base() == (uint64_t)(uintptr_t)p_mine,
                     "and ARCH_GET_FS says where\n");

    int arr_pids[CHILDREN];
    for (int i_at = 0; i_at < CHILDREN; i_at++) {
        arr_pids[i_at] = fork();
        if (arr_pids[i_at] == 0) {
            exit(keep(i_at));
        }
    }
    bool b_kept = true;
    for (int i_at = 0; i_at < CHILDREN; i_at++) {
        b_kept = child_status(arr_pids[i_at]) == 0 && b_kept;
    }
    ksft_test_result(b_kept && fs_word(0) == 0x5eed,
                     "each program's outlasts the others running in "
                     "between\n");

    i_pid = fork();
    if (i_pid == 0) {
        char *arr_args[] = { pp_argv[0], FRESH, NULL };
        if (fs_word(1) != 0xf00d) {
            exit(3);
        }
        execv(pp_argv[0], arr_args);
        exit(2);
    }
    ksft_test_result(child_status(i_pid) == 0,
                     "a forked child keeps it, and a program it runs "
                     "starts without\n");

    ksft_test_result(arch_prctl(ARCH_SET_FS, 0xffff800000000000ull) == -1
                     && arch_prctl(ARCH_SET_FS, 0x0000800000000000ull) == -1
                     && arch_prctl(ARCH_GET_FS, 0xffff800000001000ull) == -1
                     && fs_base() == (uint64_t)(uintptr_t)p_mine,
                     "an address that is not the program's is refused\n");

    uint64_t u_gs = 0;
    ksft_test_result(arch_prctl(ARCH_SET_GS, (uint64_t)(uintptr_t)p_mine) == -1
                     && arch_prctl(ARCH_GET_GS,
                                   (uint64_t)(uintptr_t)&u_gs) == -1,
                     "and gs, which is the kernel's, is not offered\n");
    ksft_finished();
}
