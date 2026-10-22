// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/signal.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * who a program may send a signal to.
 */

#include "../kselftest.h"

/* where a handler returns to, with rsp at the frame the kernel saved */
static __attribute__((naked)) void sigreturn_stub(void)
{
    __asm__ volatile ("mov %%rsp, %%rdi\n\t"
                      "mov %0, %%eax\n\t"
                      "syscall\n\t"
                      "ud2"
                      : : "i"(SYS_SIGRETURN));
}

static void never(int i_sig)
{
    exit(i_sig);
}

/* a page the program may read and not write */
static const char s_arr_rodata[8192] __attribute__((aligned(4096))) = "ro";

int main(int i_argc, char **pp_argv)
{
    int i_status = -1;

    (void)i_argc;
    (void)pp_argv;
    ksft_print_header();
    ksft_set_plan(4);

    /* init is root's, and a guest stopping it would stop the machine */
    ksft_test_result(sigsend(1, SIGTERM) == -1,
                     "a guest may not signal init\n");
    ksft_test_result(sigsend(30000, SIGTERM) == -1,
                     "nor a pid that is not there\n");

    int i_pid = fork();
    if (i_pid == 0) {
        for (;;) {
            sleep_ms(100);
        }
    }
    ksft_test_result(sigsend(i_pid, SIGTERM) == 0
                     && wait(i_pid, &i_status) == i_pid
                     && i_status == 128 + SIGTERM,
                     "but may signal its own child\n");

    /*
     * a handler's frame goes on the program's stack, so a stack the
     * program may not write has no room for one, and the signal's
     * default happens instead. the loop makes no calls, so it is the
     * timer's tick that delivers
     */
    ksft_syscall(SYS_SIGNAL, SIGTERM, (int64_t)(uintptr_t)never,
                 (int64_t)(uintptr_t)sigreturn_stub);
    i_pid = fork();
    if (i_pid == 0) {
        __asm__ volatile ("mov %0, %%rsp\n\t"
                          "1: jmp 1b"
                          : : "r"(s_arr_rodata + sizeof s_arr_rodata));
    }
    ksft_syscall(SYS_SIGNAL, SIGTERM, SIG_DEFAULT, 0);
    sleep_ms(50);
    ksft_test_result(sigsend(i_pid, SIGTERM) == 0
                     && wait(i_pid, &i_status) == i_pid
                     && i_status == 128 + SIGTERM,
                     "a stack it may not write gets no handler\n");
    ksft_finished();
}
