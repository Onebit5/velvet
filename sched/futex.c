// SPDX-License-Identifier: GPL-2.0-only
/*
 * sched/futex.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the call for when a word in a program's memory says wait.
 */

#include <velvet/futex.h>
#include <velvet/syscalls.h>
#include <velvet/errno.h>
#include <velvet/epoch.h>
#include <velvet/pit.h>
#include <velvet/process.h>
#include <velvet/sched.h>
#include <velvet/spinlock.h>
#include <velvet/thread.h>
#include <asm/uaccess.h>
#include <uapi/velvet/futex.h>
#include <uapi/velvet/time.h>

/*
 * a futex is not a thing the kernel keeps. it is a word in a program's
 * memory, which the program's own atomics take and give back, and the
 * kernel is only told when the word says somebody has to wait: a mutex
 * nobody else wants is never a call at all. so what is kept here is the
 * waiting, each waiter on its own stack for as long as it waits, filed
 * under the word it waits on.
 *
 * a word is named by the address space it is in and where in it. every
 * page here is a process's own, or its threads', a fork's child getting
 * a copy of it, so the same address in two spaces is two words, and
 * FUTEX_PRIVATE_FLAG changes nothing
 */
struct waiter {
    const struct addrspace *p_space;
    uint64_t u_addr;
    uint32_t u_bitset;          /* which wakes it answers to */
    bool b_woken;
    struct waitq queue;         /* its one thread, asleep */
    struct waiter *p_next;
};

#define BUCKETS 64

/*
 * every waiter, under one lock, which a waiter holds from reading the
 * word to being on its list: a wake that comes after the word changed
 * finds it there, and one that came before it changed it before it was
 * read, so nobody sleeps through the wake meant for them
 */
static struct spinlock s_futex_lock = SPINLOCK("futex", LOCK_RANK_FUTEX);
static struct waiter *s_arr_buckets[BUCKETS];

static struct waiter **bucket_of(const struct addrspace *p_space,
                                 uint64_t u_addr)
{
    uint64_t u_key = (uint64_t)(uintptr_t)p_space ^ (u_addr >> 2);
    u_key ^= u_key >> 17;
    return &s_arr_buckets[u_key % BUCKETS];
}

/* at the end of its bucket, so a word's waiters are woken in turn */
static void file(struct waiter *p_w)
{
    struct waiter **pp_at = bucket_of(p_w->p_space, p_w->u_addr);
    while (*pp_at != NULL) {
        pp_at = &(*pp_at)->p_next;
    }
    p_w->p_next = NULL;
    *pp_at = p_w;
}

static void unfile(struct waiter *p_w)
{
    struct waiter **pp_at = bucket_of(p_w->p_space, p_w->u_addr);
    while (*pp_at != NULL && *pp_at != p_w) {
        pp_at = &(*pp_at)->p_next;
    }
    if (*pp_at != NULL) {
        *pp_at = p_w->p_next;
    }
}

static struct addrspace *my_space(void)
{
    struct thread *p_me = sched_current();
    return (p_me != NULL) ? p_me->space : NULL;
}

/*
 * up to i_count of the waiters on a word whose bitset meets u_bitset
 * woken, and then up to i_move more of them moved to wait on u_to
 * instead, when that is not 0: how many were woken and moved. the lock
 * is held, and a woken waiter's struct is never touched again, since it
 * is on a stack that may be gone the moment the lock is let go
 */
static int64_t wake_and_move(const struct addrspace *p_space,
                             uint64_t u_addr, uint32_t u_bitset,
                             int i_count, int i_move, uint64_t u_to)
{
    int64_t i_done = 0;
    struct waiter **pp_at = bucket_of(p_space, u_addr);
    struct waiter *p_moved = NULL;
    while (*pp_at != NULL && (i_count > 0 || i_move > 0)) {
        struct waiter *p_w = *pp_at;
        if (p_w->p_space != p_space || p_w->u_addr != u_addr
            || !(p_w->u_bitset & u_bitset)) {
            pp_at = &p_w->p_next;
            continue;
        }
        *pp_at = p_w->p_next;
        if (i_count > 0) {
            i_count--;
            p_w->b_woken = true;
            waitq_wake_all(&p_w->queue);
        } else {
            i_move--;
            p_w->u_addr = u_to;
            p_w->p_next = p_moved;
            p_moved = p_w;
        }
        i_done++;
    }

    /* filed under the other word only now, its bucket may be this one */
    while (p_moved != NULL) {
        struct waiter *p_next = p_moved->p_next;
        file(p_moved);
        p_moved = p_next;
    }
    return i_done;
}

int64_t futex_wake(uint64_t u_addr, int i_count)
{
    uint64_t u_flags = spin_lock_irq(&s_futex_lock);
    int64_t i_woken = wake_and_move(my_space(), u_addr,
                                    FUTEX_BITSET_MATCH_ANY, i_count, 0, 0);
    spin_unlock_irq(&s_futex_lock, u_flags);
    return i_woken;
}

/*
 * how long a wait may be from the timespec a program handed over, in ms:
 * u_timeout from now, or until it when b_absolute, on the uptime or with
 * b_wall the wall clock. 0 for none at all. the time may have passed, and
 * then the wait is over as soon as the word has been looked at
 */
static int64_t wait_for(uint64_t u_timeout, bool b_absolute, bool b_wall,
                        uint64_t *p_ms)
{
    struct timespec ts;
    *p_ms = 0;
    if (u_timeout == 0) {
        return 0;
    }
    if (copy_from_user(&ts, u_timeout, sizeof ts) != 0) {
        return -EFAULT;
    }
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) {
        return -EINVAL;
    }
    uint64_t u_want = (uint64_t)ts.tv_sec * 1000
                      + ((uint64_t)ts.tv_nsec + 999999) / 1000000;
    if (b_absolute) {
        uint64_t u_now = b_wall ? epoch_now_ms() : pit_uptime_ms();
        u_want = (u_want > u_now) ? u_want - u_now : 0;
    }
    /* a wait that is already over still waits a tick, and is told so */
    *p_ms = (u_want != 0) ? u_want : 1;
    return 0;
}

/*
 * asleep while the word at u_addr holds u_val, until woken by a
 * wake whose bitset meets u_bitset, a signal, or u_ms, 0 for no limit: 0
 * once woken, -EAGAIN when the word said otherwise, -ETIMEDOUT, or for a
 * signal a call to be made again, the way a wait with no time limit and
 * one with one each are
 */
static int64_t wait_on(uint64_t u_addr, uint32_t u_val, uint32_t u_bitset,
                       uint64_t u_ms)
{
    struct addrspace *p_space = my_space();
    int i_tid = current_tid();
    if (p_space == NULL) {
        return -EINVAL;
    }
    uint64_t u_deadline = pit_uptime_ms() + u_ms;
    struct waiter w = {
        .p_space = p_space, .u_addr = u_addr, .u_bitset = u_bitset,
    };

    /*
     * the word read with the lock held, which a fault in the read may
     * take the space's lock under: a futex's waiters rank below a
     * program's memory for it
     */
    uint64_t u_flags = spin_lock_irq(&s_futex_lock);
    uint32_t u_now = 0;
    if (get_user_u32(&u_now, u_addr) != 0) {
        spin_unlock_irq(&s_futex_lock, u_flags);
        return -EFAULT;
    }
    if (u_now != u_val) {
        spin_unlock_irq(&s_futex_lock, u_flags);
        return -EAGAIN;
    }
    file(&w);
    waitq_enqueue_for(&w.queue, u_ms);

    /*
     * a signal already there would wake nobody, having come before there
     * was anybody asleep to wake; one after this finds the thread asleep
     */
    if (process_interrupt_pending(i_tid)) {
        sched_wake_thread(sched_current()->id);
    }
    spin_unlock_irq(&s_futex_lock, u_flags);
    waitq_sleep();

    u_flags = spin_lock_irq(&s_futex_lock);
    bool b_woken = w.b_woken;
    if (!b_woken) {
        unfile(&w);
    }
    spin_unlock_irq(&s_futex_lock, u_flags);

    if (b_woken) {
        return 0;
    }
    if (process_interrupt_pending(i_tid)) {
        /*
         * made again with no handler run, and with one for a wait with no
         * limit when SA_RESTART says so. a wait with a limit made again
         * waits the whole of it again, from the call's own timespec
         */
        return (u_ms != 0) ? -ERESTARTNOHAND : -ERESTARTSYS;
    }
    /* and woken for nothing at all is a wake, which every caller allows */
    return (u_ms != 0 && pit_uptime_ms() >= u_deadline) ? -ETIMEDOUT : 0;
}

/* a word the call may name: one a program could hold, four bytes aligned */
static bool word_ok(uint64_t u_addr)
{
    return (u_addr & 3) == 0 && access_ok(u_addr, sizeof(uint32_t));
}

int64_t sys_futex(uint64_t u_addr, int i_op, uint32_t u_val,
                  uint64_t u_timeout, uint64_t u_addr2, uint32_t u_val3)
{
    int i_cmd = i_op & FUTEX_CMD_MASK;
    bool b_wall = (i_op & FUTEX_CLOCK_REALTIME) != 0;
    if (b_wall && i_cmd != FUTEX_WAIT_BITSET && i_cmd != FUTEX_WAIT) {
        return -ENOSYS;
    }
    if (!word_ok(u_addr)) {
        return -EINVAL;
    }

    uint64_t u_ms = 0;
    int64_t i_why;
    uint64_t u_flags;
    switch (i_cmd) {
    case FUTEX_WAIT:
        i_why = wait_for(u_timeout, false, b_wall, &u_ms);
        return (i_why < 0) ? i_why
                           : wait_on(u_addr, u_val, FUTEX_BITSET_MATCH_ANY,
                                     u_ms);

    case FUTEX_WAIT_BITSET:
        if (u_val3 == 0) {
            return -EINVAL;
        }
        i_why = wait_for(u_timeout, true, b_wall, &u_ms);
        return (i_why < 0) ? i_why : wait_on(u_addr, u_val, u_val3, u_ms);

    /* a wake for none wakes one, which is what every program was given */
    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET:
        if (i_cmd == FUTEX_WAKE_BITSET && u_val3 == 0) {
            return -EINVAL;
        }
        u_flags = spin_lock_irq(&s_futex_lock);
        i_why = wake_and_move(my_space(), u_addr,
                              (i_cmd == FUTEX_WAKE) ? FUTEX_BITSET_MATCH_ANY
                                                    : u_val3,
                              ((int)u_val > 0) ? (int)u_val : 1, 0, 0);
        spin_unlock_irq(&s_futex_lock, u_flags);
        return i_why;

    /*
     * up to u_val woken, and up to as many more as the timeout argument
     * says moved to wait on u_addr2: a condition variable handing its
     * waiters to its mutex one at a time, rather than waking them all to
     * fight over it. CMP_REQUEUE first checks the word holds u_val3,
     * under the lock, which is what makes it safe to use
     */
    case FUTEX_REQUEUE:
    case FUTEX_CMP_REQUEUE:
        if ((int)u_val < 0 || (int64_t)u_timeout < 0 || !word_ok(u_addr2)) {
            return -EINVAL;
        }
        u_flags = spin_lock_irq(&s_futex_lock);
        if (i_cmd == FUTEX_CMP_REQUEUE) {
            uint32_t u_now = 0;
            i_why = (get_user_u32(&u_now, u_addr) != 0) ? -EFAULT
                    : (u_now != u_val3) ? -EAGAIN : 0;
            if (i_why < 0) {
                spin_unlock_irq(&s_futex_lock, u_flags);
                return i_why;
            }
        }
        i_why = wake_and_move(my_space(), u_addr, FUTEX_BITSET_MATCH_ANY,
                              (int)u_val, (int)u_timeout, u_addr2);
        spin_unlock_irq(&s_futex_lock, u_flags);
        return i_why;

    default:
        return -ENOSYS;
    }
}
