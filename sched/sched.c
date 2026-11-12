// SPDX-License-Identifier: GPL-2.0-only
/*
 * sched/sched.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the scheduler: the run queue, the switch, and reaping the dead.
 */

#include <velvet/sched.h>
#include <asm/cpu.h>
#include <velvet/thread.h>
#include <velvet/pit.h>
#include <velvet/pmm.h>
#include <velvet/addrspace.h>
#include <velvet/process.h>
#include <velvet/kmalloc.h>
#include <velvet/kprintf.h>
#include <velvet/panic.h>
#include <velvet/string.h>
#include <asm/cpu.h>
#include <asm/irq.h>
#include <velvet/spinlock.h>
#include <asm/context.h>
#include <velvet/pmm.h>
#include <velvet/addrspace.h>
#include <velvet/process.h>
#include <velvet/pipe.h>
#include <velvet/netif.h>
#include <velvet/backtrace.h>
#include <velvet/proc.h>
#include <velvet/loadavg.h>
#include <asm/uaccess.h>

/* how many ticks a thread gets before the kernel takes the cpu back. */
#define QUANTUM_TICKS 5

/* implemented in switch.asm */

/*
 * the run queue: a ring every core will eventually walk, and the
 * one place a thread can be in two states at once if nobody is
 * holding anything
 */
static struct spinlock sched_lock = SPINLOCK("sched", LOCK_RANK_SCHED);

/* the ring is shared: one queue that every core picks from, rather than a queue each. */
struct percpu {
    /*
     * not called `current`: there is a macro of that name just below,
     * so that the rest of this file reads as it always did, and a member
     * sharing the name would be rewritten out from under itself
     */
    struct thread *running;
    struct thread *idle;
    int quantum_left;
    bool scheduling;        /* has this core entered the scheduler yet */

    /* the thread this core switched away from and has not yet confirmed off the cpu. */
    struct thread *handoff;

    /*
     * what its ticks found it doing: a thread's work in ring 3, a
     * thread's in the kernel, or idle, which is the kernel too and
     * nobody's business, and how often it went from one thread to another
     */
    struct cpu_usage cpu;
    uint64_t u_idle;
    uint64_t u_switches;
};

static struct percpu percpu[CPU_MAX];

/* the ring itself, and any thread in it. `ring` is just a way in */
static struct thread *ring;

#define ME (&percpu[cpu_id()])
#define current (ME->running)

/* the thread that the loader handed the cpu to. */
static struct thread boot_thread;

struct thread *sched_current(void)
{
    return current;
}

/* kprintf's question, answered: the running thread's capture, if any. */
static struct kcapture *capture_of_current(void)
{
    struct thread *p_thread = current;
    if (p_thread == NULL || p_thread->u_in_interrupt != 0) {
        return NULL;
    }
    return p_thread->p_capture;
}

void sched_capture(struct kcapture *p_cap)
{
    struct thread *p_thread = current;
    if (p_thread != NULL) {
        p_thread->p_capture = p_cap;
    }
}

static void reap_dead(void);

static void idle_loop(void *arg)
{
    (void)arg;
    /* the lowest form of life in the system. */
    for (;;) {
        reap_dead();
        cpu_idle();
    }
}

uint64_t sched_quantum_ms(void)
{
    return QUANTUM_TICKS * (1000 / CLOCK_TICK_HZ);
}

void sched_add(struct thread *t)
{
    /* the ring, not `current`. */
    if (ring == NULL) {
        panic("sched_add before sched_init, the wheel hath no hub yet");
    }

    uint64_t flags = spin_lock_irq(&sched_lock);
    t->next = ring->next;
    ring->next = t;
    spin_unlock_irq(&sched_lock, flags);
}

/* free anything that has finished. */
/* dead threads, unlinked under the lock and freed outside it. */
static void reap_dead(void)
{
    struct thread *dead[8];
    int n = 0;

    uint64_t flags = spin_lock_irq(&sched_lock);

    struct thread *prev = current;
    struct thread *t = current->next;
    while (t != current && n < 8) {
        if (t->state == THREAD_DEAD && t->on_cpu < 0) {
            prev->next = t->next;
            dead[n++] = t;
            t = prev->next;
        } else {
            prev = t;
            t = t->next;
        }
    }
    ring = current;     /* whatever the kernel unlinked, this is still in the ring */

    spin_unlock_irq(&sched_lock, flags);

    for (int i = 0; i < n; i++) {
        /*
         * a thread that was *killed* never ran thread_exit, so this is
         * the only place its pipe ends get released. outside the lock,
         * because closing one wakes whoever is waiting on it
         */
        process_fds_release(dead[i]->pid);
        process_forget(dead[i]->pid);
        thread_free_stack(dead[i]);
        thread_free(dead[i]);   /* a no-op for the boot thread */
    }
}

static void unlink(struct waitq *q, struct thread *t);

/* the ones asleep until a tick, and those on a queue with a time limit */
static void wake_sleepers(void)
{
    uint64_t now = pit_ticks();
    struct thread *t = current;
    do {
        bool b_timed = t->state == THREAD_SLEEPING
                       || (t->state == THREAD_BLOCKED && t->wake_at != 0);
        if (b_timed && now >= t->wake_at) {
            if (t->waiting_on != NULL) {
                unlink(t->waiting_on, t);
            }
            t->state = THREAD_READY;
        }
        t = t->next;
    } while (t != current);
}

/* is this thread one of the idle threads, anybody's, not just the kernel's? */
static bool is_idle(const struct thread *t)
{
    for (size_t i = 0; i < CPU_MAX; i++) {
        if (percpu[i].idle == t) {
            return true;
        }
    }
    return false;
}

/*
 * what a thread has had of the cpus, given to its process as it ends and
 * cleared, under the lock, so the two are never both counted
 */
static void hand_over(struct thread *t)
{
    process_account(t->pid, &t->cpu);
    t->cpu = (struct cpu_usage){ 0 };
}

/* next in the ring who wants a cpu. */
static struct thread *pick_next(void)
{
    struct thread *start = current != NULL ? current : ring;
    if (start == NULL) {
        return ME->idle;
    }

    struct thread *t = start->next;
    while (t != start) {
        if (!is_idle(t) && t->state == THREAD_READY && !t->stopped) {
            return t;
        }
        t = t->next;
    }

    if (current != NULL && current->state == THREAD_RUNNING
        && !current->stopped) {
        return current;     /* still runnable and nobody is waiting */
    }
    return ME->idle;
}

/* the actual switch. must be entered with interrupts off */
/* the run queue is rewritten here, so the lock has to be held on the way in. */
static void schedule(void)
{
    struct thread *prev = current;
    struct thread *next = pick_next();

    ME->quantum_left = QUANTUM_TICKS;

    if (next == prev) {
        prev->state = THREAD_RUNNING;
        prev->on_cpu = (int)cpu_id();
        prev->i_last_cpu = (int)cpu_id();
        return;
    }

    /*
     * one still running when it goes had the cpu taken from it; any
     * other gave it up, to wait for something
     */
    ME->u_switches++;
    if (prev != NULL && prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
        prev->cpu.u_preempted++;
    } else if (prev != NULL) {
        prev->cpu.u_waited++;
    }
    next->state = THREAD_RUNNING;
    next->on_cpu = (int)cpu_id();
    next->i_last_cpu = (int)cpu_id();
    current = next;

    /* whose memory is real from here on. */
    addrspace_switch(next->space);

    /* both of these say "where does the kernel stand when this thread traps in from ring 3". */
    if (next->stack_phys != 0) {
        uint64_t ktop = (uint64_t)pmm_phys_to_virt(next->stack_phys)
                      + next->stack_pages * PAGE_SIZE;
        context_set_kernel_stack(ktop);
    }

    /*
     * `prev` is still on this cpu until the switch below has saved its
     * context, so it stays marked on_cpu. clearing it here, before the
     * switch, was a use-after-free that needed a second core to show
     * itself: the reaper would see a dead thread with `on_cpu < 0` and
     * free the stack and address space it was still switching on. so
     * the core that takes over does the clearing, once it is the one
     * running, and nothing else can pick `prev` in between, because
     * the lock travels with the switch
     */
    ME->handoff = prev;
    context_switch_arch(&prev->arch, &next->arch);
    switch_context(&prev->sp, &next->sp);

    /* the kernel is the thread that was switched to, back after however long. */
    if (ME->handoff != NULL) {
        ME->handoff->on_cpu = -1;
        ME->handoff = NULL;
    }
    /* and the kernel is `prev` again. everything above is somebody elses story */
}

/* the first thing a newly created thread does. */
void sched_first_run(void)
{
    /*
     * the other half of the handoff in schedule(): the kernel is the thread that
     * was switched to, and the one the kernel displaced is off the cpu now
     */
    if (ME->handoff != NULL) {
        ME->handoff->on_cpu = -1;
        ME->handoff = NULL;
    }
    spin_unlock(&sched_lock);
}

void sched_yield(void)
{
    uint64_t flags = spin_lock_irq(&sched_lock);
    schedule();
    spin_unlock_irq(&sched_lock, flags);
}

void waitq_enqueue_for(struct waitq *q, uint64_t u_ms)
{
    uint64_t u_ticks = (u_ms * PIT_HZ) / 1000;
    if (u_ticks == 0 && u_ms > 0) {
        u_ticks = 1;
    }
    uint64_t flags = spin_lock_irq(&sched_lock);
    current->wait_next = q->head;
    current->waiting_on = q;
    q->head = current;
    current->wake_at = (u_ms != 0) ? pit_ticks() + u_ticks : 0;
    current->state = THREAD_BLOCKED;
    spin_unlock_irq(&sched_lock, flags);
}

void waitq_enqueue(struct waitq *q)
{
    waitq_enqueue_for(q, 0);
}

void waitq_sleep(void)
{
    uint64_t flags = spin_lock_irq(&sched_lock);

    /*
     * if somebody woke the kernel between enqueueing and getting here, the kernel is
     * already READY and there is nothing to sleep through. that gap is
     * the whole reason these are two calls: it is what lets a caller
     * drop its own lock in the middle without losing the wakeup
     */
    if (current->state == THREAD_BLOCKED) {
        schedule();
    }
    current->waiting_on = NULL;

    spin_unlock_irq(&sched_lock, flags);
}

void waitq_block(struct waitq *q)
{
    waitq_enqueue(q);
    waitq_sleep();
}

void waitq_wake_all(struct waitq *q)
{
    uint64_t flags = spin_lock_irq(&sched_lock);

    struct thread *t = q->head;
    while (t != NULL) {
        struct thread *next = t->wait_next;
        t->wait_next = NULL;
        t->waiting_on = NULL;
        if (t->state == THREAD_BLOCKED) {
            t->state = THREAD_READY;
        }
        t = next;
    }
    q->head = NULL;

    spin_unlock_irq(&sched_lock, flags);
}

/* suspend, or let go again. */
void sched_set_stopped(int id, bool stopped)
{
    uint64_t flags = spin_lock_irq(&sched_lock);

    struct thread *t = ring;
    if (t != NULL) {
        do {
            if (t->id == id) {
                t->stopped = stopped;
                break;
            }
            t = t->next;
        } while (t != ring);
    }

    spin_unlock_irq(&sched_lock, flags);
}

bool sched_thread_stopped(int id)
{
    uint64_t flags = spin_lock_irq(&sched_lock);
    bool stopped = false;

    struct thread *t = ring;
    if (t != NULL) {
        do {
            if (t->id == id) {
                stopped = t->stopped && t->state != THREAD_DEAD;
                break;
            }
            t = t->next;
        } while (t != ring);
    }

    spin_unlock_irq(&sched_lock, flags);
    return stopped;
}

void sched_wake_thread(int id)
{
    uint64_t flags = spin_lock_irq(&sched_lock);

    struct thread *t = current;
    do {
        if (t->id == id) {
            if (t->state == THREAD_BLOCKED || t->state == THREAD_SLEEPING) {
                if (t->waiting_on != NULL) {
                    struct waitq *q = t->waiting_on;
                    t->waiting_on = NULL;
                    unlink(q, t);   /* the lock is already the kernel's */
                }
                t->state = THREAD_READY;
            }
            break;
        }
        t = t->next;
    } while (t != current);

    spin_unlock_irq(&sched_lock, flags);
}

/* the guts, for callers already holding the lock. */
static void unlink(struct waitq *q, struct thread *t)
{
    struct thread **link = &q->head;
    while (*link != NULL) {
        if (*link == t) {
            *link = t->wait_next;
            t->wait_next = NULL;
            t->waiting_on = NULL;
            break;
        }
        link = &(*link)->wait_next;
    }
}

void waitq_remove(struct waitq *q, struct thread *t)
{
    uint64_t flags = spin_lock_irq(&sched_lock);
    unlink(q, t);
    spin_unlock_irq(&sched_lock, flags);
}

void sleep_ms(uint64_t ms)
{
    uint64_t ticks = (ms * PIT_HZ) / 1000;
    if (ticks == 0 && ms > 0) {
        ticks = 1;      /* asking for less than a tick still costs a tick */
    }

    uint64_t flags = spin_lock_irq(&sched_lock);
    current->wake_at = pit_ticks() + ticks;
    current->state = THREAD_SLEEPING;
    schedule();
    spin_unlock_irq(&sched_lock, flags);
}

/*
 * the load: every five seconds and a tick, the threads that want a cpu
 * folded into its three averages. a tick more than five seconds, so the
 * count does not keep landing on whatever happens every five
 */
#define LOAD_EVERY (LOAD_EVERY_S * CLOCK_TICK_HZ + 1)

static uint64_t s_arr_load[LOAD_SPANS];
static uint64_t s_u_load_ticks;

/*
 * the threads that want a cpu: running, or ready to, and not stopped,
 * and in *p_threads, unless that is NULL, how many there are. the lock
 * is held
 */
static size_t count_runnable(size_t *p_threads)
{
    size_t sz_runnable = 0;
    size_t sz_threads = 0;
    struct thread *t = current;
    do {
        if (t->state != THREAD_DEAD && !is_idle(t)) {
            sz_threads++;
        }
        if (!is_idle(t) && !t->stopped && (t->state == THREAD_READY
                                           || t->state == THREAD_RUNNING)) {
            sz_runnable++;
        }
        t = t->next;
    } while (t != current);
    if (p_threads != NULL) {
        *p_threads = sz_threads;
    }
    return sz_runnable;
}

/* the lock is held */
static void fold_load(uint64_t u_active)
{
    for (size_t sz_at = 0; sz_at < LOAD_SPANS; sz_at++) {
        s_arr_load[sz_at] = load_fold(s_arr_load[sz_at], load_decay(sz_at),
                                      u_active);
    }
}

void sched_tick(bool b_user)
{
    if (current == NULL) {
        return;     /* timer beat the scheduler to it, nothing to do yet */
    }

    /* this walks the ring and may switch away, both of which are the lock's business. */
    uint64_t flags = spin_lock_irq(&sched_lock);

    /*
     * charge the tick to whoever was running when it arrived, and to
     * where it found them
     */
    if (b_user) {
        current->cpu.u_user++;
        ME->cpu.u_user++;
    } else if (current == ME->idle) {
        ME->u_idle++;
    } else {
        current->cpu.u_system++;
        ME->cpu.u_system++;
    }
    if (cpu_id() == 0 && ++s_u_load_ticks % LOAD_EVERY == 0) {
        fold_load(count_runnable(NULL) * LOAD_ONE);
    }
    wake_sleepers();

    if (--ME->quantum_left <= 0) {
        schedule();
    }

    spin_unlock_irq(&sched_lock, flags);
}

static void dump_one(struct thread *t)
{
    kprintf("  %2d  %s", t->id, t->name);
    for (size_t i = strlen(t->name); i < THREAD_NAME_MAX; i++) {
        kprintf(" ");
    }
    /*
     * stopped is not a state, a suspended thread is still blocked on
     * whatever it was blocked on, but it is what you want to see in
     * this column, because it is the reason it is not running
     */
    kprintf("%-9s", t->stopped ? "stopped" : thread_state_name(t->state));

    /* how much memory this one is actually holding. */
    if (t->space != NULL) {
        kprintf("%-8lu", t->space->pages_in_use * 4);   /* KiB */
    } else {
        kprintf("%-8s", "-");       /* a kernel thread owns no pages */
    }

    /* which core, if any, is running it this instant. */
    if (t->on_cpu >= 0) {
        kprintf("%-6d", t->on_cpu);
    } else {
        kprintf("%-6s", "-");
    }

    /* what share of the ticks so far went to this one. */
    uint64_t total = pit_ticks();
    uint64_t pct = (total > 0)
                   ? ((t->cpu.u_user + t->cpu.u_system) * 100) / total : 0;
    kprintf("%3lu%% ", pct);

    if (t->pid != 0) {
        kprintf("  pid %d, %lu pages", t->pid, addrspace_frames(t->space));
    }
    if (t->state == THREAD_SLEEPING) {
        kprintf(" (%lu ticks)", t->wake_at > pit_ticks()
                                ? t->wake_at - pit_ticks() : 0);
    }
    if (t->stack_phys == 0) {
        kprintf("   (bootloader's)");
    }
    kprintf("\n");
}

void sched_dump(void)
{
    uint64_t flags = spin_lock_irq(&sched_lock);

    kprintf("threads\n");
    kprintf("  id  name             state    memory  core  cpu  running\n");

    /*
     * the ring is in newest-first order, because sched_add splices each
     * new thread in just after current. that is fine for scheduling and
     * confusing to read, so print by id instead. a sweep per id is
     * quadratic and there are five threads
     */
    int highest = 0;
    for (struct thread *t = current; ; ) {
        if (t->id > highest) {
            highest = t->id;
        }
        t = t->next;
        if (t == current) break;
    }

    for (int want = 0; want <= highest; want++) {
        for (struct thread *t = current; ; ) {
            if (t->id == want) {
                dump_one(t);
                break;
            }
            t = t->next;
            if (t == current) break;
        }
    }

    spin_unlock_irq(&sched_lock, flags);
}

void sched_show_threads(void)
{
    sched_dump();

    /*
     * a pipe outlives neither end and is freed the moment both let go,
     * so this number should be zero at a prompt. anything else and
     * something died without releasing what it held, which is the one
     * failure mode of the whole arrangement that hides
     */
    size_t sz_pipes = pipe_count();
    if (sz_pipes > 0) {
        kprintf("%lu pipe%s still open\n", sz_pipes, sz_pipes == 1 ? "" : "s");
    }
}

void sched_proc_init(void)
{
    proc_register("threads", PROC_PUBLIC, sched_show_threads);
}

/* thread i_id, or NULL. the lock is held */
static struct thread *thread_by_id(int i_id)
{
    struct thread *t = current;
    do {
        if (t->id == i_id) {
            return t;
        }
        t = t->next;
    } while (t != current);
    return NULL;
}

bool sched_info(int i_id, struct sched_info *p_out)
{
    uint64_t u_flags = spin_lock_irq(&sched_lock);
    const struct thread *t = thread_by_id(i_id);
    if (t != NULL) {
        p_out->e_state = t->state;
        p_out->b_stopped = t->stopped;
        p_out->cpu = t->cpu;
        p_out->i_cpu = t->i_last_cpu;
        memcpy(p_out->arr_name, t->name, sizeof p_out->arr_name);
    }
    spin_unlock_irq(&sched_lock, u_flags);
    return t != NULL;
}

struct addrspace *sched_space_of(int i_pid)
{
    /*
     * a thread lets go of its space only once it is out of the ring, and
     * exec swaps one for another under this lock, so a space found here
     * still has the use of the thread it was found on
     */
    struct addrspace *p_space = NULL;
    uint64_t u_flags = spin_lock_irq(&sched_lock);
    struct thread *t = current;
    do {
        if (t->pid == i_pid && t->state != THREAD_DEAD && t->space != NULL) {
            p_space = addrspace_get(t->space);
            break;
        }
        t = t->next;
    } while (t != current);
    spin_unlock_irq(&sched_lock, u_flags);
    return p_space;
}

struct addrspace *sched_swap_space(struct thread *t, struct addrspace *p_new)
{
    uint64_t u_flags = spin_lock_irq(&sched_lock);
    struct addrspace *p_old = t->space;
    t->space = p_new;
    addrspace_switch(p_new);
    spin_unlock_irq(&sched_lock, u_flags);
    return p_old;
}

void sched_show_stack(int i_id)
{
    uint64_t u_flags = spin_lock_irq(&sched_lock);
    const struct thread *t = thread_by_id(i_id);
    if (t == current) {
        spin_unlock_irq(&sched_lock, u_flags);
        kbacktrace(0, 0);
        return;
    }
    /*
     * one parked stays parked while this is held, since a core has to
     * take it to switch to one, and what it was switched away by is on
     * top of its stack: the registers switch_context pushed, rbp the
     * sixth, and the address it returns to after them
     */
    if (t != NULL && t->on_cpu < 0 && t->state != THREAD_DEAD) {
        const uint64_t *p_saved = (const uint64_t *)t->sp;
        kbacktrace(p_saved[5], p_saved[6]);
    }
    spin_unlock_irq(&sched_lock, u_flags);
}

bool sched_thread_alive(int id)
{
    uint64_t flags = spin_lock_irq(&sched_lock);
    bool alive = false;

    struct thread *t = current;
    do {
        if (t->id == id && t->state != THREAD_DEAD) {
            alive = true;
            break;
        }
        t = t->next;
    } while (t != current);

    spin_unlock_irq(&sched_lock, flags);
    return alive;
}

enum sched_kill_result sched_kill(int id)
{
    uint64_t flags = spin_lock_irq(&sched_lock);
    enum sched_kill_result result = SCHED_KILL_NO_SUCH;
    int i_pid = 0;
    int i_tid = 0;

    struct thread *t = current;
    do {
        if (t->id == id) {
            if (t == current) {
                result = SCHED_KILL_SELF;
            } else if (is_idle(t) || t->pid == INIT_PID) {
                /*
                 * idle, because somebody has to be able to take the
                 * cpu, and init, because it is what starts everything
                 * else and what collects everything else. a machine
                 * that has lost init still runs; it just cannot ever
                 * start another session or take itself down tidily,
                 * which is a worse state than being told no
                 */
                result = SCHED_KILL_PROTECTED;
            } else if (t->state == THREAD_DEAD) {
                result = SCHED_KILL_NO_SUCH;    /* already gone */
            } else {
                /*
                 * if it is parked on a queue, take it off before the
                 * reaper frees it out from under that queue
                 */
                if (t->waiting_on != NULL) {
                    struct waitq *q = t->waiting_on;
                    t->waiting_on = NULL;
                    unlink(q, t);   /* the lock is already the kernel's */
                }
                i_pid = t->pid;
                i_tid = t->i_tid;
                hand_over(t);
                t->state = THREAD_DEAD;
                result = SCHED_KILL_OK;
            }
            break;
        }
        t = t->next;
    } while (t != current);

    spin_unlock_irq(&sched_lock, flags);

    /*
     * and its process told afterwards, since telling a parent its child
     * ended can wake the parent, which takes the lock let go of above.
     * the reaper may have been through by now, and found a process that
     * had not ended yet, so what it would have released is released here
     */
    if (i_pid != 0) {
        process_doom_others(i_tid, PROCESS_KILLED, false);
        process_thread_ended(i_tid, PROCESS_KILLED, pit_uptime_ms());
        process_fds_release(i_pid);
        process_forget(i_pid);
    }
    return result;
}

void sched_init(void)
{
    boot_thread.id = 0;
    boot_thread.name[0] = 'b';
    boot_thread.name[1] = 'o';
    boot_thread.name[2] = 'o';
    boot_thread.name[3] = 't';
    boot_thread.name[4] = '\0';
    boot_thread.state = THREAD_RUNNING;
    boot_thread.stack_phys = 0;     /* the bootloader's, not the kernel's to free */
    boot_thread.stack_pages = 0;
    boot_thread.next = &boot_thread;    /* a ring of one, for now */
    boot_thread.wait_next = NULL;
    boot_thread.cpu = (struct cpu_usage){ 0 };
    boot_thread.i_last_cpu = 0;
    boot_thread.from_heap = false;      /* it lives in .bss */
    context_init_thread(&boot_thread.arch);

    boot_thread.on_cpu = 0;

    ring = &boot_thread;
    current = &boot_thread;
    ME->quantum_left = QUANTUM_TICKS;
    ME->scheduling = true;

    ME->idle = thread_create("idle0", idle_loop, NULL);
    if (ME->idle == NULL) {
        panic("could not summon the idle thread. the wheel cannot turn");
    }

    /* and from here on, a thread can hold what it prints. */
    kprintf_capture_source(capture_of_current);
}

/* a core other than the first, joining in. */
bool sched_join(unsigned cpu, const char *idle_name)
{
    /*
     * parked, so that between it going into the ring and this core
     * claiming it, no other core can pick it up. an idle thread belongs
     * to one core and two cores standing on one stack is the end of the
     * machine
     */
    struct thread *idle = thread_create_parked(idle_name, idle_loop, NULL);
    if (idle == NULL) {
        return false;
    }

    uint64_t flags = spin_lock_irq(&sched_lock);
    percpu[cpu].idle = idle;
    percpu[cpu].quantum_left = QUANTUM_TICKS;
    percpu[cpu].scheduling = true;

    /*
     * claimed. it was parked so nobody else could take it; now it is
     * this core's, and this core is standing on it
     */
    idle->state = THREAD_RUNNING;
    idle->on_cpu = (int)cpu;
    percpu[cpu].running = idle;
    spin_unlock_irq(&sched_lock, flags);
    return true;
}

/* which core a thread is on, or -1. for `ps` */
int sched_thread_cpu(const struct thread *t)
{
    return t->on_cpu;
}

struct cpu_usage sched_process_usage(int i_pid, struct cpu_usage *p_children)
{
    /*
     * the ended threads' are added under this lock too, the one a thread
     * hands its own over under, so what a thread had is counted once:
     * still its own, or already its process's
     */
    uint64_t u_flags = spin_lock_irq(&sched_lock);
    struct cpu_usage sum = process_ended_usage(i_pid, p_children);
    struct thread *t = current;
    do {
        if (t->pid == i_pid && t->state != THREAD_DEAD) {
            cpu_usage_add(&sum, &t->cpu);
        }
        t = t->next;
    } while (t != current);
    spin_unlock_irq(&sched_lock, u_flags);
    return sum;
}

void sched_account(struct thread *t)
{
    uint64_t u_flags = spin_lock_irq(&sched_lock);
    hand_over(t);
    spin_unlock_irq(&sched_lock, u_flags);
}

bool sched_cpu_stat(unsigned u_cpu, struct sched_cpu_stat *p_out)
{
    if (u_cpu >= CPU_MAX || !percpu[u_cpu].scheduling) {
        return false;
    }
    uint64_t u_flags = spin_lock_irq(&sched_lock);
    p_out->cpu = percpu[u_cpu].cpu;
    p_out->u_idle = percpu[u_cpu].u_idle;
    p_out->u_switches = percpu[u_cpu].u_switches;
    spin_unlock_irq(&sched_lock, u_flags);
    return true;
}

void sched_load(uint64_t arr_load[LOAD_SPANS])
{
    uint64_t u_flags = spin_lock_irq(&sched_lock);
    memcpy(arr_load, s_arr_load, sizeof s_arr_load);
    spin_unlock_irq(&sched_lock, u_flags);
}

size_t sched_runnable(size_t *p_threads)
{
    uint64_t u_flags = spin_lock_irq(&sched_lock);
    size_t sz_runnable = count_runnable(p_threads);
    spin_unlock_irq(&sched_lock, u_flags);
    return sz_runnable;
}

const char *sched_cpu_running(unsigned cpu)
{
    if (cpu >= CPU_MAX) {
        return "?";
    }
    struct thread *t = percpu[cpu].running;
    return (t != NULL) ? t->name : "nothing yet";
}

size_t sched_cores_scheduling(void)
{
    size_t n = 0;
    for (size_t i = 0; i < CPU_MAX; i++) {
        if (percpu[i].scheduling) {
            n++;
        }
    }
    return n;
}
