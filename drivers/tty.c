// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/tty.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the terminal: foreground process, line discipline, ctrl+c.
 */

#include <velvet/tty.h>
#include <velvet/input.h>
#include <asm/irq.h>
#include <velvet/spinlock.h>
#include <velvet/kprintf.h>
#include <velvet/process.h>
#include <velvet/sched.h>
#include <velvet/thread.h>
#include <velvet/console.h>
#include <velvet/file.h>
#include <uapi/velvet/errno.h>

/* which process is at the front of the terminal */
static struct spinlock tty_lock = SPINLOCK("tty", LOCK_RANK_DEVICE);

/* one of each, per console. */
static int foreground[VCONSOLE_COUNT];

/*
 * the program that took each console as its terminal, or 0 while no
 * program has, which is a console /init has not handed out. it decides
 * who may put a group in front
 */
static int controller[VCONSOLE_COUNT];

unsigned tty_my_console(void)
{
    struct thread *t = sched_current();
    unsigned n = (t != NULL) ? t->console : console_active();
    return (n < VCONSOLE_COUNT) ? n : 0;
}

void tty_set_foreground(int pgid)
{
    unsigned n = tty_my_console();
    uint64_t flags = spin_lock_irq(&tty_lock);
    foreground[n] = pgid;
    spin_unlock_irq(&tty_lock, flags);
}

/*
 * a process may read the keyboard when it is at the front of its own
 * console, on the screen or not. a key goes into the ring of the console
 * being looked at and a reader only takes from its own, so one on a
 * hidden console waits for keys that arrive once somebody switches to
 * it, and can never take another console's
 */
bool tty_is_current(int pid)
{
    return process_pgid(pid) == foreground[tty_my_console()];
}

int tty_foreground(void)
{
    return foreground[tty_my_console()];
}

bool tty_attach(int i_pid, unsigned u_console)
{
    struct thread *p_me = sched_current();
    if (p_me == NULL || u_console >= VCONSOLE_COUNT) {
        return false;
    }
    int i_pgid = process_pgid(i_pid);

    p_me->console = u_console;
    uint64_t u_flags = spin_lock_irq(&tty_lock);
    controller[u_console] = i_pid;
    foreground[u_console] = i_pgid;
    spin_unlock_irq(&tty_lock, u_flags);
    return true;
}

bool tty_hand_to(int i_caller, int i_pgid)
{
    int i_member = 0;
    int i_uid = process_uid(i_caller);
    int i_caller_pgid = process_pgid(i_caller);

    if (process_group_pids(i_pgid, &i_member, 1) == 0
        || (i_uid != 0 && process_uid(i_member) != i_uid)) {
        return false;
    }

    unsigned u_console = tty_my_console();
    uint64_t u_flags = spin_lock_irq(&tty_lock);
    bool b_may = (controller[u_console] != 0
                  && controller[u_console] == i_caller)
                 || i_caller_pgid == foreground[u_console];
    if (b_may) {
        foreground[u_console] = i_pgid;
    }
    spin_unlock_irq(&tty_lock, u_flags);
    return b_may;
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

    /*
     * XXX: the mode read here belongs to the console of whichever thread
     * the interrupt interrupted, because tty_get_mode() asks
     * tty_my_console(), which asks the scheduler. the signal below is
     * delivered to the *active* console's foreground group. with a second
     * console running, a ctrl+c typed at the screen can be judged by the
     * mode of a console nobody is looking at: a raw editor on console 0
     * and a cooked shell on console 2 is enough to turn a byte into a
     * signal. the decision and the delivery have to come from the same
     * console, and it is this one.
     */
    /* and whether ctrl+c and ctrl+z are signals at all is the *mode's* decision. */
    struct term_mode mode = tty_get_mode();
    if (!mode.signals) {
        return false;       /* it is an ordinary byte, and the program's */
    }

    if (key != KEY_CTRL_C && key != KEY_CTRL_Z) {
        return false;
    }

    /*
     * an interrupt arrives from the keyboard, so it is aimed at
     * whichever console is being looked at, not at whichever console
     * the interrupted thread happened to be on
     */
    unsigned console = console_active();
    int pgid = foreground[console];
    if (pgid == TTY_NOBODY) {
        /* nobody to signal: ctrl+c goes to the line, ctrl+z nowhere */
        return key == KEY_CTRL_Z;
    }

    /*
     * a program in front: a signal to its group, and that is all. what
     * becomes of a job is the business of whoever started it
     */
    process_signal_group(pgid, key == KEY_CTRL_C ? SIGINT : SIGTSTP);
    return true;
}

/*
 * one per console, for the reason tty.h gives: a program that dies in
 * raw mode must leave something the shell can put right
 */
static struct term_mode modes[4] = {
    TERM_COOKED, TERM_COOKED, TERM_COOKED, TERM_COOKED
};

void tty_set_mode(const struct term_mode *m)
{
    unsigned c = tty_my_console();
    if (c < 4) {
        modes[c] = *m;
    }
}

struct term_mode tty_get_mode(void)
{
    unsigned c = tty_my_console();
    /*
     * NOTE: the cast below is redundant, TERM_COOKED already being a compound
     * literal of this type. sparse reports it as a non-scalar cast.
     */
    return (c < 4) ? modes[c] : (struct term_mode)TERM_COOKED;
}

void tty_restore_mode(void)
{
    unsigned c = tty_my_console();
    if (c < 4) {
        modes[c] = (struct term_mode)TERM_COOKED;
    }
}

int64_t tty_read_line(int pid, char *buf, uint64_t len)
{
    /* only the job at the front of the console being *looked at* may read. */
    if (len == 0 || !tty_is_current(pid)) {
        return -EIO;
    }
    if (process_take_interrupt(pid)) {
        return -EINTR;          /* interrupted before it even began */
    }

    uint64_t n = 0;
    for (;;) {
        int c = input_getchar_blocking();

        /* woken by an interrupt rather than a key, and nothing taken */
        if (c < 0) {
            process_take_interrupt(pid);
            kprintf("\n");
            return -EINTR;
        }

        /*
         * what this key means is decided by drivers/termios.c, which
         * knows nothing about buffers or screens, so the rule that
         * ctrl+c is a signal in one mode and a byte in another is one
         * place, and is checked without a machine
         */
        struct term_mode mode = tty_get_mode();
        enum term_action what = term_input(&mode, c, n);

        /* each action does its own echoing. */
        switch (what) {
        case TERM_DELIVER:
            if (mode.echo) {
                kprintf("\n");
            }
            if (c == '\n' && n < len) {
                buf[n++] = '\n';   /* a read gives you the newline too */
            }
            return (int64_t)n;

        case TERM_END:
            return 0;              /* end of input, which is not an error */

        case TERM_ERASE:
            if (n > 0) {
                n--;
                if (mode.echo) {
                    kprintf("\b \b");   /* off the screen too */
                }
            }
            continue;

        case TERM_IGNORE:
            continue;

        case TERM_INTERRUPT:
        case TERM_SUSPEND:
            /*
             * tty_intercept has already turned these into signals
             * before the key ever reached here; arriving anyway means
             * the mode changed underneath, and dropping it is safer
             * than storing a byte nobody expects
             */
            continue;

        case TERM_KEEP:
            break;      /* the echo is below, with the store */
        }

        /*
         * raw mode hands back whatever has arrived rather than waiting
         * for a line that is never coming
         */
        if (mode.raw) {
            if (n < len) {
                buf[n++] = (char)c;
            }
            return (int64_t)n;
        }

        /*
         * arrows and other non-characters have no meaning in a line
         * this simple, and printing them would draw nonsense
         */
        if (c < ' ' || c > '~') {
            continue;
        }

        if (n + 1 >= len) {
            /*
             * the buffer is full. hand over what there is rather than
             * dropping keys silently or writing past the end
             */
            return (int64_t)n;
        }
        buf[n++] = (char)c;
        if (term_should_echo(&mode, c)) {
            kprintf("%c", (char)c); /* the echo, which is the whole point
                                     *, and which a mode may turn off,
                                     * for an editor drawing its own
                                     * screen or a prompt asking for a
                                     * word nobody should see */
        }
    }
}

/*
 * the console as a file: what a program the kernel starts finds on 0, 1
 * and 2. reads take a line from the keyboard through the line
 * discipline, writes go to the console of the thread writing. there is
 * one, held by the terminal itself, so its last reference never goes
 */
static int64_t tty_file_read(struct file *p_file, void *p_buf, size_t sz_len)
{
    (void)p_file;
    struct thread *p_me = sched_current();
    return tty_read_line((p_me != NULL) ? p_me->pid : 0, p_buf, sz_len);
}

static int64_t tty_file_write(struct file *p_file, const void *p_buf,
                              size_t sz_len)
{
    (void)p_file;
    kwrite(p_buf, sz_len);
    return (int64_t)sz_len;
}

static const struct file_operations console_ops = {
    .read = tty_file_read,
    .write = tty_file_write,
};

static struct file console_file = {
    .p_ops = &console_ops,
    .u_refs = 1,
    .u_mode = FILE_READ | FILE_WRITE,
    .e_type = FILE_TYPE_TTY,
};

struct file *tty_console_file(void)
{
    file_get(&console_file);
    return &console_file;
}
