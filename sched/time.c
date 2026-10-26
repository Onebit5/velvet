// SPDX-License-Identifier: GPL-2.0-only
/*
 * sched/time.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what time it is, and sleeping until a later one.
 */

#include <velvet/syscalls.h>
#include <velvet/errno.h>
#include <velvet/epoch.h>
#include <velvet/pit.h>
#include <velvet/process.h>
#include <velvet/sched.h>
#include <velvet/thread.h>
#include <asm/cpu.h>
#include <asm/uaccess.h>
#include <uapi/velvet/time.h>

/*
 * a clock's reading in ms: the wall clock for the real time ones, the
 * timer since boot for the rest, and for the two that count a program's
 * own time, the ticks its thread has run. -EINVAL for a clock that is
 * not one of them
 */
static int64_t read_clock(int i_clock, uint64_t *p_ms)
{
    struct thread *p_me;
    switch (i_clock) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
        *p_ms = epoch_now_ms();
        return 0;
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_BOOTTIME:
        *p_ms = pit_uptime_ms();
        return 0;
    case CLOCK_PROCESS_CPUTIME_ID:
    case CLOCK_THREAD_CPUTIME_ID:
        p_me = sched_current();
        *p_ms = (p_me != NULL) ? p_me->cpu_ticks * (1000 / CLOCK_TICK_HZ) : 0;
        return 0;
    default:
        return -EINVAL;
    }
}

static struct timespec ts_of(uint64_t u_ms)
{
    struct timespec ts = {
        .tv_sec = (int64_t)(u_ms / 1000),
        .tv_nsec = (int64_t)(u_ms % 1000) * 1000000,
    };
    return ts;
}

int64_t sys_clock_gettime(int i_clock, uint64_t u_ts)
{
    uint64_t u_ms = 0;
    int64_t i_why = read_clock(i_clock, &u_ms);
    if (i_why < 0) {
        return i_why;
    }
    struct timespec ts = ts_of(u_ms);
    return (copy_to_user(u_ts, &ts, sizeof ts) == 0) ? 0 : -EFAULT;
}

/* every clock moves a ms at a time at best, and the cpu ones a tick */
int64_t sys_clock_getres(int i_clock, uint64_t u_ts)
{
    uint64_t u_ms = 0;
    int64_t i_why = read_clock(i_clock, &u_ms);
    if (i_why < 0) {
        return i_why;
    }
    bool b_cpu = i_clock == CLOCK_PROCESS_CPUTIME_ID
                 || i_clock == CLOCK_THREAD_CPUTIME_ID;
    struct timespec ts = ts_of(b_cpu ? 1000 / CLOCK_TICK_HZ : 1);
    if (u_ts != 0 && copy_to_user(u_ts, &ts, sizeof ts) != 0) {
        return -EFAULT;
    }
    return 0;
}

/* only the wall clock can be set, and only by root */
static int64_t set_wall(uint64_t u_ms)
{
    if (process_uid(current_pid()) != 0) {
        return -EPERM;
    }
    epoch_set_ms(u_ms);
    return 0;
}

int64_t sys_clock_settime(int i_clock, uint64_t u_ts)
{
    struct timespec ts;
    if (i_clock != CLOCK_REALTIME) {
        return -EINVAL;
    }
    if (copy_from_user(&ts, u_ts, sizeof ts) != 0) {
        return -EFAULT;
    }
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) {
        return -EINVAL;
    }
    return set_wall((uint64_t)ts.tv_sec * 1000
                    + (uint64_t)ts.tv_nsec / 1000000);
}

/*
 * until the deadline, or a signal: then -EINTR with what was left in
 * u_rem, for the program to sleep the rest when it wants to. a stop and
 * a continue are not a signal to be told of, they leave nothing to
 * deliver, and the sleep carries on after them
 */
int64_t sys_clock_nanosleep(int i_clock, int i_flags, uint64_t u_req,
                            uint64_t u_rem)
{
    struct timespec ts;
    uint64_t u_now = 0;
    /* a program's own time does not pass while it sleeps */
    if (i_clock == CLOCK_THREAD_CPUTIME_ID
        || i_clock == CLOCK_PROCESS_CPUTIME_ID) {
        return -EINVAL;
    }
    int64_t i_why = read_clock(i_clock, &u_now);
    if (i_why < 0) {
        return i_why;
    }
    if (copy_from_user(&ts, u_req, sizeof ts) != 0) {
        return -EFAULT;
    }
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) {
        return -EINVAL;
    }
    uint64_t u_want = (uint64_t)ts.tv_sec * 1000
                      + ((uint64_t)ts.tv_nsec + 999999) / 1000000;
    bool b_absolute = (i_flags & TIMER_ABSTIME) != 0;
    uint64_t u_until = b_absolute ? u_want : u_now + u_want;

    int i_me = current_pid();
    for (;;) {
        read_clock(i_clock, &u_now);
        if (u_now >= u_until) {
            return 0;
        }
        if (process_interrupt_pending(i_me)) {
            break;
        }
        sleep_ms(u_until - u_now);
    }
    if (!b_absolute && u_rem != 0) {
        struct timespec left = ts_of(u_until - u_now);
        if (copy_to_user(u_rem, &left, sizeof left) != 0) {
            return -EFAULT;
        }
    }
    return -EINTR;
}

int64_t sys_nanosleep(uint64_t u_req, uint64_t u_rem)
{
    return sys_clock_nanosleep(CLOCK_MONOTONIC, 0, u_req, u_rem);
}

int64_t sys_gettimeofday(uint64_t u_tv, uint64_t u_tz)
{
    uint64_t u_ms = epoch_now_ms();
    struct timeval tv = {
        .tv_sec = (int64_t)(u_ms / 1000),
        .tv_usec = (int64_t)(u_ms % 1000) * 1000,
    };
    struct timezone tz = { 0, 0 };
    if (u_tv != 0 && copy_to_user(u_tv, &tv, sizeof tv) != 0) {
        return -EFAULT;
    }
    if (u_tz != 0 && copy_to_user(u_tz, &tz, sizeof tz) != 0) {
        return -EFAULT;
    }
    return 0;
}

/* the clock is kept in utc, so a time zone given is not kept */
int64_t sys_settimeofday(uint64_t u_tv, uint64_t u_tz)
{
    struct timeval tv;
    (void)u_tz;
    if (u_tv == 0) {
        return 0;
    }
    if (copy_from_user(&tv, u_tv, sizeof tv) != 0) {
        return -EFAULT;
    }
    if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000) {
        return -EINVAL;
    }
    return set_wall((uint64_t)tv.tv_sec * 1000
                    + (uint64_t)tv.tv_usec / 1000);
}

int64_t sys_time(uint64_t u_out)
{
    int64_t i_now = (int64_t)epoch_now();
    if (u_out != 0 && put_user_u64(u_out, (uint64_t)i_now) != 0) {
        return -EFAULT;
    }
    return i_now;
}
