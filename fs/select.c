// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/select.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * poll and select: waiting on several files at once.
 */

#include <velvet/syscalls.h>
#include <velvet/errno.h>
#include <velvet/file.h>
#include <velvet/process.h>
#include <velvet/signal.h>
#include <velvet/sched.h>
#include <velvet/pit.h>
#include <velvet/kmalloc.h>
#include <velvet/string.h>
#include <asm/uaccess.h>
#include <uapi/velvet/poll.h>
#include <uapi/velvet/time.h>

/*
 * the wait is a look at every file, then a tick's sleep, and again, until
 * one is ready, the time is up, or a signal comes. a file says what it is
 * ready for through its poll, and one without a poll is a file on a disk,
 * which never makes anybody wait
 */
#define SLICE_MS 10

static uint32_t ready_for(int i_fd)
{
    struct file *p_file = process_fd_file(current_pid(), i_fd);
    if (p_file == NULL) {
        return POLLNVAL;
    }
    uint32_t u_mask = (p_file->p_ops->poll != NULL)
                      ? p_file->p_ops->poll(p_file) : (POLLIN | POLLOUT);
    file_put(p_file);
    return u_mask;
}

/*
 * a timeout as a deadline in ms since boot: UINT64_MAX for none, which
 * is what a NULL timeout means
 */
static int64_t deadline_of(uint64_t u_ts, uint64_t *p_deadline)
{
    struct timespec ts;
    if (u_ts == 0) {
        *p_deadline = UINT64_MAX;
        return 0;
    }
    if (copy_from_user(&ts, u_ts, sizeof ts) != 0) {
        return -EFAULT;
    }
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) {
        return -EINVAL;
    }
    *p_deadline = pit_uptime_ms() + (uint64_t)ts.tv_sec * 1000
                  + ((uint64_t)ts.tv_nsec + 999999) / 1000000;
    return 0;
}

/*
 * the mask a ppoll or pselect6 waits under, put in place for the wait.
 * the one it replaces goes back when the call returns, or, when a
 * handler ends the wait, when that handler does
 */
static int64_t wait_under(uint64_t u_mask, uint64_t u_size)
{
    uint64_t u_set;
    if (u_mask == 0) {
        return 0;
    }
    if (u_size != SIGSET_BYTES) {
        return -EINVAL;
    }
    if (copy_from_user(&u_set, u_mask, sizeof u_set) != 0) {
        return -EFAULT;
    }
    process_set_mask(current_pid(), u_set, true);
    return 0;
}

/* the wait over with nothing interrupting it: the mask it replaced back */
static void done_waiting(int64_t i_result)
{
    struct signal_state *p_sig = process_signal_state(current_pid());
    if (p_sig->b_suspended && i_result != -ERESTARTNOHAND) {
        p_sig->b_suspended = false;
        process_set_mask(current_pid(), p_sig->u_suspend_mask, false);
    }
}

/* until something is ready: how many, 0 at the deadline, or a signal */
static int64_t poll_wait(struct pollfd *p_fds, uint64_t u_count,
                         uint64_t u_deadline)
{
    for (;;) {
        int64_t i_ready = 0;
        for (uint64_t u_at = 0; u_at < u_count; u_at++) {
            p_fds[u_at].revents = 0;
            if (p_fds[u_at].fd < 0) {
                continue;
            }
            uint32_t u_want = (uint16_t)p_fds[u_at].events
                              | POLLERR | POLLHUP | POLLNVAL;
            p_fds[u_at].revents = (int16_t)(ready_for(p_fds[u_at].fd)
                                            & u_want);
            if (p_fds[u_at].revents != 0) {
                i_ready++;
            }
        }
        if (i_ready > 0) {
            return i_ready;
        }
        uint64_t u_now = pit_uptime_ms();
        if (u_now >= u_deadline) {
            return 0;
        }
        if (process_interrupt_pending(current_pid())) {
            return -ERESTARTNOHAND;
        }
        uint64_t u_left = u_deadline - u_now;
        sleep_ms(u_left < SLICE_MS ? u_left : SLICE_MS);
    }
}

int64_t sys_ppoll(uint64_t u_fds, uint64_t u_count, uint64_t u_timeout,
                  uint64_t u_mask, uint64_t u_masksize)
{
    uint64_t u_deadline;
    if (u_count > MAX_FDS * 4) {
        return -EINVAL;
    }
    int64_t i_why = deadline_of(u_timeout, &u_deadline);
    if (i_why < 0) {
        return i_why;
    }
    size_t sz_bytes = (size_t)u_count * sizeof(struct pollfd);
    struct pollfd *p_fds = kmalloc(sz_bytes ? sz_bytes : 1);
    if (p_fds == NULL) {
        return -ENOMEM;
    }
    if (copy_from_user(p_fds, u_fds, sz_bytes) != 0) {
        kfree(p_fds);
        return -EFAULT;
    }
    i_why = wait_under(u_mask, u_masksize);
    int64_t i_result = (i_why < 0) ? i_why
                                   : poll_wait(p_fds, u_count, u_deadline);
    if (i_result >= 0 && copy_to_user(u_fds, p_fds, sz_bytes) != 0) {
        i_result = -EFAULT;
    }
    kfree(p_fds);
    if (i_why == 0) {
        done_waiting(i_result);
    }
    return i_result;
}

int64_t sys_poll(uint64_t u_fds, uint64_t u_count, int i_timeout_ms)
{
    uint64_t u_deadline = (i_timeout_ms < 0) ? UINT64_MAX
                          : pit_uptime_ms() + (uint64_t)i_timeout_ms;
    if (u_count > MAX_FDS * 4) {
        return -EINVAL;
    }
    size_t sz_bytes = (size_t)u_count * sizeof(struct pollfd);
    struct pollfd *p_fds = kmalloc(sz_bytes ? sz_bytes : 1);
    if (p_fds == NULL) {
        return -ENOMEM;
    }
    int64_t i_result = (copy_from_user(p_fds, u_fds, sz_bytes) == 0)
                       ? poll_wait(p_fds, u_count, u_deadline) : -EFAULT;
    if (i_result >= 0 && copy_to_user(u_fds, p_fds, sz_bytes) != 0) {
        i_result = -EFAULT;
    }
    kfree(p_fds);
    return i_result;
}

/* the three sets of select, a bit per descriptor */
#define SET_WORDS ((MAX_FDS + 63) / 64)

static int64_t read_set(uint64_t u_set, int i_count, uint64_t *p_out)
{
    memset(p_out, 0, SET_WORDS * sizeof(uint64_t));
    if (u_set == 0) {
        return 0;
    }
    size_t sz_bytes = ((size_t)i_count + 7) / 8;
    return (copy_from_user(p_out, u_set, sz_bytes) == 0) ? 0 : -EFAULT;
}

static int64_t write_set(uint64_t u_set, int i_count, const uint64_t *p_in)
{
    if (u_set == 0) {
        return 0;
    }
    size_t sz_bytes = ((size_t)i_count + 7) / 8;
    return (copy_to_user(u_set, p_in, sz_bytes) == 0) ? 0 : -EFAULT;
}

static int64_t select_wait(int i_count, uint64_t u_read, uint64_t u_write,
                           uint64_t u_except, uint64_t u_deadline)
{
    uint64_t arr_in[3][SET_WORDS];
    uint64_t arr_out[3][SET_WORDS];
    uint64_t arr_sets[3] = { u_read, u_write, u_except };

    if (i_count < 0) {
        return -EINVAL;
    }
    if (i_count > MAX_FDS) {
        i_count = MAX_FDS;
    }
    for (int i_set = 0; i_set < 3; i_set++) {
        if (read_set(arr_sets[i_set], i_count, arr_in[i_set]) != 0) {
            return -EFAULT;
        }
    }
    for (;;) {
        int64_t i_ready = 0;
        memset(arr_out, 0, sizeof arr_out);
        for (int i_fd = 0; i_fd < i_count; i_fd++) {
            uint64_t u_bit = 1ull << (i_fd % 64);
            int i_word = i_fd / 64;
            bool b_asked = false;
            for (int i_set = 0; i_set < 3; i_set++) {
                b_asked |= (arr_in[i_set][i_word] & u_bit) != 0;
            }
            if (!b_asked) {
                continue;
            }
            uint32_t u_mask = ready_for(i_fd);
            if (u_mask & POLLNVAL) {
                return -EBADF;
            }
            uint32_t arr_wants[3] = {
                POLLIN | POLLHUP | POLLERR, POLLOUT | POLLERR, POLLPRI,
            };
            for (int i_set = 0; i_set < 3; i_set++) {
                if ((arr_in[i_set][i_word] & u_bit)
                    && (u_mask & arr_wants[i_set])) {
                    arr_out[i_set][i_word] |= u_bit;
                    i_ready++;
                }
            }
        }
        uint64_t u_now = pit_uptime_ms();
        if (i_ready > 0 || u_now >= u_deadline) {
            for (int i_set = 0; i_set < 3; i_set++) {
                if (write_set(arr_sets[i_set], i_count, arr_out[i_set]) != 0) {
                    return -EFAULT;
                }
            }
            return i_ready;
        }
        if (process_interrupt_pending(current_pid())) {
            return -ERESTARTNOHAND;
        }
        uint64_t u_left = u_deadline - u_now;
        sleep_ms(u_left < SLICE_MS ? u_left : SLICE_MS);
    }
}

int64_t sys_select(int i_count, uint64_t u_read, uint64_t u_write,
                   uint64_t u_except, uint64_t u_timeout)
{
    uint64_t u_deadline = UINT64_MAX;
    if (u_timeout != 0) {
        struct timeval tv;
        if (copy_from_user(&tv, u_timeout, sizeof tv) != 0) {
            return -EFAULT;
        }
        if (tv.tv_sec < 0 || tv.tv_usec < 0) {
            return -EINVAL;
        }
        u_deadline = pit_uptime_ms() + (uint64_t)tv.tv_sec * 1000
                     + ((uint64_t)tv.tv_usec + 999) / 1000;
    }
    return select_wait(i_count, u_read, u_write, u_except, u_deadline);
}

int64_t sys_pselect6(int i_count, uint64_t u_read, uint64_t u_write,
                     uint64_t u_except, uint64_t u_timeout, uint64_t u_mask)
{
    uint64_t u_deadline;
    int64_t i_why = deadline_of(u_timeout, &u_deadline);
    if (i_why < 0) {
        return i_why;
    }
    /* the mask comes as a pointer to it and its size, side by side */
    uint64_t arr_sigmask[2] = { 0, 0 };
    if (u_mask != 0
        && copy_from_user(arr_sigmask, u_mask, sizeof arr_sigmask) != 0) {
        return -EFAULT;
    }
    i_why = wait_under(arr_sigmask[0], arr_sigmask[1]);
    if (i_why < 0) {
        return i_why;
    }
    int64_t i_result = select_wait(i_count, u_read, u_write, u_except,
                                   u_deadline);
    done_waiting(i_result);
    return i_result;
}
