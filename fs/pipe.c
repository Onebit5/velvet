// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/pipe.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * pipes: a buffer with a process at each end.
 */

#include <velvet/pipe.h>
#include <velvet/kmalloc.h>
#include <velvet/string.h>
#include <velvet/process.h>
#include <velvet/file.h>
#include <velvet/sched.h>
#include <velvet/thread.h>
#include <velvet/vfs.h>
#include <velvet/errno.h>
#include <uapi/velvet/fcntl.h>
#include <uapi/velvet/poll.h>

/* how many are alive. only for saying so, nothing depends on it */
static size_t live;

void pipe_reset(struct pipe *p)
{
    memset(p, 0, sizeof *p);
    spin_init(&p->lock, "pipe", LOCK_RANK_PIPE);
    p->readers = 1;
    p->writers = 1;
}

/* head is where the next byte comes out; count says how many are in there. */

uint32_t pipe_pending(const struct pipe *p)
{
    return p->count;
}
uint32_t pipe_room(const struct pipe *p)
{
    return PIPE_BUF - p->count;
}

uint32_t pipe_put(struct pipe *p, const void *buf, uint32_t len)
{
    uint32_t room = pipe_room(p);
    if (len > room) {
        len = room;
    }

    const uint8_t *src = buf;
    uint32_t tail = (p->head + p->count) % PIPE_BUF;

    /* up to the end of the buffer, then round to the front. */
    uint32_t first = PIPE_BUF - tail;
    if (first > len) {
        first = len;
    }
    memcpy(&p->data[tail], src, first);
    memcpy(&p->data[0], src + first, len - first);

    p->count += len;
    return len;
}

uint32_t pipe_get(struct pipe *p, void *buf, uint32_t len)
{
    if (len > p->count) {
        len = p->count;
    }

    uint8_t *dst = buf;
    uint32_t first = PIPE_BUF - p->head;
    if (first > len) {
        first = len;
    }
    memcpy(dst, &p->data[p->head], first);
    memcpy(dst + first, &p->data[0], len - first);

    p->head = (p->head + len) % PIPE_BUF;
    p->count -= len;
    return len;
}



struct pipe *pipe_create(void)
{
    struct pipe *p = kmalloc(sizeof *p);
    if (p == NULL) {
        return NULL;
    }
    pipe_reset(p);
    live++;
    return p;
}

/* both ends gone means nobody can ever refer to it again, so it goes. */
static void maybe_free(struct pipe *p)
{
    uint64_t flags = spin_lock_irq(&p->lock);
    bool dead = (p->readers == 0 && p->writers == 0);
    spin_unlock_irq(&p->lock, flags);

    /*
     * FIXME: readers and writers count handles, not calls in flight, so
     * the last handle closing frees this while a thread already inside
     * pipe_read or pipe_write still holds the pointer. that thread is
     * sitting in waitq_sleep having dropped the lock, so it wakes and
     * takes a lock on freed memory. two threads of one process, one
     * reading and one closing that same descriptor, is the whole of it.
     * a reference taken for the duration of the call and dropped when it
     * returns is what closes this, and the count here is not it.
     */
    if (dead) {
        live--;
        kfree(p);
    }
}

void pipe_close_read(struct pipe *p)
{
    if (p == NULL) {
        return;
    }
    uint64_t flags = spin_lock_irq(&p->lock);
    if (p->readers > 0) {
        p->readers--;
    }
    /* a writer parked waiting for room has to find out that the room is never coming. */
    waitq_wake_all(&p->writable);
    spin_unlock_irq(&p->lock, flags);

    maybe_free(p);
}

void pipe_close_write(struct pipe *p)
{
    if (p == NULL) {
        return;
    }
    uint64_t flags = spin_lock_irq(&p->lock);
    if (p->writers > 0) {
        p->writers--;
    }
    /* this is end of file arriving. */
    waitq_wake_all(&p->readable);
    spin_unlock_irq(&p->lock, flags);

    maybe_free(p);
}

size_t pipe_count(void)
{
    return live;
}



int64_t pipe_read(struct pipe *p, int i_tid, void *buf, uint64_t len,
                  bool b_nonblock)
{
    if (p == NULL || len == 0) {
        return -EINVAL;
    }
    if (len > PIPE_BUF) {
        len = PIPE_BUF;
    }

    uint64_t flags = spin_lock_irq(&p->lock);

    while (p->count == 0) {
        if (p->writers == 0) {
            spin_unlock_irq(&p->lock, flags);
            return 0;           /* end of file, and the only kind there is */
        }
        if (b_nonblock) {
            spin_unlock_irq(&p->lock, flags);
            return -EAGAIN;
        }
        if (process_interrupt_pending(i_tid)) {
            spin_unlock_irq(&p->lock, flags);
            return -EINTR;
        }

        /* on the queue first, then let go, then sleep. */
        waitq_enqueue(&p->readable);
        spin_unlock_irq(&p->lock, flags);
        waitq_sleep();
        flags = spin_lock_irq(&p->lock);
    }

    uint32_t n = pipe_get(p, buf, (uint32_t)len);

    /* somebody may be waiting for the room the kernel just made */
    waitq_wake_all(&p->writable);
    spin_unlock_irq(&p->lock, flags);
    return (int64_t)n;
}

int64_t pipe_write(struct pipe *p, int i_tid, const void *buf, uint64_t len,
                   bool b_nonblock)
{
    if (p == NULL || len == 0) {
        return (p == NULL) ? -EINVAL : 0;
    }

    const uint8_t *src = buf;
    uint64_t done = 0;

    uint64_t flags = spin_lock_irq(&p->lock);

    while (done < len) {
        if (p->readers == 0) {
            spin_unlock_irq(&p->lock, flags);
            /* what was already written stays written. */
            return (done > 0) ? (int64_t)done : -EPIPE;
        }
        if (p->count == PIPE_BUF) {
            if (b_nonblock) {
                spin_unlock_irq(&p->lock, flags);
                return (done > 0) ? (int64_t)done : -EAGAIN;
            }
            if (process_interrupt_pending(i_tid)) {
                spin_unlock_irq(&p->lock, flags);
                return (done > 0) ? (int64_t)done : -EINTR;
            }
            waitq_enqueue(&p->writable);
            spin_unlock_irq(&p->lock, flags);
            waitq_sleep();
            flags = spin_lock_irq(&p->lock);
            continue;
        }

        done += pipe_put(p, src + done, (uint32_t)(len - done));

        /* wake the reader for each chunk rather than at the end. */
        waitq_wake_all(&p->readable);
    }

    spin_unlock_irq(&p->lock, flags);
    return (int64_t)done;
}

/*
 * a pipe's two ends are two files, one opened for reading and one for
 * writing. the pipe counts the files open on each end, not descriptors:
 * dup and fork add descriptors to a file, and the end closes when the
 * last of them does, which is when the file is released
 */
static int caller(void)
{
    struct thread *p_me = sched_current();
    return (p_me != NULL) ? p_me->i_tid : 0;
}

static int64_t end_read(struct file *p_file, void *p_buf, size_t sz_len)
{
    return pipe_read(p_file->p_private, caller(), p_buf, sz_len,
                     (p_file->u_flags & O_NONBLOCK) != 0);
}

static int64_t end_write(struct file *p_file, const void *p_buf,
                         size_t sz_len)
{
    return pipe_write(p_file->p_private, caller(), p_buf, sz_len,
                      (p_file->u_flags & O_NONBLOCK) != 0);
}

/*
 * readable with bytes in it, or with every writer gone, when the read
 * would be the end; writable with room, and an error when no reader is
 * left to take what is written
 */
static uint32_t end_poll_read(struct file *p_file)
{
    struct pipe *p = p_file->p_private;
    uint64_t u_flags = spin_lock_irq(&p->lock);
    uint32_t u_mask = (p->count > 0 ? POLLIN : 0)
                      | (p->writers == 0 ? POLLHUP : 0);
    spin_unlock_irq(&p->lock, u_flags);
    return u_mask;
}

static uint32_t end_poll_write(struct file *p_file)
{
    struct pipe *p = p_file->p_private;
    uint64_t u_flags = spin_lock_irq(&p->lock);
    uint32_t u_mask = (p->count < PIPE_BUF ? POLLOUT : 0)
                      | (p->readers == 0 ? POLLERR : 0);
    spin_unlock_irq(&p->lock, u_flags);
    return u_mask;
}

static void end_release_read(struct file *p_file)
{
    pipe_close_read(p_file->p_private);
}

static void end_release_write(struct file *p_file)
{
    pipe_close_write(p_file->p_private);
}

static const struct file_operations read_end_ops = {
    .read = end_read,
    .release = end_release_read,
    .poll = end_poll_read,
};

static const struct file_operations write_end_ops = {
    .write = end_write,
    .release = end_release_write,
    .poll = end_poll_write,
};

/*
 * a fifo is a pipe with a name. every open of the one node shares one
 * pipe, made at the first open and freed with the last close, and what
 * was left in it with it. opening an end waits for the other, which is
 * what makes it a pipe rather than a file: a reader for a writer, a
 * writer for a reader. a reader told O_NONBLOCK opens at once, and a
 * writer told it with no reader there is told -ENXIO. one opened for
 * both is both, and waits for nobody
 */
#define FIFO_MAX 32

struct fifo {
    enum vfs_kind e_kind;       /* which node it is the pipe of */
    size_t sz_mount;
    uint32_t u_ino;
    struct pipe *p_pipe;        /* NULL for a slot nobody has */
};

static struct spinlock s_fifo_lock = SPINLOCK("fifo", LOCK_RANK_FIFO);
static struct fifo s_arr_fifos[FIFO_MAX];

/* the pipe a fifo node is, made at its first open: or NULL. the lock is held */
static struct pipe *fifo_of(const struct vfs_file *p_node)
{
    struct fifo *p_free = NULL;
    for (size_t sz_at = 0; sz_at < FIFO_MAX; sz_at++) {
        struct fifo *p_f = &s_arr_fifos[sz_at];
        if (p_f->p_pipe == NULL) {
            p_free = (p_free == NULL) ? p_f : p_free;
        } else if (p_f->e_kind == p_node->kind
                   && p_f->sz_mount == p_node->mount
                   && p_f->u_ino == p_node->u_ino) {
            return p_f->p_pipe;
        }
    }
    struct pipe *p = (p_free != NULL) ? pipe_create() : NULL;
    if (p != NULL) {
        p->readers = 0;
        p->writers = 0;
        p_free->e_kind = p_node->kind;
        p_free->sz_mount = p_node->mount;
        p_free->u_ino = p_node->u_ino;
        p_free->p_pipe = p;
    }
    return p;
}

/*
 * the ends a fifo's file opened let go of, and the fifo's pipe with the
 * last of them, freed after both locks are let go of
 */
static void fifo_let_go(struct pipe *p, uint32_t u_mode)
{
    uint64_t u_flags = spin_lock_irq(&s_fifo_lock);
    uint64_t u_pipe = spin_lock_irq(&p->lock);
    p->readers -= (u_mode & FILE_READ) ? 1 : 0;
    p->writers -= (u_mode & FILE_WRITE) ? 1 : 0;
    waitq_wake_all(&p->readable);
    waitq_wake_all(&p->writable);
    bool b_gone = p->readers == 0 && p->writers == 0;
    spin_unlock_irq(&p->lock, u_pipe);
    for (size_t sz_at = 0; b_gone && sz_at < FIFO_MAX; sz_at++) {
        if (s_arr_fifos[sz_at].p_pipe == p) {
            s_arr_fifos[sz_at].p_pipe = NULL;
        }
    }
    spin_unlock_irq(&s_fifo_lock, u_flags);
    if (b_gone) {
        live--;
        kfree(p);
    }
}

static int64_t fifo_open(struct file *p_file, int i_flags)
{
    bool b_read = (p_file->u_mode & FILE_READ) != 0;
    bool b_write = (p_file->u_mode & FILE_WRITE) != 0;
    bool b_nonblock = (i_flags & O_NONBLOCK) != 0;

    uint64_t u_flags = spin_lock_irq(&s_fifo_lock);
    struct pipe *p = fifo_of(&p_file->node);
    if (p == NULL) {
        spin_unlock_irq(&s_fifo_lock, u_flags);
        return -ENFILE;
    }
    uint64_t u_pipe = spin_lock_irq(&p->lock);
    bool b_refused = b_write && !b_read && b_nonblock && p->readers == 0;
    if (!b_refused) {
        p->readers += b_read ? 1 : 0;
        p->u_read_opens += b_read ? 1 : 0;
        p->writers += b_write ? 1 : 0;
        p->u_write_opens += b_write ? 1 : 0;
        waitq_wake_all(&p->readable);
        waitq_wake_all(&p->writable);
    }
    spin_unlock_irq(&p->lock, u_pipe);
    spin_unlock_irq(&s_fifo_lock, u_flags);
    if (b_refused) {
        fifo_let_go(p, 0);
        return -ENXIO;
    }
    p_file->p_private = p;
    if ((b_read && b_write) || (b_read && b_nonblock)) {
        return 0;
    }

    /*
     * nobody at the other end: wait until somebody opens it, which is
     * that end's count of opens moving, whether or not they are still
     * there by the time this looks
     */
    u_pipe = spin_lock_irq(&p->lock);
    const uint32_t *p_opens = b_read ? &p->u_write_opens : &p->u_read_opens;
    const int *p_there = b_read ? &p->writers : &p->readers;
    uint32_t u_seen = *p_opens;
    struct waitq *p_q = b_read ? &p->readable : &p->writable;
    while (*p_there == 0 && *p_opens == u_seen) {
        if (process_interrupt_pending(caller())) {
            spin_unlock_irq(&p->lock, u_pipe);
            return -ERESTARTSYS;
        }
        waitq_enqueue(p_q);
        spin_unlock_irq(&p->lock, u_pipe);
        waitq_sleep();
        u_pipe = spin_lock_irq(&p->lock);
    }
    spin_unlock_irq(&p->lock, u_pipe);
    return 0;
}

/* a file whose open was refused never had an end, and has none to let go */
static void fifo_release(struct file *p_file)
{
    if (p_file->p_private != NULL) {
        fifo_let_go(p_file->p_private, p_file->u_mode);
    }
}

/* a fifo open for both is a readable end and a writable one at once */
static uint32_t fifo_poll(struct file *p_file)
{
    return ((p_file->u_mode & FILE_READ) ? end_poll_read(p_file) : 0)
           | ((p_file->u_mode & FILE_WRITE) ? end_poll_write(p_file) : 0);
}

static const struct file_operations fifo_ops = {
    .read = end_read,
    .write = end_write,
    .poll = fifo_poll,
    .open = fifo_open,
    .release = fifo_release,
};

struct file *fifo_file(const struct vfs_file *p_node, uint32_t u_mode)
{
    struct file *p_file = file_alloc(&fifo_ops, u_mode, FILE_TYPE_PIPE);
    if (p_file != NULL) {
        p_file->node = *p_node;
    }
    return p_file;
}

bool pipe_open(struct file **pp_read, struct file **pp_write)
{
    struct pipe *p = pipe_create();
    if (p == NULL) {
        return false;
    }
    struct file *p_read = file_alloc(&read_end_ops, FILE_READ,
                                     FILE_TYPE_PIPE);
    struct file *p_write = file_alloc(&write_end_ops, FILE_WRITE,
                                      FILE_TYPE_PIPE);
    if (p_read == NULL || p_write == NULL) {
        /*
         * the ends never made are closed by hand, and the ones that were
         * by putting them, so the pipe is freed exactly once
         */
        if (p_read == NULL) {
            pipe_close_read(p);
        }
        if (p_write == NULL) {
            pipe_close_write(p);
        }
        if (p_read != NULL) {
            p_read->p_private = p;
            file_put(p_read);
        }
        if (p_write != NULL) {
            p_write->p_private = p;
            file_put(p_write);
        }
        return false;
    }
    p_read->p_private = p;
    p_write->p_private = p;
    *pp_read = p_read;
    *pp_write = p_write;
    return true;
}
