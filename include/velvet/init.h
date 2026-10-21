// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/init.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * init: the first process, and the one that owns the rest.
 */

/* the design notes for init.h are in docs/subsystems/mm.rst */

#ifndef VELVET_INIT_H
#define VELVET_INIT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <velvet/process.h>

/* init: the first process, and the one that owns the rest. */

#define INIT_SERVICES_MAX  8
#define INIT_NAME_MAX      12

/*
 * how many times a service may come back before init decides it is not
 * coming back, and over what stretch of time.
 *
 * something that dies instantly and is restarted instantly is a machine
 * that does nothing else ever again, a login prompt that cannot draw
 * itself will happily consume every cycle there is, forever, printing
 * half a prompt. every init since sysvinit has had this rule and this is
 * why. the window matters as much as the count: five deaths in a second
 * is a loop, five deaths across an afternoon is five separate accidents
 * and each of them deserves a restart
 */
#define INIT_RESPAWN_MAX        5
#define INIT_RESPAWN_WINDOW_MS  10000

enum service_state {
    SERVICE_STOPPED = 0,    /* not running */
    SERVICE_RUNNING,
    SERVICE_GIVEN_UP,       /* died too often, too fast. left alone */
};

struct service {
    char  name[INIT_NAME_MAX];
    void (*entry)(void *);
    void *arg;

    unsigned console;

    bool respawn;

    enum service_state state;
    int      thread_id;

    unsigned starts;            /* since boot, for `init` to print */
    unsigned in_window;         /* since the current window opened */
    uint64_t window_start_ms;
};

struct init_table {
    struct service s[INIT_SERVICES_MAX];
    size_t         count;
};

enum init_action {
    INIT_LEAVE,         /* it was never meant to come back */
    INIT_RESTART,
    INIT_GIVE_UP,       /* it is dying in a loop. stop feeding it */
};

void init_table_reset(struct init_table *t);

bool init_add(struct init_table *t, const char *name, void (*entry)(void *),
              void *arg, unsigned console, bool respawn);

void init_started(struct init_table *t, size_t i, int tid, uint64_t now_ms);

/* and that it is not any more. returns what should happen next */
enum init_action init_died(struct init_table *t, size_t i, uint64_t now_ms);

/* bring one back that init had given up on, or that was never meant to restart. */
bool init_revive(struct init_table *t, size_t i);

int init_find(const struct init_table *t, const char *name);

const char *init_state_name(enum service_state s);

/*
 * declared here whether or not there is a machine to run it on, the way
 * the mouse driver declares mouse_init. the *definitions* are the half
 * that touches threads and reset lines and are guarded in init.c, a
 * declaration costs nothing, and having it means a host test can stub
 * one of these deliberately rather than the header quietly hiding a name
 * that was supposed to exist
 */

bool init_boot(void);

enum init_stop {
    INIT_REBOOT,
    INIT_POWEROFF,
};

void init_stop_machine(enum init_stop how) __attribute__((noreturn));

#endif
