// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/pty.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * ptys: /dev/ptmx, /dev/pts, and a terminal with a program at each end.
 */

#include <velvet/pty.h>
#include <velvet/tty.h>
#include <velvet/file.h>
#include <velvet/kmalloc.h>
#include <velvet/namei.h>
#include <velvet/pipe.h>
#include <velvet/process.h>
#include <velvet/sched.h>
#include <velvet/string.h>
#include <velvet/errno.h>
#include <asm/uaccess.h>
#include <uapi/velvet/fcntl.h>
#include <uapi/velvet/poll.h>
#include <uapi/velvet/termios.h>

#include "tty_end.h"

/*
 * a pty is one terminal, the slave, with a program at each end of it.
 * the one that opened /dev/ptmx holds the master: what it writes is
 * typed at the terminal and what it reads is what the terminal shows.
 * the one on /dev/pts/N holds the slave and sees a terminal like any
 * other, because it is one, discipline and all.
 *
 * the two directions are a pipe's ring each, kept under the terminals'
 * lock, since the discipline between them already is: their own locks
 * are never set up, and a pty is freed without leaving one behind
 */
struct pty {
    struct tty tty;             /* first, which is how its end finds the pty */
    struct pipe typed;          /* what the master wrote, for the discipline */
    struct pipe shown;          /* what the terminal showed, for the master */
    struct winsize size;        /* what the far side says it is */
    size_t sz_index;            /* N, of /dev/pts/N */
    uint32_t u_uid;             /* whose /dev/pts/N is */
    uint32_t u_gid;
    bool b_master;              /* the master is open */
    uint32_t u_slaves;          /* the files open on the slave */
    bool b_opened;              /* and one was opened by a program, once */
    bool b_locked;              /* nobody may open the slave until unlockpt */
};

/* the ptys there are, by number, from ptmx until each master closes */
static struct pty *s_arr_ptys[PTY_MAX];

/* what /dev/pts makes its names as, which a mount of it says */
static uint32_t s_u_mode = 0600;
static uint32_t s_u_ptmx_mode = 0;
static int64_t s_i_uid = -1;    /* -1 for whoever opened ptmx */
static int64_t s_i_gid = -1;

static struct pty *pty_of(struct tty *t)
{
    return (struct pty *)t;
}

struct tty *pty_tty_at(size_t sz_index)
{
    struct pty *p = (sz_index < PTY_MAX) ? s_arr_ptys[sz_index] : NULL;
    return (p != NULL) ? &p->tty : NULL;
}

/*
 * with the lock held, taken as u_flags: let go of it, and sleep on p_q
 * unless b_ready says there is nothing to wait for. false when a signal
 * for the caller is waiting, which also ends the sleep
 */
static bool nap(struct waitq *p_q, bool b_ready, uint64_t u_flags)
{
    int i_pid = current_pid();
    if (b_ready) {
        spin_unlock_irq(&g_tty_lock, u_flags);
        return true;
    }
    if (i_pid != 0 && process_interrupt_pending(i_pid)) {
        spin_unlock_irq(&g_tty_lock, u_flags);
        return false;
    }
    waitq_enqueue(p_q);
    spin_unlock_irq(&g_tty_lock, u_flags);
    waitq_sleep();
    return true;
}

/* everything waiting on the pty woken, to find out what changed */
static void wake_all(struct pty *p)
{
    waitq_wake_all(&p->typed.readable);
    waitq_wake_all(&p->typed.writable);
    waitq_wake_all(&p->shown.readable);
    waitq_wake_all(&p->shown.writable);
}

/* a ring thrown away, and a writer waiting for room in it told there is */
static void empty(struct pipe *p_ring)
{
    p_ring->head = 0;
    p_ring->count = 0;
    waitq_wake_all(&p_ring->writable);
}

/* the slave's terminal, between what the master writes and what it reads */
static int pty_take(struct tty *t)
{
    struct pty *p = pty_of(t);
    uint8_t u_c = 0;
    if (pipe_get(&p->typed, &u_c, 1) == 0) {
        return -1;
    }
    waitq_wake_all(&p->typed.writable);
    return u_c;
}

static void pty_flush(struct tty *t)
{
    empty(&pty_of(t)->typed);
}

static bool pty_wait(struct tty *t)
{
    struct pty *p = pty_of(t);
    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    return nap(&p->typed.readable,
               pipe_pending(&p->typed) > 0 || t->b_hung, u_flags);
}

static void pty_put(struct tty *t, const char *p_bytes, size_t sz_len)
{
    struct pty *p = pty_of(t);
    pipe_put(&p->shown, p_bytes, (uint32_t)sz_len);
    waitq_wake_all(&p->shown.readable);
}

static size_t pty_room(struct tty *t)
{
    return pipe_room(&pty_of(t)->shown);
}

static bool pty_wait_room(struct tty *t)
{
    struct pty *p = pty_of(t);
    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    return nap(&p->shown.writable,
               pipe_room(&p->shown) > 0 || t->b_hung, u_flags);
}

static void pty_size(struct tty *t, struct winsize *p_size)
{
    *p_size = pty_of(t)->size;
}

static bool pty_resize(struct tty *t, const struct winsize *p_size)
{
    struct pty *p = pty_of(t);
    bool b_changed = memcmp(&p->size, p_size, sizeof *p_size) != 0;
    p->size = *p_size;
    return b_changed;
}

static void pty_attach(struct tty *t)
{
    pty_of(t)->u_slaves++;
}

/*
 * the last file on the slave closed: a master reading finds out, and a
 * pty whose master is already gone goes too
 */
static void pty_detach(struct tty *t)
{
    struct pty *p = pty_of(t);
    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    p->u_slaves--;
    bool b_free = p->u_slaves == 0 && !p->b_master;
    waitq_wake_all(&p->shown.readable);
    spin_unlock_irq(&g_tty_lock, u_flags);
    if (b_free) {
        kfree(p);
    }
}

/* a slave is not for opening until the master has unlocked it */
static int64_t pty_may_open(struct tty *t)
{
    struct pty *p = pty_of(t);
    if (p->b_locked) {
        return -EIO;
    }
    p->b_opened = true;
    return 0;
}

static const struct tty_end pty_end = {
    .take = pty_take,
    .flush = pty_flush,
    .wait = pty_wait,
    .put = pty_put,
    .room = pty_room,
    .wait_room = pty_wait_room,
    .size = pty_size,
    .resize = pty_resize,
    .attach = pty_attach,
    .detach = pty_detach,
    .may_open = pty_may_open,
};

/* whether every slave a program opened is closed again. the lock is held */
static bool slave_gone(const struct pty *p)
{
    return p->b_opened && p->u_slaves == 0;
}

/*
 * what the terminal showed, waiting for some: how many, or -EIO once
 * the slave has been opened and closed again with nothing left to read.
 * reading is also when what was typed and not yet taken is put through,
 * so the echo of it is there to be read
 */
static int64_t master_read(struct file *p_file, void *p_buf, size_t sz_len)
{
    struct pty *p = p_file->p_private;
    if (sz_len == 0) {
        return 0;
    }
    for (;;) {
        tty_drain(&p->tty);
        uint64_t u_flags = spin_lock_irq(&g_tty_lock);
        if (pipe_pending(&p->shown) > 0) {
            uint32_t u_len = (sz_len < PIPE_BUF) ? (uint32_t)sz_len : PIPE_BUF;
            uint32_t u_got = pipe_get(&p->shown, p_buf, u_len);
            waitq_wake_all(&p->shown.writable);
            spin_unlock_irq(&g_tty_lock, u_flags);
            return u_got;
        }
        if (slave_gone(p)) {
            spin_unlock_irq(&g_tty_lock, u_flags);
            return -EIO;
        }
        if (p_file->u_flags & O_NONBLOCK) {
            spin_unlock_irq(&g_tty_lock, u_flags);
            return -EAGAIN;
        }
        if (!nap(&p->shown.readable, false, u_flags)) {
            return -ERESTARTSYS;
        }
    }
}

/*
 * typed at the terminal, a byte at a time as though at a keyboard: a
 * signal key is a signal now, and the rest waits in the ring for the
 * discipline, which takes what it has room for at once. how much went,
 * waiting for room while the program on the slave reads
 */
static int64_t master_write(struct file *p_file, const void *p_buf,
                            size_t sz_len)
{
    struct pty *p = p_file->p_private;
    const uint8_t *p_bytes = p_buf;
    size_t sz_done = 0;

    while (sz_done < sz_len) {
        int i_sig = 0;
        int i_pgrp = TTY_NOBODY;
        uint64_t u_flags = spin_lock_irq(&g_tty_lock);
        while (sz_done < sz_len && i_sig == 0
               && pipe_room(&p->typed) > 0) {
            i_sig = tty_signal_key(&p->tty, p_bytes[sz_done], &i_pgrp);
            if (i_sig == 0) {
                pipe_put(&p->typed, &p_bytes[sz_done], 1);
            }
            sz_done++;
        }
        bool b_full = sz_done < sz_len && i_sig == 0;
        waitq_wake_all(&p->typed.readable);
        spin_unlock_irq(&g_tty_lock, u_flags);

        if (i_sig != 0 && i_pgrp != TTY_NOBODY) {
            process_signal_group(i_pgrp, i_sig);
        }
        tty_drain(&p->tty);
        if (!b_full) {
            continue;
        }
        if (p_file->u_flags & O_NONBLOCK) {
            return (sz_done > 0) ? (int64_t)sz_done : -EAGAIN;
        }
        u_flags = spin_lock_irq(&g_tty_lock);
        if (!nap(&p->typed.writable, pipe_room(&p->typed) > 0, u_flags)) {
            return (sz_done > 0) ? (int64_t)sz_done : -ERESTARTSYS;
        }
    }
    return (int64_t)sz_done;
}

static uint32_t master_poll(struct file *p_file)
{
    struct pty *p = p_file->p_private;
    tty_drain(&p->tty);
    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    uint32_t u_mask = (pipe_pending(&p->shown) > 0 ? POLLIN : 0)
                      | (pipe_room(&p->typed) > 0 ? POLLOUT : 0)
                      | (slave_gone(p) ? POLLHUP : 0);
    spin_unlock_irq(&g_tty_lock, u_flags);
    return u_mask;
}

static int64_t put_int(uint64_t u_to, int32_t i_value)
{
    return (put_user_i32(u_to, i_value) == 0) ? 0 : -EFAULT;
}

/*
 * the slave opened from its master, which needs no name and so no
 * /dev/pts: a descriptor, or why not
 */
static int64_t open_peer(struct pty *p, uint64_t u_flags_in)
{
    int i_flags = (int)u_flags_in;
    if (i_flags & ~(O_ACCMODE | O_NOCTTY | O_NONBLOCK | O_CLOEXEC)) {
        return -EINVAL;
    }
    int i_access = i_flags & O_ACCMODE;
    uint32_t u_mode = (i_access != O_WRONLY ? FILE_READ : 0)
                      | (i_access != O_RDONLY ? FILE_WRITE : 0);
    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    struct file *p_file = tty_file(&p->tty, u_mode);
    spin_unlock_irq(&g_tty_lock, u_flags);
    if (p_file == NULL) {
        return -ENOMEM;
    }
    p_file->u_flags = (uint32_t)i_flags & O_NONBLOCK;
    int64_t i_why = p_file->p_ops->open(p_file, i_flags);
    if (i_why < 0) {
        file_put(p_file);
        return i_why;
    }
    return install_fd(p_file, (i_flags & O_CLOEXEC) != 0);
}

/*
 * what is the master's own: its number, its lock, its slave, what is
 * waiting to be read and which group is in front. everything else is
 * asked of the terminal, so that tcgetattr and tcsetattr on the master
 * are the slave's modes, and so is the size a program on it is told
 */
static int64_t master_ioctl(struct file *p_file, uint32_t u_cmd,
                            uint64_t u_arg)
{
    struct pty *p = p_file->p_private;
    int32_t i_value = 0;
    uint64_t u_flags;

    switch (u_cmd) {
    case TIOCGPTN:
        return put_int(u_arg, (int32_t)p->sz_index);
    case TIOCSPTLCK:
        if (copy_from_user(&i_value, u_arg, sizeof i_value) != 0) {
            return -EFAULT;
        }
        u_flags = spin_lock_irq(&g_tty_lock);
        p->b_locked = i_value != 0;
        spin_unlock_irq(&g_tty_lock, u_flags);
        return 0;
    case TIOCGPTLCK:
        return put_int(u_arg, p->b_locked ? 1 : 0);
    case TIOCGPTPEER:
        return open_peer(p, u_arg);
    case FIONREAD:
        u_flags = spin_lock_irq(&g_tty_lock);
        i_value = (int32_t)pipe_pending(&p->shown);
        spin_unlock_irq(&g_tty_lock, u_flags);
        return put_int(u_arg, i_value);
    case TIOCGPGRP:
        u_flags = spin_lock_irq(&g_tty_lock);
        i_value = tty_front(&p->tty);
        spin_unlock_irq(&g_tty_lock, u_flags);
        return put_int(u_arg, i_value);
    case TCFLSH:
        if (u_arg > 2) {
            return -EINVAL;
        }
        /* the master's input is what was shown, and its output is typed */
        u_flags = spin_lock_irq(&g_tty_lock);
        if (u_arg != 1) {
            empty(&p->shown);
        }
        if (u_arg != 0) {
            empty(&p->typed);
        }
        spin_unlock_irq(&g_tty_lock, u_flags);
        return 0;
    default:
        return tty_request(&p->tty, u_cmd, u_arg);
    }
}

/*
 * the master closed, and the pty with it: N is free and /dev/pts/N
 * gone at once, the terminal hung up, and the pty freed once the last
 * file on the slave is closed as well. it is still the master's until
 * then, so a slave closing meanwhile does not free it underneath
 */
static void master_release(struct file *p_file)
{
    struct pty *p = p_file->p_private;
    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    s_arr_ptys[p->sz_index] = NULL;
    spin_unlock_irq(&g_tty_lock, u_flags);

    tty_hangup(&p->tty);

    u_flags = spin_lock_irq(&g_tty_lock);
    p->b_master = false;
    bool b_free = p->u_slaves == 0;
    wake_all(p);
    spin_unlock_irq(&g_tty_lock, u_flags);
    if (b_free) {
        kfree(p);
    }
}

static const struct file_operations master_ops = {
    .read = master_read,
    .write = master_write,
    .poll = master_poll,
    .ioctl = master_ioctl,
    .release = master_release,
};

struct file *pty_open_master(uint32_t u_mode)
{
    struct pty *p = kmalloc(sizeof *p);
    if (p == NULL) {
        return NULL;
    }
    memset(p, 0, sizeof *p);
    p->b_master = true;
    p->b_locked = true;
    p->u_uid = (uint32_t)((s_i_uid >= 0) ? s_i_uid : current_uid());
    p->u_gid = (uint32_t)((s_i_gid >= 0) ? s_i_gid : current_gid());

    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    size_t sz_at = 0;
    while (sz_at < PTY_MAX && s_arr_ptys[sz_at] != NULL) {
        sz_at++;
    }
    if (sz_at < PTY_MAX) {
        tty_make(&p->tty, &pty_end, (PTY_MAJOR << 8) | (uint32_t)sz_at);
        p->sz_index = sz_at;
        s_arr_ptys[sz_at] = p;
    }
    spin_unlock_irq(&g_tty_lock, u_flags);
    if (sz_at == PTY_MAX) {
        kfree(p);
        return NULL;
    }

    struct file *p_file = file_alloc(&master_ops, u_mode, FILE_TYPE_TTY);
    if (p_file == NULL) {
        u_flags = spin_lock_irq(&g_tty_lock);
        s_arr_ptys[sz_at] = NULL;
        spin_unlock_irq(&g_tty_lock, u_flags);
        kfree(p);
        return NULL;
    }
    p_file->p_private = p;
    p_file->u_rdev = PTMX_RDEV;
    return p_file;
}

struct file *pty_open_slave(uint32_t u_minor, uint32_t u_mode)
{
    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    struct pty *p = (u_minor < PTY_MAX) ? s_arr_ptys[u_minor] : NULL;
    struct file *p_file = (p != NULL) ? tty_file(&p->tty, u_mode) : NULL;
    spin_unlock_irq(&g_tty_lock, u_flags);
    return p_file;
}

/* /dev/pts/ptmx, which is /dev/ptmx by another name, as open as a mount says */
static void ptmx_node(struct pty_node *p_out)
{
    memset(p_out, 0, sizeof *p_out);
    strcpy(p_out->arr_name, "ptmx");
    p_out->u_mode = s_u_ptmx_mode;
    p_out->u_rdev = PTMX_RDEV;
}

/* /dev/pts/N, the lock held */
static void slave_node(const struct pty *p, struct pty_node *p_out)
{
    memset(p_out, 0, sizeof *p_out);
    size_t sz_at = 0;
    char arr_digits[4];
    size_t sz_n = p->sz_index;
    do {
        arr_digits[sz_at++] = (char)('0' + sz_n % 10);
        sz_n /= 10;
    } while (sz_n > 0);
    for (size_t sz_i = 0; sz_i < sz_at; sz_i++) {
        p_out->arr_name[sz_i] = arr_digits[sz_at - 1 - sz_i];
    }
    p_out->u_mode = s_u_mode;
    p_out->u_uid = p->u_uid;
    p_out->u_gid = p->u_gid;
    p_out->u_rdev = p->tty.u_rdev;
}

bool pty_node_named(const char *str_name, struct pty_node *p_out)
{
    if (strcmp(str_name, "ptmx") == 0) {
        ptmx_node(p_out);
        return true;
    }
    /* a number, written the one way: no sign, no leading zero */
    size_t sz_n = 0;
    const char *p_at = str_name;
    if (*p_at == '\0' || (p_at[0] == '0' && p_at[1] != '\0')) {
        return false;
    }
    for (; *p_at != '\0'; p_at++) {
        if (*p_at < '0' || *p_at > '9' || sz_n >= PTY_MAX) {
            return false;
        }
        sz_n = sz_n * 10 + (size_t)(*p_at - '0');
    }
    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    const struct pty *p = (sz_n < PTY_MAX) ? s_arr_ptys[sz_n] : NULL;
    if (p != NULL) {
        slave_node(p, p_out);
    }
    spin_unlock_irq(&g_tty_lock, u_flags);
    return p != NULL;
}

bool pty_node_at(size_t sz_index, struct pty_node *p_out)
{
    if (sz_index == 0) {
        ptmx_node(p_out);
        return true;
    }
    bool b_found = false;
    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    for (size_t sz_at = 0; sz_at < PTY_MAX && !b_found; sz_at++) {
        if (s_arr_ptys[sz_at] != NULL && --sz_index == 0) {
            slave_node(s_arr_ptys[sz_at], p_out);
            b_found = true;
        }
    }
    spin_unlock_irq(&g_tty_lock, u_flags);
    return b_found;
}

/*
 * one option's number, which runs to the next comma or the end, in base
 * u_base and no bigger than u_max: true and *p_out, or false
 */
static bool option_number(const char *str_at, size_t sz_len, uint32_t u_base,
                          uint32_t u_max, int64_t *p_out)
{
    int64_t i_n = 0;
    if (sz_len == 0) {
        return false;
    }
    for (size_t sz_i = 0; sz_i < sz_len; sz_i++) {
        uint32_t u_digit = (uint32_t)(str_at[sz_i] - '0');
        if (str_at[sz_i] < '0' || u_digit >= u_base) {
            return false;
        }
        i_n = i_n * u_base + u_digit;
        if (i_n > u_max) {
            return false;
        }
    }
    *p_out = i_n;
    return true;
}

int64_t pty_options(const char *str_options, bool b_apply)
{
    int64_t i_uid = s_i_uid, i_gid = s_i_gid;
    int64_t i_mode = s_u_mode, i_ptmx = s_u_ptmx_mode;
    const char *str_at = str_options;

    while (*str_at != '\0') {
        size_t sz_len = 0;
        while (str_at[sz_len] != '\0' && str_at[sz_len] != ',') {
            sz_len++;
        }
        bool b_ok;
        if (sz_len == 11 && strncmp(str_at, "newinstance", 11) == 0) {
            b_ok = true;
        } else if (strncmp(str_at, "uid=", 4) == 0) {
            b_ok = option_number(str_at + 4, sz_len - 4, 10, 0x7fffffff,
                                 &i_uid);
        } else if (strncmp(str_at, "gid=", 4) == 0) {
            b_ok = option_number(str_at + 4, sz_len - 4, 10, 0x7fffffff,
                                 &i_gid);
        } else if (strncmp(str_at, "mode=", 5) == 0) {
            b_ok = option_number(str_at + 5, sz_len - 5, 8, 07777, &i_mode);
        } else if (strncmp(str_at, "ptmxmode=", 9) == 0) {
            b_ok = option_number(str_at + 9, sz_len - 9, 8, 07777, &i_ptmx);
        } else {
            b_ok = false;
        }
        if (!b_ok) {
            return -EINVAL;
        }
        str_at += sz_len;
        if (*str_at == ',') {
            str_at++;
        }
    }
    if (b_apply) {
        s_i_uid = i_uid;
        s_i_gid = i_gid;
        s_u_mode = (uint32_t)i_mode;
        s_u_ptmx_mode = (uint32_t)i_ptmx;
    }
    return 0;
}
