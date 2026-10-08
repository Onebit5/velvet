// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_init.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for init.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "sched/init.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

/* services never actually run here. */
static void nothing(void *arg)
{
    (void)arg;
}

/* start and immediately die, `times` over, `apart` milliseconds between each. */
static enum init_action churn(struct init_table *t, size_t i,
                              unsigned times, uint64_t apart,
                              uint64_t *clock)
{
    enum init_action last = INIT_LEAVE;
    for (unsigned n = 0; n < times; n++) {
        init_started(t, i, 100 + (int)n, *clock);
        *clock += apart;
        last = init_died(t, i, *clock);
    }
    return last;
}

int main(void)
{
    struct init_table t;



    init_table_reset(&t);
    CHECK(t.count == 0, "a fresh table looks after nothing");

    CHECK(init_add(&t, "flusher", nothing, NULL, 0, true), "a service goes in");
    CHECK(init_add(&t, "tty1", nothing, NULL, 0, true), "and another");
    CHECK(t.count == 2, "and the count says so");
    CHECK(t.s[0].state == SERVICE_STOPPED, "nothing is running until it is");
    CHECK(t.s[0].starts == 0, "and nothing has been started");

    CHECK(init_find(&t, "tty1") == 1, "one can be found by name");
    CHECK(init_find(&t, "tty9") == -1,
          "and asking for one that is not there is not an error, it is a no");

    /*
     * the table is fixed and small on purpose, this machine has five
     * services and will never have eight
     */
    init_table_reset(&t);
    for (int i = 0; i < INIT_SERVICES_MAX; i++) {
        char name[INIT_NAME_MAX];
        snprintf(name, sizeof name, "s%d", i);
        CHECK(init_add(&t, name, nothing, NULL, 0, true), "the table fills");
    }
    CHECK(!init_add(&t, "one too many", nothing, NULL, 0, true),
          "and then refuses rather than writing past the end of itself");

    /* a name longer than the field is cut rather than overrunning it */
    init_table_reset(&t);
    init_add(&t, "a-service-with-a-very-long-name", nothing, NULL, 0, true);
    CHECK(strlen(t.s[0].name) == INIT_NAME_MAX - 1,
          "a long name is truncated, and terminated");



    init_table_reset(&t);
    init_add(&t, "tty1", nothing, NULL, 2, true);

    init_started(&t, 0, 42, 1000);
    CHECK(t.s[0].state == SERVICE_RUNNING, "a started service is running");
    CHECK(t.s[0].thread_id == 42, "and remembers which thread it is");
    CHECK(t.s[0].starts == 1, "and that it has been started once");
    CHECK(t.s[0].console == 2, "and which screen it belongs to");

    CHECK(init_died(&t, 0, 1500) == INIT_RESTART,
          "a service that dies once comes back");
    CHECK(t.s[0].state == SERVICE_STOPPED, "and is not running in the meantime");
    CHECK(t.s[0].thread_id == 0,
          "and is not still pointing at a thread that has gone, a stale "
          "id is one that another thread will eventually be given");

    init_started(&t, 0, 43, 1600);
    CHECK(t.s[0].starts == 2, "the count of starts goes up");



    init_table_reset(&t);
    init_add(&t, "once", nothing, NULL, 0, false);
    init_started(&t, 0, 7, 0);
    CHECK(init_died(&t, 0, 10) == INIT_LEAVE,
          "a service marked not to respawn stays down");
    CHECK(t.s[0].state == SERVICE_STOPPED,
          "and is stopped rather than given up on, it did what it was "
          "asked to");

    /*
     * a login prompt that cannot draw itself will happily eat every
     * cycle the machine has, forever, printing half of itself. this is
     * the rule that stops it
     */

    init_table_reset(&t);
    init_add(&t, "broken", nothing, NULL, 0, true);

    uint64_t clock = 0;
    enum init_action last = churn(&t, 0, INIT_RESPAWN_MAX, 1, &clock);
    CHECK(last == INIT_RESTART,
          "dying five times in five milliseconds is still worth a restart");
    CHECK(t.s[0].state == SERVICE_STOPPED, "and it is not given up on yet");

    last = churn(&t, 0, 1, 1, &clock);
    CHECK(last == INIT_GIVE_UP,
          "and the one after that is where init stops, a service dying "
          "in a loop is a machine that does nothing else ever again");
    CHECK(t.s[0].state == SERVICE_GIVEN_UP, "and it says so");

    /* five deaths in a second is a loop. */

    init_table_reset(&t);
    init_add(&t, "tty1", nothing, NULL, 0, true);

    clock = 0;
    last = churn(&t, 0, INIT_RESPAWN_MAX * 4, INIT_RESPAWN_WINDOW_MS + 1,
                 &clock);
    CHECK(last == INIT_RESTART,
          "twenty deaths spread out are twenty restarts, because they are "
          "not a loop, it is somebody logging out");
    CHECK(t.s[0].state != SERVICE_GIVEN_UP, "and nothing is given up on");
    CHECK(t.s[0].starts == INIT_RESPAWN_MAX * 4,
          "and every one of them was counted");

    /* the boundary: deaths just inside the window do accumulate */
    init_table_reset(&t);
    init_add(&t, "tty1", nothing, NULL, 0, true);
    clock = 0;
    last = churn(&t, 0, INIT_RESPAWN_MAX + 1, INIT_RESPAWN_WINDOW_MS / 100,
                 &clock);
    CHECK(last == INIT_GIVE_UP,
          "deaths close enough together to fit inside the window do add up");

    /*
     * a service that dies a few times, then behaves, then dies a few
     * times much later must not be caught by the sum of the two
     */
    init_table_reset(&t);
    init_add(&t, "tty1", nothing, NULL, 0, true);
    clock = 0;
    churn(&t, 0, INIT_RESPAWN_MAX - 1, 1, &clock);
    clock += INIT_RESPAWN_WINDOW_MS * 10;      /* a long quiet stretch */
    last = churn(&t, 0, INIT_RESPAWN_MAX - 1, 1, &clock);
    CHECK(last == INIT_RESTART,
          "a quiet stretch in the middle starts the count again, what is "
          "being measured is a rate, not a total");



    init_table_reset(&t);
    init_add(&t, "broken", nothing, NULL, 0, true);
    clock = 0;
    churn(&t, 0, INIT_RESPAWN_MAX + 1, 1, &clock);
    CHECK(t.s[0].state == SERVICE_GIVEN_UP, "given up on");

    CHECK(init_revive(&t, 0), "and it can be started by hand");
    CHECK(t.s[0].state == SERVICE_STOPPED, "which clears the giving up");

    /* the important half: a revived service gets its allowance back. */
    init_started(&t, 0, 9, clock);
    CHECK(init_died(&t, 0, clock + 1) == INIT_RESTART,
          "and it gets a full allowance rather than being given up on at "
          "the first stumble");

    /*
     * reviving something that is already running would mean two threads
     * for one service, which is two shells on one console
     */
    init_table_reset(&t);
    init_add(&t, "tty1", nothing, NULL, 0, true);
    init_started(&t, 0, 3, 0);
    CHECK(!init_revive(&t, 0), "a running service is not started again");



    init_table_reset(&t);
    init_add(&t, "tty1", nothing, NULL, 0, true);
    CHECK(init_died(&t, 5, 0) == INIT_LEAVE,
          "an index past the end decides nothing rather than reading past "
          "the end of the table");
    init_started(&t, 5, 1, 0);      /* must simply do nothing */
    CHECK(t.count == 1, "and changes nothing");
    CHECK(!init_revive(&t, 5), "nor can one be revived");

    CHECK(strcmp(init_state_name(SERVICE_RUNNING), "running") == 0,
          "the states have names, since `init` prints them");
    CHECK(strcmp(init_state_name(SERVICE_GIVEN_UP), "given up") == 0,
          "including the one worth noticing");

    if (failures == 0) printf("all good\n");
    return failures;
}
