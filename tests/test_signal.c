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

#include "sched/signal.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

int main(void)
{
    struct signal_state s;
    uint64_t handler = 0;



    signal_reset(&s);
    CHECK(signal_next(&s) == 0, "nothing is pending to begin with");
    CHECK(!signal_deliverable(&s), "and there is nothing to deliver");

    CHECK(signal_raise(&s, SIGINT), "a signal is raised");
    CHECK(signal_next(&s) == SIGINT, "and is next");
    CHECK(signal_raise(&s, SIGINT), "raising it again is allowed");
    CHECK(signal_next(&s) == SIGINT, "and changes nothing");

    signal_take(&s, SIGINT, &handler);
    CHECK(signal_next(&s) == 0,
          "one take clears it, however many times it was raised, two "
          "ctrl+c presses in the same instant are one interrupt, and a "
          "design that counts them wants something else");

    CHECK(!signal_raise(&s, 0), "zero is not a signal");
    CHECK(!signal_raise(&s, SIGNAL_MAX), "and neither is one off the end");
    CHECK(!signal_raise(&s, -1), "or a negative one");



    signal_reset(&s);
    signal_raise(&s, SIGTERM);
    signal_raise(&s, SIGKILL);
    CHECK(signal_next(&s) == SIGKILL,
          "kill at 9 beats term at 15, a process asked twice, politely "
          "and then not, should get the second one");



    signal_reset(&s);
    CHECK(!signal_set_handler(&s, SIGKILL, 0x400000),
          "SIGKILL cannot be caught");
    CHECK(!signal_set_handler(&s, SIGSTOP, 0x400000),
          "and neither can SIGSTOP");
    CHECK(signal_set_handler(&s, SIGINT, 0x400000),
          "while an ordinary one can");

    /*
     * refused rather than accepted and quietly ignored: a program told
     * yes and then killed anyway has been lied to
     */
    signal_reset(&s);
    signal_set_mask(&s, 0xffffffff);
    CHECK((signal_get_mask(&s) & (1u << SIGKILL)) == 0,
          "blocking everything does not block SIGKILL");
    CHECK((signal_get_mask(&s) & (1u << SIGSTOP)) == 0,
          "nor SIGSTOP, and this is silent rather than a refusal, "
          "because a great many programs block everything as a matter "
          "of course and failing them would buy nothing");
    CHECK((signal_get_mask(&s) & (1u << SIGTERM)) != 0,
          "everything else is blocked as asked");

    signal_raise(&s, SIGKILL);
    CHECK(signal_next(&s) == SIGKILL,
          "and a blocked-everything process still gets killed, which is "
          "the entire reason the machine can be saved by somebody who "
          "can type");

    signal_reset(&s);
    signal_set_mask(&s, 1u << SIGTERM);
    signal_raise(&s, SIGTERM);
    CHECK(signal_next(&s) == 0, "a blocked signal is not delivered");
    CHECK((s.pending & (1u << SIGTERM)) != 0,
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

    signal_set_handler(&s, SIGINT, SIG_IGNORE);
    CHECK(signal_action_for(&s, SIGINT) == SIGNAL_ACTION_IGNORE,
          "an ignored signal is ignored");
    signal_set_handler(&s, SIGINT, 0x401000);
    CHECK(signal_action_for(&s, SIGINT) == SIGNAL_ACTION_HANDLER,
          "and one with a handler runs it");

    /* even with a handler somehow present, these two do not budge */
    s.handler[SIGKILL] = 0x401000;
    CHECK(signal_action_for(&s, SIGKILL) == SIGNAL_ACTION_TERMINATE,
          "a handler in the table for SIGKILL is not consulted, a "
          "process that could refuse to die leaves only the power "
          "switch");
    s.handler[SIGSTOP] = 0x401000;
    CHECK(signal_action_for(&s, SIGSTOP) == SIGNAL_ACTION_STOP,
          "and the same for SIGSTOP");


    signal_reset(&s);
    signal_set_handler(&s, SIGINT, 0x402000);
    signal_raise(&s, SIGINT);

    handler = 0;
    CHECK(signal_take(&s, SIGINT, &handler) == SIGNAL_ACTION_HANDLER,
          "taking it says to run the handler");
    CHECK(handler == 0x402000, "and gives the address");
    CHECK((signal_get_mask(&s) & (1u << SIGINT)) != 0,
          "with its own signal blocked for the duration, ctrl+c held "
          "down would otherwise re-enter the handler on top of itself "
          "until the stack met its guard page");

    signal_raise(&s, SIGINT);
    CHECK(signal_next(&s) == 0,
          "so another of the same waits rather than nesting");

    signal_handler_returned(&s);
    CHECK((signal_get_mask(&s) & (1u << SIGINT)) == 0,
          "and the mask goes back when the handler returns");
    CHECK(signal_next(&s) == SIGINT, "letting the waiting one through");

    /* a mask set *before* the handler ran is restored, not cleared */
    signal_reset(&s);
    signal_set_handler(&s, SIGINT, 0x402000);
    signal_set_mask(&s, 1u << SIGHUP);
    signal_raise(&s, SIGINT);
    signal_take(&s, SIGINT, &handler);
    signal_handler_returned(&s);
    CHECK((signal_get_mask(&s) & (1u << SIGHUP)) != 0,
          "a mask the program set itself survives a handler, restoring "
          "it to empty would quietly unblock things nobody unblocked");

    /* taking one that is not pending does nothing at all */
    signal_reset(&s);
    CHECK(signal_take(&s, SIGINT, &handler) == SIGNAL_ACTION_NONE,
          "taking a signal nobody raised does nothing");



    signal_reset(&s);
    signal_set_handler(&s, SIGINT, 0x403000);
    signal_set_mask(&s, 1u << SIGTERM);
    signal_raise(&s, SIGHUP);

    struct signal_state child;
    signal_inherit(&child, &s);

    CHECK(child.handler[SIGINT] == 0x403000, "a child inherits the handlers");
    CHECK((signal_get_mask(&child) & (1u << SIGTERM)) != 0,
          "and the mask");
    CHECK(child.pending == 0,
          "and *not* the pending set, a child is not born owing its "
          "parent's interrupts, and a ctrl+c the parent had not dealt "
          "with would otherwise kill a program that never ran");



    signal_reset(&s);
    signal_set_handler(&s, SIGINT, 0x404000);
    signal_set_handler(&s, SIGHUP, SIG_IGNORE);
    signal_set_mask(&s, 1u << SIGTERM);
    signal_raise(&s, SIGQUIT);
    signal_exec(&s);

    CHECK(s.handler[SIGINT] == SIG_DEFAULT,
          "exec puts handlers back to default, the code they pointed "
          "at is gone, and keeping them sends the next signal to "
          "whatever happens to be at that address in another program");
    CHECK(s.handler[SIGHUP] == SIG_IGNORE,
          "but an *ignored* signal stays ignored, which every unix does: "
          "a shell starting a background job with SIGINT ignored expects "
          "the child to keep ignoring it");
    CHECK((signal_get_mask(&s) & (1u << SIGTERM)) != 0,
          "and the mask survives, which is what lets a program start a "
          "child with signals deliberately blocked");
    CHECK(s.pending == 0, "nothing is left pending across it");



    CHECK(strcmp(signal_name(SIGKILL), "killed") == 0, "signals have names");
    CHECK(signal_name(31) != NULL, "and an unknown one still has something");

    if (failures == 0) printf("all good\n");
    return failures;
}
