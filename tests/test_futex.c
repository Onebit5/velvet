// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_futex.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the futex: a word, the threads waiting on it, and who wakes which.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdbool.h>

/* what the lock complains through */
void kprintf(const char *fmt, ...)
{
    (void)fmt;
}
void panic(const char *fmt, ...)
{
    printf("PANIC: ");
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    exit(1);
}

#include <velvet/futex.h>
#include <velvet/syscalls.h>
#include <velvet/sched.h>
#include <velvet/thread.h>
#include <velvet/errno.h>
#include <uapi/velvet/futex.h>

/*
 * a timespec as the call reads one, spelt here because the host's own
 * headers already have one by that name
 */
struct span {
    int64_t tv_sec;
    int64_t tv_nsec;
};

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

/*
 * one thread here, standing for every thread of the test: the one that
 * waits is put to sleep, and what happens while it sleeps is whatever
 * the test has queued up to happen, made by "another thread" from inside
 * the sleep. two address spaces, for a word that is two words
 */
static struct addrspace *s_p_one = (struct addrspace *)0x1000;
static struct addrspace *s_p_two = (struct addrspace *)0x2000;
static struct thread s_me = { .id = 5, .pid = 9, .i_tid = 9 };

struct thread *sched_current(void)
{
    return &s_me;
}
int current_tid(void)
{
    return s_me.i_tid;
}

/* a host has no mmu to fault, so every address the test hands is good */
bool host_uaccess_reachable(uint64_t u_addr, uint64_t u_len, bool b_write)
{
    (void)u_addr; (void)u_len; (void)b_write;
    return true;
}

/* the clock, which a sleep that runs out moves on */
static uint64_t s_u_now = 1000;
uint64_t pit_uptime_ms(void)
{
    return s_u_now;
}
uint64_t epoch_now_ms(void)
{
    return 5000000 + s_u_now;
}

static bool s_b_signal;
bool process_interrupt_pending(int i_tid)
{
    return i_tid == s_me.i_tid && s_b_signal;
}

/* the sleeps and the wakes, counted rather than made */
static uint64_t s_u_slept_for;
static int s_i_self_wakes;
static int s_i_wakes;
void waitq_enqueue_for(struct waitq *q, uint64_t u_ms)
{
    q->head = &s_me;
    s_u_slept_for = u_ms;
}
void sched_wake_thread(int i_id)
{
    s_i_self_wakes += (i_id == s_me.id) ? 1 : 0;
}
void waitq_wake_all(struct waitq *q)
{
    if (q->head != NULL) {
        s_i_wakes++;
    }
    q->head = NULL;
}

/* what happens while the waiter sleeps, once, and whether it runs out */
static void (*s_p_asleep)(void);
static bool s_b_runs_out;
void waitq_sleep(void)
{
    void (*p_then)(void) = s_p_asleep;
    s_p_asleep = NULL;
    if (p_then != NULL) {
        p_then();
    }
    if (s_b_runs_out) {
        s_u_now += s_u_slept_for;
    }
}

static uint32_t s_u_word;
static uint32_t s_u_other;

#define ADDR(x) ((uint64_t)(uintptr_t)&(x))

static int64_t futex(uint64_t u_addr, int i_op, uint32_t u_val,
                     uint64_t u_timeout, uint64_t u_addr2, uint32_t u_val3)
{
    return sys_futex(u_addr, i_op, u_val, u_timeout, u_addr2, u_val3);
}

/* what the waker did while the waiter slept, told after */
static int64_t s_i_then;

static void wake_one(void)
{
    s_i_then = futex(ADDR(s_u_word), FUTEX_WAKE, 1, 0, 0, 0);
}

static void wake_from_elsewhere(void)
{
    s_me.space = s_p_two;
    s_i_then = futex(ADDR(s_u_word), FUTEX_WAKE, 1, 0, 0, 0);
    s_me.space = s_p_one;
}

static void wake_other_bits(void)
{
    s_i_then = futex(ADDR(s_u_word), FUTEX_WAKE_BITSET, 1, 0, 0, 0x2);
}

static void wake_by_none(void)
{
    s_i_then = futex(ADDR(s_u_word), FUTEX_WAKE, 0, 0, 0, 0);
}

static void signal_comes(void)
{
    s_b_signal = true;
}

/*
 * the second waiter's sleep: both are waiting on the word, and one call
 * hands them to the other word without waking either, which a later
 * wake of the first word then does not reach
 */
static int64_t s_i_moved;
static int64_t s_i_first_after;
static int64_t s_i_other_after;
static void move_both(void)
{
    s_i_moved = futex(ADDR(s_u_word), FUTEX_CMP_REQUEUE, 0, 2,
                      ADDR(s_u_other), 7);
    s_i_first_after = futex(ADDR(s_u_word), FUTEX_WAKE, 2, 0, 0, 0);
    s_i_other_after = futex(ADDR(s_u_other), FUTEX_WAKE, 2, 0, 0, 0);
}

static int64_t s_i_second;
static void second_waiter(void)
{
    s_p_asleep = move_both;
    s_i_second = futex(ADDR(s_u_word), FUTEX_WAIT, 7, 0, 0, 0);
}

static int64_t s_i_kernel_woke;
static void the_thread_ends(void)
{
    s_i_kernel_woke = futex_wake(ADDR(s_u_word), 1);
}

int main(void)
{
    s_me.space = s_p_one;

    s_u_word = 3;
    CHECK(futex(ADDR(s_u_word) + 1, FUTEX_WAIT, 3, 0, 0, 0) == -EINVAL,
          "a word is four bytes, on a boundary of four");
    CHECK(futex(ADDR(s_u_word), FUTEX_WAIT, 4, 0, 0, 0) == -EAGAIN,
          "a wait on a word that has already changed does not wait, "
          "which is the whole of what makes the word and the call safe "
          "to use together");
    CHECK(futex(ADDR(s_u_word), FUTEX_WAKE, 1, 0, 0, 0) == 0,
          "and leaves nobody behind to wake");

    s_p_asleep = wake_one;
    CHECK(futex(ADDR(s_u_word), FUTEX_WAIT, 3, 0, 0, 0) == 0
          && s_i_then == 1 && s_i_wakes == 1,
          "a waiter sleeps while the word says to, until a wake");
    CHECK(futex(ADDR(s_u_word), FUTEX_WAKE, 1, 0, 0, 0) == 0,
          "and the wake took it off the word");

    s_i_wakes = 0;
    s_p_asleep = wake_from_elsewhere;
    CHECK(futex(ADDR(s_u_word), FUTEX_WAIT, 3, 0, 0, 0) == 0
          && s_i_then == 0 && s_i_wakes == 0,
          "the same address in another space is another word, and its "
          "wake is not this one's");

    s_p_asleep = wake_other_bits;
    CHECK(futex(ADDR(s_u_word), FUTEX_WAIT_BITSET, 3, 0, 0, 0x1) == 0
          && s_i_then == 0 && s_i_wakes == 0,
          "a wake whose bits miss the waiter's does not wake it");
    CHECK(futex(ADDR(s_u_word), FUTEX_WAIT_BITSET, 3, 0, 0, 0) == -EINVAL
          && futex(ADDR(s_u_word), FUTEX_WAKE_BITSET, 1, 0, 0, 0)
             == -EINVAL,
          "and a bitset of none is no wait or wake at all");

    s_p_asleep = wake_by_none;
    CHECK(futex(ADDR(s_u_word), FUTEX_WAIT, 3, 0, 0, 0) == 0
          && s_i_then == 1,
          "a wake for none wakes one, as programs have always been told");

    /* time, which waiting the whole of runs out */
    struct span ts = { 0, 2500000 };
    s_b_runs_out = true;
    CHECK(futex(ADDR(s_u_word), FUTEX_WAIT, 3, ADDR(ts), 0, 0)
          == -ETIMEDOUT && s_u_slept_for == 3,
          "a wait with a time limit sleeps that long, rounded up to a ms, "
          "and says it ran out");
    struct span past = { 0, 0 };
    CHECK(futex(ADDR(s_u_word), FUTEX_WAIT, 3, ADDR(past), 0, 0)
          == -ETIMEDOUT,
          "one that is already over still looks at the word first");
    CHECK(futex(ADDR(s_u_word), FUTEX_WAIT, 9, ADDR(past), 0, 0)
          == -EAGAIN,
          "so a changed word is told, not the time");
    struct span at = { 1, 10000000 };
    s_u_now = 1000;
    CHECK(futex(ADDR(s_u_word), FUTEX_WAIT_BITSET, 3, ADDR(at), 0, ~0u)
          == -ETIMEDOUT && s_u_slept_for == 10,
          "a bitset wait's time is a moment on the uptime, waited until");
    struct span wall = { 5001, 500000000 };
    s_u_now = 1000;
    CHECK(futex(ADDR(s_u_word), FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME,
                3, ADDR(wall), 0, ~0u) == -ETIMEDOUT
          && s_u_slept_for == 500,
          "or on the wall clock, when it says so");
    s_b_runs_out = false;
    struct span bad = { 0, 1000000000 };
    CHECK(futex(ADDR(s_u_word), FUTEX_WAIT, 3, ADDR(bad), 0, 0) == -EINVAL,
          "a timespec with a second's worth of nanoseconds is no time");

    /* signals */
    s_i_self_wakes = 0;
    s_p_asleep = signal_comes;
    CHECK(futex(ADDR(s_u_word), FUTEX_WAIT, 3, 0, 0, 0) == -ERESTARTSYS,
          "a signal ends a wait, to be made again if the handler allows");
    CHECK(s_i_self_wakes == 0
          && futex(ADDR(s_u_word), FUTEX_WAKE, 1, 0, 0, 0) == 0,
          "and took the waiter off the word");
    CHECK(futex(ADDR(s_u_word), FUTEX_WAIT, 3, 0, 0, 0) == -ERESTARTSYS
          && s_i_self_wakes == 1,
          "one already there when the wait begins wakes the waiter "
          "itself, having come before there was anybody asleep to wake");
    CHECK(futex(ADDR(s_u_word), FUTEX_WAIT, 3, ADDR(ts), 0, 0)
          == -ERESTARTNOHAND,
          "and a wait with a limit is made again only when no handler "
          "ran");
    s_b_signal = false;

    /* a condition variable handing its waiters to its mutex */
    s_u_word = 7;
    s_i_wakes = 0;
    s_p_asleep = second_waiter;
    CHECK(futex(ADDR(s_u_word), FUTEX_WAIT, 7, 0, 0, 0) == 0
          && s_i_second == 0,
          "two wait on one word");
    CHECK(s_i_moved == 2 && s_i_first_after == 0 && s_i_other_after == 2
          && s_i_wakes == 2,
          "and are moved, both, to the other, where a wake reaches them "
          "and one of the first word no longer does");
    CHECK(futex(ADDR(s_u_word), FUTEX_CMP_REQUEUE, 0, 1, ADDR(s_u_other), 8)
          == -EAGAIN,
          "a requeue that checks the word does nothing when it has moved");
    CHECK(futex(ADDR(s_u_word), FUTEX_REQUEUE, 0, 1, ADDR(s_u_other) + 2, 0)
          == -EINVAL,
          "and the word it moves them to is a word too");

    s_p_asleep = the_thread_ends;
    CHECK(futex(ADDR(s_u_word), FUTEX_WAIT, 7, 0, 0, 0) == 0
          && s_i_kernel_woke == 1,
          "the kernel wakes a waiter itself, as a thread's end wakes "
          "whoever joins it");

    CHECK(futex(ADDR(s_u_word), FUTEX_LOCK_PI, 0, 0, 0, 0) == -ENOSYS
          && futex(ADDR(s_u_word), FUTEX_WAKE_OP, 1, 0, ADDR(s_u_other), 0)
             == -ENOSYS
          && futex(ADDR(s_u_word), FUTEX_WAKE | FUTEX_CLOCK_REALTIME, 1, 0,
                   0, 0) == -ENOSYS,
          "and what this is not is said");
    CHECK(futex(ADDR(s_u_word), FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, 0, 0,
                0) == 0,
          "while a word marked as the process's own is every word");

    if (failures == 0) {
        printf("all good\n");
    }
    return failures;
}
