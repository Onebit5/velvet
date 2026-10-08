// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_process.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the process table: the one structure whose job is to outlive things.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>

/* what the lock complains through */
void kprintf(const char *fmt, ...)
{
    (void)fmt;
}
void panic(const char *fmt, ...)
{
    printf("PANIC: ");
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    exit(1);
}

#include <stdbool.h>

#include "sched/process.h"

/* a pipe a forked child inherits gains a holder rather than being copied. */
static int shared_pipes;
void pipe_share(struct pipe *p, bool writing)
{
    (void)p; (void)writing; shared_pipes++;
}

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

/*
 * raising a signal has to *wake* the process, or one sent to
 * a program blocked on a read arrives whenever that read happens to
 * finish, which for a program waiting on the keyboard is never. the
 * waking is the scheduler's, and the scheduler is not what this suite is
 * about, so it is counted here rather than performed
 */
static int wakes;
void sched_wake_thread(int id)
{
    (void)id; wakes++;
}

int main(void)
{
    /*
     * first in the file, and that is not tidiness: this table hands out
     * pids in order from 1, so the only way to have a *real* process 1
     * to test against is to make it before anything else, which is
     * exactly what the machine does, and the reason pid 1 can be relied
     * on as the number reparenting points at.
     *
     * the problem being solved: a process that has ended and not been
     * collected is holding one of thirty-two slots. normally its parent
     * collects it, that is what `wait` is, but a parent can die
     * first, and then the exit code is addressed to nobody and the slot
     * is held forever. so children are reparented to init as their
     * parent goes, and init collects them.
     *
     * which ones is the delicate part, and it is most of what is below
     */

    CHECK(process_orphan() == 0, "an empty table has no orphans");

    int p1 = process_create("init", 0, 0, false, 0);
    CHECK(p1 == INIT_PID,
          "the first process is pid 1, which is what makes 1 a number "
          "reparenting can be written against");

    /* a shell's child, which the shell will wait for itself */
    int shell_child = process_create("cat", 0, 0, false, 0);
    process_exited(shell_child, 0, 0);
    CHECK(process_orphan() == 0,
          "a process whose parent is 0 is a kernel shell's, and the shell "
          "collects its own, taking this one would be a `$?` that is "
          "right sometimes and whatever was there before the rest of the "
          "time");
    process_collect(shell_child, NULL);

    /*
     * a child whose parent is alive and well is nobody's business but
     * the parent's, however long it has been sitting there finished
     */
    int waiting = process_create("waiting", 0, 0, false, 0);
    int watched = process_create("watched", waiting, 0, false, 0);
    process_exited(watched, 0, 5);
    CHECK(process_orphan() == 0,
          "and a finished child whose parent is still running is the "
          "parent's to collect");
    process_collect(watched, NULL);
    process_exited(waiting, 0, 6);
    process_collect(waiting, NULL);

    int mother   = process_create("mother", 0, 0, false, 0);
    int kid      = process_create("kid", mother, 0, false, 0);
    int grandkid = process_create("grandkid", kid, 0, false, 0);

    CHECK(process_find(kid)->parent == mother, "a child knows its parent");

    process_exited(mother, 0, 100);
    CHECK(process_find(kid)->parent == INIT_PID,
          "and when the parent dies the child becomes init's");
    CHECK(process_find(grandkid)->parent == kid,
          "but only its own children, reparenting one generation at a "
          "time, or a whole tree moves every time anybody in it dies");

    CHECK(process_orphan() == 0,
          "an adopted child that is still running is not collectable, "
          "however orphaned it is");

    process_exited(kid, 7, 110);
    CHECK(process_orphan() == kid,
          "one that has ended is, and this one's parent really is init, "
          "which is here in the table");
    CHECK(process_find(grandkid)->parent == INIT_PID,
          "and its own children move up in turn");
    process_collect(kid, NULL);

    /* the other clause: a child whose parent is not in the table at all. */
    int ghost = process_create("ghost", 0, 0, false, 0);
    process_exited(ghost, 0, 200);
    process_collect(ghost, NULL);       /* gone from the table altogether */

    int left_behind = process_create("left behind", ghost, 0, false, 0);
    CHECK(process_find(left_behind)->parent == ghost,
          "a process can be born naming a parent that has already gone");
    process_exited(left_behind, 0, 210);
    CHECK(process_orphan() == left_behind,
          "and it is init's too, by the parent not being anywhere rather "
          "than by having been handed over");
    process_collect(left_behind, NULL);

    process_exited(grandkid, 0, 220);
    process_collect(grandkid, NULL);
    process_collect(mother, NULL);

    CHECK(process_orphan() == 0, "and then there are none");

    /*
     * and init is not its own orphan, by the same rule as everything
     * else rather than by a special case, since init's parent is 0: it
     * is what kmain started, and kmain is not a process either
     */
    process_exited(p1, 0, 300);
    CHECK(process_find(p1)->parent == 0, "init's parent is nobody");
    CHECK(process_orphan() == 0, "so init is not its own orphan");
    process_collect(p1, NULL);

    CHECK(process_count() == 0, "and the table is empty for what follows");


    int pid = process_create("bin/hello", 0, 0, false, 1000);
    CHECK(pid > 0, "a process gets a pid");
    CHECK(process_count() == 1, "and occupies a slot");

    const struct process *p = process_find(pid);
    CHECK(p != NULL && strcmp(p->name, "bin/hello") == 0, "with its name");
    CHECK(p->started_ms == 1000, "and when it began");
    CHECK(!p->exited, "and it has not finished");

    process_set_thread(pid, 7);
    CHECK(process_find(pid)->thread_id == 7, "the test can note which thread runs it");

    /* collecting one that is still running must not succeed */
    int code = 999;
    CHECK(!process_collect(pid, &code), "a running process cannot be collected");
    CHECK(code == 999, "and the code is left alone");
    CHECK(process_count() == 1, "and it keeps its slot");


    process_exited(pid, 42, 1500);
    p = process_find(pid);
    CHECK(p->exited && p->exit_code == 42, "the code is recorded");
    CHECK(p->ended_ms == 1500, "and when");
    CHECK(p->thread_id == 0, "and the thread is forgotten, being gone");
    CHECK(process_count() == 1,
          "the slot stays occupied, this is the zombie, and the point");

    CHECK(process_collect(pid, &code) && code == 42, "collecting yields the code");
    CHECK(process_count() == 0, "and frees the slot");
    CHECK(process_find(pid) == NULL, "the pid means nothing afterwards");
    CHECK(!process_collect(pid, &code), "and cannot be collected twice");


    int a = process_create("one", 0, 0, false, 0);
    int b = process_create("two", a, 0, false, 0);
    CHECK(a != b, "two processes get different pids");
    CHECK(b > a, "and later ones are later");
    CHECK(process_find(b)->parent == a, "a parent is remembered");

    process_exited(a, 0, 10);
    process_collect(a, NULL);
    int c = process_create("three", 0, 0, false, 0);
    CHECK(c != a && c != b, "a freed slot does not hand back the old pid");
    CHECK(process_collect(a, NULL) == false,
          "and the collected pid stays meaningless");

    /* collecting with a NULL code pointer is allowed */
    process_exited(b, 3, 20);
    CHECK(process_collect(b, NULL), "a caller may not care what the code was");

    process_exited(c, 0, 0);
    process_collect(c, NULL);
    CHECK(process_count() == 0, "table empty again");


    int k = process_create("victim", 0, 0, false, 0);
    process_exited(k, PROCESS_KILLED, 5);
    CHECK(process_find(k)->exit_code == PROCESS_KILLED, "a kill is recorded");

    /*
     * a process already on its way out must keep its first answer,
     * otherwise a kill racing a clean exit rewrites history
     */
    process_exited(k, 0, 9);
    CHECK(process_find(k)->exit_code == PROCESS_KILLED,
          "the first ending is the true one");
    CHECK(process_find(k)->ended_ms == 5, "including when it happened");
    process_collect(k, NULL);


    int ids[4];
    for (int i = 0; i < 4; i++) {
        char name[8] = { 'p', (char)('0' + i), 0 };
        ids[i] = process_create(name, 0, 0, false, 0);
    }
    CHECK(process_count() == 4, "four in the table");

    size_t seen = 0;
    for (size_t i = 0; process_at(i) != NULL; i++) {
        seen++;
    }
    CHECK(seen == 4, "and the walk finds all four");

    /* a hole in the middle must not stop the walk */
    process_exited(ids[1], 0, 0);
    process_collect(ids[1], NULL);
    seen = 0;
    for (size_t i = 0; process_at(i) != NULL; i++) {
        seen++;
    }
    CHECK(seen == 3, "a freed slot in the middle is skipped, not fatal");

    for (int i = 0; i < 4; i++) {
        process_exited(ids[i], 0, 0);
        process_collect(ids[i], NULL);
    }


    int made = 0;
    for (int i = 0; i < MAX_PROCESSES + 4; i++) {
        if (process_create("crowd", 0, 0, false, 0) != 0) {
            made++;
        }
    }
    CHECK(made == MAX_PROCESSES, "the table fills to exactly its size");
    CHECK(process_create("one too many", 0, 0, false, 0) == 0,
          "and then says no rather than trampling somebody");

    /* empty it again, or everything below would be testing a full table */
    for (;;) {
        const struct process *q = process_at(0);
        if (q == NULL) break;
        process_exited(q->pid, 0, 0);
        process_collect(q->pid, NULL);
    }
    CHECK(process_count() == 0, "and empties again");


    CHECK(process_find(0) == NULL, "pid 0 is not a process");
    CHECK(process_find(-1) == NULL, "nor is a negative one");
    CHECK(!process_collect(0, NULL), "and neither can be collected");

    /* a job is a group, and everything the terminal does it does to a whole one. */
    {
        int a = process_create("cat", 0, 0, false, 0);
        int b = process_create("grep", 0, 0, false, 0);
        int c = process_create("wc", 0, 0, false, 0);
        process_set_thread(a, 31);
        process_set_thread(b, 32);
        process_set_thread(c, 33);

        CHECK(process_pgid(a) == a,
              "a process is its own group until told otherwise, which is "
              "what a command typed on its own is");

        process_set_pgid(b, a);
        process_set_pgid(c, a);
        CHECK(process_pgid(b) == a && process_pgid(c) == a,
              "and a pipeline joins the first one's");

        int ids[8];
        size_t n = process_group_threads(a, ids, 8);
        CHECK(n == 3, "all three threads are in the group");

        process_interrupt_group(a);
        CHECK(process_interrupt_pending(a) && process_interrupt_pending(b)
              && process_interrupt_pending(c),
              "and one interrupt reaches every one of them");

        CHECK(process_group_alive(a), "the group is alive while any of it is");

        /*
         * a member that has ended is not one to stop, continue or
         * interrupt, and asking the scheduler about its thread after
         * the reaper has been through would be asking about nothing
         */
        process_exited(b, 0, 0);
        n = process_group_threads(a, ids, 8);
        CHECK(n == 2, "a process that ended is not in the group any more");
        CHECK(process_group_alive(a), "though the group is still going");

        process_exited(a, 0, 0);
        process_exited(c, 0, 0);
        CHECK(!process_group_alive(a),
              "and is over once the last of it has gone");

        n = process_group_threads(a, ids, 8);
        CHECK(n == 0, "with nothing left to stop or continue");

        /* the array is filled to what it can hold and no further */
        CHECK(process_group_threads(a, ids, 0) == 0,
              "and asking for none gets none rather than a scribble");

        process_collect(a, NULL);
        process_collect(b, NULL);
        process_collect(c, NULL);
    }


    {
        static const char body[] = "hello there";
        int fp = process_create("reader", 0, 0, false, 0);

        CHECK(process_fd_count(fp) == 0, "a new process holds no files");

        int fd = process_fd_open(fp, body, 11);
        CHECK(fd >= FD_FIRST_FILE,
              "an opened file gets a number above the three the console owns");
        CHECK(process_fd_count(fp) == 1, "and is counted");

        const void *data = NULL;
        uint64_t left = 0;
        CHECK(process_fd_peek(fp, fd, &data, &left), "the fd can be looked at");
        CHECK(left == 11 && data == body, "at the beginning, with it all left");

        process_fd_advance(fp, fd, 5);
        process_fd_peek(fp, fd, &data, &left);
        CHECK(left == 6 && data == body + 5,
              "and remembers where it got to between reads");

        process_fd_advance(fp, fd, 999);
        process_fd_peek(fp, fd, &data, &left);
        CHECK(left == 0 && data == body + 11,
              "reading past the end lands exactly on it, rather than beyond");

        CHECK(!process_fd_peek(fp, FD_STDIN, NULL, NULL), "fd 0 is not a file");
        CHECK(!process_fd_peek(fp, FD_STDOUT, NULL, NULL), "nor fd 1");

        /* 0, 1 and 2 are real slots, so that redirection has somewhere to put a file. */
        struct fd std;
        CHECK(process_fd_get(fp, FD_STDIN, &std) && std.kind == FD_KEYBOARD,
              "a new process reads from the keyboard");
        CHECK(process_fd_get(fp, FD_STDOUT, &std) && std.kind == FD_CONSOLE,
              "and writes to the screen");
        CHECK(process_fd_get(fp, FD_STDERR, &std) && std.kind == FD_CONSOLE,
              "on both of them");

        /* which is exactly what redirection replaces */
        struct fd redirected;
        memset(&redirected, 0, sizeof redirected);
        redirected.kind = FD_MEMORY;
        redirected.data = (const uint8_t *)body;
        redirected.size = 11;
        CHECK(process_fd_install(fp, FD_STDOUT, &redirected),
              "something else can be put in slot 1");
        CHECK(process_fd_get(fp, FD_STDOUT, &std) && std.kind == FD_MEMORY,
              "and that is what is there afterwards");

        redirected.kind = FD_CONSOLE;
        process_fd_install(fp, FD_STDOUT, &redirected);
        CHECK(!process_fd_peek(fp, 99, NULL, NULL), "nor one out of range");
        CHECK(!process_fd_peek(fp, -1, NULL, NULL), "nor a negative one");

        struct pipe_end closing;
        CHECK(process_fd_close(fp, fd, &closing), "closing works");
        CHECK(closing.p == NULL, "with no pipe to let go of, since it was a file");
        CHECK(!process_fd_close(fp, fd, &closing), "but only once");
        CHECK(!process_fd_peek(fp, fd, NULL, NULL), "and the fd is gone");

        int opened = 0;
        for (int i = 0; i < MAX_FDS + 4; i++) {
            if (process_fd_open(fp, body, 11) >= 0) opened++;
        }
        CHECK(opened == MAX_FDS - FD_FIRST_FILE,
              "it fills to exactly the fds that are not the console");
        CHECK(process_fd_open(fp, body, 11) == -1,
              "and then says no rather than overwriting one");

        process_exited(fp, 0, 0);
        CHECK(process_fd_count(fp) == 0,
              "a process that ends takes its open files with it");
        process_collect(fp, NULL);

        CHECK(process_fd_open(9999, body, 11) == -1, "no process, no fd");
        CHECK(!process_fd_peek(9999, 3, NULL, NULL), "and nothing to peek at");
    }

    /*
     * a deadline rather than a countdown: a countdown must be
     * decremented by somebody on a schedule, and every tick touching
     * every process is a cost the machine pays whether or not anybody
     * set one
     */
    {
        int who = process_create("sleeper", 0, 0, false, 0);
        CHECK(who > 0, "a process to wake");

        CHECK(process_set_alarm(who, 5, 1000) == 0,
              "setting an alarm reports nothing left on the last one");

        process_check_alarms(3000);
        CHECK(!process_interrupt_pending(who), "and it does not fire early");

        CHECK(process_set_alarm(who, 10, 3000) == 3,
              "resetting it says how many seconds the old one had left, "
              "which is what a program restoring an alarm it displaced "
              "needs to know");

        process_check_alarms(12000);
        CHECK(!process_interrupt_pending(who),
              "the replaced deadline is the one that counts");

        process_check_alarms(13000);
        CHECK(process_interrupt_pending(who),
              "and the new one fires when it is due");

        (void)process_take_interrupt(who);
        process_check_alarms(20000);
        CHECK(!process_interrupt_pending(who),
              "an alarm fires once and then is gone, a repeating one is "
              "a different thing and nobody asked for it");

        process_set_alarm(who, 5, 20000);
        CHECK(process_set_alarm(who, 0, 21000) == 4, "zero cancels");
        process_check_alarms(30000);
        CHECK(!process_interrupt_pending(who), "and nothing fires after");
    }

    if (!failures) printf("all good\n");
    return failures;
}
