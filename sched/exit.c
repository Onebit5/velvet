// SPDX-License-Identifier: GPL-2.0-only
/*
 * sched/exit.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a program ending, and its parent hearing how.
 */

#include <velvet/syscalls.h>
#include <velvet/errno.h>
#include <velvet/futex.h>
#include <velvet/process.h>
#include <velvet/sched.h>
#include <velvet/thread.h>
#include <velvet/usermode.h>
#include <velvet/string.h>
#include <asm/cpu.h>
#include <asm/uaccess.h>
#include <uapi/velvet/resource.h>
#include <uapi/velvet/wait.h>

/*
 * where set_tid_address said to write a zero when this thread goes, and
 * whoever waits on that word woken, so they know it went
 */
static void clear_child_tid(void)
{
    uint64_t u_flags;
    struct task *t = process_lock_task(current_tid(), &u_flags);
    uint64_t u_tid = (t != NULL) ? t->u_clear_tid : 0;
    process_unlock_slot(u_flags);
    if (u_tid != 0 && put_user_i32(u_tid, 0) == 0) {
        futex_wake(u_tid, 1);
    }
}

/* the one thread, and its process with it when it was the last */
int64_t sys_exit(int i_code)
{
    clear_child_tid();
    thread_exit(W_EXITCODE(i_code, 0));
}

/*
 * every thread of the process: the others doomed, each ending on its
 * way back to ring 3, and the process ending with this code when the
 * last of them has
 */
int64_t sys_exit_group(int i_code)
{
    process_doom_others(current_tid(), W_EXITCODE(i_code, 0), false);
    clear_child_tid();
    thread_exit(W_EXITCODE(i_code, 0));
}

int64_t sys_set_tid_address(uint64_t u_tid)
{
    uint64_t u_flags;
    int i_me = current_tid();
    struct task *t = process_lock_task(i_me, &u_flags);
    if (t != NULL) {
        t->u_clear_tid = u_tid;
    }
    process_unlock_slot(u_flags);
    return i_me;
}

#define WAIT_OPTIONS (WNOHANG | WUNTRACED | WCONTINUED | WNOWAIT)

/*
 * a child that changed, as the selector picks one: its pid, with how in
 * the status, 0 under WNOHANG when none has yet, or -ECHILD when the
 * caller has no such child to wait for. this looks every few ms, which
 * nobody will notice, and needs no queue for a child to wake
 */
int64_t sys_wait4(int i_pid, uint64_t u_status, int i_options,
                  uint64_t u_rusage)
{
    int i_me = current_pid();

    if (i_options & ~WAIT_OPTIONS) {
        return -EINVAL;
    }
    if ((u_status != 0 && !access_ok(u_status, sizeof(int32_t)))
        || (u_rusage != 0 && !access_ok(u_rusage, sizeof(struct rusage)))) {
        return -EFAULT;
    }
    for (;;) {
        int i_status = 0;
        bool b_ended = false;
        int i_who = process_reportable(i_me, i_pid, i_options, &i_status,
                                       &b_ended);
        if (i_who < 0) {
            return i_who;
        }
        if (i_who > 0) {
            /* what it ran for, and the children it collected */
            struct rusage usage;
            memset(&usage, 0, sizeof usage);
            struct cpu_usage children;
            struct cpu_usage used = sched_process_usage(i_who, &children);
            cpu_usage_add(&used, &children);
            usage.ru_utime = rusage_time_of(used.u_user);
            usage.ru_stime = rusage_time_of(used.u_system);
            usage.ru_nvcsw = (int64_t)used.u_waited;
            usage.ru_nivcsw = (int64_t)used.u_preempted;
            if ((u_status != 0 && put_user_i32(u_status, i_status) != 0)
                || (u_rusage != 0
                    && copy_to_user(u_rusage, &usage, sizeof usage) != 0)) {
                return -EFAULT;
            }
            if (b_ended) {
                user_collect(i_who, NULL);
            }
            return i_who;
        }
        if (i_options & WNOHANG) {
            return 0;
        }
        if (process_interrupt_pending(current_tid())) {
            return -ERESTARTSYS;
        }
        sleep_ms(10);
    }
}
