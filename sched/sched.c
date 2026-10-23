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
        net_socket_close_all(dead[i]->pid);
        net_tcp_close_all(dead[i]->pid);
        thread_free_stack(dead[i]);
        thread_free(dead[i]);   /* a no-op for the boot thread */
    }
}

static void wake_sleepers(void)
{
    uint64_t now = pit_ticks();
    struct thread *t = current;
    do {
        if (t->state == THREAD_SLEEPING && now >= t->wake_at) {
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
        return;
    }

    if (prev != NULL && prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
    }
    next->state = THREAD_RUNNING;
    next->on_cpu = (int)cpu_id();
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

static void unlink(struct waitq *q, struct thread *t);

void waitq_enqueue(struct waitq *q)
{
    uint64_t flags = spin_lock_irq(&sched_lock);
    current->wait_next = q->head;
    current->waiting_on = q;
    q->head = current;
    current->state = THREAD_BLOCKED;
    spin_unlock_irq(&sched_lock, flags);
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

void sched_tick(void)
{
    if (current == NULL) {
        return;     /* timer beat the scheduler to it, nothing to do yet */
    }

    /* this walks the ring and may switch away, both of which are the lock's business. */
    uint64_t flags = spin_lock_irq(&sched_lock);

    /* charge the tick to whoever was running when it arrived. */
    current->cpu_ticks++;

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
    uint64_t pct = (total > 0) ? (t->cpu_ticks * 100) / total : 0;
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

/*
 * the kernel's own calls that led to this read, which are the reader's:
 * the same walker a panic uses, with nothing on fire
 */
static void sched_show_stack(void)
{
    kbacktrace(0, 0);
}

/*
 * the reader's own environment, each NAME=value ended by a NUL, as the
 * block is kept. a program can only ask for a variable it can name, and
 * this is how it finds out which there are. the block's last string is
 * empty, the end of the list rather than one more name, and stays out
 */
static void sched_show_environ(void)
{
    int i_pid = (current != NULL) ? current->pid : 0;
    char *str_block = (i_pid != 0) ? kmalloc(PROC_ENV_MAX) : NULL;

    if (str_block == NULL) {
        return;
    }
    size_t sz_len = process_get_env(i_pid, str_block, PROC_ENV_MAX);
    if (sz_len > 0) {
        kwrite(str_block, sz_len - 1);
    }
    kfree(str_block);
}

void sched_proc_init(void)
{
    proc_register("threads", PROC_PUBLIC, sched_show_threads);
    proc_register("stack", PROC_PRIVATE, sched_show_stack);
    proc_register("environ", PROC_PUBLIC, sched_show_environ);
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
                if (t->pid != 0) {
                    process_exited(t->pid, PROCESS_KILLED, pit_uptime_ms());
                }
                t->state = THREAD_DEAD;
                result = SCHED_KILL_OK;
            }
            break;
        }
        t = t->next;
    } while (t != current);

    spin_unlock_irq(&sched_lock, flags);
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
    boot_thread.cpu_ticks = 0;
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
