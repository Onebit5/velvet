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
        p->uid        = uid;
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
void process_force_signal(int i_pid, int i_sig)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(i_pid);
    if (p != NULL && !p->exited && i_sig > 0 && i_sig < SIGNAL_MAX) {
        uint64_t u_bit = SIGNAL_BIT(i_sig);
        if (p->sig.handler[i_sig] == SIG_IGNORE
            || (p->sig.blocked & u_bit) != 0) {
            p->sig.handler[i_sig] = SIG_DEFAULT;
            p->sig.blocked &= ~u_bit;
        }
        signal_raise(&p->sig, i_sig);
        p->interrupted = true;
    }
    spin_unlock_irq(&process_lock, u_flags);
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



void process_inherit(int i_child, int i_parent)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p_child = slot_for(i_child);
    const struct process *p_parent = slot_for(i_parent);

    if (p_child != NULL && p_parent != NULL) {
        p_child->pgid = p_parent->pgid;
        memcpy(p_child->cwd, p_parent->cwd, sizeof p_child->cwd);
        memcpy(p_child->env, p_parent->env, sizeof p_child->env);
        p_child->env_len = p_parent->env_len;
        signal_inherit(&p_child->sig, &p_parent->sig);
        p_child->sig_trampoline = p_parent->sig_trampoline;
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

/*
 * the block itself is pure arithmetic over a run of strings and lives
 * in lib/env.c, where a suite can hold it to what "already set" means
 * without a process table to stand it in
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

/* whether descriptor i_fd closes when its process runs another program */
static void mark_cloexec(struct process *p, int i_fd, bool b_cloexec)
{
    if (b_cloexec) {
        p->u_cloexec |= 1ull << i_fd;
    } else {
        p->u_cloexec &= ~(1ull << i_fd);
    }
}

int process_fd_add(int i_pid, struct file *p_file, int i_from,
                   bool b_cloexec)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(i_pid);
    int i_fd = -EMFILE;

    if (p != NULL && i_from >= 0) {
        for (int i_at = i_from; i_at < MAX_FDS; i_at++) {
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
