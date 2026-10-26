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

/* the most one line may be, and the most typed ahead in raw mode */
#define TTY_BUF 4096

/* how long a timed raw read sleeps between looks */
#define TTY_SLICE_MS 10

struct tty {
    unsigned u_console;
    struct termios termios;

    int i_pgrp;                 /* the group in front */
    int i_session;              /* the session it is the terminal of */

    /*
     * what a read can have now: one whole line when canonical, whatever
     * has been typed when not. a canonical terminal takes no more keys
     * off the console until the line has been read, so a read never
     * runs two lines together
     */
    uint8_t arr_ready[TTY_BUF];
    size_t sz_ready;

    uint8_t arr_line[TTY_BUF];  /* the line being typed */
    size_t sz_line;
    bool b_eof;                 /* end of input: the next read is 0 */
};

static struct spinlock tty_lock = SPINLOCK("tty", LOCK_RANK_DEVICE);
static struct tty s_arr_ttys[VCONSOLE_COUNT];
static bool s_b_made;

/* the terminal on a console, made the first time any is asked for */
static struct tty *tty_at(unsigned u_console)
{
    if (!s_b_made) {
        for (unsigned u_at = 0; u_at < VCONSOLE_COUNT; u_at++) {
            s_arr_ttys[u_at].u_console = u_at;
            term_default(&s_arr_ttys[u_at].termios);
        }
        s_b_made = true;
    }
    return &s_arr_ttys[u_console < VCONSOLE_COUNT ? u_console : 0];
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

/*
 * bytes onto a terminal's console. what is printed goes to the console
 * of the thread printing it, so for as long as this takes the writer is
 * on the terminal's console, whichever one it was on before
 */
static void put_on(const struct tty *t, const char *p_bytes, size_t sz_len)
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
    bool b_flush = !(t->termios.c_lflag & NOFLSH);
    if (b_flush) {
        t->sz_line = 0;
        t->sz_ready = 0;
        t->b_eof = false;
    }
    char arr_echo[2];
    size_t sz_echo = term_echo(&t->termios, (uint8_t)key, arr_echo);
    put_on(t, arr_echo, sz_echo);
    spin_unlock_irq(&tty_lock, u_flags);

    if (b_flush) {
        input_flush(u_console);
    }
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
        uint8_t u_c = t->arr_line[t->sz_line - 1];
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
            put_on(t, "\b \b", 3);
        }
    }
}

/* the line complete, for a read to have. the lock is held */
static void deliver(struct tty *t)
{
    size_t sz_room = TTY_BUF - t->sz_ready;
    size_t sz_take = (t->sz_line < sz_room) ? t->sz_line : sz_room;
    memcpy(t->arr_ready + t->sz_ready, t->arr_line, sz_take);
    t->sz_ready += sz_take;
    t->sz_line = 0;
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

    if (!(p_t->c_lflag & ICANON)) {
        if (t->sz_ready < TTY_BUF) {
            t->arr_ready[t->sz_ready++] = u_c;
        }
        put_on(t, arr_echo, term_echo(p_t, u_c, arr_echo));
        return;
    }

    switch (term_input(p_t, u_c)) {
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
            put_on(t, "\n", (p_t->c_lflag & ECHO) ? 1 : 0);
        }
        return;
    case TERM_EOF:
        t->b_eof = (t->sz_line == 0);
        deliver(t);
        return;
    case TERM_DELIVER:
        /* the room kept for it is what makes the newline always fit */
        t->arr_line[t->sz_line++] = u_c;
        put_on(t, arr_echo, term_echo(p_t, u_c, arr_echo));
        deliver(t);
        return;
    case TERM_KEEP:
        if (t->sz_line < TTY_BUF - 1) {
            t->arr_line[t->sz_line++] = u_c;
            put_on(t, arr_echo, term_echo(p_t, u_c, arr_echo));
        }
        return;
    }
}

/*
 * whatever has been typed at the terminal's console, through the
 * discipline, as far as a read can use it: echoed as it is taken, which
 * is when somebody reads or asks whether there is anything to read
 */
static void drain(struct tty *t)
{
    for (;;) {
        uint64_t u_flags = spin_lock_irq(&tty_lock);
        bool b_full = (t->termios.c_lflag & ICANON)
                      ? (t->sz_ready > 0 || t->b_eof)
                      : (t->sz_ready == TTY_BUF);
        spin_unlock_irq(&tty_lock, u_flags);
        if (b_full) {
            return;
        }
        int i_byte = input_take(t->u_console);
        if (i_byte < 0) {
            return;
        }
        u_flags = spin_lock_irq(&tty_lock);
        take_byte(t, (uint8_t)i_byte);
        spin_unlock_irq(&tty_lock, u_flags);
    }
}

/* up to sz_len of what is ready, handed over and gone. the lock is held */
static size_t take_ready(struct tty *t, void *p_buf, size_t sz_len)
{
    size_t sz_take = (t->sz_ready < sz_len) ? t->sz_ready : sz_len;
    memcpy(p_buf, t->arr_ready, sz_take);
    memmove(t->arr_ready, t->arr_ready + sz_take, t->sz_ready - sz_take);
    t->sz_ready -= sz_take;
    return sz_take;
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
            if (t->sz_ready > 0) {
                i_got = (int64_t)take_ready(t, p_buf, sz_len);
            } else if (t->b_eof) {
                t->b_eof = false;
                i_got = 0;
            }
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
                i_got = (int64_t)take_ready(t, p_buf, sz_len);
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
        } else if (!input_wait(t->u_console)) {
            return -ERESTARTSYS;
        }
    }
}

/*
 * what is written goes to the console as it is. OPOST is not looked at:
 * the console starts a new line at the start, which is ONLCR, either way
 */
static int64_t tty_write(struct file *p_file, const void *p_buf,
                         size_t sz_len)
{
    put_on(p_file->p_private, p_buf, sz_len);
    return (int64_t)sz_len;
}

static uint32_t tty_poll(struct file *p_file)
{
    struct tty *t = p_file->p_private;
    drain(t);
    uint64_t u_flags = spin_lock_irq(&tty_lock);
    bool b_in = t->sz_ready > 0 || t->b_eof;
    spin_unlock_irq(&tty_lock, u_flags);
    return POLLOUT | (b_in ? POLLIN : 0);
}

/*
 * a new termios, and what changing it means for what is already typed:
 * a line half typed when ICANON goes off is what raw reads find first
 */
static void set_termios(struct tty *t, const struct termios *p_new,
                        bool b_flush)
{
    uint64_t u_flags = spin_lock_irq(&tty_lock);
    if ((t->termios.c_lflag & ICANON) && !(p_new->c_lflag & ICANON)) {
        deliver(t);
        t->b_eof = false;
    }
    if (b_flush) {
        t->sz_line = 0;
        t->sz_ready = 0;
        t->b_eof = false;
    }
    t->termios = *p_new;
    spin_unlock_irq(&tty_lock, u_flags);
    if (b_flush) {
        input_flush(t->u_console);
    }
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

    /* and the kernel's own lines about it appear where it is */
    struct thread *p_me = sched_current();
    if (i_result == 0 && p_me != NULL) {
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
