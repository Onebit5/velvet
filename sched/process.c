// SPDX-License-Identifier: GPL-2.0-only
/*
 * sched/process.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the process table.
 */

#include <velvet/process.h>
#include <asm/irq.h>
#include <velvet/spinlock.h>
#include <velvet/pipe.h>
#include <velvet/string.h>
#include <velvet/sched.h>
#include <uapi/velvet/syscall.h>

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

const char *process_cwd(int pid)
{
    struct process *p = slot_for(pid);
    return (p != NULL && p->cwd[0] != '\0') ? p->cwd : "/";
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
        p->uid        = uid;
        p->announce   = announce;
        signal_reset(&p->sig);
        p->started_ms = now_ms;

        /* the three every process is born with. */
        p->fds[FD_STDIN].kind  = FD_KEYBOARD;
        p->fds[FD_STDOUT].kind = FD_CONSOLE;
        p->fds[FD_STDERR].kind = FD_CONSOLE;

        /* an empty environment rather than no environment. */
        p->env[0] = '\0';
        p->env_len = 1;

        /* wherever the parent was standing. */
        const char *from = process_cwd(parent);
        size_t c = 0;
        while (from[c] != '\0' && c < PATH_MAX - 1) {
            p->cwd[c] = from[c];
            c++;
        }
        p->cwd[c] = '\0';

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

int process_take_fatal(int i_pid)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(i_pid);
    int i_found = 0;

    if (p != NULL) {
        uint32_t u_ready = p->sig.pending & ~p->sig.blocked;
        for (int i_sig = 1; i_sig < SIGNAL_MAX && i_found == 0; i_sig++) {
            if ((u_ready & (1u << i_sig))
                && signal_action_for(&p->sig, i_sig)
                   == SIGNAL_ACTION_TERMINATE) {
                p->sig.pending &= ~(1u << i_sig);
                i_found = i_sig;
            }
        }
        p->interrupted = signal_deliverable(&p->sig);
    }
    spin_unlock_irq(&process_lock, u_flags);
    return i_found;
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
    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    /*
     * the first answer is the true one: a process killed while it was
     * already on its way out should not have its code overwritten
     */
    if (p != NULL && !p->exited) {
        p->exited    = true;
        p->exit_code = code;
        p->ended_ms  = now_ms;
        p->thread_id = 0;
        /*
         * whatever it had open closes with it, except a pipe. closing
         * one wakes whoever waits at the other end, which reaches the
         * scheduler, and this lock may not be held while doing that. so
         * a pipe end stays in its slot, and pipe_release_for, which
         * every way a process ends calls after this, takes it out and
         * closes it with no lock held. clearing it here instead was how
         * a reader came to wait forever on a writer already gone
         */
        for (size_t f = 0; f < MAX_FDS; f++) {
            if (p->fds[f].kind != FD_PIPE) {
                p->fds[f].kind = FD_FREE;
            }
        }

        /* and whatever it was the parent of is init's now. */
        for (size_t j = 0; j < MAX_PROCESSES; j++) {
            if (table[j].pid != 0 && table[j].parent == pid) {
                table[j].parent = INIT_PID;
            }
        }
    }
    spin_unlock_irq(&process_lock, flags);
}

bool process_collect(int pid, int *code)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    bool collected = false;

    struct process *p = slot_for(pid);
    if (p != NULL && p->exited) {
        if (code != NULL) {
            *code = p->exit_code;
        }
        p->pid = 0;         /* the slot is free again */
        collected = true;
    }

    spin_unlock_irq(&process_lock, flags);
    return collected;
}



/* is there still a process with this pid? */
static bool present(int pid)
{
    return pid != 0 && slot_for(pid) != NULL;
}

int process_orphan(void)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    int pid = 0;

    for (size_t i = 0; i < MAX_PROCESSES; i++) {
        struct process *p = &table[i];
        if (p->pid == 0 || !p->exited) {
            continue;
        }
        /* parent 0 is a kernel shell's, and the shell collects its own. */
        if (p->parent == 0) {
            continue;
        }
        /*
         * whose parent is init, or whose parent is not in the table at
         * all, has nobody else who could
         */
        if (p->parent == INIT_PID || !present(p->parent)) {
            pid = p->pid;
            break;
        }
    }

    spin_unlock_irq(&process_lock, flags);
    return pid;
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
        signal_raise(&table[i].sig, SIGINT);
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
    uint32_t u_bit = 1u << i_sig;
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
        p->i_stopped_by = 0;
        return -1;
    }
    return 0;
}

bool process_signal(int pid, int sig)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    bool ok = false;
    int i_become = 0;
    if (p != NULL && !p->exited) {
        ok = signal_raise(&p->sig, sig);
        if (ok) {
            i_become = settle_now(p, sig);
        }

        /* `interrupted` is now derived. */
        /*
         * deliberately whether anything is *pending and unblocked*,
         * rather than whether it will be acted on. an ignored signal
         * still cuts a blocking call short here, which unix does not
         * do, and it is kept because `interrupted` predates signals
         * in this kernel and answers a different question: "stop
         * waiting", not "what should become of this process"
         */
        p->interrupted = signal_deliverable(&p->sig);
    }
    int i_thread = (p != NULL) ? p->thread_id : 0;
    bool b_wake = ok && p != NULL && p->interrupted;
    spin_unlock_irq(&process_lock, flags);

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

void process_stopped(int i_pid, int i_sig)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(i_pid);
    if (p != NULL) {
        p->i_stopped_by = i_sig;
        p->b_stop_told = false;
    }
    spin_unlock_irq(&process_lock, u_flags);
}

int process_reportable(int i_parent, int i_pid, bool b_untraced,
                       int *p_status, bool *p_ended)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    int i_found = -1;

    for (size_t sz_at = 0; sz_at < MAX_PROCESSES; sz_at++) {
        struct process *p = &table[sz_at];
        if (p->pid == 0 || p->parent != i_parent
            || (i_pid != -1 && p->pid != i_pid)) {
            continue;
        }
        i_found = 0;
        if (p->exited) {
            *p_status = p->exit_code;
            *p_ended = true;
            i_found = p->pid;
            break;
        }
        if (b_untraced && p->i_stopped_by != 0 && !p->b_stop_told) {
            p->b_stop_told = true;
            *p_status = WAIT_STOPPED | p->i_stopped_by;
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
                                       uint64_t *handler_out)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    enum signal_action a = SIGNAL_ACTION_NONE;

    if (p != NULL) {
        int sig = signal_next(&p->sig);
        if (sig != 0) {
            a = signal_take(&p->sig, sig, handler_out);
            if (sig_out != NULL) {
                *sig_out = sig;
            }
        }
        p->interrupted = signal_deliverable(&p->sig);
    }
    spin_unlock_irq(&process_lock, flags);
    return a;
}

int process_thread(int pid)
{
    const struct process *p = slot_for(pid);
    return p == NULL ? 0 : p->thread_id;
}

void process_set_trampoline(int pid, uint64_t at)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    if (p != NULL) {
        p->sig_trampoline = at;
    }
    spin_unlock_irq(&process_lock, flags);
}

uint64_t process_trampoline(int pid)
{
    const struct process *p = slot_for(pid);
    return p == NULL ? 0 : p->sig_trampoline;
}

uint64_t process_set_alarm(int pid, uint64_t seconds, uint64_t now_ms)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    uint64_t left = 0;

    if (p != NULL) {
        if (p->alarm_at > now_ms) {
            left = (p->alarm_at - now_ms + 999) / 1000;
        }
        p->alarm_at = (seconds == 0) ? 0 : now_ms + seconds * 1000;
    }
    spin_unlock_irq(&process_lock, flags);
    return left;
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
            p->alarm_at = 0;    /* an alarm fires once */
            due[n++] = p->pid;
        }
    }
    spin_unlock_irq(&process_lock, flags);

    for (size_t i = 0; i < n; i++) {
        process_signal(due[i], SIGALRM);
    }
}

void process_died_by_signal(int pid, int sig)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    if (p != NULL) {
        p->killed_by = sig;
    }
    spin_unlock_irq(&process_lock, flags);
}

int process_killed_by(int pid)
{
    const struct process *p = slot_for(pid);
    return p == NULL ? 0 : p->killed_by;
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

size_t process_group_threads(int pgid, int *ids, size_t max)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    size_t n = 0;

    for (size_t i = 0; i < MAX_PROCESSES && n < max; i++) {
        if (table[i].pid == 0 || table[i].pgid != pgid) {
            continue;
        }
        if (table[i].exited || table[i].thread_id == 0) {
            continue;
        }
        ids[n++] = table[i].thread_id;
    }

    spin_unlock_irq(&process_lock, flags);
    return n;
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

bool process_group_alive(int pgid)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    bool alive = false;

    for (size_t i = 0; i < MAX_PROCESSES; i++) {
        if (table[i].pid != 0 && table[i].pgid == pgid && !table[i].exited) {
            alive = true;
            break;
        }
    }

    spin_unlock_irq(&process_lock, flags);
    return alive;
}

void process_interrupt_group(int pgid)
{
    /*
     * one ctrl+c, the whole job, said in signals now, which is what
     * makes it wake a program that is blocked rather than only marking
     * it for the next time it happens to ask
     */
    process_signal_group(pgid, SIGINT);
}



int process_fd_open(int pid, const void *data, uint64_t size)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    int fd = -1;

    struct process *p = slot_for(pid);
    if (p != NULL) {
        for (int i = FD_FIRST_FILE; i < MAX_FDS; i++) {
            if (p->fds[i].kind != FD_FREE) {
                continue;
            }
            memset(&p->fds[i], 0, sizeof p->fds[i]);
            p->fds[i].kind = FD_MEMORY;
            p->fds[i].data = data;
            p->fds[i].size = size;
            fd = i;
            break;
        }
    }

    spin_unlock_irq(&process_lock, flags);
    return fd;
}

int process_fd_open_disk(int pid, uint32_t cluster, uint64_t size,
                         uint64_t entry_sector, uint32_t entry_offset,
                         size_t mount)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    int fd = -1;

    struct process *p = slot_for(pid);
    if (p != NULL) {
        for (int i = FD_FIRST_FILE; i < MAX_FDS; i++) {
            if (p->fds[i].kind != FD_FREE) {
                continue;
            }
            memset(&p->fds[i], 0, sizeof p->fds[i]);
            p->fds[i].kind = FD_DISK;
            p->fds[i].cluster = cluster;
            p->fds[i].entry_sector = entry_sector;
            p->fds[i].entry_offset = entry_offset;
            p->fds[i].mount = mount;
            p->fds[i].size = size;
            fd = i;
            break;
        }
    }

    spin_unlock_irq(&process_lock, flags);
    return fd;
}

int process_fd_open_source(int pid, uint64_t medium_at, uint64_t size)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    int fd = -1;

    struct process *p = slot_for(pid);
    if (p != NULL) {
        for (int i = FD_FIRST_FILE; i < MAX_FDS; i++) {
            if (p->fds[i].kind != FD_FREE) {
                continue;
            }
            memset(&p->fds[i], 0, sizeof p->fds[i]);
            p->fds[i].kind = FD_SOURCE;
            p->fds[i].medium_at = medium_at;
            p->fds[i].size = size;
            fd = i;
            break;
        }
    }

    spin_unlock_irq(&process_lock, flags);
    return fd;
}

int process_fd_open_proc(int i_pid, size_t sz_proc)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    int i_fd = -1;

    struct process *p_proc = slot_for(i_pid);
    if (p_proc != NULL) {
        for (int i_at = FD_FIRST_FILE; i_at < MAX_FDS; i_at++) {
            if (p_proc->fds[i_at].kind != FD_FREE) {
                continue;
            }
            memset(&p_proc->fds[i_at], 0, sizeof p_proc->fds[i_at]);
            p_proc->fds[i_at].kind = FD_PROC;
            p_proc->fds[i_at].sz_proc = sz_proc;
            i_fd = i_at;
            break;
        }
    }

    spin_unlock_irq(&process_lock, u_flags);
    return i_fd;
}

bool process_fd_disk(int pid, int fd, struct fd_disk *out)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    bool ok = false;

    struct process *p = slot_for(pid);
    if (p != NULL && fd >= 0 && fd < MAX_FDS && p->fds[fd].kind == FD_DISK) {
        struct fd *f = &p->fds[fd];
        out->cluster = f->cluster;
        out->size = f->size;
        out->pos = f->pos;
        out->remaining = (f->pos < f->size) ? f->size - f->pos : 0;
        out->entry_sector = f->entry_sector;
        out->entry_offset = f->entry_offset;
        out->mount = f->mount;
        ok = true;
    }

    spin_unlock_irq(&process_lock, flags);
    return ok;
}

bool process_fd_source(int pid, int fd, struct fd_source *out)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    bool ok = false;

    struct process *p = slot_for(pid);
    if (p != NULL && fd >= 0 && fd < MAX_FDS && p->fds[fd].kind == FD_SOURCE) {
        struct fd *f = &p->fds[fd];
        out->medium_at = f->medium_at;
        out->size = f->size;
        out->pos = f->pos;
        out->remaining = (f->pos < f->size) ? f->size - f->pos : 0;
        ok = true;
    }

    spin_unlock_irq(&process_lock, flags);
    return ok;
}

void process_fd_grew(int pid, int fd, uint32_t cluster, uint64_t size)
{
    uint64_t flags = spin_lock_irq(&process_lock);

    struct process *p = slot_for(pid);
    if (p != NULL && fd >= 0 && fd < MAX_FDS && p->fds[fd].kind == FD_DISK) {
        p->fds[fd].cluster = cluster;
        p->fds[fd].size = size;
    }

    spin_unlock_irq(&process_lock, flags);
}

bool process_fd_peek(int pid, int fd, const void **data, uint64_t *remaining)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    bool ok = false;

    struct process *p = slot_for(pid);
    if (p != NULL && fd >= 0 && fd < MAX_FDS && p->fds[fd].kind == FD_MEMORY) {
        struct fd *f = &p->fds[fd];
        if (data != NULL) {
            *data = f->data + f->pos;
        }
        if (remaining != NULL) {
            *remaining = f->size - f->pos;
        }
        ok = true;
    }

    spin_unlock_irq(&process_lock, flags);
    return ok;
}

void process_fd_advance(int pid, int fd, uint64_t n)
{
    uint64_t flags = spin_lock_irq(&process_lock);

    struct process *p = slot_for(pid);
    if (p != NULL && fd >= 0 && fd < MAX_FDS && p->fds[fd].kind != FD_FREE) {
        /*
         * clamped to the size, which is right for both directions: a
         * read cannot go past the end, and a write has already had the
         * new size recorded by process_fd_grew before this runs
         */
        struct fd *f = &p->fds[fd];
        if (f->kind == FD_PROC) {
            f->pos += n;        /* no size to stop at: the read finds the end */
        } else {
            f->pos = (f->pos + n > f->size) ? f->size : f->pos + n;
        }
    }

    spin_unlock_irq(&process_lock, flags);
}

/*
 * the block itself is pure arithmetic over a run of strings and lives
 * in lib/env.c, because the shell needs exactly the same operations and
 * has no process to perform them on, it is a kernel thread, so its
 * environment lives in its session. one implementation, two callers, and
 * no chance of them disagreeing about what "already set" means
 */

bool process_set_env(int pid, const char *block, size_t len)
{
    if (len > PROC_ENV_MAX) {
        return false;
    }
    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    if (p != NULL) {
        memcpy(p->env, block, len);
        p->env_len = len;
    }
    spin_unlock_irq(&process_lock, flags);
    return p != NULL;
}

size_t process_get_env(int pid, char *out, size_t max)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    size_t n = 0;
    if (p != NULL && p->env_len <= max) {
        memcpy(out, p->env, p->env_len);
        n = p->env_len;
    }
    spin_unlock_irq(&process_lock, flags);

    if (n == 0 && max > 0) {
        out[0] = '\0';     /* an empty environment is still an environment */
        n = 1;
    }
    return n;
}

bool process_env_get(int pid, const char *name, char *out, size_t max)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    bool ok = false;
    if (p != NULL) {
        ok = env_block_get(p->env, p->env_len, name, out, max);
    }
    spin_unlock_irq(&process_lock, flags);
    return ok;
}

bool process_env_set(int pid, const char *name, const char *value)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    bool ok = false;
    if (p != NULL) {
        if (p->env_len == 0) {
            p->env[0] = '\0';
            p->env_len = 1;
        }
        ok = env_block_set(p->env, &p->env_len, PROC_ENV_MAX, name, value);
    }
    spin_unlock_irq(&process_lock, flags);
    return ok;
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

bool process_fds_inherit(int pid, int from)
{
    /* copied out under the lock and the pipes referenced after it. */
    struct fd copy[MAX_FDS];

    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *child = slot_for(pid);
    struct process *parent = slot_for(from);
    bool ok = (child != NULL && parent != NULL);
    if (ok) {
        for (int i = 0; i < MAX_FDS; i++) {
            copy[i] = parent->fds[i];
            child->fds[i] = copy[i];
        }
    }
    spin_unlock_irq(&process_lock, flags);

    if (!ok) {
        return false;
    }
    for (int i = 0; i < MAX_FDS; i++) {
        if (copy[i].kind == FD_PIPE) {
            pipe_share(copy[i].pipe, copy[i].writing);
        }
    }
    return true;
}

bool process_fd_get(int pid, int fd, struct fd *out)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    bool ok = false;

    struct process *p = slot_for(pid);
    if (p != NULL && fd >= 0 && fd < MAX_FDS && p->fds[fd].kind != FD_FREE) {
        *out = p->fds[fd];
        ok = true;
    }

    spin_unlock_irq(&process_lock, flags);
    return ok;
}

int process_fd_place(int i_pid, const struct fd *p_src)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    int i_fd = -1;

    struct process *p = slot_for(i_pid);
    if (p != NULL) {
        for (int i_at = FD_FIRST_FILE; i_at < MAX_FDS; i_at++) {
            if (p->fds[i_at].kind == FD_FREE) {
                p->fds[i_at] = *p_src;
                i_fd = i_at;
                break;
            }
        }
    }

    spin_unlock_irq(&process_lock, u_flags);
    return i_fd;
}

bool process_fd_install(int pid, int fd, const struct fd *src)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    bool ok = false;

    struct process *p = slot_for(pid);
    if (p != NULL && fd >= 0 && fd < MAX_FDS) {
        p->fds[fd] = *src;
        ok = true;
    }

    spin_unlock_irq(&process_lock, flags);
    return ok;
}

size_t process_take_pipes(int pid, struct pipe_end *out, size_t max)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    size_t n = 0;

    struct process *p = slot_for(pid);
    if (p != NULL) {
        for (int i = 0; i < MAX_FDS && n < max; i++) {
            if (p->fds[i].kind != FD_PIPE) {
                continue;
            }
            out[n].p = p->fds[i].pipe;
            out[n].writing = p->fds[i].writing;
            n++;
            p->fds[i].kind = FD_FREE;
            p->fds[i].pipe = NULL;
        }
    }

    spin_unlock_irq(&process_lock, flags);
    return n;
}

bool process_fd_close(int pid, int fd, struct pipe_end *closing)
{
    if (closing != NULL) {
        closing->p = NULL;
    }
    uint64_t flags = spin_lock_irq(&process_lock);
    bool closed = false;

    struct process *p = slot_for(pid);
    if (p != NULL && fd >= 0 && fd < MAX_FDS && p->fds[fd].kind != FD_FREE) {
        /* a pipe cannot be let go of here, that wakes threads, and this is holding the table. */
        if (p->fds[fd].kind == FD_PIPE && closing != NULL) {
            closing->p = p->fds[fd].pipe;
            closing->writing = p->fds[fd].writing;
        }
        p->fds[fd].kind = FD_FREE;
        p->fds[fd].pipe = NULL;
        closed = true;
    }

    spin_unlock_irq(&process_lock, flags);
    return closed;
}

size_t process_fd_count(int pid)
{
    size_t n = 0;
    const struct process *p = slot_for(pid);
    if (p != NULL) {
        for (int i = FD_FIRST_FILE; i < MAX_FDS; i++) {
            if (p->fds[i].kind != FD_FREE) {
                n++;
            }
        }
    }
    return n;
}
