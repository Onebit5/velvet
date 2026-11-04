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

/* the most one terminal holds, typed and not yet read */
#define TTY_BUF 4096

/* and the most whole lines among that, each waiting for a read of its own */
#define TTY_LINES 128

/* how long a timed raw read sleeps between looks */
#define TTY_SLICE_MS 10

/* how much of a write is put through, and shown, under one hold of the lock */
#define TTY_CHUNK 256

struct tty;

/*
 * what a terminal sits between: where what is typed at it comes from,
 * and where what it shows goes. the discipline in the middle is the same
 * for every terminal, and these are all that make one a console's. all
 * but wait are called with the lock held
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
    /* bytes shown, as the discipline left them */
    void (*put)(struct tty *t, const char *p_bytes, size_t sz_len);
};

struct tty {
    const struct tty_end *p_end;
    unsigned u_console;         /* for a console's, which one */
    struct termios termios;

    int i_pgrp;                 /* the group in front */
    int i_session;              /* the session it is the terminal of */

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
};

static struct spinlock tty_lock = SPINLOCK("tty", LOCK_RANK_TTY);
static struct tty s_arr_ttys[VCONSOLE_COUNT];
static bool s_b_made;

static const struct tty_end console_end;

/* the terminal on a console, made the first time any is asked for */
static struct tty *tty_at(unsigned u_console)
{
    if (!s_b_made) {
        for (unsigned u_at = 0; u_at < VCONSOLE_COUNT; u_at++) {
            s_arr_ttys[u_at].p_end = &console_end;
            s_arr_ttys[u_at].u_console = u_at;
            term_default(&s_arr_ttys[u_at].termios);
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

/*
 * a session ends with its leader, and its terminal is nobody's after
 * that. asked whenever it matters rather than told at every exit, since
 * the answer is in the process table already. the lock is held
 */
static void settle(struct tty *t)
{
    if (t->i_session == 0) {
        return;
    }
    const struct process *p = process_find(t->i_session);
    if (p == NULL || p->exited || p->i_sid != t->i_session) {
        t->i_session = 0;
        t->i_pgrp = TTY_NOBODY;
    }
}

/* the terminal i_pid's session has taken, or NULL. the lock is held */
static struct tty *ctty_of(int i_pid)
{
    const struct process *p = process_find(i_pid);
    if (p == NULL) {
        return NULL;
    }
    for (unsigned u_at = 0; u_at < VCONSOLE_COUNT; u_at++) {
        struct tty *t = tty_at(u_at);
        settle(t);
        if (t->i_session != 0 && t->i_session == p->i_sid) {
            return t;
        }
    }
    return NULL;
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
 * on the terminal's console, whichever one it was on before
 */
static void console_put(struct tty *t, const char *p_bytes, size_t sz_len)
{
    if (sz_len == 0) {
        return;
    }
    struct thread *p_me = sched_current();
    if (p_me == NULL) {
        kwrite(p_bytes, sz_len);
        return;
    }
    unsigned u_was = p_me->console;
    p_me->console = t->u_console;
    kwrite(p_bytes, sz_len);
    p_me->console = u_was;
}

static const struct tty_end console_end = {
    .take = console_take,
    .flush = console_flush,
    .wait = console_wait,
    .put = console_put,
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
    unsigned u_console = console_active();
    uint64_t u_flags = spin_lock_irq(&tty_lock);
    struct tty *t = tty_at(u_console);
    int i_sig = term_signal(&t->termios, (uint8_t)key);
    if (i_sig == 0) {
        spin_unlock_irq(&tty_lock, u_flags);
        return false;
    }
    settle(t);
    int i_pgrp = t->i_pgrp;
    if (!(t->termios.c_lflag & NOFLSH)) {
        forget(t);
        t->p_end->flush(t);
    }
    char arr_echo[2];
    size_t sz_echo = term_echo(&t->termios, (uint8_t)key, arr_echo);
    t->p_end->put(t, arr_echo, sz_echo);
    spin_unlock_irq(&tty_lock, u_flags);

    if (i_pgrp != TTY_NOBODY) {
        process_signal_group(i_pgrp, i_sig);
    }
    return true;
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
            t->p_end->put(t, "\b \b", 3);
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
        t->p_end->put(t, arr_echo, term_echo(p_t, u_c, arr_echo));
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
            t->p_end->put(t, "\n", (p_t->c_lflag & ECHO) ? 1 : 0);
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
        t->p_end->put(t, arr_echo, term_echo(p_t, u_c, arr_echo));
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
static void drain(struct tty *t)
{
    for (;;) {
        uint64_t u_flags = spin_lock_irq(&tty_lock);
        int i_byte = has_room(t) ? t->p_end->take(t) : -1;
        if (i_byte >= 0) {
            take_byte(t, (uint8_t)i_byte);
        }
        spin_unlock_irq(&tty_lock, u_flags);
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
 * that would not stop, ignoring or blocking SIGTTIN, is told -EIO
 */
static int64_t may_read(struct tty *t)
{
    int i_pid = current_pid();
    if (i_pid == 0) {
        return 0;
    }
    uint64_t u_flags = spin_lock_irq(&tty_lock);
    settle(t);
    const struct process *p = process_find(i_pid);
    bool b_behind = p != NULL && t->i_session != 0
                    && t->i_session == p->i_sid && t->i_pgrp != p->pgid;
    int i_pgid = (p != NULL) ? p->pgid : 0;
    spin_unlock_irq(&tty_lock, u_flags);
    if (!b_behind) {
        return 0;
    }
    const struct signal_state *p_sig = process_signal_state(i_pid);
    if (p_sig == NULL
        || signal_action_for(p_sig, SIGTTIN) == SIGNAL_ACTION_IGNORE
        || (p_sig->blocked & SIGNAL_BIT(SIGTTIN))) {
        return -EIO;
    }
    process_signal_group(i_pgid, SIGTTIN);
    return -ERESTARTSYS;
}

/*
 * a change to a terminal from a group behind the one in front of it, its
 * own terminal, stops that group, unless it ignores or blocks SIGTTOU:
 * which is how a shell, which does, hands the terminal from job to job
 */
static int64_t may_change(struct tty *t)
{
    int i_pid = current_pid();
    uint64_t u_flags = spin_lock_irq(&tty_lock);
    settle(t);
    const struct process *p = process_find(i_pid);
    bool b_behind = p != NULL && t->i_session != 0
                    && t->i_session == p->i_sid && t->i_pgrp != p->pgid;
    int i_pgid = (p != NULL) ? p->pgid : 0;
    spin_unlock_irq(&tty_lock, u_flags);
    const struct signal_state *p_sig = process_signal_state(i_pid);
    if (!b_behind || p_sig == NULL
        || signal_action_for(p_sig, SIGTTOU) == SIGNAL_ACTION_IGNORE
        || (p_sig->blocked & SIGNAL_BIT(SIGTTOU))) {
        return 0;
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
        drain(t);
        uint64_t u_now = pit_uptime_ms();
        uint64_t u_flags = spin_lock_irq(&tty_lock);
        int64_t i_got = -1;
        bool b_timed = false;
        if (t->termios.c_lflag & ICANON) {
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
        spin_unlock_irq(&tty_lock, u_flags);
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
 * what is written is shown as it is, a chunk at a time. OPOST is not
 * looked at: the console starts a new line at the start, which is
 * ONLCR, either way
 */
static int64_t tty_write(struct file *p_file, const void *p_buf,
                         size_t sz_len)
{
    struct tty *t = p_file->p_private;
    const char *p_bytes = p_buf;
    for (size_t sz_at = 0; sz_at < sz_len; sz_at += TTY_CHUNK) {
        size_t sz_part = (sz_len - sz_at < TTY_CHUNK) ? sz_len - sz_at
                                                      : TTY_CHUNK;
        uint64_t u_flags = spin_lock_irq(&tty_lock);
        t->p_end->put(t, p_bytes + sz_at, sz_part);
        spin_unlock_irq(&tty_lock, u_flags);
    }
    return (int64_t)sz_len;
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

static uint32_t tty_poll(struct file *p_file)
{
    struct tty *t = p_file->p_private;
    drain(t);
    uint64_t u_flags = spin_lock_irq(&tty_lock);
    bool b_in = readable(t);
    spin_unlock_irq(&tty_lock, u_flags);
    return POLLOUT | (b_in ? POLLIN : 0);
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
    uint64_t u_flags = spin_lock_irq(&tty_lock);
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
    spin_unlock_irq(&tty_lock, u_flags);
}

/* the caller's own terminal, or why not: -ENOTTY for any other */
static int64_t check_own(struct tty *t)
{
    uint64_t u_flags = spin_lock_irq(&tty_lock);
    bool b_own = ctty_of(current_pid()) == t;
    spin_unlock_irq(&tty_lock, u_flags);
    return b_own ? 0 : -ENOTTY;
}

/*
 * become the terminal of the caller's session: only a session leader's,
 * and only when it has none and this one is nobody else's, except that
 * root asking with 1 may take one from another session
 */
static int64_t take_as_own(struct tty *t, uint64_t u_arg)
{
    int i_pid = current_pid();
    const struct process *p = process_find(i_pid);
    if (p == NULL || p->i_sid != i_pid) {
        return -EPERM;
    }
    uint64_t u_flags = spin_lock_irq(&tty_lock);
    struct tty *p_own = ctty_of(i_pid);
    int64_t i_result = 0;
    if (p_own == t) {
        i_result = 0;
    } else if (p_own != NULL) {
        i_result = -EPERM;
    } else if (t->i_session != 0 && !(p->uid == 0 && u_arg == 1)) {
        i_result = -EPERM;
    } else {
        t->i_session = p->i_sid;
        t->i_pgrp = p->pgid;
    }
    spin_unlock_irq(&tty_lock, u_flags);

    /* and a console's, the kernel's own lines about it appear on it */
    struct thread *p_me = sched_current();
    if (i_result == 0 && p_me != NULL && t->p_end == &console_end) {
        p_me->console = t->u_console;
    }
    return i_result;
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
    uint64_t u_flags = spin_lock_irq(&tty_lock);
    int64_t i_result = -EPERM;
    if (ctty_of(current_pid()) != t) {
        i_result = -ENOTTY;
    } else if (p_member != NULL && p_member->i_sid == t->i_session) {
        t->i_pgrp = i_pgrp;
        i_result = 0;
    }
    spin_unlock_irq(&tty_lock, u_flags);
    return i_result;
}

/* the caller's session lets go of its terminal */
static int64_t let_go(struct tty *t)
{
    int64_t i_why = check_own(t);
    if (i_why < 0) {
        return i_why;
    }
    const struct process *p = process_find(current_pid());
    uint64_t u_flags = spin_lock_irq(&tty_lock);
    if (p != NULL && p->i_sid == p->pid) {
        t->i_session = 0;
        t->i_pgrp = TTY_NOBODY;
    }
    spin_unlock_irq(&tty_lock, u_flags);
    return 0;
}

static int64_t put_int(uint64_t u_to, int32_t i_value)
{
    return (put_user_i32(u_to, i_value) == 0) ? 0 : -EFAULT;
}

static int64_t tty_ioctl(struct file *p_file, uint32_t u_cmd, uint64_t u_arg)
{
    struct tty *t = p_file->p_private;
    struct termios termios;
    int64_t i_why;

    switch (u_cmd) {
    case TCGETS: {
        uint64_t u_flags = spin_lock_irq(&tty_lock);
        termios = t->termios;
        spin_unlock_irq(&tty_lock, u_flags);
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
    case TIOCGWINSZ: {
        size_t sz_cols = 0, sz_rows = 0, sz_width = 0, sz_height = 0;
        console_size(&sz_cols, &sz_rows, &sz_width, &sz_height);
        /*
         * a machine with no framebuffer answers zero, and a program
         * that believed it would divide by it. eighty by twenty-four is
         * what every terminal has been since the vt100
         */
        if (sz_cols == 0 || sz_rows == 0) {
            sz_cols = 80;
            sz_rows = 24;
        }
        struct winsize size = {
            .ws_row = (uint16_t)sz_rows, .ws_col = (uint16_t)sz_cols,
            .ws_xpixel = (uint16_t)sz_width, .ws_ypixel = (uint16_t)sz_height,
        };
        return (copy_to_user(u_arg, &size, sizeof size) == 0) ? 0 : -EFAULT;
    }
    case TIOCSWINSZ:
        /* a console is the size of the screen, which nothing here changes */
        return -EINVAL;
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
        drain(t);
        uint64_t u_flags = spin_lock_irq(&tty_lock);
        int32_t i_count = (int32_t)t->sz_ready;
        spin_unlock_irq(&tty_lock, u_flags);
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

static const struct file_operations tty_ops = {
    .read = tty_read,
    .write = tty_write,
    .poll = tty_poll,
    .ioctl = tty_ioctl,
};

struct file *tty_open(unsigned u_console, uint32_t u_mode)
{
    if (u_console >= VCONSOLE_COUNT) {
        return NULL;
    }
    struct file *p_file = file_alloc(&tty_ops, u_mode, FILE_TYPE_TTY);
    if (p_file == NULL) {
        return NULL;
    }
    uint64_t u_flags = spin_lock_irq(&tty_lock);
    p_file->p_private = tty_at(u_console);
    spin_unlock_irq(&tty_lock, u_flags);
    p_file->u_rdev = (TTY_MAJOR << 8) | (u_console + 1);
    return p_file;
}

struct file *tty_open_own(uint32_t u_mode)
{
    uint64_t u_flags = spin_lock_irq(&tty_lock);
    const struct tty *t = ctty_of(current_pid());
    unsigned u_console = (t != NULL) ? t->u_console : VCONSOLE_COUNT;
    spin_unlock_irq(&tty_lock, u_flags);
    return tty_open(u_console, u_mode);
}

int tty_foreground(unsigned u_console)
{
    uint64_t u_flags = spin_lock_irq(&tty_lock);
    struct tty *t = tty_at(u_console);
    settle(t);
    int i_pgrp = t->i_pgrp;
    spin_unlock_irq(&tty_lock, u_flags);
    return i_pgrp;
}
