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

static bool valid(int sig)
{
    return sig > 0 && sig < SIGNAL_MAX;
}

void signal_reset(struct signal_state *s)
{
    memset(s, 0, sizeof *s);
    s->altstack.ss_flags = SS_DISABLE;
}

void signal_inherit(struct signal_state *child,
                    const struct signal_state *parent)
{
    memset(child, 0, sizeof *child);
    child->blocked = parent->blocked;
    for (int i = 0; i < SIGNAL_MAX; i++) {
        child->action[i] = parent->action[i];
    }
    child->altstack = parent->altstack;
    /* deliberately not `pending`. */
}

void signal_exec(struct signal_state *s)
{
    /* the handlers pointed into the program that is being replaced. */
    for (int i = 0; i < SIGNAL_MAX; i++) {
        bool b_ignored = s->action[i].sa_handler == SIG_IGNORE;
        memset(&s->action[i], 0, sizeof s->action[i]);
        s->action[i].sa_handler = b_ignored ? SIG_IGNORE : SIG_DEFAULT;
    }
    s->altstack.ss_sp = 0;
    s->altstack.ss_size = 0;
    s->altstack.ss_flags = SS_DISABLE;
    s->b_suspended = false;
}

bool signal_raise(struct signal_state *s, int sig,
                  const struct signal_origin *origin)
{
    if (!valid(sig)) {
        return false;
    }
    /*
     * one that would be ignored is gone as it is sent, the way it would
     * be on taking it, unless it is blocked: then it waits, since the
     * action may have changed by the time it is unblocked. dropped here,
     * it never cuts short a call that was waiting for something else
     */
    if (signal_action_for(s, sig) == SIGNAL_ACTION_IGNORE
        && !(s->blocked & SIGNAL_BIT(sig))) {
        return true;
    }
    /*
     * a set, not a queue. raising one already raised changes nothing
     * but who it is from, which is why two ctrl+c presses in the same
     * instant are one interrupt
     */
    s->pending |= SIGNAL_BIT(sig);
    if (origin != NULL) {
        s->origin[sig] = *origin;
    } else {
        memset(&s->origin[sig], 0, sizeof s->origin[sig]);
        s->origin[sig].i_code = SI_KERNEL;
    }
    return true;
}

int signal_next(const struct signal_state *s)
{
    uint64_t ready = s->pending & ~(s->blocked & ~UNBLOCKABLE);
    /*
     * lowest first. SIGKILL is 9 and comes before most things a program
     * could be busy handling, and a stop or a fault, SIGSEGV 11, comes
     * before the signals a program sends itself
     */
    for (int sig = 1; sig < SIGNAL_MAX; sig++) {
        if (ready & SIGNAL_BIT(sig)) {
            return sig;
        }
    }
    return 0;
}

enum signal_action signal_action_for(const struct signal_state *s, int sig)
{
    if (!valid(sig)) {
        return SIGNAL_ACTION_NONE;
    }

    /*
     * checked before the handler table, so that a program which
     * installed a handler for SIGKILL, which set_handler refuses, but
     * a corrupted table might still hold, cannot escape it
     */
    if (sig == SIGKILL) {
        return SIGNAL_ACTION_TERMINATE;
    }
    if (sig == SIGSTOP) {
        return SIGNAL_ACTION_STOP;
    }

    uint64_t h = s->action[sig].sa_handler;
    if (h == SIG_IGNORE) {
        return SIGNAL_ACTION_IGNORE;
    }
    if (h != SIG_DEFAULT) {
        return SIGNAL_ACTION_HANDLER;
    }

    switch (sig) {
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

enum signal_action signal_take(struct signal_state *s, int sig,
                               struct sigaction *act_out,
                               uint64_t *old_mask_out,
                               struct signal_origin *origin_out)
{
    if (!valid(sig) || !(s->pending & SIGNAL_BIT(sig))) {
        return SIGNAL_ACTION_NONE;
    }
    s->pending &= ~SIGNAL_BIT(sig);
    if (origin_out != NULL) {
        *origin_out = s->origin[sig];
    }

    enum signal_action a = signal_action_for(s, sig);
    if (a != SIGNAL_ACTION_HANDLER) {
        return a;
    }
    if (act_out != NULL) {
        *act_out = s->action[sig];
    }

    /*
     * the mask the handler interrupted goes in its frame, and comes back
     * from there when it returns, so a handler interrupted by another
     * gets its own mask back and not the inner one's. that of a
     * suspend is the one it was waiting under the caller's mask for
     */
    if (old_mask_out != NULL) {
        *old_mask_out = s->b_suspended ? s->u_suspend_mask : s->blocked;
    }
    s->b_suspended = false;

    uint64_t u_mask = s->action[sig].sa_mask;
    if (!(s->action[sig].sa_flags & SA_NODEFER)) {
        u_mask |= SIGNAL_BIT(sig);
    }
    s->blocked = (s->blocked | u_mask) & ~UNBLOCKABLE;
    if (s->action[sig].sa_flags & SA_RESETHAND) {
        memset(&s->action[sig], 0, sizeof s->action[sig]);
    }
    return SIGNAL_ACTION_HANDLER;
}

bool signal_set_action(struct signal_state *s, int sig,
                       const struct sigaction *act)
{
    if (!valid(sig)) {
        return false;
    }
    if (sig == SIGKILL || sig == SIGSTOP) {
        /* the only thing that may be asked of these is to leave them be */
        return act->sa_handler == SIG_DEFAULT;
    }
    s->action[sig] = *act;
    s->action[sig].sa_mask &= ~UNBLOCKABLE;

    /* an ignored signal already pending is gone, not merely deferred */
    if (signal_action_for(s, sig) == SIGNAL_ACTION_IGNORE) {
        s->pending &= ~SIGNAL_BIT(sig);
    }
    return true;
}

void signal_set_mask(struct signal_state *s, uint64_t mask)
{
    /* KILL and STOP cannot be blocked, whatever was asked */
    s->blocked = mask & ~UNBLOCKABLE;
}

uint64_t signal_get_mask(const struct signal_state *s)
{
    return s->blocked;
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

bool signal_deliverable(const struct signal_state *s)
{
    return signal_next(s) != 0;
}

const char *signal_name(int sig)
{
    switch (sig) {
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
