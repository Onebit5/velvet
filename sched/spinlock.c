// SPDX-License-Identifier: GPL-2.0-only
/*
 * sched/spinlock.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * spinlocks.
 */

#include <velvet/spinlock.h>
#include <asm/irq.h>
#include <asm/cpu.h>
#include <velvet/panic.h>
#include <velvet/kprintf.h>

/*
 * which core this is used to be answered here, twice, behind an #ifdef
 *, the apic on a real machine and a thread-local counter under the
 * host tests. that is the shape of every arch leak: a portable file
 * carrying two answers because nowhere else was willing to hold them.
 * <asm/cpu.h> holds them now and this file asks one question
 */
static uint32_t whoami(void)
{
    return cpu_id();
}

/* a core that spins forever is a machine that has stopped with nothing to say. */
#define SPIN_LIMIT 400000000ull

/* what each core is holding, so the ordering rule can be checked rather than hoped for. */
#define MAX_CORES 32
static volatile uint32_t held_rank[MAX_CORES];

/* every lock, so the ones that are actually contended can be found */
#define MAX_LOCKS 32
static struct spinlock *registry[MAX_LOCKS];
static volatile uint32_t registered;

/* put a lock on the list, once, whoever gets there first */
static void remember(struct spinlock *l)
{
    if (__atomic_exchange_n(&l->listed, 1, __ATOMIC_ACQ_REL) != 0) {
        return;
    }
    uint32_t slot = __atomic_fetch_add(&registered, 1, __ATOMIC_SEQ_CST);
    if (slot < MAX_LOCKS) {
        registry[slot] = l;
    }
}

void spin_init(struct spinlock *l, const char *name, enum lock_rank rank)
{
    l->held = 0;
    l->name = name;
    l->rank = rank;
    l->owner = ~0u;
    l->contended = 0;
    l->warned = false;
    l->listed = 0;
    remember(l);
}

/* set when the machine is on its way down. */
static volatile bool abandoned;

void spin_abandon_all(void)
{
    abandoned = true;
}

static void take(struct spinlock *l)
{
    if (abandoned) {
        return;
    }
    remember(l);
    uint32_t me = whoami();

    /*
     * a lock cannot be taken twice by the same core: the second attempt
     * waits for a release that only the waiting code could perform
     */
    if (l->held && l->owner == me) {
        panic("spinlock: cpu %u is taking %s, which it already holds",
              me, l->name != NULL ? l->name : "an unnamed lock");
    }

    /*
     * and never out of order, which is the deadlock that leaves no
     * evidence at all: two cores, two locks, opposite orders, and a
     * machine that simply stops.
     *
     * this complains rather than panics, and the difference is the whole
     * point of doing the audit a version early. with one core running
     * kernel code the ordering cannot actually deadlock anything, so a
     * rank the kernel got wrong should cost a line of text, not a working
     * machine. it becomes fatal when it can bite
     */
    if (me < MAX_CORES && l->rank <= held_rank[me] && !l->warned) {
        l->warned = true;
        kprintf("[locks] %s (rank %d) taken while holding rank %d. that "
                "order will deadlock once more than one core runs\n",
                l->name, (int)l->rank, (int)held_rank[me]);
    }

    for (uint64_t spin = 0; ; spin++) {
        if (__atomic_exchange_n(&l->held, 1, __ATOMIC_ACQUIRE) == 0) {
            break;
        }
        if (spin == 0) {
            __atomic_fetch_add(&l->contended, 1, __ATOMIC_RELAXED);
        }
        if (spin > SPIN_LIMIT) {
            panic("spinlock: cpu %u waited out %s, held by cpu %u",
                  me, l->name, l->owner);
        }
        /* tell the processor this is a spin loop. */
        cpu_relax();
    }

    l->owner = me;
    if (me < MAX_CORES) {
        held_rank[me] = (uint32_t)l->rank;
    }
}

static void give_back(struct spinlock *l)
{
    if (abandoned) {
        return;
    }
    uint32_t me = whoami();

    if (!l->held) {
        panic("spinlock: %s released by cpu %u without being held", l->name, me);
    }
    if (l->owner != me) {
        panic("spinlock: %s released by cpu %u but held by cpu %u",
              l->name, me, l->owner);
    }

    /*
     * FIXME: this clears the whole ordering state rather than the rank
     * of the lock being released. held_rank[] means the highest rank
     * this core holds, so after any release inside a nested section it
     * reads zero and every later take looks legal. a lock taken while a
     * higher ranked one is still held is then never reported, which is
     * exactly the ordering this check exists to catch. restore the
     * enclosing rank instead: keep a per core stack of held ranks, or
     * rescan the registry for the highest rank this core still holds.
     */
    if (me < MAX_CORES) {
        held_rank[me] = 0;
    }
    l->owner = ~0u;
    __atomic_store_n(&l->held, 0, __ATOMIC_RELEASE);
}

uint64_t spin_lock_irq(struct spinlock *l)
{
    /*
     * interrupts first. taking the lock and *then* turning them off
     * leaves a window where this core's own handler can arrive wanting
     * the same lock, and wait for a release that cannot happen until it
     * returns, which is a deadlock against nobody but itself
     */
    uint64_t flags = irq_save();
    take(l);
    return flags;
}

void spin_unlock_irq(struct spinlock *l, uint64_t flags)
{
    give_back(l);
    irq_restore(flags);
}

void spin_lock(struct spinlock *l)
{
    take(l);
}
void spin_unlock(struct spinlock *l)
{
    give_back(l);
}

size_t spin_count(void)
{
    uint32_t n = registered;
    return n < MAX_LOCKS ? n : MAX_LOCKS;
}

const struct spinlock *spin_at(size_t index)
{
    return index < spin_count() ? registry[index] : NULL;
}
