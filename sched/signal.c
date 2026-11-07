// SPDX-License-Identifier: GPL-2.0-only
/*
 * sched/signal.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * signals: disposition, delivery, and what cannot be caught.
 */

#include <velvet/signal.h>
#include <velvet/string.h>

/* the two that cannot be argued with. */
#define UNBLOCKABLE (SIGNAL_BIT(SIGKILL) | SIGNAL_BIT(SIGSTOP))

static bool valid(int i_sig)
{
    return i_sig > 0 && i_sig < SIGNAL_MAX;
}

void signal_reset(struct signal_state *s)
{
    memset(s, 0, sizeof *s);
}

void signal_reset_thread(struct signal_thread *t)
{
    memset(t, 0, sizeof *t);
    t->altstack.ss_flags = SS_DISABLE;
}

void signal_inherit(struct signal_state *p_child,
                    const struct signal_state *p_parent)
{
    memset(p_child, 0, sizeof *p_child);
    for (int i = 0; i < SIGNAL_MAX; i++) {
        p_child->arr_action[i] = p_parent->arr_action[i];
    }
    /* deliberately not what is pending. */
}

void signal_inherit_thread(struct signal_thread *p_child,
                           const struct signal_thread *p_parent,
                           bool b_altstack)
{
    signal_reset_thread(p_child);
    p_child->u_blocked = p_parent->u_blocked;
    if (b_altstack) {
        p_child->altstack = p_parent->altstack;
    }
}

void signal_exec(struct signal_state *s, struct signal_thread *t)
{
    /* the handlers pointed into the program that is being replaced. */
    for (int i = 0; i < SIGNAL_MAX; i++) {
        bool b_ignored = s->arr_action[i].sa_handler == SIG_IGNORE;
        memset(&s->arr_action[i], 0, sizeof s->arr_action[i]);
        s->arr_action[i].sa_handler = b_ignored ? SIG_IGNORE : SIG_DEFAULT;
    }
    t->altstack.ss_sp = 0;
    t->altstack.ss_size = 0;
    t->altstack.ss_flags = SS_DISABLE;
    t->b_suspended = false;
}

bool signal_raise(const struct signal_state *s, struct signal_set *p_set,
                  uint64_t u_blocked, int i_sig,
                  const struct signal_origin *origin)
{
    if (!valid(i_sig)) {
        return false;
    }
    /*
     * one that would be ignored is gone as it is sent, the way it would
     * be on taking it, unless it is blocked: then it waits, since the
     * action may have changed by the time it is unblocked. dropped here,
     * it never cuts short a call that was waiting for something else
     */
    if (signal_action_for(s, i_sig) == SIGNAL_ACTION_IGNORE
        && !(u_blocked & SIGNAL_BIT(i_sig))) {
        return true;
    }
    /*
     * a set, not a queue. raising one already raised changes nothing
     * but who it is from, which is why two ctrl+c presses in the same
     * instant are one interrupt
     */
    p_set->u_pending |= SIGNAL_BIT(i_sig);
    if (origin != NULL) {
        p_set->arr_origin[i_sig] = *origin;
    } else {
        memset(&p_set->arr_origin[i_sig], 0, sizeof p_set->arr_origin[i_sig]);
        p_set->arr_origin[i_sig].i_code = SI_KERNEL;
    }
    return true;
}

/*
 * lowest first. SIGKILL is 9 and comes before most things a program
 * could be busy handling, and a stop or a fault, SIGSEGV 11, comes
 * before the signals a program sends itself
 */
static int lowest(uint64_t u_ready)
{
    for (int i_sig = 1; i_sig < SIGNAL_MAX; i_sig++) {
        if (u_ready & SIGNAL_BIT(i_sig)) {
            return i_sig;
        }
    }
    return 0;
}

int signal_next(const struct signal_state *s, const struct signal_thread *t)
{
    /*
     * the thread's own first, a fault's among them: a fault has to be
     * the next thing the thread hears of, since the instruction that
     * made it is the next thing it would run
     */
    uint64_t u_allowed = ~(t->u_blocked & ~UNBLOCKABLE);
    int i_sig = lowest(t->own.u_pending & u_allowed);
    return (i_sig != 0) ? i_sig : lowest(s->shared.u_pending & u_allowed);
}

enum signal_action signal_action_for(const struct signal_state *s, int i_sig)
{
    if (!valid(i_sig)) {
        return SIGNAL_ACTION_NONE;
    }

    /*
     * checked before the handler table, so that a program which
     * installed a handler for SIGKILL, which set_handler refuses, but
     * a corrupted table might still hold, cannot escape it
     */
    if (i_sig == SIGKILL) {
        return SIGNAL_ACTION_TERMINATE;
    }
    if (i_sig == SIGSTOP) {
        return SIGNAL_ACTION_STOP;
    }

    uint64_t h = s->arr_action[i_sig].sa_handler;
    if (h == SIG_IGNORE) {
        return SIGNAL_ACTION_IGNORE;
    }
    if (h != SIG_DEFAULT) {
        return SIGNAL_ACTION_HANDLER;
    }

    switch (i_sig) {
    case SIGCHLD:
        /*
         * the one default that is not death, and it matters more than
         * it looks: a child finishing is the commonest event on a
         * machine running programs, and a default of "die" would have
         * every shell in history exit the moment a command completed
         */
        return SIGNAL_ACTION_IGNORE;
    case SIGURG:
    case SIGWINCH:
        return SIGNAL_ACTION_IGNORE;    /* news, not a reason to die */
    case SIGCONT:
        return SIGNAL_ACTION_CONTINUE;
    case SIGTSTP:
    case SIGTTIN:
    case SIGTTOU:
        return SIGNAL_ACTION_STOP;
    default:
        return SIGNAL_ACTION_TERMINATE;
    }
}

enum signal_action signal_take(struct signal_state *s,
                               struct signal_thread *t, int i_sig,
                               struct sigaction *act_out,
                               uint64_t *old_mask_out,
                               struct signal_origin *origin_out)
{
    if (!valid(i_sig)) {
        return SIGNAL_ACTION_NONE;
    }
    struct signal_set *p_set = (t->own.u_pending & SIGNAL_BIT(i_sig))
                               ? &t->own : &s->shared;
    if (!(p_set->u_pending & SIGNAL_BIT(i_sig))) {
        return SIGNAL_ACTION_NONE;
    }
    p_set->u_pending &= ~SIGNAL_BIT(i_sig);
    if (origin_out != NULL) {
        *origin_out = p_set->arr_origin[i_sig];
    }

    enum signal_action a = signal_action_for(s, i_sig);
    if (a != SIGNAL_ACTION_HANDLER) {
        return a;
    }
    if (act_out != NULL) {
        *act_out = s->arr_action[i_sig];
    }

    /*
     * the mask the handler interrupted goes in its frame, and comes back
     * from there when it returns, so a handler interrupted by another
     * gets its own mask back and not the inner one's. that of a
     * suspend is the one it was waiting under the caller's mask for
     */
    if (old_mask_out != NULL) {
        *old_mask_out = t->b_suspended ? t->u_suspend_mask : t->u_blocked;
    }
    t->b_suspended = false;

    uint64_t u_mask = s->arr_action[i_sig].sa_mask;
    if (!(s->arr_action[i_sig].sa_flags & SA_NODEFER)) {
        u_mask |= SIGNAL_BIT(i_sig);
    }
    t->u_blocked = (t->u_blocked | u_mask) & ~UNBLOCKABLE;
    if (s->arr_action[i_sig].sa_flags & SA_RESETHAND) {
        memset(&s->arr_action[i_sig], 0, sizeof s->arr_action[i_sig]);
    }
    return SIGNAL_ACTION_HANDLER;
}

bool signal_set_action(struct signal_state *s, int i_sig,
                       const struct sigaction *act)
{
    if (!valid(i_sig)) {
        return false;
    }
    if (i_sig == SIGKILL || i_sig == SIGSTOP) {
        /* the only thing that may be asked of these is to leave them be */
        return act->sa_handler == SIG_DEFAULT;
    }
    s->arr_action[i_sig] = *act;
    s->arr_action[i_sig].sa_mask &= ~UNBLOCKABLE;

    /* an ignored signal already pending is gone, not merely deferred */
    if (signal_action_for(s, i_sig) == SIGNAL_ACTION_IGNORE) {
        signal_forget(&s->shared, i_sig);
    }
    return true;
}

void signal_forget(struct signal_set *p_set, int i_sig)
{
    if (valid(i_sig)) {
        p_set->u_pending &= ~SIGNAL_BIT(i_sig);
    }
}

void signal_set_mask(struct signal_thread *t, uint64_t mask)
{
    /* KILL and STOP cannot be blocked, whatever was asked */
    t->u_blocked = mask & ~UNBLOCKABLE;
}

uint64_t signal_get_mask(const struct signal_thread *t)
{
    return t->u_blocked;
}

void signal_info(int i_sig, const struct signal_origin *p_origin,
                 struct siginfo *p_info)
{
    memset(p_info, 0, sizeof *p_info);
    p_info->si_signo = i_sig;
    p_info->si_code = p_origin->i_code;
    if (i_sig == SIGCHLD && p_origin->i_code >= CLD_EXITED
        && p_origin->i_code <= CLD_CONTINUED) {
        p_info->u.proc.si_pid = p_origin->i_pid;
        p_info->u.proc.si_uid = p_origin->u_uid;
        p_info->u.proc.si_status = p_origin->i_status;
    } else if (p_origin->i_code > 0 && p_origin->i_code != SI_KERNEL) {
        p_info->u.si_addr = p_origin->u_addr;     /* a fault's */
    } else if (p_origin->i_code <= 0) {
        p_info->u.proc.si_pid = p_origin->i_pid;  /* a sender's */
        p_info->u.proc.si_uid = p_origin->u_uid;
    }
}

bool signal_deliverable(const struct signal_state *s,
                        const struct signal_thread *t)
{
    return signal_next(s, t) != 0;
}

const char *signal_name(int i_sig)
{
    switch (i_sig) {
    case SIGHUP:    return "hangup";
    case SIGINT:    return "interrupt";
    case SIGQUIT:   return "quit";
    case SIGILL:    return "illegal instruction";
    case SIGTRAP:   return "trap";
    case SIGABRT:   return "abort";
    case SIGBUS:    return "bus error";
    case SIGFPE:    return "arithmetic fault";
    case SIGKILL:   return "killed";
    case SIGUSR1:   return "user signal 1";
    case SIGSEGV:   return "bad address";
    case SIGUSR2:   return "user signal 2";
    case SIGPIPE:   return "broken pipe";
    case SIGALRM:   return "alarm";
    case SIGTERM:   return "terminated";
    case SIGSTKFLT: return "stack fault";
    case SIGCHLD:   return "child finished";
    case SIGCONT:   return "continue";
    case SIGSTOP:   return "stop";
    case SIGTSTP:   return "stop from the keyboard";
    case SIGTTIN:   return "stop for reading the terminal";
    case SIGTTOU:   return "stop for writing the terminal";
    case SIGURG:    return "urgent data";
    case SIGXCPU:   return "out of cpu time";
    case SIGXFSZ:   return "file too large";
    case SIGVTALRM: return "virtual alarm";
    case SIGPROF:   return "profiling alarm";
    case SIGWINCH:  return "window changed";
    case SIGIO:     return "i/o possible";
    case SIGPWR:    return "power failure";
    case SIGSYS:    return "bad system call";
    default:        return "signal";
    }
}
