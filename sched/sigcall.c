// SPDX-License-Identifier: GPL-2.0-only
/*
 * sched/sigcall.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * signals as a program asks for them: sent, caught, blocked, waited for.
 */

#include <velvet/syscalls.h>
#include <velvet/errno.h>
#include <velvet/process.h>
#include <velvet/sched.h>
#include <velvet/signal.h>
#include <velvet/pit.h>
#include <velvet/string.h>
#include <asm/uaccess.h>
#include <uapi/velvet/signal.h>
#include <uapi/velvet/time.h>

/* how long a wait for a signal sleeps between looks, at most */
#define WAIT_SLICE_MS 1000

int64_t sys_rt_sigaction(int i_sig, uint64_t u_act, uint64_t u_old,
                         uint64_t u_setsize)
{
    struct sigaction act;
    struct sigaction old;
    if (u_setsize != SIGSET_BYTES || i_sig <= 0 || i_sig >= SIGNAL_MAX) {
        return -EINVAL;
    }
    if (u_act != 0 && copy_from_user(&act, u_act, sizeof act) != 0) {
        return -EFAULT;
    }
    uint64_t u_flags;
    struct process *p = process_lock_slot(current_pid(), &u_flags);
    int64_t i_result = 0;
    if (p == NULL) {
        i_result = -ESRCH;
    } else {
        old = p->sig.action[i_sig];
        if (u_act != 0 && !signal_set_action(&p->sig, i_sig, &act)) {
            i_result = -EINVAL;
        }
    }
    process_unlock_slot(u_flags);
    if (i_result == 0 && u_old != 0
        && copy_to_user(u_old, &old, sizeof old) != 0) {
        i_result = -EFAULT;
    }
    return i_result;
}

int64_t sys_rt_sigprocmask(int i_how, uint64_t u_set, uint64_t u_old,
                           uint64_t u_setsize)
{
    uint64_t u_new = 0;
    if (u_setsize != SIGSET_BYTES) {
        return -EINVAL;
    }
    if (u_set != 0) {
        if (i_how != SIG_BLOCK && i_how != SIG_UNBLOCK
            && i_how != SIG_SETMASK) {
            return -EINVAL;
        }
        if (get_user_u64(&u_new, u_set) != 0) {
            return -EFAULT;
        }
    }
    uint64_t u_flags;
    struct process *p = process_lock_slot(current_pid(), &u_flags);
    uint64_t u_was = 0;
    if (p != NULL) {
        u_was = signal_get_mask(&p->sig);
        if (u_set != 0) {
            uint64_t u_mask = (i_how == SIG_BLOCK) ? (u_was | u_new)
                            : (i_how == SIG_UNBLOCK) ? (u_was & ~u_new)
                            : u_new;
            signal_set_mask(&p->sig, u_mask);
            p->interrupted = signal_deliverable(&p->sig);
        }
    }
    process_unlock_slot(u_flags);
    if (u_old != 0 && put_user_u64(u_old, u_was) != 0) {
        return -EFAULT;
    }
    return 0;
}

/* those raised while blocked, which are all that can still be pending */
int64_t sys_rt_sigpending(uint64_t u_set, uint64_t u_setsize)
{
    if (u_setsize != SIGSET_BYTES) {
        return -EINVAL;
    }
    uint64_t u_flags;
    struct process *p = process_lock_slot(current_pid(), &u_flags);
    uint64_t u_pending = (p != NULL) ? (p->sig.pending & p->sig.blocked) : 0;
    process_unlock_slot(u_flags);
    return (put_user_u64(u_set, u_pending) == 0) ? 0 : -EFAULT;
}

/*
 * sleep until a signal is there to be delivered. the wait ends early for
 * one, so the slice is only how often a wait that nothing ended looks
 * again
 */
static void wait_for_signal(void)
{
    int i_me = current_pid();
    while (!process_interrupt_pending(i_me)) {
        sleep_ms(WAIT_SLICE_MS);
    }
}

/*
 * under the caller's mask until a handler runs. the mask it replaced is
 * what that handler returns to, which is why the signal state keeps it
 * rather than this call putting it back
 */
int64_t sys_rt_sigsuspend(uint64_t u_set, uint64_t u_setsize)
{
    uint64_t u_mask;
    if (u_setsize != SIGSET_BYTES) {
        return -EINVAL;
    }
    if (get_user_u64(&u_mask, u_set) != 0) {
        return -EFAULT;
    }
    process_set_mask(current_pid(), u_mask, true);
    wait_for_signal();
    return -ERESTARTNOHAND;
}

int64_t sys_pause(void)
{
    wait_for_signal();
    return -ERESTARTNOHAND;
}

/*
 * one of the set, taken without a handler running for it: its number,
 * with what sent it, -EAGAIN when the time is up first, or -EINTR when a
 * signal outside the set came instead. the set is let through the mask
 * for the wait, since a blocked signal would never end one
 */
int64_t sys_rt_sigtimedwait(uint64_t u_set, uint64_t u_info,
                            uint64_t u_timeout, uint64_t u_setsize)
{
    uint64_t u_want;
    struct timespec ts;
    if (u_setsize != SIGSET_BYTES) {
        return -EINVAL;
    }
    if (get_user_u64(&u_want, u_set) != 0) {
        return -EFAULT;
    }
    u_want &= ~(SIGNAL_BIT(SIGKILL) | SIGNAL_BIT(SIGSTOP));
    uint64_t u_deadline = UINT64_MAX;
    if (u_timeout != 0) {
        if (copy_from_user(&ts, u_timeout, sizeof ts) != 0) {
            return -EFAULT;
        }
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) {
            return -EINVAL;
        }
        u_deadline = pit_uptime_ms() + (uint64_t)ts.tv_sec * 1000
                     + ((uint64_t)ts.tv_nsec + 999999) / 1000000;
    }

    int i_me = current_pid();
    const struct signal_state *p_sig = process_signal_state(i_me);
    uint64_t u_was = (p_sig != NULL) ? signal_get_mask(p_sig) : 0;
    process_set_mask(i_me, u_was & ~u_want, false);

    struct signal_origin origin;
    int64_t i_result;
    for (;;) {
        int i_sig = process_take_from(i_me, u_want, &origin);
        if (i_sig != 0) {
            i_result = i_sig;
            break;
        }
        uint64_t u_now = pit_uptime_ms();
        if (u_now >= u_deadline) {
            i_result = -EAGAIN;
            break;
        }
        if (process_interrupt_pending(i_me)) {
            i_result = -EINTR;
            break;
        }
        uint64_t u_left = u_deadline - u_now;
        sleep_ms(u_left < WAIT_SLICE_MS ? u_left : WAIT_SLICE_MS);
    }

    process_set_mask(i_me, u_was, false);

    if (i_result > 0 && u_info != 0) {
        struct siginfo info;
        signal_info((int)i_result, &origin, &info);
        if (copy_to_user(u_info, &info, sizeof info) != 0) {
            return -EFAULT;
        }
    }
    return i_result;
}

/*
 * the alternate stack. one being run on cannot be changed or turned off,
 * and u_sp, where the caller's stack is, says whether it is
 */
int64_t sys_sigaltstack(uint64_t u_new, uint64_t u_old, uint64_t u_sp)
{
    stack_t next;
    if (u_new != 0 && copy_from_user(&next, u_new, sizeof next) != 0) {
        return -EFAULT;
    }
    uint64_t u_flags;
    struct process *p = process_lock_slot(current_pid(), &u_flags);
    stack_t was = { 0, SS_DISABLE, 0, 0 };
    int64_t i_result = 0;
    if (p != NULL) {
        was = p->sig.altstack;
        bool b_on = !(was.ss_flags & SS_DISABLE) && u_sp > was.ss_sp
                    && u_sp <= was.ss_sp + was.ss_size;
        was.ss_flags = b_on ? SS_ONSTACK
                            : (was.ss_flags & SS_DISABLE) ? SS_DISABLE : 0;
        if (u_new != 0) {
            if (b_on) {
                i_result = -EPERM;
            } else if (next.ss_flags & ~(SS_DISABLE | SS_ONSTACK)) {
                i_result = -EINVAL;
            } else if (next.ss_flags & SS_DISABLE) {
                p->sig.altstack.ss_sp = 0;
                p->sig.altstack.ss_size = 0;
                p->sig.altstack.ss_flags = SS_DISABLE;
            } else if (next.ss_size < MINSIGSTKSZ) {
                i_result = -ENOMEM;
            } else {
                p->sig.altstack = next;
                p->sig.altstack.ss_flags = 0;
            }
        }
    }
    process_unlock_slot(u_flags);
    if (i_result == 0 && u_old != 0
        && copy_to_user(u_old, &was, sizeof was) != 0) {
        i_result = -EFAULT;
    }
    return i_result;
}

/*
 * a signal goes only where the sender has a say: to its own processes,
 * to anybody's when the sender is root, and SIGCONT to anybody in its
 * session. a guest able to stop root's programs, init among them, is a
 * guest able to stop the machine
 */
static bool may_signal(const struct process *p_from,
                       const struct process *p_to, int i_sig)
{
    return p_from->uid == 0 || p_from->uid == p_to->uid
           || (i_sig == SIGCONT && p_from->i_sid == p_to->i_sid);
}

/*
 * to one process, or to a group as minus its pgid, the caller's own
 * group as 0, or everybody the caller may signal but init and itself as
 * -1. signal 0 sends nothing and says whether it could have. -ESRCH when
 * there was nobody there, -EPERM when there was and none of them could
 * be signalled
 */
static int64_t send(int i_pid, int i_sig, int i_code)
{
    if (i_sig < 0 || i_sig >= SIGNAL_MAX) {
        return -EINVAL;
    }
    int i_me = current_pid();
    const struct process *p_me = process_find(i_me);
    if (p_me == NULL) {
        return -ESRCH;
    }
    struct signal_origin origin = {
        .i_code = i_code, .i_pid = i_me, .u_uid = (uint32_t)p_me->uid,
    };

    int arr_pids[MAX_PROCESSES];
    size_t sz_count = 0;
    if (i_pid > 0) {
        arr_pids[sz_count++] = i_pid;
    } else if (i_pid == -1) {
        for (size_t sz_at = 0; sz_at < MAX_PROCESSES; sz_at++) {
            const struct process *p = process_at(sz_at);
            if (p == NULL) {
                break;
            }
            if (p->pid != INIT_PID && p->pid != i_me && !p->exited) {
                arr_pids[sz_count++] = p->pid;
            }
        }
    } else {
        int i_pgid = (i_pid == 0) ? p_me->pgid : -i_pid;
        sz_count = process_group_pids(i_pgid, arr_pids, MAX_PROCESSES);
    }

    bool b_found = false;
    bool b_sent = false;
    for (size_t sz_at = 0; sz_at < sz_count; sz_at++) {
        const struct process *p = process_find(arr_pids[sz_at]);
        if (p == NULL || p->exited) {
            continue;
        }
        b_found = true;
        if (!may_signal(p_me, p, i_sig)) {
            continue;
        }
        b_sent = true;
        if (i_sig != 0) {
            process_send_signal(p->pid, i_sig, &origin);
        }
    }

    /* a sender that stopped itself stops here, not some instructions on */
    struct thread *p_self = sched_current();
    if (p_self != NULL && p_self->stopped) {
        sched_yield();
    }
    return b_sent ? 0 : b_found ? -EPERM : -ESRCH;
}

int64_t sys_kill(int i_pid, int i_sig)
{
    return send(i_pid, i_sig, SI_USER);
}

/* a thread is its process, so a thread group is one thread */
int64_t sys_tgkill(int i_tgid, int i_tid, int i_sig)
{
    if (i_tgid <= 0 || i_tid <= 0) {
        return -EINVAL;
    }
    return (i_tgid == i_tid) ? send(i_tid, i_sig, SI_TKILL) : -ESRCH;
}

int64_t sys_tkill(int i_tid, int i_sig)
{
    return (i_tid <= 0) ? -EINVAL : send(i_tid, i_sig, SI_TKILL);
}

/* ITIMER_REAL, in ms: a whole second for what is left of one */
int64_t sys_alarm(uint32_t u_seconds)
{
    uint64_t u_left = 0;
    process_set_timer(current_pid(), (uint64_t)u_seconds * 1000, 0,
                      pit_uptime_ms(), &u_left, NULL);
    return (int64_t)((u_left + 999) / 1000);
}

static uint64_t ms_of(const struct timeval *p_tv)
{
    return (uint64_t)p_tv->tv_sec * 1000 + ((uint64_t)p_tv->tv_usec + 999)
           / 1000;
}

static struct timeval tv_of(uint64_t u_ms)
{
    struct timeval tv = {
        .tv_sec = (int64_t)(u_ms / 1000),
        .tv_usec = (int64_t)(u_ms % 1000) * 1000,
    };
    return tv;
}

/*
 * only the real time one is kept: the others count the time a process
 * runs, which nothing here counts finely enough to fire on
 */
int64_t sys_setitimer(int i_which, uint64_t u_new, uint64_t u_old)
{
    struct itimerval next;
    if (i_which != ITIMER_REAL) {
        return -EINVAL;
    }
    if (copy_from_user(&next, u_new, sizeof next) != 0) {
        return -EFAULT;
    }
    if (next.it_value.tv_sec < 0 || next.it_value.tv_usec < 0
        || next.it_value.tv_usec >= 1000000 || next.it_interval.tv_sec < 0
        || next.it_interval.tv_usec < 0
        || next.it_interval.tv_usec >= 1000000) {
        return -EINVAL;
    }
    uint64_t u_left = 0;
    uint64_t u_every = 0;
    process_set_timer(current_pid(), ms_of(&next.it_value),
                      ms_of(&next.it_interval), pit_uptime_ms(), &u_left,
                      &u_every);
    if (u_old != 0) {
        struct itimerval was = {
            .it_interval = tv_of(u_every), .it_value = tv_of(u_left),
        };
        if (copy_to_user(u_old, &was, sizeof was) != 0) {
            return -EFAULT;
        }
    }
    return 0;
}

int64_t sys_getitimer(int i_which, uint64_t u_value)
{
    if (i_which != ITIMER_REAL) {
        return -EINVAL;
    }
    const struct process *p = process_find(current_pid());
    uint64_t u_now = pit_uptime_ms();
    uint64_t u_left = (p != NULL && p->alarm_at > u_now)
                      ? p->alarm_at - u_now : 0;
    struct itimerval now = {
        .it_interval = tv_of((p != NULL) ? p->u_alarm_every : 0),
        .it_value = tv_of(u_left),
    };
    return (copy_to_user(u_value, &now, sizeof now) == 0) ? 0 : -EFAULT;
}
