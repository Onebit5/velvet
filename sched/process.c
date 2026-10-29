// SPDX-License-Identifier: GPL-2.0-only
/*
 * sched/process.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the process table.
 */

#include <velvet/process.h>
#include <asm/irq.h>
#include <velvet/spinlock.h>
#include <velvet/file.h>
#include <uapi/velvet/errno.h>
#include <velvet/string.h>
#include <velvet/sched.h>
#include <uapi/velvet/fcntl.h>
#include <velvet/thread.h>
#include <velvet/usermode.h>
#include <uapi/velvet/wait.h>

/* a fixed table rather than a linked list of allocations. */
/* the process table and every descriptor in it. */
static struct spinlock process_lock = SPINLOCK("process", LOCK_RANK_PROCESS);

static struct process table[MAX_PROCESSES];
static int next_pid = 1;

static struct process *slot_for(int pid)
{
    if (pid <= 0) {
        return NULL;
    }
    for (size_t i = 0; i < MAX_PROCESSES; i++) {
        if (table[i].pid == pid) {
            return &table[i];
        }
    }
    return NULL;
}

int current_pid(void)
{
    struct thread *p_me = sched_current();
    return (p_me != NULL) ? p_me->pid : 0;
}

const char *process_cwd(int pid)
{
    struct process *p = slot_for(pid);
    return (p != NULL && p->cwd[0] != '\0') ? p->cwd : "/";
}

const char *process_root(int i_pid)
{
    struct process *p = slot_for(i_pid);
    return (p != NULL && p->arr_root[0] != '\0') ? p->arr_root : "/";
}

void process_set_root(int i_pid, const char *str_path)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(i_pid);
    if (p != NULL) {
        strncpy(p->arr_root, str_path, PATH_MAX - 1);
        p->arr_root[PATH_MAX - 1] = '\0';
    }
    spin_unlock_irq(&process_lock, u_flags);
}

void process_set_cwd(int pid, const char *path)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    if (p != NULL) {
        size_t i = 0;
        while (path[i] != '\0' && i < PATH_MAX - 1) {
            p->cwd[i] = path[i];
            i++;
        }
        p->cwd[i] = '\0';
    }
    spin_unlock_irq(&process_lock, flags);
}

int process_create(const char *name, int parent, int uid, bool announce,
                   uint64_t now_ms)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    int pid = 0;

    /*
     * a parent that has ended has already handed its children to init,
     * so one born to it now is init's from birth, and so is one born to
     * a parent that is not there at all. deciding it here is what lets
     * init find every orphan by its parent alone
     */
    const struct process *p_parent = (parent != 0) ? slot_for(parent) : NULL;
    if (parent != 0 && (p_parent == NULL || p_parent->exited)) {
        parent = INIT_PID;
    }

    for (size_t i = 0; i < MAX_PROCESSES; i++) {
        if (table[i].pid != 0) {
            continue;
        }
        struct process *p = &table[i];

        /*
         * the slot still holds whatever the last process in it left. a
         * field nobody sets below would start out as that process's
         */
        memset(p, 0, sizeof *p);

        p->pid        = next_pid++;
        p->parent     = parent;
        /*
         * its own group until somebody says otherwise, which is what a
         * command typed on its own is: a job of one
         */
        p->pgid       = p->pid;
        p->i_sid        = (p_parent != NULL) ? p_parent->i_sid : p->pid;
        p->uid        = uid;
        p->i_gid        = uid;
        p->u_umask    = 022;
        for (size_t sz_at = 0; sz_at < RLIM_NLIMITS; sz_at++) {
            p->arr_limits[sz_at].rlim_cur = RLIM_INFINITY;
            p->arr_limits[sz_at].rlim_max = RLIM_INFINITY;
        }
        p->arr_limits[RLIMIT_NOFILE].rlim_cur = MAX_FDS;
        p->arr_limits[RLIMIT_NOFILE].rlim_max = MAX_FDS;
        p->arr_limits[RLIMIT_STACK].rlim_cur = USER_STACK_PAGES * 4096ull;
        p->arr_limits[RLIMIT_NPROC].rlim_cur = MAX_PROCESSES;
        p->arr_limits[RLIMIT_NPROC].rlim_max = MAX_PROCESSES;
        p->announce   = announce;
        signal_reset(&p->sig);
        p->started_ms = now_ms;

        /*
         * nothing open: whoever makes a process hands it what it starts
         * with, a copy of its own for a fork, the console for a program
         * the kernel starts
         */
        for (size_t sz_fd = 0; sz_fd < MAX_FDS; sz_fd++) {
            p->fds[sz_fd] = NULL;
        }
        p->u_cloexec = 0;


        /* wherever the parent was standing, under whatever root it had. */
        const char *from = process_cwd(parent);
        size_t c = 0;
        while (from[c] != '\0' && c < PATH_MAX - 1) {
            p->cwd[c] = from[c];
            c++;
        }
        p->cwd[c] = '\0';
        if (p_parent != NULL) {
            memcpy(p->arr_root, p_parent->arr_root, sizeof p->arr_root);
        }

        size_t n = 0;
        while (name[n] != '\0' && n < PROC_NAME_MAX - 1) {
            p->name[n] = name[n];
            n++;
        }
        p->name[n] = '\0';

        pid = p->pid;
        break;
    }

    spin_unlock_irq(&process_lock, flags);
    return pid;
}

/*
 * what a child's parent is to be told, gathered under the lock and sent
 * after it, since sending reaches the scheduler
 */
struct child_news {
    int i_parent;
    struct signal_origin origin;
};

/*
 * SIGCHLD for the parent of p, with i_code saying what happened and
 * i_status what it came to: nothing for a stop the parent asked not to
 * hear of, and nothing at all for the kernel's own
 */
static void child_news(const struct process *p, int i_code, int i_status,
                       struct child_news *p_out)
{
    p_out->i_parent = 0;
    const struct process *p_parent = slot_for(p->parent);
    if (p_parent == NULL || p_parent->exited) {
        return;
    }
    if (i_code == CLD_STOPPED
        && (p_parent->sig.action[SIGCHLD].sa_flags & SA_NOCLDSTOP)) {
        return;
    }
    p_out->i_parent = p_parent->pid;
    memset(&p_out->origin, 0, sizeof p_out->origin);
    p_out->origin.i_code = i_code;
    p_out->origin.i_pid = p->pid;
    p_out->origin.u_uid = (uint32_t)p->uid;
    p_out->origin.i_status = i_status;
}

static void tell_parent(const struct child_news *p_news)
{
    if (p_news->i_parent != 0) {
        process_send_signal(p_news->i_parent, SIGCHLD, &p_news->origin);
    }
}

/* a parent that has said it will never wait for its children */
static bool parent_detaches(const struct process *p)
{
    const struct process *p_parent = slot_for(p->parent);
    if (p_parent == NULL) {
        return false;
    }
    const struct sigaction *p_act = &p_parent->sig.action[SIGCHLD];
    return p_act->sa_handler == SIG_IGNORE
           || (p_act->sa_flags & SA_NOCLDWAIT) != 0;
}

void process_set_thread(int pid, int thread_id)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    if (p != NULL) {
        p->thread_id = thread_id;
    }
    spin_unlock_irq(&process_lock, flags);
}

void process_exited(int pid, int code, uint64_t now_ms)
{
    struct child_news news = { 0 };
    bool b_first = false;
    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    /*
     * the first answer is the true one: a process killed while it was
     * already on its way out should not have its code overwritten
     */
    if (p != NULL && !p->exited) {
        p->exited    = true;
        p->i_wait_status = code;
        b_first = true;
        p->ended_ms  = now_ms;
        p->thread_id = 0;
        /*
         * what it had open stays in its slots. closing a pipe's end wakes
         * whoever waits at the other, which reaches the scheduler, and
         * this lock may not be held while doing that, so
         * process_fds_release, which every way a process ends calls after
         * this, closes them with no lock held
         */

        /* and whatever it was the parent of is init's now. */
        for (size_t j = 0; j < MAX_PROCESSES; j++) {
            if (table[j].pid != 0 && table[j].parent == pid) {
                table[j].parent = INIT_PID;
            }
        }
    }
    if (b_first) {
        /*
         * the parent hears of it, and a parent that ignores SIGCHLD
         * will never wait, so the child is nobody's to collect but the
         * reaper's
         */
        p->b_detached = parent_detaches(p);
        child_news(p, (code & 0x7f) != 0 ? CLD_KILLED : CLD_EXITED,
                   (code & 0x7f) != 0 ? (code & 0x7f) : (code >> 8) & 0xff,
                   &news);
    }
    spin_unlock_irq(&process_lock, flags);
    tell_parent(&news);
}

void process_account(int i_pid, uint64_t u_ticks)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(i_pid);
    if (p != NULL) {
        p->u_ticks = u_ticks;
    }
    spin_unlock_irq(&process_lock, u_flags);
}

bool process_collect(int pid, int *code)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    bool collected = false;

    struct process *p = slot_for(pid);
    if (p != NULL && p->exited) {
        if (code != NULL) {
            *code = p->i_wait_status;
        }
        /* what it and its own children ran for, the parent's children's now */
        struct process *p_parent = slot_for(p->parent);
        if (p_parent != NULL) {
            p_parent->u_child_ticks += p->u_ticks + p->u_child_ticks;
        }
        p->pid = 0;         /* the slot is free again */
        collected = true;
    }

    spin_unlock_irq(&process_lock, flags);
    return collected;
}



void process_forget(int i_pid)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(i_pid);
    if (p != NULL && p->exited && p->b_detached) {
        p->pid = 0;
    }
    spin_unlock_irq(&process_lock, u_flags);
}

size_t process_interrupt_all(void)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    size_t n = 0;

    for (size_t i = 0; i < MAX_PROCESSES; i++) {
        if (table[i].pid == 0 || table[i].exited
            || table[i].pid == INIT_PID) {
            continue;
        }
        signal_raise(&table[i].sig, SIGINT, NULL);
        table[i].interrupted = true;
        n++;
    }

    spin_unlock_irq(&process_lock, flags);
    return n;
}

size_t process_running_threads(int *ids, size_t max)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    size_t n = 0;

    for (size_t i = 0; i < MAX_PROCESSES && n < max; i++) {
        if (table[i].pid == 0 || table[i].exited
            || table[i].pid == INIT_PID || table[i].thread_id == 0) {
            continue;
        }
        ids[n++] = table[i].thread_id;
    }

    spin_unlock_irq(&process_lock, flags);
    return n;
}

bool process_announces(int pid)
{
    const struct process *p = slot_for(pid);
    return p != NULL && p->announce;
}

int process_uid(int pid)
{
    const struct process *p = slot_for(pid);
    return (p != NULL) ? p->uid : -1;
}

const struct process *process_find(int pid)
{
    return slot_for(pid);
}

const struct process *process_at(size_t index)
{
    size_t seen = 0;
    for (size_t i = 0; i < MAX_PROCESSES; i++) {
        if (table[i].pid == 0) {
            continue;
        }
        if (seen == index) {
            return &table[i];
        }
        seen++;
    }
    return NULL;
}

size_t process_count(void)
{
    size_t n = 0;
    for (size_t i = 0; i < MAX_PROCESSES; i++) {
        if (table[i].pid != 0) {
            n++;
        }
    }
    return n;
}



void process_interrupt(int pid)
{
    /* what ctrl+c has always meant here, said in the new vocabulary */
    process_signal(pid, SIGINT);
}

/*
 * stop and continue take effect when the signal is sent, not when the
 * target next comes back from a call. a stopped thread never runs to
 * come back from anything, so a SIGCONT left pending for it would wait
 * forever, and a program busy in a loop makes no calls to come back
 * from. a stop that is caught, ignored or blocked is left to delivery.
 * returns what the thread should become: 1 stopped, -1 running, 0 as
 * it is
 */
static int settle_now(struct process *p, int i_sig)
{
    uint64_t u_bit = SIGNAL_BIT(i_sig);
    enum signal_action e_act = signal_action_for(&p->sig, i_sig);
    bool b_blocked = (p->sig.blocked & u_bit) != 0 && i_sig != SIGSTOP;

    if (e_act == SIGNAL_ACTION_STOP && !b_blocked) {
        p->sig.pending &= ~u_bit;       /* acted on, nothing to deliver */
        p->i_stopped_by = i_sig;
        p->b_stop_told = false;
        return 1;
    }

    /* and a kill, which a stopped process has to be running to die of */
    if (i_sig == SIGCONT || i_sig == SIGKILL) {
        if (e_act == SIGNAL_ACTION_CONTINUE) {
            p->sig.pending &= ~u_bit;   /* continuing was all it meant */
        }
        if (p->i_stopped_by != 0 && i_sig == SIGCONT) {
            p->b_continued = true;
        }
        p->i_stopped_by = 0;
        return -1;
    }
    return 0;
}

/*
 * a fault's own signal, which cannot be refused: blocked or ignored, it
 * goes back to its default first. a program that ignores SIGSEGV and
 * then dereferences null would otherwise return to the same instruction
 * forever, faulting each time
 */
void process_vfork_done(int i_pid)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(i_pid);
    if (p != NULL) {
        p->b_vforked = false;
    }
    spin_unlock_irq(&process_lock, u_flags);
}

bool process_vforking(int i_pid)
{
    const struct process *p = slot_for(i_pid);
    return p != NULL && p->b_vforked && !p->exited;
}

void process_force_signal(int i_pid, int i_sig,
                          const struct signal_origin *origin)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(i_pid);
    if (p != NULL && !p->exited && i_sig > 0 && i_sig < SIGNAL_MAX) {
        uint64_t u_bit = SIGNAL_BIT(i_sig);
        if (p->sig.action[i_sig].sa_handler == SIG_IGNORE
            || (p->sig.blocked & u_bit) != 0) {
            p->sig.action[i_sig].sa_handler = SIG_DEFAULT;
            p->sig.blocked &= ~u_bit;
        }
        signal_raise(&p->sig, i_sig, origin);
        p->interrupted = true;
    }
    spin_unlock_irq(&process_lock, u_flags);
}

bool process_send_signal(int pid, int sig,
                         const struct signal_origin *origin)
{
    struct child_news news = { 0 };
    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    bool ok = false;
    int i_become = 0;
    /*
     * init takes only what it has a handler for: one it left to the
     * default would end, or stop, the process the machine needs. sent
     * all the same, as far as the sender can tell
     */
    bool b_init_ignores = p != NULL && p->pid == INIT_PID && sig > 0
                          && sig < SIGNAL_MAX
                          && p->sig.action[sig].sa_handler == SIG_DEFAULT;
    if (b_init_ignores) {
        ok = true;
    } else if (p != NULL && !p->exited) {
        ok = signal_raise(&p->sig, sig, origin);
        if (ok) {
            i_become = settle_now(p, sig);
            if (i_become > 0) {
                child_news(p, CLD_STOPPED, sig, &news);
            } else if (i_become < 0 && sig == SIGCONT) {
                child_news(p, CLD_CONTINUED, SIGCONT, &news);
            }
        }

        /*
         * whether anything is pending and unblocked: what a blocking
         * call stops waiting for. an ignored one was dropped as it was
         * raised, so it is not among them
         */
        p->interrupted = signal_deliverable(&p->sig);
    }
    int i_thread = (p != NULL) ? p->thread_id : 0;
    bool b_wake = ok && p != NULL && p->interrupted;
    spin_unlock_irq(&process_lock, flags);
    tell_parent(&news);

    if (i_become != 0 && i_thread > 0) {
        sched_set_stopped(i_thread, i_become > 0);
    }

    /*
     * and a thread asleep on a queue has to be woken, or a signal to a
     * program blocked on a read arrives whenever that read happens to
     * finish, which for a program waiting on the keyboard is never.
     * this is the half the roadmap warned about: every blocking call
     * was written assuming it ends for one of two reasons
     */
    if (b_wake && i_thread > 0) {
        sched_wake_thread(i_thread);
    }
    return ok;
}

bool process_signal(int pid, int sig)
{
    return process_send_signal(pid, sig, NULL);
}

void process_stopped(int i_pid, int i_sig)
{
    struct child_news news = { 0 };
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(i_pid);
    if (p != NULL) {
        p->i_stopped_by = i_sig;
        p->b_stop_told = false;
        child_news(p, CLD_STOPPED, i_sig, &news);
    }
    spin_unlock_irq(&process_lock, u_flags);
    tell_parent(&news);
}
/* whether a selector, as wait4 takes one, picks this child */
static bool picks(int i_selector, int i_caller_pgid, const struct process *p)
{
    if (i_selector > 0) {
        return p->pid == i_selector;
    }
    if (i_selector == -1) {
        return true;
    }
    if (i_selector == 0) {
        return p->pgid == i_caller_pgid;
    }
    return p->pgid == -i_selector;
}

int process_reportable(int i_parent, int i_selector, int i_options,
                       int *p_status, bool *p_ended)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    const struct process *p_me = slot_for(i_parent);
    int i_pgid = (p_me != NULL) ? p_me->pgid : 0;
    bool b_keep = (i_options & WNOWAIT) != 0;
    int i_found = -ECHILD;

    for (size_t sz_at = 0; sz_at < MAX_PROCESSES; sz_at++) {
        struct process *p = &table[sz_at];
        if (p->pid == 0 || p->parent != i_parent || p->b_detached
            || !picks(i_selector, i_pgid, p)) {
            continue;
        }
        i_found = 0;
        if (p->exited) {
            *p_status = p->i_wait_status;
            *p_ended = !b_keep;
            i_found = p->pid;
            break;
        }
        if ((i_options & WUNTRACED) && p->i_stopped_by != 0
            && !p->b_stop_told) {
            p->b_stop_told = !b_keep;
            *p_status = W_STOPCODE(p->i_stopped_by);
            *p_ended = false;
            i_found = p->pid;
            break;
        }
        if ((i_options & WCONTINUED) && p->b_continued) {
            p->b_continued = b_keep;
            *p_status = W_CONTINUED;
            *p_ended = false;
            i_found = p->pid;
            break;
        }
    }

    spin_unlock_irq(&process_lock, u_flags);
    return i_found;
}

bool process_signal_group(int pgid, int sig)
{
    /*
     * the pids are collected under the lock and signalled without it,
     * because process_signal wakes a thread and taking the scheduler's
     * lock inside this one is an ordering nobody else here uses
     */
    int pids[MAX_PROCESSES];
    size_t n = 0;

    uint64_t flags = spin_lock_irq(&process_lock);
    for (size_t i = 0; i < MAX_PROCESSES; i++) {
        if (table[i].pid != 0 && table[i].pgid == pgid && !table[i].exited) {
            pids[n++] = table[i].pid;
        }
    }
    spin_unlock_irq(&process_lock, flags);

    bool any = false;
    for (size_t i = 0; i < n; i++) {
        any |= process_signal(pids[i], sig);
    }
    return any;
}

struct signal_state *process_signal_state(int pid)
{
    struct process *p = slot_for(pid);
    return p == NULL ? NULL : &p->sig;
}

enum signal_action process_take_signal(int pid, int *sig_out,
                                       struct sigaction *act_out,
                                       uint64_t *old_mask_out,
                                       struct signal_origin *origin_out)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    enum signal_action a = SIGNAL_ACTION_NONE;

    if (p != NULL) {
        int sig = signal_next(&p->sig);
        if (sig != 0) {
            a = signal_take(&p->sig, sig, act_out, old_mask_out, origin_out);
            if (sig_out != NULL) {
                *sig_out = sig;
            }
        }
        p->interrupted = signal_deliverable(&p->sig);
    }
    spin_unlock_irq(&process_lock, flags);
    return a;
}
uint64_t process_set_mask(int i_pid, uint64_t u_mask, bool b_suspend)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(i_pid);
    uint64_t u_was = 0;
    if (p != NULL) {
        u_was = signal_get_mask(&p->sig);
        if (b_suspend) {
            p->sig.u_suspend_mask = u_was;
            p->sig.b_suspended = true;
        }
        signal_set_mask(&p->sig, u_mask);
        p->interrupted = signal_deliverable(&p->sig);
    }
    spin_unlock_irq(&process_lock, u_flags);
    return u_was;
}

int process_take_from(int i_pid, uint64_t u_set,
                      struct signal_origin *p_origin)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(i_pid);
    int i_sig = 0;
    if (p != NULL) {
        uint64_t u_ready = p->sig.pending & u_set;
        for (int i_at = 1; i_at < SIGNAL_MAX && i_sig == 0; i_at++) {
            if (u_ready & SIGNAL_BIT(i_at)) {
                i_sig = i_at;
            }
        }
        if (i_sig != 0) {
            p->sig.pending &= ~SIGNAL_BIT(i_sig);
            *p_origin = p->sig.origin[i_sig];
        }
        p->interrupted = signal_deliverable(&p->sig);
    }
    spin_unlock_irq(&process_lock, u_flags);
    return i_sig;
}

int process_thread(int pid)
{
    const struct process *p = slot_for(pid);
    return p == NULL ? 0 : p->thread_id;
}



void process_set_timer(int i_pid, uint64_t u_value_ms, uint64_t u_every_ms,
                       uint64_t u_now_ms, uint64_t *p_old_value,
                       uint64_t *p_old_every)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(i_pid);
    uint64_t u_left = 0;
    uint64_t u_every = 0;

    if (p != NULL) {
        if (p->alarm_at > u_now_ms) {
            u_left = p->alarm_at - u_now_ms;
        }
        u_every = p->u_alarm_every;
        p->alarm_at = (u_value_ms == 0) ? 0 : u_now_ms + u_value_ms;
        p->u_alarm_every = (u_value_ms == 0) ? 0 : u_every_ms;
    }
    spin_unlock_irq(&process_lock, u_flags);
    if (p_old_value != NULL) {
        *p_old_value = u_left;
    }
    if (p_old_every != NULL) {
        *p_old_every = u_every;
    }
}
void process_check_alarms(uint64_t now_ms)
{
    /*
     * the pids are collected under the lock and signalled without it,
     * because raising one wakes a thread and taking the scheduler's
     * lock inside this one is an ordering nothing else here uses
     */
    int due[MAX_PROCESSES];
    size_t n = 0;

    uint64_t flags = spin_lock_irq(&process_lock);
    for (size_t i = 0; i < MAX_PROCESSES; i++) {
        struct process *p = &table[i];
        if (p->pid != 0 && !p->exited && p->alarm_at != 0
            && now_ms >= p->alarm_at) {
            p->alarm_at = (p->u_alarm_every != 0)
                          ? now_ms + p->u_alarm_every : 0;
            due[n++] = p->pid;
        }
    }
    spin_unlock_irq(&process_lock, flags);

    for (size_t i = 0; i < n; i++) {
        process_signal(due[i], SIGALRM);
    }
}



bool process_interrupt_pending(int pid)
{
    const struct process *p = slot_for(pid);
    return p != NULL && p->interrupted;
}

bool process_take_interrupt(int pid)
{
    /*
     * "was it interrupted, and clear it", which is what every blocking
     * call in this kernel asks and has asked since long before signals.
     * it consumes the *flag* and deliberately not the signal: the
     * signal is taken on the way back to ring 3, where a handler can
     * actually be run.
     *
     * keeping those two separate matters twice over. a read interrupted
     * by SIGINT must still deliver the SIGINT afterwards, or ctrl+c
     * would cancel the read and then vanish. and the flag must be
     * consumed here, because a caller that loops until it clears,
     * several do, spins forever otherwise. exactly that was made
     * mistake first, and the syscall suite hung on a `write` that had
     * nothing to do with signals
     */
    uint64_t flags = spin_lock_irq(&process_lock);
    bool had = false;
    struct process *p = slot_for(pid);
    if (p != NULL && p->interrupted) {
        p->interrupted = false;
        had = true;
    }
    spin_unlock_irq(&process_lock, flags);
    return had;
}



void process_inherit(int i_child, int i_parent)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p_child = slot_for(i_child);
    const struct process *p_parent = slot_for(i_parent);

    if (p_child != NULL && p_parent != NULL) {
        p_child->pgid = p_parent->pgid;
        p_child->i_sid = p_parent->i_sid;
        p_child->uid = p_parent->uid;
        p_child->i_gid = p_parent->i_gid;
        memcpy(p_child->arr_groups, p_parent->arr_groups,
               sizeof p_child->arr_groups);
        p_child->i_groups = p_parent->i_groups;
        p_child->u_umask = p_parent->u_umask;
        memcpy(p_child->arr_limits, p_parent->arr_limits,
               sizeof p_child->arr_limits);
        memcpy(p_child->cwd, p_parent->cwd, sizeof p_child->cwd);
        signal_inherit(&p_child->sig, &p_parent->sig);
    }
    spin_unlock_irq(&process_lock, u_flags);
}

void process_set_uid(int i_pid, int i_uid)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(i_pid);
    if (p != NULL) {
        p->uid = i_uid;
    }
    spin_unlock_irq(&process_lock, u_flags);
}

int process_pgid(int pid)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    int g = (p != NULL) ? p->pgid : -1;
    spin_unlock_irq(&process_lock, flags);
    return g;
}

void process_set_pgid(int pid, int pgid)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    if (p != NULL) {
        p->pgid = pgid;
    }
    spin_unlock_irq(&process_lock, flags);
}

size_t process_group_pids(int i_pgid, int *p_pids, size_t sz_max)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    size_t sz_n = 0;

    for (size_t sz_at = 0; sz_at < MAX_PROCESSES && sz_n < sz_max; sz_at++) {
        if (table[sz_at].pid != 0 && table[sz_at].pgid == i_pgid
            && !table[sz_at].exited) {
            p_pids[sz_n++] = table[sz_at].pid;
        }
    }

    spin_unlock_irq(&process_lock, u_flags);
    return sz_n;
}

const char *process_name(int pid)
{
    const struct process *p = slot_for(pid);
    return (p != NULL) ? p->name : "?";
}

void process_set_name(int i_pid, const char *str_name)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(i_pid);

    if (p != NULL) {
        size_t sz_at = 0;
        while (str_name[sz_at] != '\0' && sz_at < PROC_NAME_MAX - 1) {
            p->name[sz_at] = str_name[sz_at];
            sz_at++;
        }
        p->name[sz_at] = '\0';
    }
    spin_unlock_irq(&process_lock, u_flags);
}

/* whether descriptor i_fd closes when its process runs another program */
static void mark_cloexec(struct process *p, int i_fd, bool b_cloexec)
{
    if (b_cloexec) {
        p->u_cloexec |= 1ull << i_fd;
    } else {
        p->u_cloexec &= ~(1ull << i_fd);
    }
}

struct process *process_lock_slot(int i_pid, uint64_t *p_flags)
{
    *p_flags = spin_lock_irq(&process_lock);
    return slot_for(i_pid);
}

void process_unlock_slot(uint64_t u_flags)
{
    spin_unlock_irq(&process_lock, u_flags);
}

int process_fd_cloexec(int i_pid, int i_fd, int i_set)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(i_pid);
    int i_was = -EBADF;

    if (p != NULL && i_fd >= 0 && i_fd < MAX_FDS && p->fds[i_fd] != NULL) {
        i_was = (p->u_cloexec >> i_fd) & 1;
        if (i_set >= 0) {
            mark_cloexec(p, i_fd, i_set != 0);
        }
    }
    spin_unlock_irq(&process_lock, u_flags);
    return i_was;
}

/* how many descriptors p may have: its limit, never past the table */
static int fd_limit(const struct process *p)
{
    uint64_t u_limit = p->arr_limits[RLIMIT_NOFILE].rlim_cur;
    return (u_limit < MAX_FDS) ? (int)u_limit : MAX_FDS;
}

int process_fd_limit(int i_pid)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(i_pid);
    int i_limit = (p != NULL) ? fd_limit(p) : 0;
    spin_unlock_irq(&process_lock, u_flags);
    return i_limit;
}

int process_fd_add(int i_pid, struct file *p_file, int i_from,
                   bool b_cloexec)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(i_pid);
    int i_fd = -EMFILE;

    if (p != NULL && i_from >= 0) {
        for (int i_at = i_from; i_at < fd_limit(p); i_at++) {
            if (p->fds[i_at] == NULL) {
                p->fds[i_at] = p_file;
                mark_cloexec(p, i_at, b_cloexec);
                i_fd = i_at;
                break;
            }
        }
    }
    spin_unlock_irq(&process_lock, u_flags);
    return i_fd;
}

struct file *process_fd_file(int i_pid, int i_fd)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(i_pid);
    struct file *p_file = NULL;

    if (p != NULL && i_fd >= 0 && i_fd < MAX_FDS) {
        p_file = p->fds[i_fd];
        if (p_file != NULL) {
            file_get(p_file);
        }
    }
    spin_unlock_irq(&process_lock, u_flags);
    return p_file;
}

bool process_fd_replace(int i_pid, int i_fd, struct file *p_file,
                        bool b_cloexec, struct file **pp_old)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(i_pid);
    bool b_done = false;

    *pp_old = NULL;
    if (p != NULL && i_fd >= 0 && i_fd < MAX_FDS) {
        *pp_old = p->fds[i_fd];
        p->fds[i_fd] = p_file;
        mark_cloexec(p, i_fd, b_cloexec);
        b_done = true;
    }
    spin_unlock_irq(&process_lock, u_flags);
    return b_done;
}

struct file *process_fd_take(int i_pid, int i_fd)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(i_pid);
    struct file *p_file = NULL;

    if (p != NULL && i_fd >= 0 && i_fd < MAX_FDS) {
        p_file = p->fds[i_fd];
        p->fds[i_fd] = NULL;
        mark_cloexec(p, i_fd, false);
    }
    spin_unlock_irq(&process_lock, u_flags);
    return p_file;
}

bool process_fds_inherit(int pid, int from)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p_child = slot_for(pid);
    struct process *p_parent = slot_for(from);
    bool b_ok = (p_child != NULL && p_parent != NULL);

    if (b_ok) {
        for (int i_at = 0; i_at < MAX_FDS; i_at++) {
            p_child->fds[i_at] = p_parent->fds[i_at];
            if (p_child->fds[i_at] != NULL) {
                file_get(p_child->fds[i_at]);
            }
        }
        p_child->u_cloexec = p_parent->u_cloexec;
    }
    spin_unlock_irq(&process_lock, u_flags);
    return b_ok;
}

/*
 * the descriptors in u_which taken out under the lock and their files put
 * after it, since putting the last reference to a pipe's end wakes
 * whoever waits at the other one, which reaches the scheduler
 */
static void close_fds(int i_pid, uint64_t u_which)
{
    struct file *arr_files[MAX_FDS];
    size_t sz_count = 0;

    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(i_pid);
    if (p != NULL) {
        for (int i_at = 0; i_at < MAX_FDS; i_at++) {
            if ((u_which & (1ull << i_at)) != 0 && p->fds[i_at] != NULL) {
                arr_files[sz_count++] = p->fds[i_at];
                p->fds[i_at] = NULL;
                mark_cloexec(p, i_at, false);
            }
        }
    }
    spin_unlock_irq(&process_lock, u_flags);

    for (size_t sz_at = 0; sz_at < sz_count; sz_at++) {
        file_put(arr_files[sz_at]);
    }
}

void process_fds_release(int i_pid)
{
    close_fds(i_pid, ~0ull);
}

void process_fds_exec(int i_pid)
{
    const struct process *p = slot_for(i_pid);
    if (p != NULL) {
        close_fds(i_pid, p->u_cloexec);
    }
}

bool process_any_file(bool (*p_test)(const struct file *p_file,
                                      const void *p_ctx),
                      const void *p_ctx)
{
    bool b_any = false;
    uint64_t u_flags = spin_lock_irq(&process_lock);
    for (size_t i = 0; i < MAX_PROCESSES && !b_any; i++) {
        if (table[i].pid == 0) {
            continue;
        }
        for (int i_fd = 0; i_fd < MAX_FDS && !b_any; i_fd++) {
            const struct file *p_file = table[i].fds[i_fd];
            b_any = p_file != NULL && p_test(p_file, p_ctx);
        }
    }
    spin_unlock_irq(&process_lock, u_flags);
    return b_any;
}

bool process_any_cwd(bool (*p_test)(const char *str_cwd, const void *p_ctx),
                     const void *p_ctx)
{
    bool b_any = false;
    uint64_t u_flags = spin_lock_irq(&process_lock);
    for (size_t i = 0; i < MAX_PROCESSES && !b_any; i++) {
        if (table[i].pid != 0) {
            b_any = p_test((table[i].cwd[0] != '\0') ? table[i].cwd : "/",
                           p_ctx);
        }
    }
    spin_unlock_irq(&process_lock, u_flags);
    return b_any;
}

void process_map_paths(void (*p_map)(char *str_path, bool b_place,
                                     const void *p_ctx),
                       const void *p_ctx)
{
    /* a file shared by many descriptors is marked, so it is done once */
    static uint32_t s_u_pass;

    uint64_t u_flags = spin_lock_irq(&process_lock);
    s_u_pass++;
    for (size_t i = 0; i < MAX_PROCESSES; i++) {
        struct process *p = &table[i];
        if (p->pid == 0) {
            continue;
        }
        if (p->cwd[0] != '\0') {
            p_map(p->cwd, true, p_ctx);
        }
        if (p->arr_root[0] != '\0') {
            p_map(p->arr_root, true, p_ctx);
        }
        for (int i_fd = 0; i_fd < MAX_FDS; i_fd++) {
            struct file *p_file = p->fds[i_fd];
            if (p_file != NULL && p_file->u_pass != s_u_pass) {
                p_file->u_pass = s_u_pass;
                if (p_file->arr_path[0] != '\0') {
                    p_map(p_file->arr_path, false, p_ctx);
                }
            }
        }
    }
    spin_unlock_irq(&process_lock, u_flags);
}

size_t process_fd_count(int pid)
{
    size_t n = 0;
    const struct process *p = slot_for(pid);
    if (p != NULL) {
        for (int i = 0; i < MAX_FDS; i++) {
            if (p->fds[i] != NULL) {
                n++;
            }
        }
    }
    return n;
}
