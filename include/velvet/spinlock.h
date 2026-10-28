// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/spinlock.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * locks, at last.
 */

/* the design notes for spinlock.h are in docs/subsystems/mm.rst */

#ifndef VELVET_SPINLOCK_H
#define VELVET_SPINLOCK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * locks, at last. this kernel spent a long time using `cli` as mutual
 * exclusion, which was correct for one core, since the only thing that
 * could interrupt a critical section was an interrupt. it stopped being
 * correct the moment a second core could run kernel code, and every one of
 * those call sites was quietly reclassified by a change that did not touch
 * any of them.
 *
 * so a real lock, and both halves of the problem at once: a critical
 * section usually needs protecting from this core's own interrupts and
 * from the other cores, and needs both or neither. spin_lock_irq does both
 * and hands back the flags, so it drops into the shape irq_save and
 * irq_restore already had.
 *
 * two locks taken in opposite orders by two cores is a machine that stops
 * with no fault and nothing printed, and the only defence is a rank every
 * lock declares and never violates: a lock may only be taken while holding
 * locks of lower rank. it is checked, not hoped for
 */

/* the order is not invented, it is read off the call graph. */
enum lock_rank {
    /*
     * a pipe is the lowest thing there is: it wakes threads and does
     * nothing else at all, so it may be held while reaching up to the
     * scheduler and there is nothing beneath it to reach down to
     */
    LOCK_RANK_PIPE = 1,
    LOCK_RANK_DEVICE,           /* tty, input, pci, the disk, ramfs */

    LOCK_RANK_CLOCK,
    LOCK_RANK_SCHED,            /* the run queue */
    LOCK_RANK_PROCESS,          /* the process table, which sched reaches into */
    LOCK_RANK_HEAP,             /* slab, and kmalloc above it */
    LOCK_RANK_PMM,              /* which the heap calls into, never the reverse */
    LOCK_RANK_PRINT,            /* anything may print; printing takes nothing */
};

struct spinlock {
    volatile uint32_t held;
    const char *name;
    enum lock_rank rank;

    volatile uint32_t owner;

    volatile uint64_t contended;

    bool warned;                /* an out-of-order take, reported once */

    volatile uint32_t listed;
};

#define SPINLOCK(nm, rk) { 0, nm, rk, ~0u, 0, false, 0 }

void spin_init(struct spinlock *l, const char *name, enum lock_rank rank);

/*
 * the workhorse: interrupts off *and* the lock held, because a section
 * that needs one almost always needs the other. returns the flags to
 * hand back
 */
uint64_t spin_lock_irq(struct spinlock *l);
void spin_unlock_irq(struct spinlock *l, uint64_t flags);

/* when interrupts are already off, or cannot matter */
void spin_lock(struct spinlock *l);
void spin_unlock(struct spinlock *l);

void spin_abandon_all(void);

size_t spin_count(void);
const struct spinlock *spin_at(size_t index);

#endif
