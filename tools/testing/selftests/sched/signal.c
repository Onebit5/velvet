// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/signal.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * who a program may send a signal to.
 */

#include "../kselftest.h"

#include <sys/time.h>

/* a page the program may read and not write */
static const char s_arr_rodata[8192] __attribute__((aligned(4096))) = "ro";

static volatile sig_atomic_t s_i_caught;
static volatile pid_t s_i_from;
static volatile int s_i_code;
static volatile uintptr_t s_u_where;

static void note(int i_sig)
{
    s_i_caught = i_sig;
}

static void note_info(int i_sig, siginfo_t *p_info, void *p_context)
{
    (void)p_context;
    s_i_caught = i_sig;
    s_i_from = p_info->si_pid;
    s_i_code = p_info->si_code;
}

/* where the handler's own stack is, to see which stack it ran on */
static void note_stack(int i_sig)
{
    volatile char c_here = 0;

    s_u_where = (uintptr_t)&c_here;
    s_i_caught = i_sig;
}

static void catch_with(int i_sig, void (*p_fn)(int), int i_flags)
{
    struct sigaction act = { .sa_handler = p_fn, .sa_flags = i_flags };
    sigaction(i_sig, &act, NULL);
}

/* a child that sends `i_sig` to its parent after `i_ms` */
static pid_t send_later(int i_sig, long i_ms)
{
    pid_t i_parent = getpid();
    pid_t i_pid = fork();

    if (i_pid == 0) {
        ksft_sleep_ms(i_ms);
        kill(i_parent, i_sig);
        _exit(0);
    }
    return i_pid;
}

static void reap(pid_t i_pid)
{
    int i_status;

    waitpid(i_pid, &i_status, 0);
}

int main(void)
{
    int i_status = -1;
    int arr_fds[2];
    char arr_buf[8];
    sigset_t set;
    sigset_t was;

    ksft_print_header();
    ksft_set_plan(13);

    /* init is root's, and a guest stopping it would stop the machine */
    ksft_test_result(kill(1, SIGTERM) == -1 && errno == EPERM,
                     "a guest may not signal init\n");
    ksft_test_result(kill(30000, SIGTERM) == -1 && errno == ESRCH,
                     "nor a pid that is not there\n");

    pid_t i_pid = fork();
    if (i_pid == 0) {
        for (;;) {
            ksft_sleep_ms(100);
        }
    }
    ksft_test_result(kill(i_pid, SIGTERM) == 0
                     && waitpid(i_pid, &i_status, 0) == i_pid
                     && ksft_killed_by(i_status, SIGTERM),
                     "but may signal its own child\n");

    /*
     * a handler's frame goes on the program's stack, so a stack the
     * program may not write has no room for one, and the program is
     * ended as a fault would end it. the loop makes no calls, so it is
     * the timer's tick that delivers
     */
    catch_with(SIGTERM, note, 0);
    i_pid = fork();
    if (i_pid == 0) {
        __asm__ volatile ("mov %0, %%rsp\n\t"
                          "1: jmp 1b"
                          : : "r"(s_arr_rodata + sizeof s_arr_rodata));
    }
    signal(SIGTERM, SIG_DFL);
    ksft_sleep_ms(50);
    ksft_test_result(kill(i_pid, SIGTERM) == 0
                     && waitpid(i_pid, &i_status, 0) == i_pid
                     && ksft_killed_by(i_status, SIGSEGV),
                     "a stack it may not write gets no handler, and is "
                     "SIGSEGV\n");

    /* who sent it, and that a program did */
    struct sigaction info = { .sa_sigaction = note_info,
                              .sa_flags = SA_SIGINFO };
    sigaction(SIGUSR1, &info, NULL);
    s_i_caught = 0;
    i_pid = send_later(SIGUSR1, 50);
    while (s_i_caught == 0) {
        ksft_sleep_ms(10);
    }
    reap(i_pid);
    ksft_test_result(s_i_from == i_pid && s_i_code == SI_USER,
                     "a handler is told who sent the signal\n");

    /* blocked, a signal waits, and arrives once let through */
    catch_with(SIGUSR1, note, 0);
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, &was);
    s_i_caught = 0;
    raise(SIGUSR1);
    sigset_t pending;
    sigpending(&pending);
    bool b_waited = s_i_caught == 0 && sigismember(&pending, SIGUSR1);
    sigprocmask(SIG_SETMASK, &was, NULL);
    ksft_test_result(b_waited && s_i_caught == SIGUSR1,
                     "a blocked signal waits, and comes when unblocked\n");

    /* a read cut short: begun again under SA_RESTART, EINTR without */
    pipe(arr_fds);
    catch_with(SIGALRM, note, SA_RESTART);
    pid_t i_writer = fork();
    if (i_writer == 0) {
        ksft_sleep_ms(300);
        write(arr_fds[1], "late", 4);
        _exit(0);
    }
    s_i_caught = 0;
    struct itimerval soon = { .it_value = { 0, 100000 } };
    setitimer(ITIMER_REAL, &soon, NULL);
    ksft_test_result(read(arr_fds[0], arr_buf, 4) == 4
                     && s_i_caught == SIGALRM,
                     "under SA_RESTART a read a handler cut short goes "
                     "on\n");
    reap(i_writer);
    catch_with(SIGALRM, note, 0);
    s_i_caught = 0;
    setitimer(ITIMER_REAL, &soon, NULL);
    ksft_test_result(read(arr_fds[0], arr_buf, 4) == -1 && errno == EINTR
                     && s_i_caught == SIGALRM,
                     "and without it is EINTR\n");

    /* sigsuspend waits under its mask, and the old one comes back */
    sigprocmask(SIG_BLOCK, &set, &was);
    catch_with(SIGUSR1, note, 0);
    s_i_caught = 0;
    i_pid = send_later(SIGUSR1, 50);
    sigset_t open_mask;
    sigemptyset(&open_mask);
    bool b_suspended = sigsuspend(&open_mask) == -1 && errno == EINTR
                       && s_i_caught == SIGUSR1;
    sigset_t now;
    sigprocmask(SIG_SETMASK, NULL, &now);
    reap(i_pid);
    ksft_test_result(b_suspended && sigismember(&now, SIGUSR1),
                     "sigsuspend waits for one, and puts the mask back\n");

    /* sigtimedwait takes one without its handler, or times out */
    i_pid = send_later(SIGUSR1, 50);
    siginfo_t got;
    struct timespec wait_for = { 2, 0 };
    s_i_caught = 0;
    bool b_took = sigtimedwait(&set, &got, &wait_for) == SIGUSR1
                  && got.si_pid == i_pid && s_i_caught == 0;
    reap(i_pid);
    struct timespec brief = { 0, 50000000 };
    ksft_test_result(b_took && sigtimedwait(&set, &got, &brief) == -1
                     && errno == EAGAIN,
                     "sigtimedwait takes a blocked one, or says EAGAIN\n");
    sigprocmask(SIG_SETMASK, &was, NULL);

    /* a handler on the alternate stack, when the action asks for it */
    static char s_arr_alt[16384];
    stack_t alt = { .ss_sp = s_arr_alt, .ss_size = sizeof s_arr_alt };
    catch_with(SIGUSR2, note_stack, SA_ONSTACK);
    s_i_caught = 0;
    bool b_alt = sigaltstack(&alt, NULL) == 0 && raise(SIGUSR2) == 0
                 && s_i_caught == SIGUSR2
                 && s_u_where >= (uintptr_t)s_arr_alt
                 && s_u_where < (uintptr_t)s_arr_alt + sizeof s_arr_alt;
    ksft_test_result(b_alt, "SA_ONSTACK runs it on the alternate stack\n");

    /* SA_RESETHAND: once, and the default after */
    catch_with(SIGUSR2, note, SA_RESETHAND);
    raise(SIGUSR2);
    struct sigaction after;
    sigaction(SIGUSR2, NULL, &after);
    ksft_test_result(after.sa_handler == SIG_DFL,
                     "SA_RESETHAND leaves the default behind\n");

    /* alarm, and pause until it comes */
    catch_with(SIGALRM, note, 0);
    s_i_caught = 0;
    alarm(1);
    ksft_test_result(pause() == -1 && errno == EINTR
                     && s_i_caught == SIGALRM && alarm(0) == 0,
                     "an alarm ends a pause\n");
    ksft_finished();
}
