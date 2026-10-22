// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/x86_64/fault.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a fault in ring 3 ends the program that raised it, and nothing else.
 */

#include "../kselftest.h"

/*
 * one child per fault, each doing the one thing it is named for. what
 * the parent reads back is what it reads for any program a signal ends:
 * 128 plus the signal, the number a shell prints
 */

/*
 * NULL from somewhere the compiler cannot see into: a store it can prove
 * goes through NULL is one it may drop as undefined
 */
static volatile uintptr_t s_u_null;

static void null_write(void)
{
    *(volatile int64_t *)s_u_null = 1;
}

static void invalid_opcode(void)
{
    __asm__ volatile ("ud2");
}

static void divide_by_zero(void)
{
    /*
     * 100 and not 1: gcc turns 1 / x into a compare, since the only
     * answers are 1, -1 and 0, and never emits the divide
     */
    volatile int64_t i_zero = 0;
    volatile int64_t i_result = 100 / i_zero;

    (void)i_result;
}

static void privileged(void)
{
    __asm__ volatile ("hlt");       /* ring 0 only, so #GP from ring 3 */
}

static void breakpoint(void)
{
    __asm__ volatile ("int3");
}

static const struct {
    void (*p_fault)(void);
    int i_signal;
    const char *str_name;
} FAULTS[] = {
    { null_write,     SIGSEGV, "a write through NULL ends it with SIGSEGV" },
    { invalid_opcode, SIGILL,  "ud2 ends it with SIGILL" },
    { divide_by_zero, SIGFPE,  "a divide by zero ends it with SIGFPE" },
    { privileged,     SIGSEGV, "hlt ends it with SIGSEGV" },
    { breakpoint,     SIGTRAP, "int3 ends it with SIGTRAP" },
};

/*
 * where a handler returns to: rsp is then the frame the kernel saved,
 * which is what sigreturn is handed
 */
static __attribute__((naked)) void sigreturn_stub(void)
{
    __asm__ volatile ("mov %%rsp, %%rdi\n\t"
                      "mov %0, %%eax\n\t"
                      "syscall\n\t"
                      "ud2"
                      : : "i"(SYS_SIGRETURN));
}

static void handle(int i_sig, void (*p_handler)(int))
{
    ksft_syscall(SYS_SIGNAL, i_sig, (int64_t)(uintptr_t)p_handler,
                 (int64_t)(uintptr_t)sigreturn_stub);
}

static void exit_40(int i_sig)
{
    exit(i_sig == SIGFPE ? 40 : 1);
}

static volatile int s_i_caught;

static void note(int i_sig)
{
    s_i_caught = i_sig;
}

/* what the child ended with, or -1000 when there was no child */
static int child_status(int i_pid)
{
    int i_status = -1000;

    if (i_pid > 0) {
        wait(i_pid, &i_status);
    }
    return i_status;
}

int main(int i_argc, char **pp_argv)
{
    size_t sz_count = sizeof(FAULTS) / sizeof(FAULTS[0]);
    int i_pid;
    int i_status;

    (void)i_argc;
    (void)pp_argv;
    ksft_print_header();
    ksft_set_plan(sz_count + 3);

    for (size_t sz_at = 0; sz_at < sz_count; sz_at++) {
        i_pid = fork();
        if (i_pid == 0) {
            FAULTS[sz_at].p_fault();
            exit(0);        /* still here, so the fault did not end it */
        }

        int i_want = 128 + FAULTS[sz_at].i_signal;
        i_status = child_status(i_pid);
        if (i_status != i_want) {
            ksft_print_msg("wanted %d, read %d\n", i_want, i_status);
        }
        ksft_test_result(i_status == i_want, "%s\n", FAULTS[sz_at].str_name);
    }

    /*
     * ignoring a fault would return the program to the instruction that
     * raised it, to raise it again forever
     */
    i_pid = fork();
    if (i_pid == 0) {
        ksft_syscall(SYS_SIGNAL, SIGSEGV, SIG_IGNORE, 0);
        null_write();
        exit(0);
    }
    i_status = child_status(i_pid);
    ksft_test_result(i_status == 128 + SIGSEGV,
                     "an ignored SIGSEGV ends it all the same\n");

    /* and a handler is the program's to have */
    i_pid = fork();
    if (i_pid == 0) {
        handle(SIGFPE, exit_40);
        divide_by_zero();
        exit(0);
    }
    i_status = child_status(i_pid);
    ksft_test_result(i_status == 40, "a handler catches a divide by zero\n");

    /*
     * a loop that makes no calls is reached by the timer's tick, which
     * runs the handler and, through sigreturn, gives the loop back
     * exactly as it was. the handler is set before the fork so that the
     * child has it from its first instruction
     */
    handle(SIGTERM, note);
    i_pid = fork();
    if (i_pid == 0) {
        while (s_i_caught == 0) {
        }
        exit(s_i_caught == SIGTERM ? 42 : 1);
    }
    ksft_syscall(SYS_SIGNAL, SIGTERM, SIG_DEFAULT, 0);
    sleep_ms(50);
    sigsend(i_pid, SIGTERM);
    i_status = child_status(i_pid);
    ksft_test_result(i_status == 42,
                     "a busy loop runs its handler and carries on\n");
    ksft_finished();
}
