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
#include <velvet/addrspace.h>
#include <velvet/sched.h>
#include <velvet/shared.h>
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
 * a word is named by what holds it and where in that: in memory mapped
 * shared, by its set of shared pages and its place in the set, which is
 * the same in every process mapping it, and anywhere else by the address
 * space it is in and its address, the same address in two spaces being
 * two words. FUTEX_PRIVATE_FLAG says the word is the process's own, and
 * names it by its space whatever memory it is in
 */
struct futex_key {
    const void *p_object;
    uint64_t u_offset;
};

struct waiter {
    struct futex_key key;
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

static struct waiter **bucket_of(const struct futex_key *p_key)
{
    uint64_t u_key = (uint64_t)(uintptr_t)p_key->p_object
                     ^ (p_key->u_offset >> 2);
    u_key ^= u_key >> 17;
    return &s_arr_buckets[u_key % BUCKETS];
}

static bool same_key(const struct futex_key *p_a, const struct futex_key *p_b)
{
    return p_a->p_object == p_b->p_object && p_a->u_offset == p_b->u_offset;
}

/* at the end of its bucket, so a word's waiters are woken in turn */
static void file(struct waiter *p_w)
{
    struct waiter **pp_at = bucket_of(&p_w->key);
    while (*pp_at != NULL) {
        pp_at = &(*pp_at)->p_next;
    }
    p_w->p_next = NULL;
    *pp_at = p_w;
}

static void unfile(struct waiter *p_w)
{
    struct waiter **pp_at = bucket_of(&p_w->key);
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
 * the key of the word at u_addr in the running thread's memory, and the
 * set of shared pages it names, held in *pp_held until the caller is
 * done with the key and puts it, with no lock held. false with no memory
 */
static bool key_of(uint64_t u_addr, bool b_private, struct futex_key *p_key,
                   struct shared_pages **pp_held)
{
    struct addrspace *p_space = my_space();
    *pp_held = NULL;
    if (p_space == NULL) {
        return false;
    }
    p_key->p_object = p_space;
    p_key->u_offset = u_addr;
    if (!b_private) {
        addrspace_futex_key(p_space, u_addr, &p_key->p_object,
                            &p_key->u_offset, pp_held);
    }
    return true;
}

/*
 * up to i_count of the waiters on a word whose bitset meets u_bitset
 * woken, and then up to i_move more of them moved to wait on the word
 * p_to names instead: how many were woken and moved. the lock is held,
 * and a woken waiter's struct is never touched again, since it is on a
 * stack that may be gone the moment the lock is let go
 */
static int64_t wake_and_move(const struct futex_key *p_key,
                             uint32_t u_bitset, int i_count, int i_move,
                             const struct futex_key *p_to)
{
    int64_t i_done = 0;
    struct waiter **pp_at = bucket_of(p_key);
    struct waiter *p_moved = NULL;
    while (*pp_at != NULL && (i_count > 0 || i_move > 0)) {
        struct waiter *p_w = *pp_at;
        if (!same_key(&p_w->key, p_key) || !(p_w->u_bitset & u_bitset)) {
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
            p_w->key = *p_to;
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
    struct futex_key key;
    struct shared_pages *p_held;
    if (!key_of(u_addr, false, &key, &p_held)) {
        return 0;
    }
    uint64_t u_flags = spin_lock_irq(&s_futex_lock);
    int64_t i_woken = wake_and_move(&key, FUTEX_BITSET_MATCH_ANY, i_count,
                                    0, NULL);
    spin_unlock_irq(&s_futex_lock, u_flags);
    shared_put(p_held);
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
static int64_t wait_on(uint64_t u_addr, bool b_private, uint32_t u_val,
                       uint32_t u_bitset, uint64_t u_ms)
{
    int i_tid = current_tid();
    struct waiter w = { .u_bitset = u_bitset };
    struct shared_pages *p_held;
    if (!key_of(u_addr, b_private, &w.key, &p_held)) {
        return -EINVAL;
    }
    int64_t i_why = 0;
    uint64_t u_deadline = pit_uptime_ms() + u_ms;

    /*
     * the word read with the lock held, which a fault in the read may
     * take the space's lock under: a futex's waiters rank below a
     * program's memory for it
     */
    uint64_t u_flags = spin_lock_irq(&s_futex_lock);
    uint32_t u_now = 0;
    if (get_user_u32(&u_now, u_addr) != 0) {
        i_why = -EFAULT;
    } else if (u_now != u_val) {
        i_why = -EAGAIN;
    }
    if (i_why < 0) {
        spin_unlock_irq(&s_futex_lock, u_flags);
        shared_put(p_held);
        return i_why;
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
    shared_put(p_held);

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
    bool b_private = (i_op & FUTEX_PRIVATE_FLAG) != 0;
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
    struct futex_key key;
    struct futex_key to;
    struct shared_pages *p_held = NULL;
    struct shared_pages *p_held_to = NULL;
    switch (i_cmd) {
    case FUTEX_WAIT:
        i_why = wait_for(u_timeout, false, b_wall, &u_ms);
        return (i_why < 0) ? i_why
                           : wait_on(u_addr, b_private, u_val,
                                     FUTEX_BITSET_MATCH_ANY, u_ms);

    case FUTEX_WAIT_BITSET:
        if (u_val3 == 0) {
            return -EINVAL;
        }
        i_why = wait_for(u_timeout, true, b_wall, &u_ms);
        return (i_why < 0) ? i_why
                           : wait_on(u_addr, b_private, u_val, u_val3, u_ms);

    /* a wake for none wakes one, which is what every program was given */
    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET:
        if (i_cmd == FUTEX_WAKE_BITSET && u_val3 == 0) {
            return -EINVAL;
        }
        if (!key_of(u_addr, b_private, &key, &p_held)) {
            return -EINVAL;
        }
        u_flags = spin_lock_irq(&s_futex_lock);
        i_why = wake_and_move(&key,
                              (i_cmd == FUTEX_WAKE) ? FUTEX_BITSET_MATCH_ANY
                                                    : u_val3,
                              ((int)u_val > 0) ? (int)u_val : 1, 0, NULL);
        spin_unlock_irq(&s_futex_lock, u_flags);
        shared_put(p_held);
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
        if ((int)u_val < 0 || (int64_t)u_timeout < 0 || !word_ok(u_addr2)
            || !key_of(u_addr, b_private, &key, &p_held)) {
            return -EINVAL;
        }
        key_of(u_addr2, b_private, &to, &p_held_to);
        u_flags = spin_lock_irq(&s_futex_lock);
        i_why = 0;
        if (i_cmd == FUTEX_CMP_REQUEUE) {
            uint32_t u_now = 0;
            i_why = (get_user_u32(&u_now, u_addr) != 0) ? -EFAULT
                    : (u_now != u_val3) ? -EAGAIN : 0;
        }
        if (i_why == 0) {
            i_why = wake_and_move(&key, FUTEX_BITSET_MATCH_ANY, (int)u_val,
                                  (int)u_timeout, &to);
        }
        spin_unlock_irq(&s_futex_lock, u_flags);
        shared_put(p_held);
        shared_put(p_held_to);
        return i_why;

    default:
        return -ENOSYS;
    }
}

/*
 * the mutex whose word is at u_word, which the thread i_tid held as it
 * ended: marked as its owner gone, keeping the waiters it says it has,
 * and one of them woken to take it and be told, unless it is a priority
 * inheriting one, which nothing here hands over. one the thread was
 * taking, which says nobody holds it, still wakes a waiter, who may have
 * been woken for it and missed it. the word may change under the swap,
 * another process taking its waiters' bit, and each change is looked at
 * again. false when the word cannot be reached, which ends the walk
 */
static bool owner_gone(uint64_t u_word, int i_tid, bool b_pending,
                       bool b_pi)
{
    uint32_t u_was = 0;
    if ((u_word & 3) != 0 || get_user_u32(&u_was, u_word) != 0) {
        return false;
    }
    for (;;) {
        if (b_pending && !b_pi && u_was == 0) {
            futex_wake(u_word, 1);
            return true;
        }
        if ((u_was & FUTEX_TID_MASK) != (uint32_t)i_tid) {
            return true;
        }
        uint32_t u_mark = (u_was & FUTEX_WAITERS) | FUTEX_OWNER_DIED;
        uint32_t u_now = 0;
        if (cmpxchg_user_u32(u_word, u_was, u_mark, &u_now) != 0) {
            return false;
        }
        if (u_now == u_was) {
            if (!b_pi && (u_was & FUTEX_WAITERS)) {
                futex_wake(u_word, 1);
            }
            return true;
        }
        u_was = u_now;
    }
}

/*
 * the list is the libc's, in the thread's own memory: a head of three
 * words, the first entry, where in each entry its mutex's word is, and
 * the entry being taken or let go of as the thread ended, which may or
 * may not be on the list yet. each entry's first word is the next, the
 * last's the head again, and its low bit says priority inheriting. a
 * list that does not end is followed ROBUST_LIST_LIMIT entries and no
 * further, and the walk stops at the first entry, or word, that cannot
 * be read, leaving the one being taken as it is
 */
void futex_robust_release(void)
{
    uint64_t u_flags;
    int i_tid = current_tid();
    struct task *t = process_lock_task(i_tid, &u_flags);
    uint64_t u_head = (t != NULL) ? t->u_robust : 0;
    if (t != NULL) {
        t->u_robust = 0;
    }
    process_unlock_slot(u_flags);

    uint64_t u_entry = 0;
    uint64_t u_offset = 0;
    uint64_t u_pending = 0;
    if (u_head == 0 || get_user_u64(&u_entry, u_head) != 0
        || get_user_u64(&u_offset, u_head + 8) != 0
        || get_user_u64(&u_pending, u_head + 16) != 0) {
        return;
    }
    uint64_t u_busy = u_pending & ~1ull;
    for (int i_left = ROBUST_LIST_LIMIT;
         (u_entry & ~1ull) != u_head && i_left > 0; i_left--) {
        uint64_t u_at = u_entry & ~1ull;
        uint64_t u_next = 0;
        bool b_read = get_user_u64(&u_next, u_at) == 0;
        if ((u_at != u_busy
             && !owner_gone(u_at + u_offset, i_tid, false,
                            (u_entry & 1) != 0))
            || !b_read) {
            return;
        }
        u_entry = u_next;
    }
    if (u_busy != 0) {
        owner_gone(u_busy + u_offset, i_tid, true, (u_pending & 1) != 0);
    }
}
