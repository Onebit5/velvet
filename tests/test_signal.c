// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_signal.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for the signal rules.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include <velvet/signal.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

/* an action that runs a handler at u_at, with flags and nothing blocked */
static struct sigaction handler_at(uint64_t u_at, uint64_t u_flags)
{
    struct sigaction act = { .sa_handler = u_at, .sa_flags = u_flags };
    return act;
}

static void set_handler(struct signal_state *p_s, int i_sig, uint64_t u_at)
{
    struct sigaction act = handler_at(u_at, 0);
    signal_set_action(p_s, i_sig, &act);
}

int main(void)
{
    struct signal_state s;
    struct sigaction act;
    uint64_t u_old = 0;
    struct signal_origin origin;

    signal_reset(&s);
    CHECK(signal_next(&s) == 0, "nothing is pending to begin with");
    CHECK(!signal_deliverable(&s), "and there is nothing to deliver");

    CHECK(signal_raise(&s, SIGINT, NULL), "a signal is raised");
    CHECK(signal_next(&s) == SIGINT, "and is next");
    CHECK(signal_raise(&s, SIGINT, NULL), "raising it again is allowed");
    CHECK(signal_next(&s) == SIGINT, "and changes nothing");

    signal_take(&s, SIGINT, &act, &u_old, &origin);
    CHECK(signal_next(&s) == 0,
          "one take clears it, however many times it was raised, two "
          "ctrl+c presses in the same instant are one interrupt, and a "
          "design that counts them wants something else");
    CHECK(origin.i_code == SI_KERNEL,
          "and one nobody sent is the kernel's");

    CHECK(!signal_raise(&s, 0, NULL), "zero is not a signal");
    CHECK(!signal_raise(&s, SIGNAL_MAX, NULL),
          "and neither is one off the end");
    CHECK(!signal_raise(&s, -1, NULL), "or a negative one");

    signal_reset(&s);
    signal_raise(&s, SIGTERM, NULL);
    signal_raise(&s, SIGKILL, NULL);
    CHECK(signal_next(&s) == SIGKILL,
          "kill at 9 beats term at 15, a process asked twice, politely "
          "and then not, should get the second one");

    /* who sent it travels with it, the last sender's when it was twice */
    signal_reset(&s);
    struct signal_origin from = { .i_code = SI_USER, .i_pid = 42, .u_uid = 7 };
    signal_raise(&s, SIGTERM, &from);
    signal_take(&s, SIGTERM, &act, &u_old, &origin);
    CHECK(origin.i_code == SI_USER && origin.i_pid == 42 && origin.u_uid == 7,
          "a signal says who sent it");
    struct siginfo info;
    signal_info(SIGTERM, &origin, &info);
    CHECK(info.si_signo == SIGTERM && info.si_code == SI_USER
          && info.u.proc.si_pid == 42 && info.u.proc.si_uid == 7,
          "and a handler's siginfo says so too");
    struct signal_origin fault = { .i_code = SEGV_MAPERR, .u_addr = 0x1230 };
    signal_info(SIGSEGV, &fault, &info);
    CHECK(info.si_code == SEGV_MAPERR && info.u.si_addr == 0x1230,
          "a fault's says where");

    signal_reset(&s);
    act = handler_at(0x400000, 0);
    CHECK(!signal_set_action(&s, SIGKILL, &act), "SIGKILL cannot be caught");
    CHECK(!signal_set_action(&s, SIGSTOP, &act), "and neither can SIGSTOP");
    act = handler_at(SIG_IGNORE, 0);
    CHECK(!signal_set_action(&s, SIGKILL, &act), "or ignored");
    act = handler_at(SIG_DEFAULT, 0);
    CHECK(signal_set_action(&s, SIGKILL, &act),
          "though asking for its default is no harm");
    act = handler_at(0x400000, 0);
    CHECK(signal_set_action(&s, SIGINT, &act), "while an ordinary one can");

    /*
     * refused rather than accepted and quietly ignored: a program told
     * yes and then killed anyway has been lied to
     */
    signal_reset(&s);
    signal_set_mask(&s, ~0ull);
    CHECK((signal_get_mask(&s) & SIGNAL_BIT(SIGKILL)) == 0,
          "blocking everything does not block SIGKILL");
    CHECK((signal_get_mask(&s) & SIGNAL_BIT(SIGSTOP)) == 0,
          "nor SIGSTOP, and this is silent rather than a refusal, "
          "because a great many programs block everything as a matter "
          "of course and failing them would buy nothing");
    CHECK((signal_get_mask(&s) & SIGNAL_BIT(SIGTERM)) != 0,
          "everything else is blocked as asked");

    signal_raise(&s, SIGKILL, NULL);
    CHECK(signal_next(&s) == SIGKILL,
          "and a blocked-everything process still gets killed, which is "
          "the entire reason the machine can be saved by somebody who "
          "can type");

    signal_reset(&s);
    signal_set_mask(&s, SIGNAL_BIT(SIGTERM));
    signal_raise(&s, SIGTERM, NULL);
    CHECK(signal_next(&s) == 0, "a blocked signal is not delivered");
    CHECK((s.pending & SIGNAL_BIT(SIGTERM)) != 0,
          "but it is still pending, blocking defers, it does not "
          "discard, and a signal thrown away by a mask is one the "
          "program never finds out about");

    signal_set_mask(&s, 0);
    CHECK(signal_next(&s) == SIGTERM, "and it arrives once unblocked");

    signal_reset(&s);
    CHECK(signal_action_for(&s, SIGTERM) == SIGNAL_ACTION_TERMINATE,
          "most signals kill by default");
    CHECK(signal_action_for(&s, SIGINT) == SIGNAL_ACTION_TERMINATE,
          "including the interrupt");
    CHECK(signal_action_for(&s, SIGCHLD) == SIGNAL_ACTION_IGNORE,
          "a child finishing does not, it is the commonest event on a "
          "machine running programs, and a default of death would have "
          "every shell exit the moment a command completed");
    CHECK(signal_action_for(&s, SIGTSTP) == SIGNAL_ACTION_STOP,
          "ctrl+z stops rather than kills");
    CHECK(signal_action_for(&s, SIGCONT) == SIGNAL_ACTION_CONTINUE,
          "and continue continues");
    CHECK(signal_action_for(&s, SIGTTIN) == SIGNAL_ACTION_STOP
          && signal_action_for(&s, SIGTTOU) == SIGNAL_ACTION_STOP,
          "a background job touching its terminal stops too");
    CHECK(signal_action_for(&s, SIGWINCH) == SIGNAL_ACTION_IGNORE
          && signal_action_for(&s, SIGURG) == SIGNAL_ACTION_IGNORE,
          "a terminal changing size, or urgent data, is news and no more");
    CHECK(signal_action_for(&s, SIGUSR1) == SIGNAL_ACTION_TERMINATE
          && signal_action_for(&s, SIGRTMAX) == SIGNAL_ACTION_TERMINATE,
          "the ones that mean what a program says end it unhandled");
    CHECK(signal_action_for(&s, SIGNAL_MAX) == SIGNAL_ACTION_NONE
          && !signal_raise(&s, SIGNAL_MAX, NULL)
          && !signal_raise(&s, 0, NULL),
          "and there is nothing past the last, or before the first");
    CHECK(signal_raise(&s, SIGRTMAX, NULL) && s.pending == SIGNAL_BIT(SIGRTMAX)
          && SIGNAL_BIT(SIGRTMAX) == 1ull << 63 && signal_next(&s) == SIGRTMAX,
          "the last of them is the top bit of the set");
    s.pending = 0;

    set_handler(&s, SIGINT, SIG_IGNORE);
    CHECK(signal_action_for(&s, SIGINT) == SIGNAL_ACTION_IGNORE,
          "an ignored signal is ignored");
    CHECK(signal_raise(&s, SIGINT, NULL) && s.pending == 0,
          "and is gone as it is sent, so it can cut no wait short");
    CHECK(signal_raise(&s, SIGCHLD, NULL) && s.pending == 0,
          "and the same for one ignored by default");
    signal_set_mask(&s, SIGNAL_BIT(SIGINT));
    CHECK(signal_raise(&s, SIGINT, NULL)
          && (s.pending & SIGNAL_BIT(SIGINT)) != 0,
          "unless it is blocked, when the action may have changed by the "
          "time it is let through");
    signal_set_mask(&s, 0);
    s.pending = 0;
    set_handler(&s, SIGINT, 0x401000);
    CHECK(signal_action_for(&s, SIGINT) == SIGNAL_ACTION_HANDLER,
          "and one with a handler runs it");

    set_handler(&s, SIGHUP, 0x401000);
    signal_raise(&s, SIGHUP, NULL);
    set_handler(&s, SIGHUP, SIG_IGNORE);
    CHECK((s.pending & SIGNAL_BIT(SIGHUP)) == 0,
          "ignoring one already pending throws it away");

    /* even with a handler somehow present, these two do not budge */
    s.action[SIGKILL].sa_handler = 0x401000;
    CHECK(signal_action_for(&s, SIGKILL) == SIGNAL_ACTION_TERMINATE,
          "a handler in the table for SIGKILL is not consulted, a "
          "process that could refuse to die leaves only the power "
          "switch");
    s.action[SIGSTOP].sa_handler = 0x401000;
    CHECK(signal_action_for(&s, SIGSTOP) == SIGNAL_ACTION_STOP,
          "and the same for SIGSTOP");

    signal_reset(&s);
    act = handler_at(0x402000, SA_RESTART);
    act.sa_mask = SIGNAL_BIT(SIGQUIT);
    signal_set_action(&s, SIGINT, &act);
    signal_set_mask(&s, SIGNAL_BIT(SIGHUP));
    signal_raise(&s, SIGINT, NULL);

    struct sigaction taken;
    CHECK(signal_take(&s, SIGINT, &taken, &u_old, &origin)
          == SIGNAL_ACTION_HANDLER,
          "taking it says to run the handler");
    CHECK(taken.sa_handler == 0x402000 && (taken.sa_flags & SA_RESTART),
          "and gives the action, its flags with it");
    CHECK(u_old == SIGNAL_BIT(SIGHUP),
          "and the mask it interrupted, for its frame to put back");
    CHECK((signal_get_mask(&s) & SIGNAL_BIT(SIGINT)) != 0
          && (signal_get_mask(&s) & SIGNAL_BIT(SIGQUIT)) != 0
          && (signal_get_mask(&s) & SIGNAL_BIT(SIGHUP)) != 0,
          "with its own signal and its sa_mask blocked for the duration, "
          "ctrl+c held down would otherwise re-enter the handler on top "
          "of itself until the stack met its guard page");

    signal_raise(&s, SIGINT, NULL);
    CHECK(signal_next(&s) == 0,
          "so another of the same waits rather than nesting");

    signal_set_mask(&s, u_old);
    CHECK(signal_next(&s) == SIGINT,
          "and the mask from the frame lets the waiting one through");

    /* SA_NODEFER and SA_RESETHAND */
    signal_reset(&s);
    act = handler_at(0x405000, SA_NODEFER | SA_RESETHAND);
    signal_set_action(&s, SIGUSR1, &act);
    signal_raise(&s, SIGUSR1, NULL);
    signal_take(&s, SIGUSR1, &taken, &u_old, &origin);
    CHECK((signal_get_mask(&s) & SIGNAL_BIT(SIGUSR1)) == 0,
          "SA_NODEFER leaves its own signal unblocked in the handler");
    CHECK(s.action[SIGUSR1].sa_handler == SIG_DEFAULT
          && taken.sa_handler == 0x405000,
          "and SA_RESETHAND runs it once, the default after");

    /* a wait under a mask of its own: the handler returns to the old one */
    signal_reset(&s);
    set_handler(&s, SIGUSR2, 0x406000);
    signal_set_mask(&s, SIGNAL_BIT(SIGTERM));
    s.u_suspend_mask = signal_get_mask(&s);
    s.b_suspended = true;
    signal_set_mask(&s, 0);
    signal_raise(&s, SIGUSR2, NULL);
    signal_take(&s, SIGUSR2, &taken, &u_old, &origin);
    CHECK(u_old == SIGNAL_BIT(SIGTERM) && !s.b_suspended,
          "a signal ending a sigsuspend hands its handler the mask from "
          "before the wait");

    /* taking one that is not pending does nothing at all */
    signal_reset(&s);
    CHECK(signal_take(&s, SIGINT, &taken, &u_old, &origin)
          == SIGNAL_ACTION_NONE,
          "taking a signal nobody raised does nothing");

    signal_reset(&s);
    set_handler(&s, SIGINT, 0x403000);
    signal_set_mask(&s, SIGNAL_BIT(SIGTERM));
    signal_raise(&s, SIGHUP, NULL);

    struct signal_state child;
    signal_inherit(&child, &s);

    CHECK(child.action[SIGINT].sa_handler == 0x403000,
          "a child inherits the handlers");
    CHECK((signal_get_mask(&child) & SIGNAL_BIT(SIGTERM)) != 0,
          "and the mask");
    CHECK(child.pending == 0,
          "and *not* the pending set, a child is not born owing its "
          "parent's interrupts, and a ctrl+c the parent had not dealt "
          "with would otherwise kill a program that never ran");

    signal_reset(&s);
    set_handler(&s, SIGINT, 0x404000);
    set_handler(&s, SIGHUP, SIG_IGNORE);
    signal_set_mask(&s, SIGNAL_BIT(SIGTERM) | SIGNAL_BIT(SIGQUIT));
    signal_raise(&s, SIGQUIT, NULL);
    stack_t alt = { .ss_sp = 0x7000, .ss_size = 8192 };
    s.altstack = alt;
    signal_exec(&s);

    CHECK(s.action[SIGINT].sa_handler == SIG_DEFAULT,
          "exec puts handlers back to default, the code they pointed "
          "at is gone, and keeping them sends the next signal to "
          "whatever happens to be at that address in another program");
    CHECK(s.action[SIGHUP].sa_handler == SIG_IGNORE,
          "but an *ignored* signal stays ignored, which every unix does: "
          "a shell starting a background job with SIGINT ignored expects "
          "the child to keep ignoring it");
    CHECK((signal_get_mask(&s) & SIGNAL_BIT(SIGTERM)) != 0,
          "and the mask survives, which is what lets a program start a "
          "child with signals deliberately blocked");
    CHECK((s.pending & SIGNAL_BIT(SIGQUIT)) != 0,
          "and so does what was pending, owed to the process and not to "
          "the program it was running");
    CHECK((s.altstack.ss_flags & SS_DISABLE) && s.altstack.ss_sp == 0,
          "while the alternate stack, in memory that is gone, goes");

    CHECK(strcmp(signal_name(SIGKILL), "killed") == 0, "signals have names");
    CHECK(signal_name(31) != NULL, "and an unknown one still has something");

    if (failures == 0) printf("all good\n");
    return failures;
}
