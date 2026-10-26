// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/x86_64/fpu.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the x87 and sse registers are each program's own.
 */

#include "../kselftest.h"

extern char **environ;

/* nothing in this file may touch an sse register but the test itself */
#pragma GCC target("general-regs-only")

/* the sixteen xmm registers, 16 bytes apiece */
#define XMM_BYTES 256

/* more children than a small machine has cores, so they take turns */
#define CHILDREN 6

#define FCW_DEFAULT   0x037f
#define MXCSR_DEFAULT 0x1f80

#define FRESH "--fresh"

/* the seed of what is left behind for a program exec starts */
#define LEFT_BEHIND 5

/*
 * the registers are the caller's to lose across any call, so between
 * loading them and looking at them nothing of the libc may run: these
 * make the calls themselves
 */
static inline long raw3(long i_nr, long i_a0, long i_a1, long i_a2)
{
    long i_ret;

    __asm__ volatile ("syscall"
                      : "=a"(i_ret)
                      : "a"(i_nr), "D"(i_a0), "S"(i_a1), "d"(i_a2)
                      : "rcx", "r11", "memory");
    return i_ret;
}

static __attribute__((noreturn)) void raw_exit(int i_code)
{
    raw3(SYS_exit_group, i_code, 0, 0);
    __builtin_unreachable();
}

/*
 * written in assembly, and this file compiled without sse, so the
 * compiler never touches these registers and what is in them is exactly
 * what the test put there
 */
static void load_xmm(const uint8_t *p_from)
{
    __asm__ volatile (
                      "movdqu 0(%0), %%xmm0\n\t"
                      "movdqu 16(%0), %%xmm1\n\t"
                      "movdqu 32(%0), %%xmm2\n\t"
                      "movdqu 48(%0), %%xmm3\n\t"
                      "movdqu 64(%0), %%xmm4\n\t"
                      "movdqu 80(%0), %%xmm5\n\t"
                      "movdqu 96(%0), %%xmm6\n\t"
                      "movdqu 112(%0), %%xmm7\n\t"
                      "movdqu 128(%0), %%xmm8\n\t"
                      "movdqu 144(%0), %%xmm9\n\t"
                      "movdqu 160(%0), %%xmm10\n\t"
                      "movdqu 176(%0), %%xmm11\n\t"
                      "movdqu 192(%0), %%xmm12\n\t"
                      "movdqu 208(%0), %%xmm13\n\t"
                      "movdqu 224(%0), %%xmm14\n\t"
                      "movdqu 240(%0), %%xmm15\n\t"
                      : : "r"(p_from) : "memory");
}

static void store_xmm(uint8_t *p_to)
{
    __asm__ volatile (
                      "movdqu %%xmm0, 0(%0)\n\t"
                      "movdqu %%xmm1, 16(%0)\n\t"
                      "movdqu %%xmm2, 32(%0)\n\t"
                      "movdqu %%xmm3, 48(%0)\n\t"
                      "movdqu %%xmm4, 64(%0)\n\t"
                      "movdqu %%xmm5, 80(%0)\n\t"
                      "movdqu %%xmm6, 96(%0)\n\t"
                      "movdqu %%xmm7, 112(%0)\n\t"
                      "movdqu %%xmm8, 128(%0)\n\t"
                      "movdqu %%xmm9, 144(%0)\n\t"
                      "movdqu %%xmm10, 160(%0)\n\t"
                      "movdqu %%xmm11, 176(%0)\n\t"
                      "movdqu %%xmm12, 192(%0)\n\t"
                      "movdqu %%xmm13, 208(%0)\n\t"
                      "movdqu %%xmm14, 224(%0)\n\t"
                      "movdqu %%xmm15, 240(%0)\n\t"
                      : : "r"(p_to) : "memory");
}

static uint16_t get_fcw(void)
{
    uint16_t u_fcw;

    __asm__ volatile ("fnstcw %0" : "=m"(u_fcw));
    return u_fcw;
}

static void set_fcw(uint16_t u_fcw)
{
    __asm__ volatile ("fldcw %0" : : "m"(u_fcw));
}

static uint32_t get_mxcsr(void)
{
    uint32_t u_mxcsr;

    __asm__ volatile ("stmxcsr %0" : "=m"(u_mxcsr));
    return u_mxcsr;
}

static void set_mxcsr(uint32_t u_mxcsr)
{
    __asm__ volatile ("ldmxcsr %0" : : "m"(u_mxcsr));
}

/* a pattern no two callers share, from a seed */
static void fill(uint8_t *p_bytes, unsigned int u_seed)
{
    for (size_t sz_at = 0; sz_at < XMM_BYTES; sz_at++) {
        p_bytes[sz_at] = (uint8_t)(u_seed * 37 + sz_at * 11 + 1);
    }
}

static bool is_zero(const uint8_t *p_bytes)
{
    for (size_t sz_at = 0; sz_at < XMM_BYTES; sz_at++) {
        if (p_bytes[sz_at] != 0) {
            return false;
        }
    }
    return true;
}

/* what a handler finds: the abi's control words and empty registers */
static bool fresh(void)
{
    uint8_t arr_xmm[XMM_BYTES];

    store_xmm(arr_xmm);
    return get_fcw() == FCW_DEFAULT && get_mxcsr() == MXCSR_DEFAULT
           && is_zero(arr_xmm);
}

/*
 * what a program a pattern was left behind for finds: the abi's control
 * words, and none of the pattern in its registers, whatever its libc
 * has put there since
 */
static bool fresh_after(unsigned int u_seed)
{
    uint8_t arr_xmm[XMM_BYTES];
    uint8_t arr_old[XMM_BYTES];

    store_xmm(arr_xmm);
    fill(arr_old, u_seed);
    for (size_t sz_reg = 0; sz_reg < XMM_BYTES; sz_reg += 16) {
        for (size_t sz_old = 0; sz_old < XMM_BYTES; sz_old += 16) {
            if (memcmp(arr_xmm + sz_reg, arr_old + sz_old, 16) == 0) {
                return false;
            }
        }
    }
    return get_fcw() == FCW_DEFAULT && get_mxcsr() == MXCSR_DEFAULT;
}

/*
 * busy for a while, in ring 3 and in the kernel both: long enough that
 * the timer takes the core away more than once, and a sleep that gives
 * it away on purpose
 */
static void spend_time(void)
{
    struct timespec ts;
    struct timespec nap = { 0, 20 * 1000000L };

    raw3(SYS_clock_gettime, CLOCK_MONOTONIC, (long)(uintptr_t)&ts, 0);
    long i_until = ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + 300;
    for (;;) {
        raw3(SYS_clock_gettime, CLOCK_MONOTONIC, (long)(uintptr_t)&ts, 0);
        if (ts.tv_sec * 1000 + ts.tv_nsec / 1000000 >= i_until) {
            break;
        }
        for (volatile int i_spin = 0; i_spin < 100000; i_spin++) {
        }
    }
    raw3(SYS_nanosleep, (long)(uintptr_t)&nap, 0, 0);
}

static int child_status(pid_t i_pid)
{
    int i_status = -1;

    if (i_pid > 0) {
        waitpid(i_pid, &i_status, 0);
    }
    return i_status;
}

/* each child its own registers, held across the time it spends */
static int keep(unsigned int u_seed)
{
    uint8_t arr_want[XMM_BYTES];
    uint8_t arr_got[XMM_BYTES];
    uint16_t u_fcw = (uint16_t)((FCW_DEFAULT & ~0xc00u) | ((u_seed & 3) << 10));
    uint32_t u_mxcsr = (MXCSR_DEFAULT & ~0x6000u) | ((u_seed & 3) << 13);

    fill(arr_want, u_seed);
    load_xmm(arr_want);
    set_fcw(u_fcw);
    set_mxcsr(u_mxcsr);
    spend_time();
    store_xmm(arr_got);
    return memcmp(arr_want, arr_got, XMM_BYTES) == 0 && get_fcw() == u_fcw
           && get_mxcsr() == u_mxcsr ? 0 : 1;
}

static volatile sig_atomic_t s_i_caught;
static volatile bool s_b_handler_fresh;

/* a handler that finds a fresh set, and scribbles on it */
static void scribble(int i_sig)
{
    uint8_t arr_noise[XMM_BYTES];

    s_b_handler_fresh = fresh();
    fill(arr_noise, 99);
    load_xmm(arr_noise);
    set_mxcsr(MXCSR_DEFAULT | 0x6000);
    set_fcw(FCW_DEFAULT & ~0x300u);
    s_i_caught = i_sig;
}

static int interrupted(void)
{
    uint8_t arr_want[XMM_BYTES];
    uint8_t arr_got[XMM_BYTES];
    uint32_t u_mxcsr = MXCSR_DEFAULT | 0x2000;

    fill(arr_want, 7);
    load_xmm(arr_want);
    set_mxcsr(u_mxcsr);
    while (s_i_caught == 0) {
    }
    store_xmm(arr_got);
    return memcmp(arr_want, arr_got, XMM_BYTES) == 0
           && get_mxcsr() == u_mxcsr && get_fcw() == FCW_DEFAULT
           && s_b_handler_fresh ? 42 : 1;
}

static void x87_divide_by_zero(void)
{
    set_fcw(FCW_DEFAULT & ~0x4u);               /* unmask divide by zero */
    __asm__ volatile ("fld1\n\t"
                      "fldz\n\t"
                      "fdivrp\n\t"
                      "fwait");
}

int main(int i_argc, char **pp_argv)
{
    pid_t i_pid;

    if (i_argc >= 2 && ksft_streq(pp_argv[1], FRESH)) {
        return fresh_after(LEFT_BEHIND) ? 0 : 1;
    }

    ksft_print_header();
    ksft_set_plan(5);

    pid_t arr_pids[CHILDREN];
    for (int i_at = 0; i_at < CHILDREN; i_at++) {
        arr_pids[i_at] = fork();
        if (arr_pids[i_at] == 0) {
            _exit(keep((unsigned int)i_at + 1));
        }
    }
    bool b_kept = true;
    for (int i_at = 0; i_at < CHILDREN; i_at++) {
        b_kept = ksft_exited(child_status(arr_pids[i_at]), 0) && b_kept;
    }
    ksft_test_result(b_kept, "each program's registers outlast the others "
                             "running in between\n");

    uint8_t arr_want[XMM_BYTES];
    uint8_t arr_got[XMM_BYTES];
    fill(arr_want, 3);
    load_xmm(arr_want);
    i_pid = (pid_t)raw3(SYS_fork, 0, 0, 0);
    if (i_pid == 0) {
        store_xmm(arr_got);
        raw_exit(memcmp(arr_want, arr_got, XMM_BYTES) == 0 ? 0 : 1);
    }
    ksft_test_result(ksft_exited(child_status(i_pid), 0),
                     "a forked child starts with its parent's\n");

    i_pid = fork();
    if (i_pid == 0) {
        char *arr_args[] = { pp_argv[0], FRESH, NULL };
        uint8_t arr_left[XMM_BYTES];
        fill(arr_left, LEFT_BEHIND);
        set_mxcsr(MXCSR_DEFAULT | 0x8000);
        set_fcw(FCW_DEFAULT & ~0x300u);
        load_xmm(arr_left);
        raw3(SYS_execve, (long)(uintptr_t)pp_argv[0],
             (long)(uintptr_t)arr_args, (long)(uintptr_t)environ);
        raw_exit(2);
    }
    ksft_test_result(ksft_exited(child_status(i_pid), 0),
                     "and a program it runs starts over\n");

    i_pid = fork();
    if (i_pid == 0) {
        x87_divide_by_zero();
        _exit(0);
    }
    ksft_test_result(ksft_killed_by(child_status(i_pid), SIGFPE),
                     "an x87 exception it unmasked is SIGFPE\n");

    signal(SIGTERM, scribble);
    i_pid = fork();
    if (i_pid == 0) {
        _exit(interrupted());
    }
    signal(SIGTERM, SIG_DFL);
    ksft_sleep_ms(50);
    kill(i_pid, SIGTERM);
    ksft_test_result(ksft_exited(child_status(i_pid), 42),
                     "a handler gets a fresh set, and the program its own "
                     "back\n");
    ksft_finished();
}
