// SPDX-License-Identifier: GPL-2.0-only
/*
 * sched/thread.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * threads: creation, stacks, and exit.
 */

#include <velvet/thread.h>
#include <velvet/sched.h>
#include <velvet/pmm.h>
#include <velvet/vmm.h>
#include <velvet/addrspace.h>
#include <velvet/kmalloc.h>
#include <velvet/slab.h>
#include <asm/mmu.h>
#include <asm/context.h>
#include <velvet/kprintf.h>
#include <velvet/string.h>
#include <asm/irq.h>
#include <velvet/spinlock.h>
#include <velvet/process.h>
#include <velvet/pipe.h>
#include <velvet/netif.h>
#include <velvet/pit.h>

static int next_id = 1;     /* 0 belongs to the boot thread */

const char *thread_state_name(enum thread_state s)
{
    switch (s) {
    case THREAD_READY:    return "ready";
    case THREAD_RUNNING:  return "running";
    case THREAD_SLEEPING: return "sleeping";
    case THREAD_BLOCKED:  return "blocked";
    case THREAD_DEAD:     return "dead";
    default:              return "???";
    }
}

void thread_set_name(struct thread *t, const char *name)
{
    size_t n = 0;
    while (name[n] && n < THREAD_NAME_MAX - 1) {
        t->name[n] = name[n];
        n++;
    }
    t->name[n] = '\0';
}

/* where every new thread opens its eyes. */
static void thread_bootstrap(void)
{
    /*
     * the kernel was switched to with the scheduler's lock held, and the release
     * for it is on the stack of whoever switched to the kernel, a stack the kernel will
     * never return to. so it is the kernel's to give back
     */
    sched_first_run();

    /*
     * the kernel inherited IF=0 from whoever switched to the kernel, because switching
     * happens with interrupts off. a preempted thread would get its
     * flags back from the iretq it eventually returns through, and a
     * yielding one from irq_restore, but the kernel has no such history to
     * return through. so the kernel let interrupts back in itself.
     * forget this line and the first thread you spawn quietly kills
     * preemption for the whole system
     */
    irq_enable();

    struct thread *me = sched_current();
    me->entry(me->arg);
    thread_exit(0);     /* a thread that simply returned did fine */
}

/*
 * threads are all exactly the same size and get created and destroyed
 * constantly, which is precisely what an object cache is for
 */
/* the thread ids, which must not be handed out twice */
static struct spinlock thread_lock = SPINLOCK("thread", LOCK_RANK_SCHED);

static struct slab_cache thread_cache;

static struct thread *create(const char *name, void (*entry)(void *),
                             void *arg, bool parked);

struct thread *thread_create(const char *name, void (*entry)(void *), void *arg)
{
    return create(name, entry, arg, false);
}

struct thread *thread_create_parked(const char *name, void (*entry)(void *),
                                    void *arg)
{
    return create(name, entry, arg, true);
}

static struct thread *create(const char *name, void (*entry)(void *),
                             void *arg, bool parked)
{
    slab_cache_init(&thread_cache, "thread", sizeof(struct thread));

    struct thread *t = slab_alloc(&thread_cache);
    if (t == NULL) {
        return NULL;
    }

    /* one extra page at the bottom, which the kernel then unmap. */
    uint64_t phys = pmm_alloc_pages(THREAD_STACK_PAGES + 1);
    if (phys == 0) {
        slab_free(t);
        return NULL;
    }

    t->stack_phys  = phys;
    t->stack_pages = THREAD_STACK_PAGES + 1;

    t->state       = parked ? THREAD_BLOCKED : THREAD_READY;
    t->on_cpu      = -1;    /* ready is not the same as running */
    t->wake_at     = 0;
    t->cpu_ticks   = 0;
    t->entry       = entry;
    t->arg         = arg;
    t->id          = next_id++;

    /* whichever console the thread that made this one belongs to. */
    {
        struct thread *maker = sched_current();
        t->console = (maker != NULL) ? maker->console : 0;
    }
    t->next        = NULL;
    t->wait_next   = NULL;
    t->from_heap   = true;
    t->space       = NULL;
    t->waiting_on  = NULL;
    t->pid         = 0;

    thread_set_name(t, name);

    /*
     * punch the hole. the vmm may not exist yet if somebody creates a
     * thread before vmm_init, in which case the stack is merely
     * unguarded rather than broken
     */
    uint64_t guard = (uint64_t)pmm_phys_to_virt(phys);
    if (vmm_kernel_pml4() != 0) {
        vmm_unmap_page(vmm_kernel_pml4(), guard);
        vmm_flush_page(guard);

        /*
         * and every other core, which is still holding the translation
         * the kernel just took away and has no way of noticing
         */
        mmu_shootdown();
    }

    /*
     * and a stack that looks like a thread already parked inside
     * switch_context, waiting to be resumed into thread_bootstrap.
     *
     * this file used to build that frame by hand, six zeroes with a
     * comment naming each register. it was wrong in a way nothing could
     * see until there was a second architecture to be wrong on: aarch64
     * has ten callee-saved registers and returns through one of them
     * rather than through a slot on the stack.
     *
     * stack top is page aligned, so whatever the architecture puts
     * there lands 16-aligned and bootstrap gets the alignment the abi
     * promises it
     */
    uint8_t *stack = pmm_phys_to_virt(phys + PAGE_SIZE);   /* past the guard */
    uint64_t top = (uint64_t)(stack + THREAD_STACK_PAGES * PAGE_SIZE);

    t->sp = context_make_stack(top, thread_bootstrap);

    sched_add(t);
    return t;
}

void thread_exit(int code)
{
    struct thread *me = sched_current();

    /*
     * tell the process table before the thread goes, because the
     * process is what a parent will still be able to ask
     */
    bool narrate = true;
    if (me->pid != 0) {
        narrate = process_announces(me->pid);
        process_exited(me->pid, code, pit_uptime_ms());

        /* let go of both ends of whatever pipeline this was part of. */
        pipe_release_for(me->pid);
        net_socket_close_all(me->pid);
        net_tcp_close_all(me->pid);
    }

    /* a kernel thread always says so, `summon` exists to be watched. */
    if (narrate) {
        kprintf("[%s] hath returned to the sea of souls\n", me->name);
    }

    uint64_t flags = spin_lock_irq(&thread_lock);
    me->state = THREAD_DEAD;
    spin_unlock_irq(&thread_lock, flags);

    /* the scheduler will never pick a dead thread, so this yield is a one way door. */
    for (;;) {
        sched_yield();
    }
}

/* give a dead thread's stack back. */
void thread_free_stack(struct thread *t)
{
    /*
     * the address space owns the program's pages and its user stack, so
     * letting it go reclaims all of them at once
     */
    if (t->space != NULL) {
        addrspace_destroy(t->space);
        t->space = NULL;
    }
    if (t->stack_phys == 0) {
        return;     /* the boot thread's stack came from the bootloader, not the kernel */
    }

    uint64_t guard = (uint64_t)pmm_phys_to_virt(t->stack_phys);
    if (vmm_kernel_pml4() != 0) {
        vmm_map_range(vmm_kernel_pml4(), guard, t->stack_phys, PAGE_SIZE,
                      PTE_WRITE | vmm_nx());
        vmm_flush_page(guard);
        mmu_shootdown();
    }

    pmm_free_pages(t->stack_phys, t->stack_pages);
}

/*
 * the boot thread is a global that was never allocated, so it must not
 * be handed to any allocator. keeping that check here means the
 * scheduler does not have to know how a thread was made
 */
void thread_free(struct thread *t)
{
    if (t == NULL || !t->from_heap) {
        return;
    }
    slab_free(t);
}
