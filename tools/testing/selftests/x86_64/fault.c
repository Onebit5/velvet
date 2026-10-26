// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/x86_64/fault.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a fault in ring 3 ends the program that raised it, and nothing else.
 */

#include "../kselftest.h"

/*
 * one child per fault, each doing the one thing it is named for. what
 * the parent reads back is what it reads for any program a signal ends,
 * the signal that ended it
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
    int i_code;
    const char *str_name;
} FAULTS[] = {
    { null_write,     SIGSEGV, SEGV_MAPERR,
      "a write through NULL is SIGSEGV, nothing mapped there" },
    { invalid_opcode, SIGILL,  ILL_ILLOPN, "ud2 is SIGILL" },
    { divide_by_zero, SIGFPE,  FPE_INTDIV, "a divide by zero is SIGFPE" },
    { privileged,     SIGSEGV, SI_KERNEL,  "hlt is SIGSEGV" },
    { breakpoint,     SIGTRAP, SI_KERNEL,  "int3 is SIGTRAP" },
};

/* what a fault's handler was told, written back as its exit code */
static void tell(int i_sig, siginfo_t *p_info, void *p_context)
{
    (void)p_context;
    _exit(i_sig == p_info->si_signo ? (p_info->si_code & 0xff) : 255);
}

static void exit_40(int i_sig)
{
    _exit(i_sig == SIGFPE ? 40 : 1);
}

static volatile sig_atomic_t s_i_caught;

static void note(int i_sig)
{
    s_i_caught = i_sig;
}

static int child_status(pid_t i_pid)
{
    int i_status = -1;

    if (i_pid > 0) {
        waitpid(i_pid, &i_status, 0);
    }
    return i_status;
}

int main(void)
{
    size_t sz_count = sizeof(FAULTS) / sizeof(FAULTS[0]);
    pid_t i_pid;
    int i_status;

    ksft_print_header();
    ksft_set_plan(2 * sz_count + 4);

    for (size_t sz_at = 0; sz_at < sz_count; sz_at++) {
        i_pid = fork();
        if (i_pid == 0) {
            FAULTS[sz_at].p_fault();
            _exit(0);       /* still here, so the fault did not end it */
        }
        i_status = child_status(i_pid);
        ksft_test_result(ksft_killed_by(i_status, FAULTS[sz_at].i_signal),
                         "%s\n", FAULTS[sz_at].str_name);

        /* and a handler told why, as the siginfo's code */
        i_pid = fork();
        if (i_pid == 0) {
            struct sigaction act = { .sa_sigaction = tell,
                                     .sa_flags = SA_SIGINFO };
            sigaction(FAULTS[sz_at].i_signal, &act, NULL);
            FAULTS[sz_at].p_fault();
            _exit(254);
        }
        i_status = child_status(i_pid);
        ksft_test_result(ksft_exited(i_status, FAULTS[sz_at].i_code & 0xff),
                         "and its handler is told why\n");
    }

    /* where it was, for a fault on memory */
    i_pid = fork();
    if (i_pid == 0) {
        s_u_null = 0x1230;
        struct sigaction act = { .sa_sigaction = tell,
                                 .sa_flags = SA_SIGINFO };
        sigaction(SIGSEGV, &act, NULL);
        null_write();
        _exit(254);
    }
    waitpid(i_pid, &i_status, 0);
    ksft_test_result(ksft_exited(i_status, SEGV_MAPERR),
                     "a fault on memory is a fault on memory\n");

    /*
     * ignoring a fault would return the program to the instruction that
     * raised it, to raise it again forever
     */
    i_pid = fork();
    if (i_pid == 0) {
        signal(SIGSEGV, SIG_IGN);
        null_write();
        _exit(0);
    }
    ksft_test_result(ksft_killed_by(child_status(i_pid), SIGSEGV),
                     "an ignored SIGSEGV ends it all the same\n");

    /* and a handler is the program's to have */
    i_pid = fork();
    if (i_pid == 0) {
        signal(SIGFPE, exit_40);
        divide_by_zero();
        _exit(0);
    }
    ksft_test_result(ksft_exited(child_status(i_pid), 40),
                     "a handler catches a divide by zero\n");

    /*
     * a loop that makes no calls is reached by the timer's tick, which
     * runs the handler and, through sigreturn, gives the loop back
     * exactly as it was. the handler is set before the fork so that the
     * child has it from its first instruction
     */
    signal(SIGTERM, note);
    i_pid = fork();
    if (i_pid == 0) {
        while (s_i_caught == 0) {
        }
        _exit(s_i_caught == SIGTERM ? 42 : 1);
    }
    signal(SIGTERM, SIG_DFL);
    ksft_sleep_ms(50);
    kill(i_pid, SIGTERM);
    ksft_test_result(ksft_exited(child_status(i_pid), 42),
                     "a busy loop runs its handler and carries on\n");
    ksft_finished();
}
