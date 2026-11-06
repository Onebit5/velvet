// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/tty.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the terminals: line discipline, the group in front, and ctrl+c.
 */

#include <velvet/tty.h>
#include <velvet/termios.h>
#include <velvet/input.h>
#include <velvet/spinlock.h>
#include <velvet/kprintf.h>
#include <velvet/process.h>
#include <velvet/sched.h>
#include <velvet/thread.h>
#include <velvet/console.h>
#include <velvet/file.h>
#include <velvet/pit.h>
#include <velvet/errno.h>
#include <velvet/string.h>
#include <asm/uaccess.h>
#include <uapi/velvet/fcntl.h>
#include <uapi/velvet/poll.h>

#include <velvet/pty.h>

#include "tty_end.h"

/* how long a timed raw read sleeps between looks */
#define TTY_SLICE_MS 10

/* how much of a write is put through, and shown, under one hold of the lock */
#define TTY_CHUNK 256

struct spinlock g_tty_lock = SPINLOCK("tty", LOCK_RANK_TTY);
static struct tty s_arr_ttys[VCONSOLE_COUNT];
static bool s_b_made;

static const struct tty_end console_end;

void tty_make(struct tty *t, const struct tty_end *p_end, uint32_t u_rdev)
{
    memset(t, 0, sizeof *t);
    t->p_end = p_end;
    t->u_rdev = u_rdev;
    term_default(&t->termios);
}

/* the terminal on a console, made the first time any is asked for */
static struct tty *tty_at(unsigned u_console)
{
    if (!s_b_made) {
        for (unsigned u_at = 0; u_at < VCONSOLE_COUNT; u_at++) {
            tty_make(&s_arr_ttys[u_at], &console_end,
                     (TTY_MAJOR << 8) | (u_at + 1));
            s_arr_ttys[u_at].u_console = u_at;
        }
        s_b_made = true;
    }
    return &s_arr_ttys[u_console < VCONSOLE_COUNT ? u_console : 0];
}

/* everything typed and not yet read, thrown away. the lock is held */
static void forget(struct tty *t)
{
    t->sz_ready = 0;
    t->sz_line = 0;
    t->sz_lines = 0;
}

/* the session's terminal, if t is it. the lock is held */
static bool is_of(const struct tty *t, int i_sid)
{
    return t != NULL && t->i_session != 0 && t->i_session == i_sid;
}

/*
 * the terminal i_pid's session has taken, or NULL: a console's or a
 * pty's, which are looked through alike. the lock is held
 */
static struct tty *ctty_of(int i_pid)
{
    const struct process *p = process_find(i_pid);
    if (p == NULL) {
        return NULL;
    }
    for (unsigned u_at = 0; u_at < VCONSOLE_COUNT; u_at++) {
        if (is_of(tty_at(u_at), p->i_sid)) {
            return tty_at(u_at);
        }
    }
    for (size_t sz_at = 0; sz_at < PTY_MAX; sz_at++) {
        if (is_of(pty_tty_at(sz_at), p->i_sid)) {
            return pty_tty_at(sz_at);
        }
    }
    return NULL;
}

int tty_front(struct tty *t)
{
    return t->i_pgrp;
}

/*
 * a session's terminal let go of, the lock held: nobody's, with the
 * group that was in front, TTY_NOBODY for none
 */
static int let_go_of(struct tty *t)
{
    int i_pgrp = t->i_pgrp;
    t->i_session = 0;
    t->i_pgrp = TTY_NOBODY;
    return i_pgrp;
}

void tty_session_ended(int i_sid)
{
    int i_pgrp = TTY_NOBODY;
    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    struct tty *t = NULL;
    for (unsigned u_at = 0; u_at < VCONSOLE_COUNT && t == NULL; u_at++) {
        t = is_of(tty_at(u_at), i_sid) ? tty_at(u_at) : NULL;
    }
    for (size_t sz_at = 0; sz_at < PTY_MAX && t == NULL; sz_at++) {
        t = is_of(pty_tty_at(sz_at), i_sid) ? pty_tty_at(sz_at) : NULL;
    }
    if (t != NULL) {
        i_pgrp = let_go_of(t);
    }
    spin_unlock_irq(&g_tty_lock, u_flags);
    if (i_pgrp != TTY_NOBODY) {
        process_signal_group(i_pgrp, SIGHUP);
    }
}

/* a console's terminal: what is typed at that console, onto its screen */
static int console_take(struct tty *t)
{
    return input_take(t->u_console);
}

static void console_flush(struct tty *t)
{
    input_flush(t->u_console);
}

static bool console_wait(struct tty *t)
{
    return input_wait(t->u_console);
}

/*
 * bytes onto a terminal's console. what is printed goes to the console
 * of the thread printing it, so for as long as this takes the writer is
 * on the terminal's console, whichever one it was on before.
 *
 * a console starts a new line at the left whatever it is told, the way
 * a terminal set to new line mode does, and so does the serial line,
 * which puts a return before every newline itself. the return ONLCR
 * puts in front of a newline is one both have made already, and is left
 * out, rather than written twice to the wire and once into the log
 */
static void console_put(struct tty *t, const char *p_bytes, size_t sz_len)
{
    struct thread *p_me = sched_current();
    unsigned u_was = (p_me != NULL) ? p_me->console : 0;
    if (p_me != NULL) {
        p_me->console = t->u_console;
    }
    size_t sz_from = 0;
    for (size_t sz_at = 0; sz_at + 1 < sz_len; sz_at++) {
        if (p_bytes[sz_at] == '\r' && p_bytes[sz_at + 1] == '\n') {
            kwrite(p_bytes + sz_from, sz_at - sz_from);
            sz_from = sz_at + 1;
        }
    }
    kwrite(p_bytes + sz_from, sz_len - sz_from);
    if (p_me != NULL) {
        p_me->console = u_was;
    }
}

/*
 * bytes shown on a terminal, through output processing, handed to its
 * end a chunk at a time, for as long as what they become fits in
 * sz_room: how many of them went. what a program writes and what the
 * terminal echoes both come this way, so a newline echoed is a newline
 * written. the lock is held
 */
static size_t show(struct tty *t, const char *p_bytes, size_t sz_len,
                   size_t sz_room)
{
    char arr_out[TTY_CHUNK + TERM_OUT_MAX];
    size_t sz_out = 0;
    size_t sz_at = 0;

    for (; sz_at < sz_len; sz_at++) {
        size_t sz_column = t->sz_column;
        size_t sz_one = term_output(&t->termios, (uint8_t)p_bytes[sz_at],
                                    &sz_column, arr_out + sz_out);
        if (sz_one > sz_room) {
            break;
        }
        t->sz_column = sz_column;
        sz_room -= sz_one;
        sz_out += sz_one;
        if (sz_out >= TTY_CHUNK) {
            t->p_end->put(t, arr_out, sz_out);
            sz_out = 0;
        }
    }
    if (sz_out > 0) {
        t->p_end->put(t, arr_out, sz_out);
    }
    return sz_at;
}

/* an echo, which is never waited for: what an end has no room for is lost */
static void echo(struct tty *t, const char *p_bytes, size_t sz_len)
{
    show(t, p_bytes, sz_len, SIZE_MAX);
}

/*
 * a console is the size of its screen. a machine with no framebuffer
 * answers zero, and a program that believed it would divide by it, so it
 * is eighty by twenty-four, which every terminal has been since the vt100
 */
static void console_size_of(struct tty *t, struct winsize *p_size)
{
    size_t sz_cols = 0, sz_rows = 0, sz_width = 0, sz_height = 0;
    (void)t;
    console_size(&sz_cols, &sz_rows, &sz_width, &sz_height);
    if (sz_cols == 0 || sz_rows == 0) {
        sz_cols = 80;
        sz_rows = 24;
    }
    p_size->ws_row = (uint16_t)sz_rows;
    p_size->ws_col = (uint16_t)sz_cols;
    p_size->ws_xpixel = (uint16_t)sz_width;
    p_size->ws_ypixel = (uint16_t)sz_height;
}

static const struct tty_end console_end = {
    .take = console_take,
    .flush = console_flush,
    .wait = console_wait,
    .put = console_put,
    .size = console_size_of,
};

bool tty_intercept(int key)
{
    /* switching consoles is the terminal's business and nobody else's. */
    if (key >= KEY_CONSOLE_1 && key < KEY_CONSOLE_1 + VCONSOLE_COUNT) {
        console_switch((unsigned)(key - KEY_CONSOLE_1));
        return true;
    }
    if (key == KEY_SCROLL_UP) {
        console_scroll_back(8);
        return true;
    }
    if (key == KEY_SCROLL_DOWN) {
        console_scroll_back(-8);
        return true;
    }
    if (key < 0 || key >= 0x100) {
        return false;
    }

    /*
     * a key is typed at the console being looked at, so both whether it
     * is a signal and who gets it are that console's terminal's to say
     */
    int i_pgrp = TTY_NOBODY;
    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    int i_sig = tty_signal_key(tty_at(console_active()), (uint8_t)key,
                               &i_pgrp);
    spin_unlock_irq(&g_tty_lock, u_flags);

    if (i_sig != 0 && i_pgrp != TTY_NOBODY) {
        process_signal_group(i_pgrp, i_sig);
    }
    return i_sig != 0;
}

int tty_signal_key(struct tty *t, uint8_t u_c, int *p_pgrp)
{
    int i_sig = term_signal(&t->termios, u_c);
    if (i_sig == 0) {
        return 0;
    }
    *p_pgrp = tty_front(t);
    if (!(t->termios.c_lflag & NOFLSH)) {
        forget(t);
        t->p_end->flush(t);
    }
    char arr_echo[2];
    echo(t, arr_echo, term_echo(&t->termios, u_c, arr_echo));
    return i_sig;
}

/*
 * the line being typed, up to sz_count bytes of it, taken back. the lock
 * is held
 */
static void erase(struct tty *t, size_t sz_count, bool b_word)
{
    const struct termios *p_t = &t->termios;
    bool b_show = (p_t->c_lflag & ECHO) && (p_t->c_lflag & ECHOE);
    bool b_in_word = false;

    while (sz_count > 0 && t->sz_line > 0) {
        uint8_t u_c = t->arr_buf[t->sz_ready + t->sz_line - 1];
        if (b_word) {
            bool b_space = (u_c == ' ' || u_c == '\t');
            if (b_space && b_in_word) {
                break;
            }
            b_in_word |= !b_space;
        } else {
            sz_count--;
        }
        t->sz_line--;
        for (size_t sz_col = 0; b_show && sz_col < term_width(p_t, u_c);
             sz_col++) {
            echo(t, "\b \b", 3);
        }
    }
}

/*
 * the line being typed complete, for a read of its own to have, which it
 * already is where it lies. the lock is held
 */
static void deliver(struct tty *t)
{
    t->arr_lines[t->sz_lines++] = (uint16_t)t->sz_line;
    t->sz_ready += t->sz_line;
    t->sz_line = 0;
}

/*
 * whether the discipline may take another byte: in raw mode while there
 * is room, and canonical while there is room for another line and the
 * lines already typed are not what is taking it. a line too long for the
 * terminal loses its end, as it would anywhere, but a line typed after
 * others loses nothing: it waits where it was typed until they are read
 */
static bool has_room(const struct tty *t)
{
    if (!(t->termios.c_lflag & ICANON)) {
        return t->sz_ready < TTY_BUF;
    }
    return t->sz_lines < TTY_LINES
           && (t->sz_ready == 0 || t->sz_ready + t->sz_line < TTY_BUF - 1);
}

/* one byte as it was typed, through the discipline. the lock is held */
static void take_byte(struct tty *t, uint8_t u_raw)
{
    const struct termios *p_t = &t->termios;
    int i_c = term_translate(p_t, u_raw);
    if (i_c < 0) {
        return;
    }
    uint8_t u_c = (uint8_t)i_c;
    char arr_echo[2];
    size_t sz_used = t->sz_ready + t->sz_line;

    if (!(p_t->c_lflag & ICANON)) {
        if (sz_used < TTY_BUF) {
            t->arr_buf[t->sz_ready++] = u_c;
        }
        echo(t, arr_echo, term_echo(p_t, u_c, arr_echo));
        return;
    }

    enum term_action e_act = term_input(p_t, u_c);
    switch (e_act) {
    case TERM_ERASE:
        erase(t, 1, false);
        return;
    case TERM_WERASE:
        erase(t, TTY_BUF, true);
        return;
    case TERM_KILL:
        if ((p_t->c_lflag & ECHOKE) || !(p_t->c_lflag & ECHOK)) {
            erase(t, TTY_BUF, false);
        } else {
            t->sz_line = 0;
            echo(t, "\n", (p_t->c_lflag & ECHO) ? 1 : 0);
        }
        return;
    case TERM_EOF:
        deliver(t);
        return;
    case TERM_DELIVER:
    case TERM_KEEP:
        /* the byte kept back from every line is what a newline goes in */
        if (sz_used >= ((e_act == TERM_DELIVER) ? TTY_BUF : TTY_BUF - 1)) {
            return;
        }
        t->arr_buf[sz_used] = u_c;
        t->sz_line++;
        echo(t, arr_echo, term_echo(p_t, u_c, arr_echo));
        if (e_act == TERM_DELIVER) {
            deliver(t);
        }
        return;
    }
}

/*
 * whatever has been typed at the terminal, through the discipline, for
 * as long as it has room: echoed as it is taken, which is when somebody
 * reads or asks whether there is anything to read. a byte is taken and
 * put through under one hold of the lock, so two readers draining at
 * once cannot put two keys in the wrong order
 */
void tty_drain(struct tty *t)
{
    for (;;) {
        uint64_t u_flags = spin_lock_irq(&g_tty_lock);
        int i_byte = has_room(t) ? t->p_end->take(t) : -1;
        if (i_byte >= 0) {
            take_byte(t, (uint8_t)i_byte);
        }
        spin_unlock_irq(&g_tty_lock, u_flags);
        if (i_byte < 0) {
            return;
        }
    }
}

/*
 * up to sz_len of what is ready, handed over and gone: when canonical, of
 * the first line only, the rest of it left for the next read. how many,
 * or -1 for nothing to hand over. the lock is held
 */
static int64_t take_ready(struct tty *t, void *p_buf, size_t sz_len)
{
    bool b_canon = (t->termios.c_lflag & ICANON) != 0;
    if (b_canon ? t->sz_lines == 0 : t->sz_ready == 0) {
        return -1;
    }
    size_t sz_have = b_canon ? t->arr_lines[0] : t->sz_ready;
    size_t sz_take = (sz_have < sz_len) ? sz_have : sz_len;
    memcpy(p_buf, t->arr_buf, sz_take);
    memmove(t->arr_buf, t->arr_buf + sz_take,
            t->sz_ready + t->sz_line - sz_take);
    t->sz_ready -= sz_take;
    /* a read of nothing takes nothing, not even the end of input */
    if (b_canon && sz_len > 0 && sz_take == sz_have) {
        t->sz_lines--;
        memmove(t->arr_lines, t->arr_lines + 1,
                t->sz_lines * sizeof t->arr_lines[0]);
    } else if (b_canon) {
        t->arr_lines[0] = (uint16_t)(sz_have - sz_take);
    }
    return (int64_t)sz_take;
}

/*
 * a read from a group behind the one in front of its own terminal stops
 * that group, the way a job put in the background stops when it wants
 * the keyboard, and the read is made again once it is continued. a group
 * that would not stop, ignoring or blocking SIGTTIN, is told -EIO, and
 * so is an orphaned one, which nobody would continue
 */
static int64_t may_read(struct tty *t)
{
    int i_pid = current_pid();
    if (i_pid == 0) {
        return 0;
    }
    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    const struct process *p = process_find(i_pid);
    bool b_behind = p != NULL && t->i_session != 0
                    && t->i_session == p->i_sid && t->i_pgrp != p->pgid;
    int i_pgid = (p != NULL) ? p->pgid : 0;
    spin_unlock_irq(&g_tty_lock, u_flags);
    if (!b_behind) {
        return 0;
    }
    const struct signal_state *p_sig = process_signal_state(i_pid);
    if (p_sig == NULL
        || signal_action_for(p_sig, SIGTTIN) == SIGNAL_ACTION_IGNORE
        || (p_sig->blocked & SIGNAL_BIT(SIGTTIN))
        || process_group_orphaned(i_pgid)) {
        return -EIO;
    }
    process_signal_group(i_pgid, SIGTTIN);
    return -ERESTARTSYS;
}

/*
 * a change to a terminal from a group behind the one in front of it, its
 * own terminal, stops that group, unless it ignores or blocks SIGTTOU:
 * which is how a shell, which does, hands the terminal from job to job.
 * an orphaned group that would be stopped is told -EIO instead
 */
static int64_t may_change(struct tty *t)
{
    int i_pid = current_pid();
    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    const struct process *p = process_find(i_pid);
    bool b_behind = p != NULL && t->i_session != 0
                    && t->i_session == p->i_sid && t->i_pgrp != p->pgid;
    int i_pgid = (p != NULL) ? p->pgid : 0;
    spin_unlock_irq(&g_tty_lock, u_flags);
    const struct signal_state *p_sig = process_signal_state(i_pid);
    if (!b_behind || p_sig == NULL
        || signal_action_for(p_sig, SIGTTOU) == SIGNAL_ACTION_IGNORE
        || (p_sig->blocked & SIGNAL_BIT(SIGTTOU))) {
        return 0;
    }
    if (process_group_orphaned(i_pgid)) {
        return -EIO;
    }
    process_signal_group(i_pgid, SIGTTOU);
    return -ERESTARTSYS;
}

/*
 * a read: one line when canonical, and otherwise as VMIN and VTIME say,
 * MIN bytes with no time limit, whatever there is with neither, or what
 * arrives before TIME tenths of a second have passed, since the call or
 * since the last byte when MIN is set too
 */
static int64_t tty_read(struct file *p_file, void *p_buf, size_t sz_len)
{
    struct tty *t = p_file->p_private;
    int64_t i_why = may_read(t);
    if (i_why < 0) {
        return i_why;
    }
    uint64_t u_start = pit_uptime_ms();
    uint64_t u_last = u_start;
    size_t sz_seen = 0;

    for (;;) {
        tty_drain(t);
        uint64_t u_now = pit_uptime_ms();
        uint64_t u_flags = spin_lock_irq(&g_tty_lock);
        int64_t i_got = -1;
        bool b_timed = false;
        if (t->b_hung) {
            i_got = 0;
        } else if (t->termios.c_lflag & ICANON) {
            i_got = take_ready(t, p_buf, sz_len);
        } else {
            size_t sz_min = t->termios.c_cc[VMIN];
            uint64_t u_time = t->termios.c_cc[VTIME] * 100ull;
            if (t->sz_ready != sz_seen) {
                sz_seen = t->sz_ready;
                u_last = u_now;
            }
            uint64_t u_since = (sz_min == 0) ? u_start : u_last;
            b_timed = u_time != 0 && (sz_min == 0 || t->sz_ready > 0);
            size_t sz_want = (sz_min < sz_len) ? sz_min : sz_len;
            if ((t->sz_ready >= sz_want && t->sz_ready > 0)
                || (sz_min == 0 && u_time == 0)
                || (b_timed && u_now - u_since >= u_time)) {
                i_got = take_ready(t, p_buf, sz_len);
                i_got = (i_got < 0) ? 0 : i_got;
            }
        }
        spin_unlock_irq(&g_tty_lock, u_flags);
        if (i_got >= 0) {
            return i_got;
        }
        if (p_file->u_flags & O_NONBLOCK) {
            return -EAGAIN;
        }
        if (b_timed) {
            if (process_interrupt_pending(current_pid())) {
                return -ERESTARTSYS;
            }
            sleep_ms(TTY_SLICE_MS);
        } else if (!t->p_end->wait(t)) {
            return -ERESTARTSYS;
        }
    }
}

/*
 * what is written is shown, through output processing, a chunk at a
 * time, waiting for room where the end can be full: how much went, which
 * is less only for a signal or a terminal that would not wait, or -EIO
 * once the far side is gone. with TOSTOP a write from a group behind is
 * a change like any other, and stops that group the same way
 */
static int64_t tty_write(struct file *p_file, const void *p_buf,
                         size_t sz_len)
{
    struct tty *t = p_file->p_private;
    const char *p_bytes = p_buf;
    size_t sz_done = 0;

    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    bool b_tostop = (t->termios.c_lflag & TOSTOP) != 0;
    spin_unlock_irq(&g_tty_lock, u_flags);
    int64_t i_why = b_tostop ? may_change(t) : 0;
    if (i_why < 0) {
        return i_why;
    }

    while (sz_done < sz_len) {
        size_t sz_part = (sz_len - sz_done < TTY_CHUNK) ? sz_len - sz_done
                                                        : TTY_CHUNK;
        u_flags = spin_lock_irq(&g_tty_lock);
        bool b_hung = t->b_hung;
        size_t sz_room = (t->p_end->room != NULL) ? t->p_end->room(t)
                                                  : SIZE_MAX;
        size_t sz_went = b_hung ? 0 : show(t, p_bytes + sz_done, sz_part,
                                           sz_room);
        spin_unlock_irq(&g_tty_lock, u_flags);
        sz_done += sz_went;
        if (b_hung) {
            return (sz_done > 0) ? (int64_t)sz_done : -EIO;
        }
        if (sz_went > 0) {
            continue;
        }
        if (p_file->u_flags & O_NONBLOCK) {
            return (sz_done > 0) ? (int64_t)sz_done : -EAGAIN;
        }
        if (!t->p_end->wait_room(t)) {
            return (sz_done > 0) ? (int64_t)sz_done : -ERESTARTSYS;
        }
    }
    return (int64_t)sz_done;
}

/*
 * whether a read would not wait, as poll asks it: a whole line, or raw,
 * the bytes VMIN asks for, or one when VTIME would only be waited out
 * for the first. the lock is held
 */
static bool readable(const struct tty *t)
{
    if (t->termios.c_lflag & ICANON) {
        return t->sz_lines > 0;
    }
    size_t sz_want = (t->termios.c_cc[VTIME] == 0) ? t->termios.c_cc[VMIN]
                                                   : 1;
    return t->sz_ready >= sz_want;
}

/* and one whose far side is gone is everything at once, since nothing waits */
static uint32_t tty_poll(struct file *p_file)
{
    struct tty *t = p_file->p_private;
    tty_drain(t);
    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    uint32_t u_mask = (readable(t) ? POLLIN : 0)
                      | ((t->p_end->room == NULL || t->p_end->room(t) > 0)
                         ? POLLOUT : 0);
    if (t->b_hung) {
        u_mask = POLLIN | POLLOUT | POLLERR | POLLHUP;
    }
    spin_unlock_irq(&g_tty_lock, u_flags);
    return u_mask;
}

/*
 * a new termios, and what changing it means for what is already typed.
 * with ICANON going off the lines and the one half typed are bytes, for
 * raw reads to find first; coming on, what was typed raw is one line,
 * for a read to hand over as it is
 */
static void set_termios(struct tty *t, const struct termios *p_new,
                        bool b_flush)
{
    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    bool b_was = (t->termios.c_lflag & ICANON) != 0;
    bool b_now = (p_new->c_lflag & ICANON) != 0;
    if (b_was && !b_now) {
        t->sz_ready += t->sz_line;
        t->sz_line = 0;
        t->sz_lines = 0;
    } else if (!b_was && b_now && t->sz_ready > 0) {
        t->arr_lines[0] = (uint16_t)t->sz_ready;
        t->sz_lines = 1;
    }
    if (b_flush) {
        forget(t);
        t->p_end->flush(t);
    }
    t->termios = *p_new;
    spin_unlock_irq(&g_tty_lock, u_flags);
}

/* the caller's own terminal, or why not: -ENOTTY for any other */
static int64_t check_own(struct tty *t)
{
    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    bool b_own = ctty_of(current_pid()) == t;
    spin_unlock_irq(&g_tty_lock, u_flags);
    return b_own ? 0 : -ENOTTY;
}

/*
 * t made the terminal of the caller's session: only a session leader's,
 * and only when it has none and t is nobody else's, unless b_steal says
 * to take it from whichever session has it. 0 or -EPERM, and for a
 * console's, the kernel's own lines about the caller appear on it from
 * then on
 */
static int64_t claim(struct tty *t, bool b_steal)
{
    int i_pid = current_pid();
    const struct process *p = process_find(i_pid);
    if (p == NULL || p->i_sid != i_pid) {
        return -EPERM;
    }
    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    struct tty *p_own = ctty_of(i_pid);
    int64_t i_result = 0;
    if (p_own == t) {
        i_result = 0;
    } else if (p_own != NULL || (t->i_session != 0 && !b_steal)) {
        i_result = -EPERM;
    } else {
        t->i_session = p->i_sid;
        t->i_pgrp = p->pgid;
    }
    spin_unlock_irq(&g_tty_lock, u_flags);

    struct thread *p_me = sched_current();
    if (i_result == 0 && p_me != NULL && t->p_end == &console_end) {
        p_me->console = t->u_console;
    }
    return i_result;
}

/* TIOCSCTTY, with which root asking with 1 may take one from another session */
static int64_t take_as_own(struct tty *t, uint64_t u_arg)
{
    return claim(t, process_uid(current_pid()) == 0 && u_arg == 1);
}

/* the group in front, which has to be in the terminal's session */
static int64_t set_front(struct tty *t, uint64_t u_arg)
{
    int32_t i_pgrp = 0;
    if (copy_from_user(&i_pgrp, u_arg, sizeof i_pgrp) != 0) {
        return -EFAULT;
    }
    if (i_pgrp <= 0) {
        return -EINVAL;
    }
    int64_t i_why = may_change(t);
    if (i_why < 0) {
        return i_why;
    }
    int i_member = 0;
    if (process_group_pids(i_pgrp, &i_member, 1) == 0) {
        return -EPERM;
    }
    const struct process *p_member = process_find(i_member);
    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    int64_t i_result = -EPERM;
    if (ctty_of(current_pid()) != t) {
        i_result = -ENOTTY;
    } else if (p_member != NULL && p_member->i_sid == t->i_session) {
        t->i_pgrp = i_pgrp;
        i_result = 0;
    }
    spin_unlock_irq(&g_tty_lock, u_flags);
    return i_result;
}

/*
 * the caller's session lets go of its terminal, which only its leader
 * can say, and the group in front hears it hung up on and is continued
 * to hear it
 */
static int64_t let_go(struct tty *t)
{
    int64_t i_why = check_own(t);
    if (i_why < 0) {
        return i_why;
    }
    const struct process *p = process_find(current_pid());
    int i_pgrp = TTY_NOBODY;
    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    if (p != NULL && p->i_sid == p->pid) {
        i_pgrp = let_go_of(t);
    }
    spin_unlock_irq(&g_tty_lock, u_flags);
    if (i_pgrp != TTY_NOBODY) {
        process_signal_group(i_pgrp, SIGHUP);
        process_signal_group(i_pgrp, SIGCONT);
    }
    return 0;
}

static int64_t put_int(uint64_t u_to, int32_t i_value)
{
    return (put_user_i32(u_to, i_value) == 0) ? 0 : -EFAULT;
}

/* the size the far side says, and a new one, which the group in front hears */
static int64_t get_size(struct tty *t, uint64_t u_arg)
{
    struct winsize size;
    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    t->p_end->size(t, &size);
    spin_unlock_irq(&g_tty_lock, u_flags);
    return (copy_to_user(u_arg, &size, sizeof size) == 0) ? 0 : -EFAULT;
}

static int64_t set_size(struct tty *t, uint64_t u_arg)
{
    struct winsize size;
    /* a console is the size of the screen, which nothing here changes */
    if (t->p_end->resize == NULL) {
        return -EINVAL;
    }
    if (copy_from_user(&size, u_arg, sizeof size) != 0) {
        return -EFAULT;
    }
    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    bool b_changed = t->p_end->resize(t, &size);
    int i_pgrp = tty_front(t);
    spin_unlock_irq(&g_tty_lock, u_flags);
    if (b_changed && i_pgrp != TTY_NOBODY) {
        process_signal_group(i_pgrp, SIGWINCH);
    }
    return 0;
}

int64_t tty_request(struct tty *t, uint32_t u_cmd, uint64_t u_arg)
{
    struct termios termios;
    int64_t i_why;

    switch (u_cmd) {
    case TCGETS: {
        uint64_t u_flags = spin_lock_irq(&g_tty_lock);
        termios = t->termios;
        spin_unlock_irq(&g_tty_lock, u_flags);
        return (copy_to_user(u_arg, &termios, sizeof termios) == 0)
               ? 0 : -EFAULT;
    }
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        if (copy_from_user(&termios, u_arg, sizeof termios) != 0) {
            return -EFAULT;
        }
        i_why = may_change(t);
        if (i_why < 0) {
            return i_why;
        }
        set_termios(t, &termios, u_cmd == TCSETSF);
        return 0;
    case TIOCGWINSZ:
        return get_size(t, u_arg);
    case TIOCSWINSZ:
        return set_size(t, u_arg);
    case TIOCGPGRP:
        i_why = check_own(t);
        return (i_why < 0) ? i_why : put_int(u_arg, t->i_pgrp);
    case TIOCSPGRP:
        return set_front(t, u_arg);
    case TIOCGSID:
        i_why = check_own(t);
        return (i_why < 0) ? i_why : put_int(u_arg, t->i_session);
    case TIOCSCTTY:
        return take_as_own(t, u_arg);
    case TIOCNOTTY:
        return let_go(t);
    case FIONREAD: {
        tty_drain(t);
        uint64_t u_flags = spin_lock_irq(&g_tty_lock);
        int32_t i_count = (int32_t)t->sz_ready;
        spin_unlock_irq(&g_tty_lock, u_flags);
        return put_int(u_arg, i_count);
    }
    case TCFLSH:
        if (u_arg > 2) {
            return -EINVAL;
        }
        /* nothing waits to go out, so only the input has anything to lose */
        if (u_arg != 1) {
            set_termios(t, &t->termios, true);
        }
        return 0;
    case TIOCOUTQ:
        return put_int(u_arg, 0);
    case TCSBRK:
    case TCXONC:
        return 0;
    default:
        return -ENOTTY;
    }
}

/*
 * a request of a terminal whose far side is gone is -EIO, except that
 * putting a group in front of it is a terminal that is not the caller's
 * any more
 */
static int64_t tty_ioctl(struct file *p_file, uint32_t u_cmd, uint64_t u_arg)
{
    struct tty *t = p_file->p_private;
    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    bool b_hung = t->b_hung;
    spin_unlock_irq(&g_tty_lock, u_flags);
    if (b_hung) {
        return (u_cmd == TIOCSPGRP) ? -ENOTTY : -EIO;
    }
    return tty_request(t, u_cmd, u_arg);
}

/*
 * a program opening a terminal by name. a session leader with no
 * terminal that opens one nobody has, for reading and without O_NOCTTY,
 * has it for its terminal from then on, as though it had asked with
 * TIOCSCTTY. /dev/console never is, being every session's to write to,
 * and a pty's master is no terminal of this kind at all
 */
static int64_t tty_opening(struct file *p_file, int i_flags)
{
    struct tty *t = p_file->p_private;
    int64_t i_why = 0;
    if (t->p_end->may_open != NULL) {
        uint64_t u_flags = spin_lock_irq(&g_tty_lock);
        i_why = t->p_end->may_open(t);
        spin_unlock_irq(&g_tty_lock, u_flags);
    }
    if (i_why == 0 && !(i_flags & O_NOCTTY) && (p_file->u_mode & FILE_READ)
        && p_file->u_rdev != TTY_CONSOLE_RDEV) {
        claim(t, false);
    }
    return i_why;
}

static void tty_release(struct file *p_file)
{
    struct tty *t = p_file->p_private;
    if (t->p_end->detach != NULL) {
        t->p_end->detach(t);
    }
}

static const struct file_operations tty_ops = {
    .read = tty_read,
    .write = tty_write,
    .poll = tty_poll,
    .ioctl = tty_ioctl,
    .open = tty_opening,
    .release = tty_release,
};

struct file *tty_file(struct tty *t, uint32_t u_mode)
{
    struct file *p_file = file_alloc(&tty_ops, u_mode, FILE_TYPE_TTY);
    if (p_file == NULL) {
        return NULL;
    }
    p_file->p_private = t;
    p_file->u_rdev = t->u_rdev;
    if (t->p_end->attach != NULL) {
        t->p_end->attach(t);
    }
    return p_file;
}

struct file *tty_open(unsigned u_console, uint32_t u_mode)
{
    if (u_console >= VCONSOLE_COUNT) {
        return NULL;
    }
    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    struct file *p_file = tty_file(tty_at(u_console), u_mode);
    spin_unlock_irq(&g_tty_lock, u_flags);
    return p_file;
}

/*
 * found and opened under one hold of the lock, so a pty cannot be let go
 * of in between
 */
struct file *tty_open_own(uint32_t u_mode)
{
    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    struct tty *t = ctty_of(current_pid());
    struct file *p_file = (t != NULL) ? tty_file(t, u_mode) : NULL;
    spin_unlock_irq(&g_tty_lock, u_flags);
    return p_file;
}

int tty_foreground(unsigned u_console)
{
    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    int i_pgrp = tty_front(tty_at(u_console));
    spin_unlock_irq(&g_tty_lock, u_flags);
    return i_pgrp;
}

void tty_hangup(struct tty *t)
{
    uint64_t u_flags = spin_lock_irq(&g_tty_lock);
    int i_leader = t->i_session;
    t->b_hung = true;
    let_go_of(t);
    forget(t);
    spin_unlock_irq(&g_tty_lock, u_flags);
    if (i_leader != 0) {
        process_signal(i_leader, SIGHUP);
        process_signal(i_leader, SIGCONT);
    }
}
