// SPDX-License-Identifier: GPL-2.0-only
/*
 * sched/init.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * init: the first process, and the policy it applies.
 */

#include <velvet/init.h>
#include <velvet/string.h>

/* everything above the #ifndef is arithmetic over a table and a clock reading. */

void init_table_reset(struct init_table *t)
{
    memset(t, 0, sizeof *t);
}

bool init_add(struct init_table *t, const char *name, void (*entry)(void *),
              void *arg, unsigned console, bool respawn)
{
    if (t->count >= INIT_SERVICES_MAX) {
        return false;
    }
    struct service *s = &t->s[t->count];
    memset(s, 0, sizeof *s);

    size_t i = 0;
    while (name[i] != '\0' && i < INIT_NAME_MAX - 1) {
        s->name[i] = name[i];
        i++;
    }
    s->name[i]  = '\0';
    s->entry    = entry;
    s->arg      = arg;
    s->console  = console;
    s->respawn  = respawn;
    s->state    = SERVICE_STOPPED;

    t->count++;
    return true;
}

void init_started(struct init_table *t, size_t i, int tid, uint64_t now_ms)
{
    if (i >= t->count) {
        return;
    }
    struct service *s = &t->s[i];
    s->state     = SERVICE_RUNNING;
    s->thread_id = tid;
    s->starts++;

    /* the window opens on the *first* start of a run, not on every one. */
    if (s->in_window == 0) {
        s->window_start_ms = now_ms;
    }
    s->in_window++;
}

enum init_action init_died(struct init_table *t, size_t i, uint64_t now_ms)
{
    if (i >= t->count) {
        return INIT_LEAVE;
    }
    struct service *s = &t->s[i];
    s->state     = SERVICE_STOPPED;
    s->thread_id = 0;

    if (!s->respawn) {
        return INIT_LEAVE;
    }

    /* deaths spread out are not a loop. */
    if (now_ms - s->window_start_ms > INIT_RESPAWN_WINDOW_MS) {
        s->in_window = 0;
        s->window_start_ms = now_ms;
        return INIT_RESTART;
    }

    if (s->in_window > INIT_RESPAWN_MAX) {
        s->state = SERVICE_GIVEN_UP;
        return INIT_GIVE_UP;
    }
    return INIT_RESTART;
}

bool init_revive(struct init_table *t, size_t i)
{
    if (i >= t->count || t->s[i].state == SERVICE_RUNNING) {
        return false;
    }
    struct service *s = &t->s[i];
    s->state     = SERVICE_STOPPED;
    s->respawn   = true;

    /* a full allowance again. */
    s->in_window = 0;
    s->window_start_ms = 0;
    return true;
}

int init_find(const struct init_table *t, const char *name)
{
    for (size_t i = 0; i < t->count; i++) {
        if (strcmp(t->s[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

const char *init_state_name(enum service_state s)
{
    switch (s) {
    case SERVICE_RUNNING:   return "running";
    case SERVICE_GIVEN_UP:  return "given up";
    case SERVICE_STOPPED:   break;
    }
    return "stopped";
}



#ifndef VELVETOS_HOSTED

#include <velvet/sched.h>
#include <velvet/thread.h>
#include <velvet/spinlock.h>
#include <velvet/shell.h>
#include <velvet/console.h>
#include <velvet/pit.h>
#include <velvet/tty.h>
#include <asm/machine.h>
#include <asm/cpu.h>
#include <velvet/disk.h>
#include <velvet/netif.h>
#include <velvet/e1000.h>
#include <velvet/pipe.h>
#include <velvet/kprintf.h>
#include <velvet/pmm.h>
#include <velvet/vfs.h>
#include <velvet/usermode.h>
#include <velvet/panic.h>

/* how often init looks around. */
#define INIT_TICK_MS   400

/* how long the machine waits for programs to notice it is going down, before ending them itself. */
#define INIT_GRACE_MS  1000

/* the table is written by init's thread and read by whichever shell was asked to print it. */
static struct spinlock init_lock = SPINLOCK("init", LOCK_RANK_DEVICE);
static struct init_table services;

static int init_tid;

/*
 * the medium's own init. when it carries one, that program is pid 1:
 * the sessions, the consoles and the orphans are its business, and the
 * kernel's thread here keeps only the kernel's own services and the road
 * down. when it does not, this thread is pid 1 and does all of it
 */
#define INIT_PROGRAM "/init"

static bool s_b_program;

/*
 * a shutdown that has been asked for and not yet acted on, and where it
 * was asked from, so the machine says goodbye on the screen the person
 * is looking at rather than on whichever one init happens to be on
 */
static volatile bool     stop_pending;
static volatile int      stop_how;
static volatile unsigned stop_console;

bool init_stopping(void)
{
    return stop_pending;
}



/*
 * a console session. it returns when somebody logs out, and init starts
 * a fresh one, which is the point, and is a real difference rather
 * than a tidier way of writing the same thing. `logout` used to call
 * `login` from inside the running session, so the next person inherited
 * the last one's working directory, history, jobs and variables. a
 * session that *ends* leaves nothing behind, because there is nothing
 * left to leave it in
 */
static void console_service(void *arg)
{
    (void)arg;
    shell_run();
    thread_exit(0);
}

/* the disk, kept roughly honest. */
#define FLUSH_EVERY_MS 3000

/*
 * the wire, watched.
 *
 * this thread does every decision the network makes: parsing, answering
 * an arp, delivering to a socket. all of it wants locks and none of it
 * belongs in an interrupt handler, which is why the card's handler wakes
 * this and returns rather than doing the work itself.
 *
 * it woke on a timer, twenty times a second, whether or not
 * anything had arrived. now it sleeps until the card says otherwise,
 * and the sleep is *bounded* rather than indefinite, deliberately: a pci
 * interrupt that never arrives because the line was misrouted would
 * otherwise be a network that is silently dead. half a second late is a
 * working machine with a symptom; never waking is neither.
 *
 * it is a service like any other, so if it ever dies init brings it back
 */
static void network_service(void *arg)
{
    (void)arg;
    net_service_ready();

    /* ask the wire for an address. */
    net_dhcp_start();

    for (;;) {
        net_poll();
        sleep_ms(NET_FALLBACK_MS);
    }
}

static void flusher_service(void *arg)
{
    (void)arg;
    for (;;) {
        sleep_ms(FLUSH_EVERY_MS);
        if (disk_ready(DISK_ROOT) && disk_dirty()) {
            (void)disk_sync();
        }
    }
}



/*
 * start service `i`. the thread is created *parked* and released after
 * it has been told which console it belongs to: a thread inherits the
 * console of whoever made it, and everything here is made by init, so a
 * session that ran for even one instruction before being moved would
 * print its first line on init's screen instead of its own
 */
static bool start_service(size_t i)
{
    struct service copy;

    uint64_t flags = spin_lock_irq(&init_lock);
    if (i >= services.count || services.s[i].state == SERVICE_RUNNING) {
        spin_unlock_irq(&init_lock, flags);
        return false;
    }
    /* claimed before the thread exists, and that ordering is the whole of it. */
    services.s[i].state = SERVICE_RUNNING;
    services.s[i].thread_id = 0;
    copy = services.s[i];
    spin_unlock_irq(&init_lock, flags);

    struct thread *t = thread_create_parked(copy.name, copy.entry, copy.arg);
    if (t == NULL) {
        flags = spin_lock_irq(&init_lock);
        services.s[i].state = SERVICE_STOPPED;      /* the claim, given back */
        spin_unlock_irq(&init_lock, flags);
        return false;
    }
    t->console = copy.console;

    flags = spin_lock_irq(&init_lock);
    init_started(&services, i, t->id, pit_uptime_ms());
    spin_unlock_irq(&init_lock, flags);

    sched_wake_thread(t->id);
    return true;
}

static void stop_service(size_t i)
{
    uint64_t flags = spin_lock_irq(&init_lock);
    int tid = services.s[i].thread_id;
    services.s[i].respawn = false;
    services.s[i].state = SERVICE_STOPPED;
    services.s[i].thread_id = 0;
    spin_unlock_irq(&init_lock, flags);

    if (tid != 0) {
        sched_kill(tid);
    }
}

/* move init's own output to a given screen, and say where it was. */
static unsigned speak_on(unsigned console)
{
    struct thread *me = sched_current();
    if (me == NULL) {
        return console;
    }
    unsigned was = me->console;
    me->console = console;
    return was;
}

/* every service that is not running, brought back if it should be. */
static void supervise(void)
{
    size_t count;
    uint64_t flags = spin_lock_irq(&init_lock);
    count = services.count;
    spin_unlock_irq(&init_lock, flags);

    for (size_t i = 0; i < count; i++) {
        flags = spin_lock_irq(&init_lock);
        bool running = services.s[i].state == SERVICE_RUNNING;
        int  tid     = services.s[i].thread_id;
        spin_unlock_irq(&init_lock, flags);

        /* a zero thread id is a start that has been claimed and not yet finished. */
        if (!running || tid == 0 || sched_thread_alive(tid)) {
            continue;
        }

        flags = spin_lock_irq(&init_lock);
        enum init_action what = init_died(&services, i, pit_uptime_ms());
        char name[INIT_NAME_MAX];
        memcpy(name, services.s[i].name, sizeof name);
        unsigned tries   = services.s[i].in_window;
        unsigned console = services.s[i].console;
        spin_unlock_irq(&init_lock, flags);

        if (what == INIT_RESTART) {
            if (!start_service(i)) {
                unsigned was = speak_on(console);
                kprintf("init: no memory to bring %s back. it stays down\n",
                        name);
                speak_on(was);
            }
        } else if (what == INIT_GIVE_UP) {
            /* the message matters as much as the rule, and where it is printed matters as much as the message. */
            unsigned was = speak_on(console);
            kprintf("\ninit: %s has died %u times in under %u seconds. "
                    "leaving it down.\n"
                    "      `init start %s` from another console to try "
                    "again\n",
                    name, tries, INIT_RESPAWN_WINDOW_MS / 1000, name);
            speak_on(was);
        }
    }
}

bool init_restart(const char *name)
{
    uint64_t flags = spin_lock_irq(&init_lock);
    int i = init_find(&services, name);
    bool ok = (i >= 0) && init_revive(&services, (size_t)i);
    spin_unlock_irq(&init_lock, flags);

    if (ok) {
        ok = start_service((size_t)i);
    }
    return ok;
}

void init_snapshot(struct init_table *out)
{
    uint64_t flags = spin_lock_irq(&init_lock);
    *out = services;
    spin_unlock_irq(&init_lock, flags);
}



void init_stop_machine(enum init_stop how)
{
    stop_how     = (int)how;
    stop_console = tty_my_console();
    stop_pending = true;
    sched_wake_thread(init_tid);

    /* and whoever asked is finished. */
    for (;;) {
        sleep_ms(1000);
    }
}

/* ask everything still running to stop, and then insist. */
static void stop_the_programs(void)
{
    size_t running = process_interrupt_all();
    if (running == 0) {
        return;
    }

    kprintf("init: asking %lu program%s to stop\n",
            running, running == 1 ? "" : "s");

    for (uint64_t waited = 0; waited < INIT_GRACE_MS; waited += 100) {
        sleep_ms(100);
        if (process_interrupt_all() == 0) {
            return;
        }
    }

    int ids[MAX_PROCESSES];
    size_t n = process_running_threads(ids, MAX_PROCESSES);
    if (n > 0) {
        kprintf("init: %lu did not answer. ending %s\n",
                n, n == 1 ? "it" : "them");
        for (size_t i = 0; i < n; i++) {
            sched_kill(ids[i]);
        }
    }
}

/* the last thing on the screen. */
static void say_goodbye(enum init_stop how)
{
    console_set_colors(0x7b8ce0, 0x101018);

    if (how == INIT_REBOOT) {
        kprintf("\nThou art I... And I am thou...\n");
        kprintf("Thou hast established a genuine bond...\n\n");
        kprintf("The innermost power of the Computer\n");
        kprintf("Arcana hath been set free.\n\n");
        kprintf("the kernel bestows upon thee the ability to\n");
        kprintf("create velvetOS, the ultimate form\n");
        kprintf("of the Computer's Arcana...\n\n");
        pit_busy_wait(3000);
    } else {
        kprintf("\nThe Velvet Room fades...\n");
        kprintf("Till the kernel meets again.\n");
        pit_busy_wait(1500);
    }
}

static void take_the_machine_down(void)
{
    enum init_stop how = (enum init_stop)stop_how;

    /* say it where it was asked. */
    struct thread *me = sched_current();
    if (me != NULL) {
        me->console = stop_console;
    }

    /* and nothing announcing the shutdown either. */
    stop_the_programs();

    /* the services, in the reverse of the order they came up. */
    size_t count;
    uint64_t flags = spin_lock_irq(&init_lock);
    count = services.count;
    spin_unlock_irq(&init_lock, flags);

    for (size_t i = count; i-- > 0;) {
        stop_service(i);
    }

    /* and only now, with nothing left that could add to it. */
    if (disk_ready(DISK_ROOT)) {
        if (disk_dirty()) {
            kprintf("init: writing what is still in memory...\n");
        }
        /*
         * and the journal closed behind it, which is the difference
         * between a disk that was unmounted and one the power was
         * pulled on. a log left open says "replay the kernel" to whoever mounts
         * this next, correctly, because nobody could know otherwise.
         * saying it after a tidy shutdown is a lie the next boot pays
         * for, and this is the one place that can tell the difference
         */
        if (!disk_unmount()) {
            kprintf("init: the drive refused. something is being lost here\n");
        }
    }

    say_goodbye(how);

    if (how == INIT_REBOOT) {
        machine_reset();        /* which does not come back either way */
    }

    /* and a poweroff that nothing answered is worth saying out loud. */
    machine_poweroff();
    kprintf("init: nothing answered. halting instead, close the window\n");
    cpu_stop();
}



static void init_thread(void *arg)
{
    (void)arg;

    /* the loader's memory, handed back before anything else exists. */
    uint64_t gained = pmm_reclaim_bootloader();
    kprintf("reclaimed %lu KiB of bootloader memory (%lu MiB usable now)\n",
            gained / 1024, pmm_total_bytes() / (1024 * 1024));
    kprintf("four consoles: alt+1..4 (or alt+f1..f4, if your host does "
            "not eat them),\n");
    kprintf("             ctrl+\\ then a digit on serial, or `chvt`. "
            "shift+pageup looks back\n");

    /*
     * the order below is the order the machine comes up in, and it is
     * the reverse of the order it goes down in.
     *
     * the flusher first, because it is what stands between a
     * write-back cache and a machine that loses the last thing anybody
     * typed, and it should be there before there is anybody able to
     * type. the sessions after it, so the last thing to appear on a
     * screen is a prompt rather than a service report
     */
    uint64_t flags = spin_lock_irq(&init_lock);
    init_table_reset(&services);
    init_add(&services, "flusher", flusher_service, NULL, 0, true);
    if (e1000_present()) {
        init_add(&services, "network", network_service, NULL, 0, true);
    }
    for (unsigned c = 0; c < VCONSOLE_MONITOR && !s_b_program; c++) {
        char name[INIT_NAME_MAX];
        name[0] = 't'; name[1] = 't'; name[2] = 'y';
        name[3] = (char)('1' + c); name[4] = '\0';
        init_add(&services, name, console_service, NULL, c, true);
    }
    size_t count = services.count;
    spin_unlock_irq(&init_lock, flags);

    /* no line saying init came up. */
    kprintf(s_b_program ? "boot complete, handing the machine to "
                          INIT_PROGRAM "\n\n"
                        : "boot complete, handing the screen to the shell\n\n");

    for (size_t i = 0; i < count; i++) {
        if (!start_service(i)) {
            /*
             * it stays down. a service that could not be started is not
             * a service that died, so the sweep below will not try
             * again, which is right: the reason it failed is almost
             * always no memory, and retrying that four times a second
             * forever is how a machine that is merely short of memory
             * becomes a machine that is doing nothing else
             */
            kprintf("init: could not start %s. it stays down, "
                    "`init start %s` to try again\n",
                    services.s[i].name, services.s[i].name);
        }
    }

    /*
     * and only now, with the flusher there before anybody can write. in
     * front of console 0, so what it says before it has taken a console
     * of its own reaches the screen rather than nowhere
     */
    if (s_b_program) {
        tty_set_foreground(INIT_PID);
        user_start(INIT_PID);
    }

    for (;;) {
        sleep_ms(INIT_TICK_MS);

        if (stop_pending) {
            take_the_machine_down();     /* which never comes back */
        }

        /*
         * a program that is pid 1 and has ended leaves nothing to start
         * a session, take an orphan, or bring the machine down when
         * asked. saying so is better than a machine that looks alive
         */
        if (s_b_program) {
            const struct process *p_init = process_find(INIT_PID);
            if (p_init == NULL || p_init->exited) {
                panic("%s ended, with %d, and there is no other init",
                      INIT_PROGRAM, p_init != NULL ? p_init->exit_code : -1);
            }
            supervise();
            continue;       /* orphans are its to collect, not this one's */
        }

        /* adopted children, collected. */
        for (;;) {
            int pid = process_orphan();
            if (pid == 0) {
                break;
            }
            /*
             * before the slot goes, since afterwards there is nothing
             * left to ask which pipes it was holding
             */
            pipe_release_for(pid);
            process_collect(pid, NULL);
        }

        supervise();
    }
}

bool init_boot(void)
{
    /*
     * whichever is pid 1 goes into the table first, which is what makes
     * it 1. the program is made parked, and this thread lets it go once
     * the kernel's services are up
     */
    struct vfs_file f;
    s_b_program = vfs_open(INIT_PROGRAM, &f) && !f.is_dir;

    int pid;
    if (s_b_program) {
        const char *arr_argv[1] = { INIT_PROGRAM };
        const char *str_why = NULL;
        pid = user_spawn(INIT_PROGRAM, 1, arr_argv, "/", 0, 0, true, NULL,
                         &str_why);
        if (pid != INIT_PID) {
            kprintf("init: %s would not start: %s\n", INIT_PROGRAM,
                    str_why != NULL ? str_why : "it was not pid 1");
            return false;
        }
    } else {
        pid = process_create("init", 0, 0, false, pit_uptime_ms());
        if (pid != INIT_PID) {
            return false;
        }
    }

    struct thread *t = thread_create_parked(s_b_program ? "services" : "init",
                                            init_thread, NULL);
    if (t == NULL) {
        return false;
    }
    t->console = 0;
    init_tid = t->id;
    if (!s_b_program) {
        t->pid = INIT_PID;
        process_set_thread(INIT_PID, t->id);
    }

    sched_wake_thread(t->id);
    return true;
}

#endif /* VELVETOS_HOSTED */
