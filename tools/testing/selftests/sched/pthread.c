// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/pthread.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a libc's threads, as a program written against them uses them.
 */

#include "../kselftest.h"

#include <pthread.h>

/* how many threads count at once, and how far each */
#define COUNTERS 4
#define COUNT_TO 20000

static pid_t gettid_now(void)
{
    return (pid_t)syscall(SYS_gettid);
}

/* a thread's own view of who it is */
struct who {
    pid_t i_pid;
    pid_t i_tid;
};

static void *say_who(void *p_arg)
{
    struct who *p_who = p_arg;
    p_who->i_pid = getpid();
    p_who->i_tid = gettid_now();
    return (void *)(uintptr_t)0x5a;
}

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static long s_l_count;

static void *count(void *p_arg)
{
    (void)p_arg;
    for (int i_at = 0; i_at < COUNT_TO; i_at++) {
        pthread_mutex_lock(&s_lock);
        s_l_count++;
        pthread_mutex_unlock(&s_lock);
    }
    return NULL;
}

/* a value handed over through a condition variable, one at a time */
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
static int s_i_box;
static bool s_b_full;

static void *produce(void *p_arg)
{
    (void)p_arg;
    for (int i_at = 1; i_at <= 100; i_at++) {
        pthread_mutex_lock(&s_lock);
        while (s_b_full) {
            pthread_cond_wait(&s_cond, &s_lock);
        }
        s_i_box = i_at;
        s_b_full = true;
        pthread_cond_broadcast(&s_cond);
        pthread_mutex_unlock(&s_lock);
    }
    return NULL;
}

static __thread int s_i_mine = 11;

static void *change_mine(void *p_arg)
{
    s_i_mine = 22;
    *(int *)p_arg = s_i_mine;
    return NULL;
}

/* which thread a handler ran in */
static volatile pid_t s_i_ran_in;

static void note_thread(int i_sig)
{
    (void)i_sig;
    s_i_ran_in = gettid_now();
}

static volatile pid_t s_i_waiter;

/*
 * a thread that waits for a handler to have run somewhere, and with
 * p_arg, unblocks SIGUSR2 first, which it was made with blocked
 */
static void *wait_for_it(void *p_arg)
{
    if (p_arg != NULL) {
        sigset_t mine;
        sigemptyset(&mine);
        sigaddset(&mine, SIGUSR2);
        pthread_sigmask(SIG_UNBLOCK, &mine, NULL);
    }
    s_i_waiter = gettid_now();
    while (s_i_ran_in == 0) {
        usleep(1000);
    }
    return NULL;
}

static void *sleep_long(void *p_arg)
{
    (void)p_arg;
    for (;;) {
        sleep(10);
    }
    return NULL;
}

static void *exit_six(void *p_arg)
{
    (void)p_arg;
    usleep(20000);
    _exit(6);
}

static int s_i_pipe[2];

static void *outlive_main(void *p_arg)
{
    (void)p_arg;
    usleep(30000);
    write(s_i_pipe[1], "late", 4);
    return NULL;
}

static char *s_str_self;

static void *run_again(void *p_arg)
{
    char arr_pid[16];
    snprintf(arr_pid, sizeof arr_pid, "%d", (int)getpid());
    char *arr_argv[] = { s_str_self, "exec-child", arr_pid, NULL };
    execv(s_str_self, arr_argv);
    (void)p_arg;
    _exit(1);
}

static void *fork_here(void *p_arg)
{
    (void)p_arg;
    pid_t i_kid = fork();
    if (i_kid == 0) {
        _exit(getpid() == gettid_now() ? 7 : 1);
    }
    int i_status = 0;
    waitpid(i_kid, &i_status, 0);
    return (void *)(intptr_t)i_status;
}

/* the status a child ended with, or -1 */
static int status_of(pid_t i_pid)
{
    int i_status = 0;
    return (waitpid(i_pid, &i_status, 0) == i_pid) ? i_status : -1;
}

int main(int i_argc, char **arr_argv)
{
    /* what a thread ran: the same process, its only thread now */
    if (i_argc == 3 && ksft_streq(arr_argv[1], "exec-child")) {
        bool b_same = getpid() == atoi(arr_argv[2])
                      && gettid_now() == getpid();
        return b_same ? 0x33 : 1;
    }
    s_str_self = arr_argv[0];

    ksft_print_header();
    ksft_set_plan(12);

    struct who who = { 0, 0 };
    pthread_t thread;
    void *p_back = NULL;
    bool b_ok = pthread_create(&thread, NULL, say_who, &who) == 0
                && pthread_join(thread, &p_back) == 0;
    ksft_test_result(b_ok && p_back == (void *)(uintptr_t)0x5a,
                     "a thread runs, and is joined with what it returned\n");
    ksft_test_result(who.i_pid == getpid() && who.i_tid != getpid()
                     && who.i_tid > 0 && gettid_now() == getpid(),
                     "in the same process, a thread of its own\n");

    pthread_t arr_counters[COUNTERS];
    for (int i_at = 0; i_at < COUNTERS; i_at++) {
        pthread_create(&arr_counters[i_at], NULL, count, NULL);
    }
    for (int i_at = 0; i_at < COUNTERS; i_at++) {
        pthread_join(arr_counters[i_at], NULL);
    }
    ksft_test_result(s_l_count == (long)COUNTERS * COUNT_TO,
                     "threads counting under one mutex miss nothing\n");

    long l_sum = 0;
    pthread_create(&thread, NULL, produce, NULL);
    for (int i_at = 1; i_at <= 100; i_at++) {
        pthread_mutex_lock(&s_lock);
        while (!s_b_full) {
            pthread_cond_wait(&s_cond, &s_lock);
        }
        l_sum += s_i_box;
        s_b_full = false;
        pthread_cond_broadcast(&s_cond);
        pthread_mutex_unlock(&s_lock);
    }
    pthread_join(thread, NULL);
    ksft_test_result(l_sum == 5050,
                     "and a condition variable hands each value over once\n");

    int i_theirs = 0;
    pthread_create(&thread, NULL, change_mine, &i_theirs);
    pthread_join(thread, NULL);
    ksft_test_result(i_theirs == 22 && s_i_mine == 11,
                     "a thread's own variables are its own\n");

    /* a signal at one thread, and one at the process */
    signal(SIGUSR1, note_thread);
    signal(SIGUSR2, note_thread);
    pthread_create(&thread, NULL, wait_for_it, NULL);
    while (s_i_waiter == 0) {
        usleep(1000);
    }
    pthread_kill(thread, SIGUSR1);
    pthread_join(thread, NULL);
    ksft_test_result(s_i_ran_in == s_i_waiter && s_i_ran_in != getpid(),
                     "a signal sent to a thread runs its handler there\n");

    s_i_ran_in = 0;
    s_i_waiter = 0;
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &blocked, NULL);
    pthread_t waiter;
    pthread_create(&waiter, NULL, wait_for_it, (void *)1);
    while (s_i_waiter == 0) {
        usleep(1000);
    }
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL);
    kill(getpid(), SIGUSR2);
    pthread_join(waiter, NULL);
    pthread_sigmask(SIG_UNBLOCK, &all, NULL);
    ksft_test_result(s_i_ran_in == s_i_waiter,
                     "and one sent to the process, by the thread that does "
                     "not block it\n");

    /* a whole process ends with exit, from any of its threads */
    pid_t i_kid = fork();
    if (i_kid == 0) {
        pthread_create(&thread, NULL, sleep_long, NULL);
        usleep(10000);
        exit(5);
    }
    ksft_test_result(ksft_exited(status_of(i_kid), 5),
                     "exit ends every thread of a process\n");
    i_kid = fork();
    if (i_kid == 0) {
        pthread_create(&thread, NULL, exit_six, NULL);
        pthread_join(thread, NULL);
        _exit(1);
    }
    ksft_test_result(ksft_exited(status_of(i_kid), 6),
                     "from whichever thread calls it\n");

    /* and with the last of them, the first having gone before */
    pipe(s_i_pipe);
    i_kid = fork();
    if (i_kid == 0) {
        pthread_create(&thread, NULL, outlive_main, NULL);
        pthread_exit(NULL);
    }
    char arr_buf[8] = { 0 };
    bool b_late = read(s_i_pipe[0], arr_buf, 4) == 4
                  && memcmp(arr_buf, "late", 4) == 0;
    ksft_test_result(b_late && ksft_exited(status_of(i_kid), 0),
                     "a process outlives its first thread, until its last\n");

    /* another program, run by a thread that is not the first */
    i_kid = fork();
    if (i_kid == 0) {
        pthread_create(&thread, NULL, sleep_long, NULL);
        pthread_create(&thread, NULL, run_again, NULL);
        for (;;) {
            pause();
        }
    }
    ksft_test_result(ksft_exited(status_of(i_kid), 0x33),
                     "a thread running a program is all of the process "
                     "after, with its pid\n");

    pthread_create(&thread, NULL, fork_here, NULL);
    pthread_join(thread, &p_back);
    ksft_test_result(ksft_exited((int)(intptr_t)p_back, 7),
                     "and one forking makes a process of one thread\n");
    ksft_finished();
}
