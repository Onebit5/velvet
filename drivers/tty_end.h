// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/tty_end.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a terminal, for the drivers that are one of its two ends.
 */

#ifndef DRIVERS_TTY_END_H
#define DRIVERS_TTY_END_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <velvet/spinlock.h>
#include <velvet/termios.h>

/* the most one terminal holds, typed and not yet read */
#define TTY_BUF 4096

/* and the most whole lines among that, each waiting for a read of its own */
#define TTY_LINES 128

struct tty;
struct file;

/*
 * every terminal is kept under this one lock, and so is whatever an end
 * keeps between the terminal and whoever is on the far side of it
 */
extern struct spinlock g_tty_lock;

/*
 * what a terminal sits between: where what is typed at it comes from,
 * and where what it shows goes. the discipline in the middle is the same
 * for every terminal, and these are all that make one a console's or a
 * pty's. all but wait, wait_room and detach are called with the lock
 * held, and one left NULL is a thing the end does not do
 */
struct tty_end {
    /* the next byte typed and not yet taken, or -1 for none */
    int (*take)(struct tty *t);
    /* every byte typed and not yet taken, thrown away */
    void (*flush)(struct tty *t);
    /*
     * sleep until a byte may have been typed: true, or false when a
     * signal for the caller ended the wait first
     */
    bool (*wait)(struct tty *t);
    /* bytes shown, as the discipline left them: what does not fit is lost */
    void (*put)(struct tty *t, const char *p_bytes, size_t sz_len);
    /* how many put would take now. NULL for an end that is never full */
    size_t (*room)(struct tty *t);
    /* and sleep until it may take more: false for a signal */
    bool (*wait_room)(struct tty *t);
    /* how big the far side says the terminal is */
    void (*size)(struct tty *t, struct winsize *p_size);
    /* a size for it, which is the far side's to say: whether that changed */
    bool (*resize)(struct tty *t, const struct winsize *p_size);
    /* a file is open on it now, and one of them is closed for the last time */
    void (*attach)(struct tty *t);
    void (*detach)(struct tty *t);
    /* a program opening it by name: 0, or why it may not */
    int64_t (*may_open)(struct tty *t);
};

struct tty {
    const struct tty_end *p_end;
    unsigned u_console;         /* for a console's, which one */
    uint32_t u_rdev;            /* the device it is, major << 8 | minor */
    struct termios termios;

    int i_pgrp;                 /* the group in front */
    int i_session;              /* the session it is the terminal of */
    bool b_hung;                /* the far side is gone, and will not be back */

    /*
     * what has been typed, through the discipline: first the sz_ready
     * bytes a read can have, and after them the sz_line of the line
     * being typed. canonical, what is ready is whole lines, arr_lines
     * long each and in the order typed, a line of none being the end of
     * input; raw, it is bytes, and there are no lines
     */
    uint8_t arr_buf[TTY_BUF];
    size_t sz_ready;
    size_t sz_line;
    uint16_t arr_lines[TTY_LINES];
    size_t sz_lines;

    size_t sz_column;           /* where output has left the cursor */
};

/*
 * a terminal between the halves of p_end, numbered u_rdev, as a terminal
 * starts: a line at a time, echoed, nobody's
 */
void tty_make(struct tty *t, const struct tty_end *p_end, uint32_t u_rdev);

/* a file open on a terminal, with one reference, or NULL. the lock is held */
struct file *tty_file(struct tty *t, uint32_t u_mode);

/* what has been typed, through the discipline, for as long as it has room */
void tty_drain(struct tty *t);

/*
 * a byte typed at a terminal, taken as a signal if it is one: the
 * signal, with the group in front in *p_pgrp for the caller to send it
 * to once the lock is let go, or 0 for a byte that is not one. the lock
 * is held
 */
int tty_signal_key(struct tty *t, uint8_t u_c, int *p_pgrp);

/*
 * the far side gone for good: the session's leader told with SIGHUP
 * and then SIGCONT, the terminal nobody's, and from then on a read is
 * the end and a write -EIO
 */
void tty_hangup(struct tty *t);

/* a request of a terminal, as its own ioctl makes it */
int64_t tty_request(struct tty *t, uint32_t u_cmd, uint64_t u_arg);

/* the group in front, TTY_NOBODY for none. the lock is held */
int tty_front(struct tty *t);

/* the terminal of pty sz_index, or NULL for none. the lock is held */
struct tty *pty_tty_at(size_t sz_index);

#endif
