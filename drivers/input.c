// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/input.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * where every source of typing meets.
 */

#include <velvet/input.h>
#include <asm/irq.h>
#include <velvet/spinlock.h>
#include <velvet/sched.h>
#include <velvet/tty.h>
#include <velvet/console.h>
#include <velvet/thread.h>
#include <velvet/process.h>

/*
 * one ring per console, and that is not tidiness, it is the only
 * arrangement in which the question has one answer.
 *
 * with a single ring, four shells all block on it and a keypress wakes
 * every one of them; whichever the scheduler happens to pick takes the
 * character. gating *afterwards*, checking whether the reader is on
 * the console being looked at, and putting the key back if not, is a
 * race with extra steps, because by then somebody has already been
 * woken and something has already been consumed.
 *
 * so a key goes into the ring of whichever console is on the screen,
 * and a reader only ever looks at its own. a shell on a console nobody
 * is watching is not competing for keys; it is asleep on a queue that
 * nothing is filling.
 *
 * the ring holds bytes, what a terminal sends: a key that is not a
 * character goes in as the sequence one sends for it, so a program
 * reads the keyboard and a serial line the same way, and a serial
 * line's own sequences pass through untouched
 */
#define INPUT_BUF_SIZE 256

/*
 * the ring the keyboard, the serial port and the mouse's paste all push
 * into, from interrupt handlers, on whichever core takes the interrupt
 */
static struct spinlock input_lock = SPINLOCK("input", LOCK_RANK_DEVICE);

struct ring {
    uint8_t buf[INPUT_BUF_SIZE];
    unsigned int head, tail;
    struct waitq waiters;
};

static struct ring rings[VCONSOLE_COUNT];

static struct ring *ring_of(unsigned u_console)
{
    return &rings[u_console < VCONSOLE_COUNT ? u_console : 0];
}

/*
 * the sequence a console sends for each key that is not a character,
 * the ones every terminal description of a pc console expects
 */
static const char *sequence_for(int i_key)
{
    switch (i_key) {
    case KEY_UP:     return "\x1b[A";
    case KEY_DOWN:   return "\x1b[B";
    case KEY_RIGHT:  return "\x1b[C";
    case KEY_LEFT:   return "\x1b[D";
    case KEY_HOME:   return "\x1b[1~";
    case KEY_DELETE: return "\x1b[3~";
    case KEY_END:    return "\x1b[4~";
    case KEY_PGUP:   return "\x1b[5~";
    case KEY_PGDN:   return "\x1b[6~";
    default:         return NULL;
    }
}

/* the lock is held. a byte that does not fit is lost, as typing ahead is */
static void put_byte(struct ring *r, uint8_t u_byte)
{
    unsigned int next = (r->head + 1) % INPUT_BUF_SIZE;
    if (next == r->tail) {
        return;
    }
    r->buf[r->head] = u_byte;
    r->head = next;
}

void input_push(int key)
{
    /* the tty gets first refusal. */
    if (tty_intercept(key)) {
        return;
    }

    char arr_one[2] = { (char)key, '\0' };
    const char *str_bytes = (key >= 0 && key < 0x100) ? arr_one
                                                       : sequence_for(key);
    if (str_bytes == NULL) {
        return;
    }
    size_t sz_len = (str_bytes == arr_one) ? 1 : 0;
    while (str_bytes != arr_one && str_bytes[sz_len] != '\0') {
        sz_len++;
    }

    /* into the ring of whichever console is being looked at. */
    struct ring *r = ring_of(console_active());
    uint64_t u_flags = spin_lock_irq(&input_lock);
    for (size_t sz_at = 0; sz_at < sz_len; sz_at++) {
        put_byte(r, (uint8_t)str_bytes[sz_at]);
    }
    spin_unlock_irq(&input_lock, u_flags);

    waitq_wake_all(&r->waiters);
}

int input_take(unsigned u_console)
{
    struct ring *r = ring_of(u_console);
    uint64_t u_flags = spin_lock_irq(&input_lock);
    int i_byte = -1;
    if (r->tail != r->head) {
        i_byte = r->buf[r->tail];
        r->tail = (r->tail + 1) % INPUT_BUF_SIZE;
    }
    spin_unlock_irq(&input_lock, u_flags);
    return i_byte;
}

bool input_waiting(unsigned u_console)
{
    struct ring *r = ring_of(u_console);
    uint64_t u_flags = spin_lock_irq(&input_lock);
    bool b_any = r->tail != r->head;
    spin_unlock_irq(&input_lock, u_flags);
    return b_any;
}

void input_flush(unsigned u_console)
{
    struct ring *r = ring_of(u_console);
    uint64_t u_flags = spin_lock_irq(&input_lock);
    r->tail = r->head;
    spin_unlock_irq(&input_lock, u_flags);
}

bool input_wait(unsigned u_console)
{
    struct ring *r = ring_of(u_console);
    struct thread *p_me = sched_current();
    int i_pid = (p_me != NULL) ? p_me->pid : 0;

    for (;;) {
        uint64_t flags = spin_lock_irq(&input_lock);
        if (r->tail != r->head) {
            spin_unlock_irq(&input_lock, flags);
            return true;
        }

        /*
         * nothing there and a signal waiting, which is what woke a
         * reader that had no key: the wait ends with no key taken, so
         * the next one typed is there for whoever reads next
         */
        if (i_pid != 0 && process_interrupt_pending(i_pid)) {
            spin_unlock_irq(&input_lock, flags);
            return false;
        }

        /*
         * on the queue while the lock is still held, so a key arriving
         * the instant it is let go finds this thread on it, which is what
         * closes the gap between looking and sleeping
         */
        waitq_enqueue(&r->waiters);
        spin_unlock_irq(&input_lock, flags);

        /* and only now stop running. */
        waitq_sleep();
    }
}
