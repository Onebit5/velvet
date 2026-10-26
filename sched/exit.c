// SPDX-License-Identifier: GPL-2.0-only
/*
 * sched/exit.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a program ending, and its parent hearing how.
 */

#include <velvet/syscalls.h>
#include <velvet/errno.h>
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
 * where set_tid_address said to write a zero when this thread goes, so
 * whoever waits on that word knows it went
 */
static void clear_child_tid(void)
{
    uint64_t u_flags;
    struct process *p = process_lock_slot(current_pid(), &u_flags);
    uint64_t u_tid = (p != NULL) ? p->u_clear_tid : 0;
    process_unlock_slot(u_flags);
    if (u_tid != 0) {
        put_user_i32(u_tid, 0);
    }
}

int64_t sys_exit(int i_code)
{
    clear_child_tid();
    thread_exit(W_EXITCODE(i_code, 0));
}

/* every thread of the process, which is one until there are more */
int64_t sys_exit_group(int i_code)
{
    clear_child_tid();
    thread_exit(W_EXITCODE(i_code, 0));
}

int64_t sys_set_tid_address(uint64_t u_tid)
{
    uint64_t u_flags;
    int i_pid = current_pid();
    struct process *p = process_lock_slot(i_pid, &u_flags);
    if (p != NULL) {
        p->u_clear_tid = u_tid;
    }
    process_unlock_slot(u_flags);
    return i_pid;
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
            /*
             * what it ran for, and the children it collected, all of it
             * told as user time, the two never being told apart
             */
            struct rusage usage;
            memset(&usage, 0, sizeof usage);
            const struct process *p_child = process_find(i_who);
            uint64_t u_ms = (p_child == NULL) ? 0
                : (p_child->u_ticks + p_child->u_child_ticks)
                  * (1000 / CLOCK_TICK_HZ);
            usage.ru_utime.tv_sec = (int64_t)(u_ms / 1000);
            usage.ru_utime.tv_usec = (int64_t)(u_ms % 1000) * 1000;
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
        if (process_interrupt_pending(i_me)) {
            return -ERESTARTSYS;
        }
        sleep_ms(10);
    }
}
