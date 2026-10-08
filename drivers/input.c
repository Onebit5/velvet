// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/input.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * where every source of typing meets.
 */

#include "drivers/input.h"
#include "arch/irq.h"
#include "sched/spinlock.h"
#include "sched/sched.h"
#include "drivers/tty.h"
#include "drivers/console.h"

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
 * nothing is filling. 16 bits wide because arrow keys dont fit in a
 * char
 */
#define INPUT_BUF_SIZE 256

/*
 * the ring the keyboard and the serial port both push into, from
 * interrupt handlers, on whichever core takes the interrupt
 */
static struct spinlock input_lock = SPINLOCK("input", LOCK_RANK_DEVICE);

struct ring {
    uint16_t buf[INPUT_BUF_SIZE];
    volatile unsigned int head, tail;
    struct waitq waiters;
};

static struct ring rings[VCONSOLE_COUNT];

/* whose keys these are. */
static struct ring *mine(void)
{
    unsigned n = tty_my_console();
    return &rings[n < VCONSOLE_COUNT ? n : 0];
}

/*
 * XXX: this pushes without input_lock, and the note at the top of the
 * file names the two producers that make that wrong: the keyboard on irq1
 * and the serial port on irq4, each arriving on whichever core the
 * interrupt lands on. two of them can read head together, write the same
 * slot and store the same successor, so a keystroke is lost and the ring's
 * head stops describing what is in it. one core cannot do this to itself,
 * which is why it has always looked right. take the lock here as well, or
 * give each source a ring of its own.
 */
void input_push(int key)
{
    /* the tty gets first refusal. */
    if (tty_intercept(key)) {
        return;
    }

    /* into the ring of whichever console is being looked at. */
    struct ring *r = &rings[console_active() < VCONSOLE_COUNT
                            ? console_active() : 0];

    unsigned int next = (r->head + 1) % INPUT_BUF_SIZE;
    if (next == r->tail) {
        return;     /* buffer full, the keystroke returns to the sea of souls */
    }
    r->buf[r->head] = (uint16_t)key;
    r->head = next;

    waitq_wake_all(&r->waiters);
}

/* the raw pop, no locking. callers below hold interrupts down */
static int buf_pop(struct ring *r)
{
    if (r->tail == r->head) {
        return -1;
    }
    int c = r->buf[r->tail];
    r->tail = (r->tail + 1) % INPUT_BUF_SIZE;
    return c;
}

int input_getchar(void)
{
    uint64_t flags = spin_lock_irq(&input_lock);
    int c = buf_pop(mine());
    spin_unlock_irq(&input_lock, flags);
    return c;
}

int input_peek(void)
{
    uint64_t flags = spin_lock_irq(&input_lock);
    struct ring *r = mine();
    int c = (r->tail == r->head) ? -1 : r->buf[r->tail];
    spin_unlock_irq(&input_lock, flags);
    return c;
}

bool input_haskey(void)
{
    struct ring *r = mine();
    return r->tail != r->head;
}

int input_getchar_blocking(void)
{
    struct ring *r = mine();

    for (;;) {
        uint64_t flags = spin_lock_irq(&input_lock);
        int c = buf_pop(r);
        if (c >= 0) {
            spin_unlock_irq(&input_lock, flags);
            return c;
        }

        /*
         * nothing there. go on the queue while the kernel still hold the lock, so
         * a key arriving the instant the kernel let go finds the kernel on it, that is
         * what closes the gap between looking and sleeping
         */
        waitq_enqueue(&r->waiters);
        spin_unlock_irq(&input_lock, flags);

        /* and only now stop running. */
        waitq_sleep();
    }
}
