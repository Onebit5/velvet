// SPDX-License-Identifier: GPL-2.0-only
/*
 * sched/signal.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * signals: disposition, delivery, and what cannot be caught.
 */

#include <velvet/signal.h>
#include <velvet/string.h>

/* the two that cannot be argued with. */
#define UNBLOCKABLE ((1u << SIGKILL) | (1u << SIGSTOP))

static bool valid(int sig)
{
    return sig > 0 && sig < SIGNAL_MAX;
}

void signal_reset(struct signal_state *s)
{
    memset(s, 0, sizeof *s);
}

void signal_inherit(struct signal_state *child,
                    const struct signal_state *parent)
{
    memset(child, 0, sizeof *child);
    child->blocked = parent->blocked;
    for (int i = 0; i < SIGNAL_MAX; i++) {
        child->handler[i] = parent->handler[i];
    }
    /* deliberately not `pending`. */
}

void signal_exec(struct signal_state *s)
{
    /* the handlers pointed into the program that is being replaced. */
    for (int i = 0; i < SIGNAL_MAX; i++) {
        if (s->handler[i] != SIG_IGNORE) {
            s->handler[i] = SIG_DEFAULT;
        }
    }
    s->pending = 0;
    s->running = 0;
}

bool signal_raise(struct signal_state *s, int sig)
{
    if (!valid(sig)) {
        return false;
    }
    /*
     * a set, not a queue. raising one already raised changes nothing,
     * which is why two ctrl+c presses in the same instant are one
     * interrupt
     */
    s->pending |= (1u << sig);
    return true;
}

int signal_next(const struct signal_state *s)
{
    uint32_t ready = s->pending & ~(s->blocked & ~UNBLOCKABLE);

    /*
     * lowest number first, so that SIGKILL at 9 is delivered before
     * SIGTERM at 15 when a process has been asked twice. the ordering
     * is not otherwise meaningful and this is the one case where it
     * decides something
     */
    for (int sig = 1; sig < SIGNAL_MAX; sig++) {
        if (ready & (1u << sig)) {
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

    uint64_t h = s->handler[sig];
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
    case SIGCONT:
        return SIGNAL_ACTION_CONTINUE;
    case SIGTSTP:
        return SIGNAL_ACTION_STOP;
    default:
        return SIGNAL_ACTION_TERMINATE;
    }
}

enum signal_action signal_take(struct signal_state *s, int sig,
                               uint64_t *handler_out)
{
    if (!valid(sig) || !(s->pending & (1u << sig))) {
        return SIGNAL_ACTION_NONE;
    }
    s->pending &= ~(1u << sig);

    enum signal_action a = signal_action_for(s, sig);
    if (a != SIGNAL_ACTION_HANDLER) {
        return a;
    }

    if (handler_out != NULL) {
        *handler_out = s->handler[sig];
    }

    /* the handler runs with its own signal blocked, and the mask is put back when it returns. */
    s->saved_mask = s->blocked;
    s->blocked   |= (1u << sig);
    s->running    = sig;
    return SIGNAL_ACTION_HANDLER;
}

void signal_handler_returned(struct signal_state *s)
{
    /*
     * FIXME: one saved mask and one `running`, so two handlers that nest
     * lose the outer one. a signal arriving while a handler for a
     * different one runs, SIGALRM inside a SIGCHLD handler being the
     * ordinary case, goes through signal_take again and overwrites
     * saved_mask and running with the inner values. the inner return
     * restores the inner mask and clears running; the outer return then
     * sees running at zero, comes back here, and never puts its own
     * blocked bit back. that signal stays blocked for the rest of the
     * process, so a program with an alarm never gets its second one. the
     * masks want a stack, or a depth counter and an array of them.
     */
    if (s->running == 0) {
        return;
    }
    s->blocked = s->saved_mask;
    s->running = 0;
}

bool signal_set_handler(struct signal_state *s, int sig, uint64_t handler)
{
    if (!valid(sig)) {
        return false;
    }
    if (sig == SIGKILL || sig == SIGSTOP) {
        /* refused rather than accepted-and-ignored. */
        return false;
    }
    s->handler[sig] = handler;
    return true;
}

void signal_set_mask(struct signal_state *s, uint32_t mask)
{
    /* the two special ones stay unblocked however hard anybody tries. */
    s->blocked = mask & ~UNBLOCKABLE;
}

uint32_t signal_get_mask(const struct signal_state *s)
{
    return s->blocked;
}

bool signal_deliverable(const struct signal_state *s)
{
    return signal_next(s) != 0;
}

const char *signal_name(int sig)
{
    switch (sig) {
    case SIGHUP:  return "hangup";
    case SIGINT:  return "interrupt";
    case SIGQUIT: return "quit";
    case SIGILL:  return "illegal instruction";
    case SIGTRAP: return "trap";
    case SIGABRT: return "abort";
    case SIGBUS:  return "bus error";
    case SIGFPE:  return "arithmetic fault";
    case SIGKILL: return "killed";
    case SIGSEGV: return "bad address";
    case SIGPIPE: return "broken pipe";
    case SIGALRM: return "alarm";
    case SIGTERM: return "terminated";
    case SIGCHLD: return "child finished";
    case SIGCONT: return "continue";
    case SIGSTOP: return "stop";
    case SIGTSTP: return "stop from the keyboard";
    default:      return "signal";
    }
}
