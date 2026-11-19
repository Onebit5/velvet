// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/robust.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * futexes in memory processes share: a wake from one reaching a waiter
 * in another, and a robust mutex whose owner ended holding it handed to
 * the next with word of that, however its owner ended.
 */

#include "../kselftest.h"

#include <pthread.h>
#include <sys/mman.h>
#include <velvet/futex.h>

/* what two processes share: a word, a flag, and a mutex */
struct both {
    int i_word;
    volatile int i_ready;
    pthread_mutex_t mutex;
};

static long futex(int *p_word, int i_op, int i_val, long i_ms)
{
    struct timespec ts = { i_ms / 1000, (i_ms % 1000) * 1000000L };
    return syscall(SYS_futex, p_word, i_op, i_val,
                   (i_ms > 0) ? &ts : NULL, NULL, 0);
}

/* a robust mutex in p_both, which processes may share */
static bool made(struct both *p_both)
{
    pthread_mutexattr_t attr;
    return pthread_mutexattr_init(&attr) == 0
           && pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == 0
           && pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) == 0
           && pthread_mutex_init(&p_both->mutex, &attr) == 0;
}

/* the child's end, after it took the mutex: 0 exits, 1 is killed */
static pid_t owner(struct both *p_both, int i_how, int i_hold_ms)
{
    p_both->i_ready = 0;
    pid_t i_pid = fork();
    if (i_pid == 0) {
        pthread_mutex_lock(&p_both->mutex);
        p_both->i_ready = 1;
        ksft_sleep_ms(i_hold_ms);
        if (i_how == 1) {
            kill(getpid(), SIGKILL);
        }
        _exit(0);
    }
    while (i_pid > 0 && !p_both->i_ready) {
        ksft_sleep_ms(5);
    }
    return i_pid;
}

/* the mutex taken after its owner ended, and made whole again */
static bool inherited(struct both *p_both, int i_got)
{
    return i_got == EOWNERDEAD
           && pthread_mutex_consistent(&p_both->mutex) == 0
           && pthread_mutex_unlock(&p_both->mutex) == 0
           && pthread_mutex_lock(&p_both->mutex) == 0
           && pthread_mutex_unlock(&p_both->mutex) == 0;
}

int main(void)
{
    ksft_print_header();
    ksft_set_plan(5);

    struct both *p_both = mmap(NULL, sizeof *p_both, PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p_both == MAP_FAILED || !made(p_both)) {
        ksft_print_msg("no shared memory, or no mutex in it: %s\n",
                       strerror(errno));
        ksft_finished();
    }

    /* a waiter in a child, woken from here */
    int i_status = -1;
    pid_t i_pid = fork();
    if (i_pid == 0) {
        _exit((futex(&p_both->i_word, FUTEX_WAIT, 0, 3000) == 0) ? 0 : 1);
    }
    long i_woken = 0;
    for (int i_try = 0; i_try < 300 && i_woken == 0; i_try++) {
        ksft_sleep_ms(10);
        i_woken = futex(&p_both->i_word, FUTEX_WAKE, 1, 0);
    }
    waitpid(i_pid, &i_status, 0);
    ksft_test_result(i_woken == 1 && ksft_exited(i_status, 0),
                     "a wake reaches a waiter in another process on a word "
                     "they share\n");

    i_pid = fork();
    if (i_pid == 0) {
        long i_got = futex(&p_both->i_word,
                           FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, 500);
        _exit((i_got == -1 && errno == ETIMEDOUT) ? 0 : 1);
    }
    ksft_sleep_ms(100);
    i_woken = futex(&p_both->i_word, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, 0);
    waitpid(i_pid, &i_status, 0);
    ksft_test_result(i_woken == 0 && ksft_exited(i_status, 0),
                     "but a private one is the waker's own process's\n");

    i_pid = owner(p_both, 0, 0);
    waitpid(i_pid, &i_status, 0);
    ksft_test_result(ksft_exited(i_status, 0)
                     && inherited(p_both, pthread_mutex_lock(&p_both->mutex)),
                     "a robust mutex its owner exited holding is the next "
                     "one's, with EOWNERDEAD\n");

    i_pid = owner(p_both, 1, 0);
    waitpid(i_pid, &i_status, 0);
    ksft_test_result(ksft_killed_by(i_status, SIGKILL)
                     && inherited(p_both, pthread_mutex_lock(&p_both->mutex)),
                     "and one its owner was killed holding the same\n");

    /* waiting for it while the owner ends */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 3;
    i_pid = owner(p_both, 0, 200);
    int i_got = pthread_mutex_timedlock(&p_both->mutex, &ts);
    waitpid(i_pid, &i_status, 0);
    ksft_test_result(inherited(p_both, i_got),
                     "and a thread waiting for it when its owner ends is "
                     "woken to take it\n");
    ksft_finished();
}
