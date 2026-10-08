// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/pipe.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * pipes: a buffer with a process at each end.
 */

/* the design notes for pipe.h are in docs/subsystems/mm.rst */

#ifndef FS_PIPE_H
#define FS_PIPE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "sched/sched.h"        /* for struct waitq */
#include "sched/spinlock.h"

#define PIPE_BUF 4096

struct pipe {
    struct spinlock lock;

    uint8_t  data[PIPE_BUF];
    uint32_t head;              /* where the next byte comes out */
    uint32_t count;             /* how many are in there */

    int      readers;
    int      writers;

    struct waitq readable;      /* writers wake this */
    struct waitq writable;      /* readers wake this */
};

/*
 * split out on purpose: a ring buffer is exactly the kind of thing that
 * is wrong at the wrap and right everywhere else, and that is only
 * findable by a test that can run it a hundred thousand times. these
 * take no locks and never block, so the host suite can do that
 */

uint32_t pipe_pending(const struct pipe *p);    /* bytes waiting */
uint32_t pipe_room(const struct pipe *p);       /* bytes that would fit */

uint32_t pipe_put(struct pipe *p, const void *buf, uint32_t len);
uint32_t pipe_get(struct pipe *p, void *buf, uint32_t len);

void pipe_reset(struct pipe *p);

struct pipe *pipe_create(void);

void pipe_close_read(struct pipe *p);
void pipe_close_write(struct pipe *p);

void pipe_share(struct pipe *p, bool writing);

int64_t pipe_read(struct pipe *p, int pid, void *buf, uint64_t len);

int64_t pipe_write(struct pipe *p, int pid, const void *buf, uint64_t len);

void pipe_release_for(int pid);

size_t pipe_count(void);

#endif
