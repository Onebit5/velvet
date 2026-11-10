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
#include <velvet/tty.h>
#include <uapi/velvet/wait.h>

/* a fixed table rather than a linked list of allocations. */
/* the process table and every descriptor in it. */
static struct spinlock process_lock = SPINLOCK("process", LOCK_RANK_PROCESS);

static struct process table[MAX_PROCESSES];
static int next_pid = 1;

/* and every process's threads, under the same lock */
static struct task s_arr_tasks[MAX_THREADS];

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

static struct task *task_for(int i_tid)
{
    if (i_tid <= 0) {
        return NULL;
    }
    for (size_t sz_i = 0; sz_i < MAX_THREADS; sz_i++) {
        if (s_arr_tasks[sz_i].i_tid == i_tid) {
            return &s_arr_tasks[sz_i];
        }
    }
    return NULL;
}

/* a free slot for a thread, or NULL. the lock is held */
static struct task *free_task(void)
{
    for (size_t sz_i = 0; sz_i < MAX_THREADS; sz_i++) {
        if (s_arr_tasks[sz_i].i_tid == 0) {
            return &s_arr_tasks[sz_i];
        }
    }
    return NULL;
}

/*
 * what every thread of p blocks, which is what the process as a whole
 * has blocked: a signal raised at it waits only when there is no thread
 * of it that could take it now. the lock is held
 */
static uint64_t blocked_by_all(const struct process *p)
{
    uint64_t u_all = ~0ull;
    bool b_any = false;
    for (size_t sz_i = 0; sz_i < MAX_THREADS; sz_i++) {
        const struct task *t = &s_arr_tasks[sz_i];
        if (t->i_tid != 0 && t->i_pid == p->pid) {
            u_all &= t->sig.u_blocked;
            b_any = true;
        }
    }
    return b_any ? u_all : 0;
}

/*
 * the thread of p that a signal raised at the process as a whole is
 * for: its first thread when that one does not block it, as a program
 * with one thread taking signals and the rest blocking them expects,
 * and otherwise the first that does not. NULL when every one blocks it,
 * and it waits for whichever unblocks it first. the lock is held
 */
static struct task *taker_of(const struct process *p, int i_sig)
{
    struct task *p_main = task_for(p->pid);
    uint64_t u_bit = SIGNAL_BIT(i_sig);
    if (p_main != NULL && !(p_main->sig.u_blocked & u_bit)) {
        return p_main;
    }
    for (size_t sz_i = 0; sz_i < MAX_THREADS; sz_i++) {
        struct task *t = &s_arr_tasks[sz_i];
        if (t->i_tid != 0 && t->i_pid == p->pid
            && !(t->sig.u_blocked & u_bit)) {
            return t;
        }
    }
    return NULL;
}

/*
 * whether t has a signal to look at now, which a doomed thread always
 * has, so a wait it is in ends. the lock is held
 */
static void recalc(struct task *t)
{
    const struct process *p = slot_for(t->i_pid);
    t->b_interrupted = t->b_doomed
                       || (p != NULL && signal_deliverable(&p->sig, &t->sig));
}

int current_pid(void)
{
    struct thread *p_me = sched_current();
    return (p_me != NULL) ? p_me->pid : 0;
}

int current_tid(void)
{
    struct thread *p_me = sched_current();
    return (p_me != NULL) ? p_me->i_tid : 0;
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

    /* a process is never without its first thread, so it needs room for one */
    struct task *t = free_task();
    for (size_t i = 0; i < MAX_PROCESSES && t != NULL; i++) {
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

        memset(t, 0, sizeof *t);
        t->i_tid = p->pid;
        t->i_pid = p->pid;
        signal_reset_thread(&t->sig);

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
        && (p_parent->sig.arr_action[SIGCHLD].sa_flags & SA_NOCLDSTOP)) {
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

/*
 * a group is orphaned when none of its members has a parent in another
 * group of the same session: there is nobody left with job control over
 * it, nobody who could put it in front or continue it. one with no
 * members left is not a group at all. the lock is held
 */
static bool group_orphaned(int i_pgid)
{
    bool b_any = false;
    for (size_t sz_i = 0; sz_i < MAX_PROCESSES; sz_i++) {
        const struct process *p = &table[sz_i];
        if (p->pid == 0 || p->exited || p->pgid != i_pgid) {
            continue;
        }
        b_any = true;
        const struct process *p_parent = slot_for(p->parent);
        if (p_parent != NULL && !p_parent->exited
            && p_parent->pgid != i_pgid && p_parent->i_sid == p->i_sid) {
            return false;
        }
    }
    return b_any;
}

/* whether any of a group is stopped. the lock is held */
static bool group_stopped(int i_pgid)
{
    for (size_t sz_i = 0; sz_i < MAX_PROCESSES; sz_i++) {
        const struct process *p = &table[sz_i];
        if (p->pid != 0 && !p->exited && p->pgid == i_pgid
            && p->i_stopped_by != 0) {
            return true;
        }
    }
    return false;
}

bool process_group_orphaned(int i_pgid)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    bool b_orphaned = group_orphaned(i_pgid);
    spin_unlock_irq(&process_lock, u_flags);
    return b_orphaned;
}

/*
 * the groups p's end may orphan, into p_out: its own, when its parent
 * is in another group of the session and so may have been all that kept
 * it from being orphaned, and each of its children's that is another
 * group of the session, since the child goes to init. how many. the
 * lock is held, and p has not ended yet
 */
static size_t orphan_candidates(const struct process *p, int *p_out)
{
    size_t sz_count = 0;
    const struct process *p_parent = slot_for(p->parent);
    if (p_parent != NULL && p_parent->pgid != p->pgid
        && p_parent->i_sid == p->i_sid) {
        p_out[sz_count++] = p->pgid;
    }
    for (size_t sz_i = 0; sz_i < MAX_PROCESSES; sz_i++) {
        const struct process *p_child = &table[sz_i];
        if (p_child->pid == 0 || p_child->exited
            || p_child->parent != p->pid || p_child->pgid == p->pgid
            || p_child->i_sid != p->i_sid) {
            continue;
        }
        bool b_seen = false;
        for (size_t sz_at = 0; sz_at < sz_count; sz_at++) {
            b_seen |= p_out[sz_at] == p_child->pgid;
        }
        if (!b_seen) {
            p_out[sz_count++] = p_child->pgid;
        }
    }
    return sz_count;
}

/*
 * of those, the ones that are orphaned now with a member stopped, kept
 * in place: how many. the lock is held
 */
static size_t still_stopped(int *p_groups, size_t sz_count)
{
    size_t sz_kept = 0;
    for (size_t sz_at = 0; sz_at < sz_count; sz_at++) {
        if (group_orphaned(p_groups[sz_at])
            && group_stopped(p_groups[sz_at])) {
            p_groups[sz_kept++] = p_groups[sz_at];
        }
    }
    return sz_kept;
}

/* a parent that has said it will never wait for its children */
static bool parent_detaches(const struct process *p)
{
    const struct process *p_parent = slot_for(p->parent);
    if (p_parent == NULL) {
        return false;
    }
    const struct sigaction *p_act = &p_parent->sig.arr_action[SIGCHLD];
    return p_act->sa_handler == SIG_IGNORE
           || (p_act->sa_flags & SA_NOCLDWAIT) != 0;
}

void process_set_thread(int i_tid, int i_thread)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct task *t = task_for(i_tid);
    if (t != NULL) {
        t->i_thread = i_thread;
    }
    spin_unlock_irq(&process_lock, u_flags);
}

int process_add_thread(int i_from)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    const struct task *p_from = task_for(i_from);
    const struct process *p = (p_from != NULL) ? slot_for(p_from->i_pid)
                                               : NULL;
    struct task *t = free_task();
    int i_tid = 0;
    if (t != NULL && p != NULL && !p->exited && !p->b_group_exit
        && !p_from->b_doomed) {
        memset(t, 0, sizeof *t);
        t->i_tid = next_pid++;
        t->i_pid = p->pid;
        signal_inherit_thread(&t->sig, &p_from->sig, false);
        i_tid = t->i_tid;
    }
    spin_unlock_irq(&process_lock, u_flags);
    return i_tid;
}

/* how many threads p has. the lock is held */
static size_t count_threads(const struct process *p)
{
    size_t sz_count = 0;
    for (size_t sz_i = 0; sz_i < MAX_THREADS; sz_i++) {
        if (s_arr_tasks[sz_i].i_tid != 0
            && s_arr_tasks[sz_i].i_pid == p->pid) {
            sz_count++;
        }
    }
    return sz_count;
}

size_t process_thread_count(int i_pid)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    const struct process *p = slot_for(i_pid);
    size_t sz_count = (p != NULL) ? count_threads(p) : 0;
    spin_unlock_irq(&process_lock, u_flags);
    return sz_count;
}

bool process_thread_ended(int i_tid, int i_code, uint64_t u_now_ms)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct task *t = task_for(i_tid);
    struct process *p = (t != NULL) ? slot_for(t->i_pid) : NULL;
    bool b_last = false;
    int i_status = 0;
    if (t != NULL) {
        if (p != NULL && !p->b_group_exit && t->i_tid == p->pid) {
            p->i_wait_status = i_code;
        }
        t->i_tid = 0;
        b_last = p != NULL && !p->exited && count_threads(p) == 0;
        i_status = (p != NULL) ? p->i_wait_status : 0;
    }
    int i_pid = (p != NULL) ? p->pid : 0;
    spin_unlock_irq(&process_lock, u_flags);
    if (b_last) {
        process_exited(i_pid, i_status, u_now_ms);
    }
    return b_last;
}

bool process_doom_others(int i_tid, int i_code, bool b_exec)
{
    int arr_ids[MAX_THREADS];
    size_t sz_count = 0;
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct task *p_me = task_for(i_tid);
    struct process *p = (p_me != NULL) ? slot_for(p_me->i_pid) : NULL;
    bool b_alone = p != NULL && !p_me->b_doomed;
    if (b_alone) {
        if (!b_exec && !p->b_group_exit) {
            p->b_group_exit = true;
            p->i_wait_status = i_code;
        }
        for (size_t sz_i = 0; sz_i < MAX_THREADS; sz_i++) {
            struct task *t = &s_arr_tasks[sz_i];
            if (t->i_tid == 0 || t->i_pid != p->pid || t == p_me) {
                continue;
            }
            t->b_doomed = true;
            t->b_interrupted = true;
            if (t->i_thread != 0) {
                arr_ids[sz_count++] = t->i_thread;
            }
        }
    }
    spin_unlock_irq(&process_lock, u_flags);

    /* a stopped thread has to run to end, and an asleep one to wake */
    for (size_t sz_i = 0; sz_i < sz_count; sz_i++) {
        sched_set_stopped(arr_ids[sz_i], false);
        sched_wake_thread(arr_ids[sz_i]);
    }
    return b_alone;
}

bool process_doomed(int i_tid)
{
    const struct task *t = task_for(i_tid);
    return t != NULL && t->b_doomed;
}

void process_lead(int i_tid)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct task *t = task_for(i_tid);
    struct process *p = (t != NULL) ? slot_for(t->i_pid) : NULL;
    if (p != NULL) {
        t->i_tid = p->pid;
    }
    spin_unlock_irq(&process_lock, u_flags);
}

int process_pid_of(int i_tid)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    const struct task *t = task_for(i_tid);
    int i_pid = (t != NULL) ? t->i_pid : 0;
    spin_unlock_irq(&process_lock, u_flags);
    return i_pid;
}

/* the scheduler's threads of p's threads, into p_ids. the lock is held */
static size_t threads_of(const struct process *p, int *p_ids)
{
    size_t sz_count = 0;
    for (size_t sz_i = 0; sz_i < MAX_THREADS; sz_i++) {
        const struct task *t = &s_arr_tasks[sz_i];
        if (t->i_tid != 0 && t->i_pid == p->pid && t->i_thread != 0) {
            p_ids[sz_count++] = t->i_thread;
        }
    }
    return sz_count;
}

/* each of them stopped or let go, which reaches the scheduler: no lock */
static void stop_threads(const int *p_ids, size_t sz_count, bool b_stopped)
{
    for (size_t sz_i = 0; sz_i < sz_count; sz_i++) {
        sched_set_stopped(p_ids[sz_i], b_stopped);
    }
}

void process_set_stopped(int i_pid, bool b_stopped)
{
    int arr_ids[MAX_THREADS];
    uint64_t u_flags = spin_lock_irq(&process_lock);
    const struct process *p = slot_for(i_pid);
    size_t sz_count = (p != NULL) ? threads_of(p, arr_ids) : 0;
    spin_unlock_irq(&process_lock, u_flags);
    stop_threads(arr_ids, sz_count, b_stopped);
}

void process_exited(int pid, int code, uint64_t now_ms)
{
    struct child_news news = { 0 };
    bool b_first = false;
    int arr_groups[MAX_PROCESSES + 1];
    size_t sz_groups = 0;
    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    /*
     * the first answer is the true one: a process killed while it was
     * already on its way out should not have its code overwritten
     */
    if (p != NULL && !p->exited) {
        sz_groups = orphan_candidates(p, arr_groups);
        p->exited    = true;
        p->i_wait_status = code;
        b_first = true;
        p->ended_ms  = now_ms;

        /* and its threads are gone with it */
        for (size_t sz_i = 0; sz_i < MAX_THREADS; sz_i++) {
            if (s_arr_tasks[sz_i].i_pid == pid) {
                s_arr_tasks[sz_i].i_tid = 0;
            }
        }
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
        sz_groups = still_stopped(arr_groups, sz_groups);
    }
    spin_unlock_irq(&process_lock, flags);
    tell_parent(&news);

    /*
     * a stopped job nobody is left to continue would stay stopped for
     * ever, so it is told it was hung up on, and continued to hear it
     */
    for (size_t sz_at = 0; sz_at < sz_groups; sz_at++) {
        process_signal_group(arr_groups[sz_at], SIGHUP);
        process_signal_group(arr_groups[sz_at], SIGCONT);
    }
}

void process_account(int i_pid, const struct cpu_usage *p_used)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(i_pid);
    if (p != NULL) {
        cpu_usage_add(&p->used, p_used);
    }
    spin_unlock_irq(&process_lock, u_flags);
}

struct cpu_usage process_ended_usage(int i_pid, struct cpu_usage *p_children)
{
    struct cpu_usage none = { 0 };
    uint64_t u_flags = spin_lock_irq(&process_lock);
    const struct process *p = slot_for(i_pid);
    struct cpu_usage used = (p != NULL) ? p->used : none;
    if (p_children != NULL) {
        *p_children = (p != NULL) ? p->children : none;
    }
    spin_unlock_irq(&process_lock, u_flags);
    return used;
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
            cpu_usage_add(&p_parent->children, &p->used);
            cpu_usage_add(&p_parent->children, &p->children);
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
        struct process *p = &table[i];
        if (p->pid == 0 || p->exited || p->pid == INIT_PID) {
            continue;
        }
        signal_raise(&p->sig, &p->sig.shared, blocked_by_all(p), SIGINT,
                     NULL);
        struct task *t = taker_of(p, SIGINT);
        if (t != NULL) {
            t->b_interrupted = true;
        }
        n++;
    }

    spin_unlock_irq(&process_lock, flags);
    return n;
}

size_t process_running_threads(int *ids, size_t max)
{
    uint64_t flags = spin_lock_irq(&process_lock);
    size_t n = 0;

    for (size_t i = 0; i < MAX_THREADS && n < max; i++) {
        const struct task *t = &s_arr_tasks[i];
        const struct process *p = slot_for(t->i_pid);
        if (t->i_tid == 0 || t->i_thread == 0 || p == NULL || p->exited
            || p->pid == INIT_PID) {
            continue;
        }
        ids[n++] = t->i_thread;
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
static int settle_now(struct process *p, struct signal_set *p_set,
                      uint64_t u_blocked, int i_sig)
{
    uint64_t u_bit = SIGNAL_BIT(i_sig);
    enum signal_action e_act = signal_action_for(&p->sig, i_sig);
    bool b_blocked = (u_blocked & u_bit) != 0 && i_sig != SIGSTOP;

    /*
     * a stop from a terminal does nothing to an orphaned group, which
     * nobody would ever continue: SIGSTOP alone is never refused
     */
    if (e_act == SIGNAL_ACTION_STOP && !b_blocked && i_sig != SIGSTOP
        && group_orphaned(p->pgid)) {
        p_set->u_pending &= ~u_bit;
        return 0;
    }
    if (e_act == SIGNAL_ACTION_STOP && !b_blocked) {
        p_set->u_pending &= ~u_bit;     /* acted on, nothing to deliver */
        p->i_stopped_by = i_sig;
        p->b_stop_told = false;
        return 1;
    }

    /* and a kill, which a stopped process has to be running to die of */
    if (i_sig == SIGCONT || i_sig == SIGKILL) {
        if (e_act == SIGNAL_ACTION_CONTINUE) {
            p_set->u_pending &= ~u_bit; /* continuing was all it meant */
        }
        if (p->i_stopped_by != 0 && i_sig == SIGCONT) {
            p->b_continued = true;
        }
        p->i_stopped_by = 0;
        return -1;
    }
    return 0;
}

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

/*
 * a fault's own signal, which cannot be refused: blocked or ignored, it
 * goes back to its default first. a program that ignores SIGSEGV and
 * then dereferences null would otherwise return to the same instruction
 * forever, faulting each time
 */
void process_force_signal(int i_tid, int i_sig,
                          const struct signal_origin *origin)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct task *t = task_for(i_tid);
    struct process *p = (t != NULL) ? slot_for(t->i_pid) : NULL;
    if (p != NULL && !p->exited && i_sig > 0 && i_sig < SIGNAL_MAX) {
        uint64_t u_bit = SIGNAL_BIT(i_sig);
        if (p->sig.arr_action[i_sig].sa_handler == SIG_IGNORE
            || (t->sig.u_blocked & u_bit) != 0) {
            p->sig.arr_action[i_sig].sa_handler = SIG_DEFAULT;
            t->sig.u_blocked &= ~u_bit;
        }
        signal_raise(&p->sig, &t->sig.own, t->sig.u_blocked, i_sig, origin);
        t->b_interrupted = true;
    }
    spin_unlock_irq(&process_lock, u_flags);
}

/* what sending a signal leaves to be done once the lock is let go */
struct after_send {
    struct child_news news;
    int i_become;               /* 1 stopped, -1 running, 0 as it was */
    int arr_threads[MAX_THREADS];
    size_t sz_threads;
    int i_wake;                 /* the scheduler's thread to wake, or 0 */
};

/*
 * sig raised at p, or at its thread t alone, and what that changes: a
 * stop or a continue acted on at once, the parent told, and the thread
 * that is to take it marked to be woken, all of which reaches the
 * scheduler and is left in *p_after. the lock is held
 */
static bool raise_at(struct process *p, struct task *t, int i_sig,
                     const struct signal_origin *origin,
                     struct after_send *p_after)
{
    memset(p_after, 0, sizeof *p_after);

    /*
     * init takes only what it has a handler for: one it left to the
     * default would end, or stop, the process the machine needs. sent
     * all the same, as far as the sender can tell
     */
    if (p->pid == INIT_PID && i_sig > 0 && i_sig < SIGNAL_MAX
        && p->sig.arr_action[i_sig].sa_handler == SIG_DEFAULT) {
        return true;
    }
    if (p->exited) {
        return false;
    }
    struct signal_set *p_set = (t != NULL) ? &t->sig.own : &p->sig.shared;
    uint64_t u_blocked = (t != NULL) ? t->sig.u_blocked : blocked_by_all(p);
    if (!signal_raise(&p->sig, p_set, u_blocked, i_sig, origin)) {
        return false;
    }
    p_after->i_become = settle_now(p, p_set, u_blocked, i_sig);
    if (p_after->i_become > 0) {
        child_news(p, CLD_STOPPED, i_sig, &p_after->news);
    } else if (p_after->i_become < 0 && i_sig == SIGCONT) {
        child_news(p, CLD_CONTINUED, SIGCONT, &p_after->news);
    }
    if (p_after->i_become != 0) {
        p_after->sz_threads = threads_of(p, p_after->arr_threads);
    }

    /*
     * whether anything is pending and unblocked: what a blocking call
     * stops waiting for. an ignored one was dropped as it was raised, so
     * it is not among them, and of a process's threads it is the one it
     * is for that is told, not every one that could take it
     */
    struct task *p_taker = (t != NULL) ? t : taker_of(p, i_sig);
    if (p_taker != NULL) {
        recalc(p_taker);
        if (p_taker->b_interrupted) {
            p_after->i_wake = p_taker->i_thread;
        }
    }
    return true;
}

/*
 * and the half that reaches the scheduler, with no lock held: the
 * parent told, the threads stopped or let go, and the one the signal is
 * for woken, since a thread asleep on a queue has to be, or a signal to
 * a program blocked on a read arrives whenever that read happens to
 * finish, which for a program waiting on the keyboard is never. this is
 * the half the roadmap warned about: every blocking call was written
 * assuming it ends for one of two reasons
 */
static void after_send(const struct after_send *p_after)
{
    tell_parent(&p_after->news);
    if (p_after->i_become != 0) {
        stop_threads(p_after->arr_threads, p_after->sz_threads,
                     p_after->i_become > 0);
    }
    if (p_after->i_wake > 0) {
        sched_wake_thread(p_after->i_wake);
    }
}

bool process_send_signal(int pid, int sig,
                         const struct signal_origin *origin)
{
    struct after_send after;
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    bool b_ok = p != NULL && raise_at(p, NULL, sig, origin, &after);
    spin_unlock_irq(&process_lock, u_flags);
    if (b_ok) {
        after_send(&after);
    }
    return b_ok;
}

bool process_tkill(int i_tid, int i_sig, const struct signal_origin *origin)
{
    struct after_send after;
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct task *t = task_for(i_tid);
    struct process *p = (t != NULL) ? slot_for(t->i_pid) : NULL;
    bool b_ok = p != NULL && raise_at(p, t, i_sig, origin, &after);
    spin_unlock_irq(&process_lock, u_flags);
    if (b_ok) {
        after_send(&after);
    }
    return b_ok;
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

struct signal_thread *process_thread_signals(int i_tid)
{
    struct task *t = task_for(i_tid);
    return t == NULL ? NULL : &t->sig;
}

/* thread i_tid and its process, or false when either is gone */
static bool task_and_process(int i_tid, struct task **pp_task,
                             struct process **pp_proc)
{
    *pp_task = task_for(i_tid);
    *pp_proc = (*pp_task != NULL) ? slot_for((*pp_task)->i_pid) : NULL;
    return *pp_proc != NULL;
}

enum signal_action process_take_signal(int i_tid, int *sig_out,
                                       struct sigaction *act_out,
                                       uint64_t *old_mask_out,
                                       struct signal_origin *origin_out)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct task *t;
    struct process *p;
    enum signal_action a = SIGNAL_ACTION_NONE;

    if (task_and_process(i_tid, &t, &p)) {
        int sig = signal_next(&p->sig, &t->sig);
        if (sig != 0) {
            a = signal_take(&p->sig, &t->sig, sig, act_out, old_mask_out,
                            origin_out);
            if (sig_out != NULL) {
                *sig_out = sig;
            }
            /* and one blocked when it was sent is no different later */
            if (a == SIGNAL_ACTION_STOP && sig != SIGSTOP
                && group_orphaned(p->pgid)) {
                a = SIGNAL_ACTION_IGNORE;
            }
        }
        recalc(t);
    }
    spin_unlock_irq(&process_lock, u_flags);
    return a;
}

int process_set_action(int i_pid, int i_sig, const struct sigaction *p_act,
                       struct sigaction *p_old)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(i_pid);
    int i_result = 0;
    if (p == NULL || i_sig <= 0 || i_sig >= SIGNAL_MAX) {
        i_result = -EINVAL;
    } else {
        if (p_old != NULL) {
            *p_old = p->sig.arr_action[i_sig];
        }
        if (p_act != NULL && !signal_set_action(&p->sig, i_sig, p_act)) {
            i_result = -EINVAL;
        }
        /* gone from every thread's own set too, not merely the process's */
        if (signal_action_for(&p->sig, i_sig) == SIGNAL_ACTION_IGNORE) {
            for (size_t sz_i = 0; sz_i < MAX_THREADS; sz_i++) {
                struct task *t = &s_arr_tasks[sz_i];
                if (t->i_tid != 0 && t->i_pid == i_pid) {
                    signal_forget(&t->sig.own, i_sig);
                    recalc(t);
                }
            }
        }
    }
    spin_unlock_irq(&process_lock, u_flags);
    return i_result;
}

uint64_t process_set_mask(int i_tid, uint64_t u_mask, bool b_suspend)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct task *t = task_for(i_tid);
    uint64_t u_was = 0;
    if (t != NULL) {
        u_was = signal_get_mask(&t->sig);
        if (b_suspend) {
            t->sig.u_suspend_mask = u_was;
            t->sig.b_suspended = true;
        }
        signal_set_mask(&t->sig, u_mask);
        recalc(t);
    }
    spin_unlock_irq(&process_lock, u_flags);
    return u_was;
}

void process_unsuspend(int i_tid)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct task *t = task_for(i_tid);
    if (t != NULL && t->sig.b_suspended) {
        signal_set_mask(&t->sig, t->sig.u_suspend_mask);
        t->sig.b_suspended = false;
        recalc(t);
    }
    spin_unlock_irq(&process_lock, u_flags);
}

uint64_t process_pending(int i_tid)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct task *t;
    struct process *p;
    uint64_t u_pending = 0;
    if (task_and_process(i_tid, &t, &p)) {
        u_pending = (t->sig.own.u_pending | p->sig.shared.u_pending)
                    & t->sig.u_blocked;
    }
    spin_unlock_irq(&process_lock, u_flags);
    return u_pending;
}

bool process_would_take(int i_tid, int i_sig)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct task *t;
    struct process *p;
    bool b_takes = task_and_process(i_tid, &t, &p) && i_sig > 0
                   && i_sig < SIGNAL_MAX
                   && signal_action_for(&p->sig, i_sig)
                      != SIGNAL_ACTION_IGNORE
                   && !(t->sig.u_blocked & SIGNAL_BIT(i_sig));
    spin_unlock_irq(&process_lock, u_flags);
    return b_takes;
}

int process_take_from(int i_tid, uint64_t u_set,
                      struct signal_origin *p_origin)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct task *t;
    struct process *p;
    int i_sig = 0;
    if (task_and_process(i_tid, &t, &p)) {
        /* the thread's own first, as delivery takes them */
        struct signal_set *arr_sets[2] = { &t->sig.own, &p->sig.shared };
        for (size_t sz_set = 0; sz_set < 2 && i_sig == 0; sz_set++) {
            struct signal_set *p_set = arr_sets[sz_set];
            uint64_t u_ready = p_set->u_pending & u_set;
            for (int i_at = 1; i_at < SIGNAL_MAX && i_sig == 0; i_at++) {
                if (u_ready & SIGNAL_BIT(i_at)) {
                    i_sig = i_at;
                    p_set->u_pending &= ~SIGNAL_BIT(i_sig);
                    *p_origin = p_set->arr_origin[i_sig];
                }
            }
        }
        recalc(t);
    }
    spin_unlock_irq(&process_lock, u_flags);
    return i_sig;
}

int process_thread(int i_tid)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    const struct task *t = task_for(i_tid);
    int i_thread = (t != NULL) ? t->i_thread : 0;
    spin_unlock_irq(&process_lock, u_flags);
    return i_thread;
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



bool process_interrupt_pending(int i_tid)
{
    const struct task *t = task_for(i_tid);
    return t != NULL && t->b_interrupted;
}

bool process_take_interrupt(int i_tid)
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
    struct task *t = task_for(i_tid);
    if (t != NULL && t->b_interrupted) {
        t->b_interrupted = false;
        had = true;
    }
    spin_unlock_irq(&process_lock, flags);
    return had;
}



void process_inherit(int i_child, int i_from)
{
    uint64_t u_flags = spin_lock_irq(&process_lock);
    struct process *p_child = slot_for(i_child);
    struct task *t_child = task_for(i_child);
    const struct task *t_from = task_for(i_from);
    const struct process *p_parent = (t_from != NULL)
                                     ? slot_for(t_from->i_pid) : NULL;

    if (p_child != NULL && t_child != NULL && p_parent != NULL) {
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
        signal_inherit_thread(&t_child->sig, &t_from->sig, true);
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

struct task *process_lock_task(int i_tid, uint64_t *p_flags)
{
    *p_flags = spin_lock_irq(&process_lock);
    return task_for(i_tid);
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

/*
 * and a session's leader ending is its session's end, which is told to
 * the session's terminal here, every way a process ends coming through
 */
void process_fds_release(int i_pid)
{
    /*
     * the descriptors are every thread's, and stay open for as long as
     * one of them is left, whichever thread's end this is
     */
    uint64_t u_flags = spin_lock_irq(&process_lock);
    const struct process *p = slot_for(i_pid);
    bool b_ended = p != NULL && p->exited;
    bool b_leader = b_ended && p->i_sid == i_pid;
    spin_unlock_irq(&process_lock, u_flags);
    if (!b_ended) {
        return;
    }
    close_fds(i_pid, ~0ull);
    if (b_leader) {
        tty_session_ended(i_pid);
    }
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
