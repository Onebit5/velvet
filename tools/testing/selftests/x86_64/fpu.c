// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/x86_64/fpu.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the x87 and sse registers are each program's own.
 */

#include "../kselftest.h"

/* the sixteen xmm registers, 16 bytes apiece */
#define XMM_BYTES 256

/* more children than a small machine has cores, so they take turns */
#define CHILDREN 6

#define FCW_DEFAULT   0x037f
#define MXCSR_DEFAULT 0x1f80

#define FRESH "--fresh"

/*
 * written in assembly because the selftests are built without sse, so
 * the compiler never touches these registers and what is in them is
 * exactly what the test put there
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

/* what a program finds: the abi's control words and empty registers */
static bool fresh(void)
{
    uint8_t arr_xmm[XMM_BYTES];

    store_xmm(arr_xmm);
    return get_fcw() == FCW_DEFAULT && get_mxcsr() == MXCSR_DEFAULT
           && is_zero(arr_xmm);
}

/*
 * busy for a while, in ring 3 and in the kernel both: long enough that
 * the timer takes the core away more than once, and a sleep that gives
 * it away on purpose
 */
static void spend_time(void)
{
    int64_t i_until = ksft_syscall(SYS_UPTIME, 0, 0, 0) + 300;

    while (ksft_syscall(SYS_UPTIME, 0, 0, 0) < i_until) {
        for (volatile int i_spin = 0; i_spin < 100000; i_spin++) {
        }
    }
    sleep_ms(20);
}

static int child_status(int i_pid)
{
    int i_status = -1000;

    if (i_pid > 0) {
        wait(i_pid, &i_status);
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
    return ksft_memeq(arr_want, arr_got, XMM_BYTES) && get_fcw() == u_fcw
           && get_mxcsr() == u_mxcsr ? 0 : 1;
}

static __attribute__((naked)) void sigreturn_stub(void)
{
    __asm__ volatile ("mov %%rsp, %%rdi\n\t"
                      "mov %0, %%eax\n\t"
                      "syscall\n\t"
                      "ud2"
                      : : "i"(SYS_SIGRETURN));
}

static volatile int s_i_caught;
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
    return ksft_memeq(arr_want, arr_got, XMM_BYTES)
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
    int i_pid;
    int i_status;

    if (i_argc >= 2 && ksft_streq(pp_argv[1], FRESH)) {
        exit(fresh() ? 0 : 1);
    }

    ksft_print_header();
    ksft_set_plan(6);

    ksft_test_result(fresh(),
                     "a program starts with the abi's control words "
                     "and empty registers\n");

    int arr_pids[CHILDREN];
    for (int i_at = 0; i_at < CHILDREN; i_at++) {
        arr_pids[i_at] = fork();
        if (arr_pids[i_at] == 0) {
            exit(keep((unsigned int)i_at + 1));
        }
    }
    bool b_kept = true;
    for (int i_at = 0; i_at < CHILDREN; i_at++) {
        b_kept = child_status(arr_pids[i_at]) == 0 && b_kept;
    }
    ksft_test_result(b_kept, "each program's registers outlast the others "
                             "running in between\n");

    uint8_t arr_want[XMM_BYTES];
    uint8_t arr_got[XMM_BYTES];
    fill(arr_want, 3);
    load_xmm(arr_want);
    i_pid = fork();
    if (i_pid == 0) {
        store_xmm(arr_got);
        exit(ksft_memeq(arr_want, arr_got, XMM_BYTES) ? 0 : 1);
    }
    ksft_test_result(child_status(i_pid) == 0,
                     "a forked child starts with its parent's\n");

    i_pid = fork();
    if (i_pid == 0) {
        char *arr_args[] = { pp_argv[0], FRESH, NULL };
        set_mxcsr(MXCSR_DEFAULT | 0x8000);
        set_fcw(FCW_DEFAULT & ~0x300u);
        execv(pp_argv[0], arr_args);
        exit(2);
    }
    ksft_test_result(child_status(i_pid) == 0,
                     "and a program it runs starts over\n");

    i_pid = fork();
    if (i_pid == 0) {
        x87_divide_by_zero();
        exit(0);
    }
    i_status = child_status(i_pid);
    ksft_test_result(i_status == 128 + SIGFPE,
                     "an x87 exception it unmasked is SIGFPE\n");

    ksft_syscall(SYS_SIGNAL, SIGTERM, (int64_t)(uintptr_t)scribble,
                 (int64_t)(uintptr_t)sigreturn_stub);
    i_pid = fork();
    if (i_pid == 0) {
        exit(interrupted());
    }
    ksft_syscall(SYS_SIGNAL, SIGTERM, SIG_DEFAULT, 0);
    sleep_ms(50);
    sigsend(i_pid, SIGTERM);
    i_status = child_status(i_pid);
    ksft_test_result(i_status == 42,
                     "a handler gets a fresh set, and the program its own "
                     "back\n");
    ksft_finished();
}
