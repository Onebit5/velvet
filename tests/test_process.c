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

#include <velvet/process.h>
#include <uapi/velvet/syscall.h>
#include <uapi/velvet/errno.h>

/*
 * files are counted here rather than made: the table under test only
 * takes and lets go of references, and these say how many it holds
 */
#include <velvet/file.h>
static int s_i_released;
void file_get(struct file *p_file)
{
    p_file->u_refs++;
}
void file_put(struct file *p_file)
{
    if (p_file != NULL && --p_file->u_refs == 0) {
        s_i_released++;
    }
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

/* which thread was last stopped or let go, and which way */
static int s_i_stopped_id;
static bool s_b_stopped;
void sched_set_stopped(int i_id, bool b_stopped)
{
    s_i_stopped_id = i_id;
    s_b_stopped = b_stopped;
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

    int p1 = process_create("init", 0, 0, false, 0);
    CHECK(p1 == INIT_PID,
          "the first process is pid 1, which is what makes 1 a number "
          "reparenting can be written against");

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

    process_exited(kid, 7, 110);
    CHECK(process_find(grandkid)->parent == INIT_PID,
          "and its own children move up in turn");
    process_collect(kid, NULL);

    /*
     * a parent that ended before its child was made had already handed
     * its children on, so the child has to be init's from birth, or
     * nobody would ever collect it
     */
    int late = process_create("late", mother, 0, false, 0);
    CHECK(process_find(late)->parent == INIT_PID,
          "a child born to a parent that has ended is init's");
    process_exited(late, 0, 205);
    process_collect(late, NULL);

    int ghost = process_create("ghost", 0, 0, false, 0);
    process_exited(ghost, 0, 200);
    process_collect(ghost, NULL);       /* gone from the table altogether */

    int left_behind = process_create("left behind", ghost, 0, false, 0);
    CHECK(process_find(left_behind)->parent == INIT_PID,
          "and so is one born to a parent that is not there at all");
    process_exited(left_behind, 0, 210);
    process_collect(left_behind, NULL);

    process_exited(grandkid, 0, 220);
    process_collect(grandkid, NULL);
    process_collect(mother, NULL);

    /* init's parent is 0: kmain started it, and kmain is not a process */
    process_exited(p1, 0, 300);
    CHECK(process_find(p1)->parent == 0, "init's parent is nobody");
    process_collect(p1, NULL);

    CHECK(process_count() == 0, "and the table is empty for what follows");


    /*
     * a process that ends keeps what it had open in its slots, for
     * process_fds_release to let go of with no lock held
     */
    {
        struct file pipe_end = { .u_refs = 1 };
        struct file memory_file = { .u_refs = 1 };
        struct file *p_old = NULL;
        int i_writer = process_create("bin/writer", 0, 0, false, 400);

        process_fd_replace(i_writer, FD_STDOUT, &pipe_end, false, &p_old);
        process_fd_add(i_writer, &memory_file, 0, false);
        process_exited(i_writer, 0, 410);
        CHECK(process_fd_count(i_writer) == 2 && pipe_end.u_refs == 1,
              "an exited process still holds what it had open");
        process_fds_release(i_writer);
        CHECK(process_fd_count(i_writer) == 0 && pipe_end.u_refs == 0
              && memory_file.u_refs == 0,
              "until its descriptors are released, which puts each file");
        process_collect(i_writer, NULL);
    }

    /*
     * a fault's signal cannot be refused: one the program ignored or
     * blocked goes back to its default, so the program dies of it rather
     * than returning to the same instruction to fault again forever
     */
    {
        int i_busy = process_create("busy", 0, 0, false, 600);
        struct signal_state *p_sig = process_signal_state(i_busy);

        signal_set_handler(p_sig, SIGSEGV, SIG_IGNORE);
        process_force_signal(i_busy, SIGSEGV);
        CHECK(signal_action_for(p_sig, SIGSEGV) == SIGNAL_ACTION_TERMINATE
              && (p_sig->pending & SIGNAL_BIT(SIGSEGV)),
              "an ignored SIGSEGV is raised all the same, to end it");
        p_sig->pending = 0;
        p_sig->blocked |= SIGNAL_BIT(SIGILL);
        process_force_signal(i_busy, SIGILL);
        CHECK(!(p_sig->blocked & SIGNAL_BIT(SIGILL))
              && process_interrupt_pending(i_busy),
              "a blocked SIGILL is unblocked and delivered");
        p_sig->pending = 0;
        signal_set_handler(p_sig, SIGFPE, 0x401000);
        process_force_signal(i_busy, SIGFPE);
        CHECK(signal_action_for(p_sig, SIGFPE) == SIGNAL_ACTION_HANDLER,
              "and a handled one keeps its handler");
        process_exited(i_busy, 0, 610);
        process_collect(i_busy, NULL);
    }

    /*
     * stop and continue act when sent, since a stopped thread never runs
     * to take a pending SIGCONT, and a wait that asks is told of a stop
     * once. a stop that is ignored or blocked does nothing here
     */
    {
        int i_parent = process_create("sh", 0, 0, false, 500);
        int i_job = process_create("job", i_parent, 0, false, 500);
        int i_status = 0;
        bool b_ended = true;

        process_set_thread(i_job, 77);
        process_signal(i_job, SIGSTOP);
        CHECK(s_i_stopped_id == 77 && s_b_stopped,
              "SIGSTOP stops the thread when it is sent");
        CHECK(!(process_signal_state(i_job)->pending & SIGNAL_BIT(SIGSTOP)),
              "and leaves nothing pending to act on again");
        CHECK(process_reportable(i_parent, -1, true, &i_status, &b_ended)
              == i_job && !b_ended && i_status == (WAIT_STOPPED | SIGSTOP),
              "a wait that asks about stops is told");
        CHECK(process_reportable(i_parent, i_job, true, &i_status, &b_ended)
              == 0, "once");
        CHECK(process_reportable(i_parent, i_job, false, &i_status, &b_ended)
              == 0, "and one that does not ask is not told at all");

        process_signal(i_job, SIGCONT);
        CHECK(s_i_stopped_id == 77 && !s_b_stopped,
              "SIGCONT lets it go when it is sent");

        s_i_stopped_id = 0;
        signal_set_handler(process_signal_state(i_job), SIGTSTP, SIG_IGNORE);
        process_signal(i_job, SIGTSTP);
        CHECK(s_i_stopped_id == 0, "an ignored SIGTSTP stops nothing");
        signal_set_handler(process_signal_state(i_job), SIGTSTP, SIG_DEFAULT);
        process_signal_state(i_job)->blocked |= SIGNAL_BIT(SIGTSTP);
        process_signal(i_job, SIGTSTP);
        CHECK(s_i_stopped_id == 0
              && (process_signal_state(i_job)->pending & SIGNAL_BIT(SIGTSTP)),
              "and a blocked one waits, pending, for delivery");

        CHECK(process_reportable(i_parent, 4040, true, &i_status, &b_ended)
              == -1, "a pid that is not a child is -1");
        process_exited(i_job, 3, 510);
        CHECK(process_reportable(i_parent, -1, true, &i_status, &b_ended)
              == i_job && b_ended && i_status == 3,
              "and an ended child is told with its code");
        process_collect(i_job, NULL);
        process_exited(i_parent, 0, 510);
        process_collect(i_parent, NULL);
    }

    /*
     * a forked child is its parent's copy in everything it holds, and
     * owes nothing its parent had pending
     */
    {
        static const char s_arr_env[] = "PATH=/bin\0HOME=/\0";
        char arr_got[64];
        int i_parent = process_create("sh", 0, 1000, false, 700);
        int i_child = process_create("sh", i_parent, 1000, false, 700);
        struct signal_state *p_parent = process_signal_state(i_parent);
        struct signal_state *p_child = process_signal_state(i_child);

        process_set_env(i_parent, s_arr_env, sizeof s_arr_env);
        process_set_cwd(i_parent, "/work");
        process_set_pgid(i_parent, 77);
        process_set_trampoline(i_parent, 0x401234);
        signal_set_handler(p_parent, SIGINT, SIG_IGNORE);
        p_parent->blocked = SIGNAL_BIT(SIGALRM);
        process_signal(i_parent, SIGTERM);

        process_inherit(i_child, i_parent);
        CHECK(process_get_env(i_child, arr_got, sizeof arr_got)
              == sizeof s_arr_env
              && memcmp(arr_got, s_arr_env, sizeof s_arr_env) == 0,
              "the environment comes with it");
        CHECK(strcmp(process_cwd(i_child), "/work") == 0
              && process_pgid(i_child) == 77,
              "and where it stands, and the job it is part of");
        CHECK(p_child->handler[SIGINT] == SIG_IGNORE
              && p_child->blocked == SIGNAL_BIT(SIGALRM)
              && process_trampoline(i_child) == 0x401234,
              "and what it does with each signal");
        CHECK(p_child->pending == 0, "but nothing that was pending");

        process_exited(i_child, 0, 710);
        process_collect(i_child, NULL);
        process_exited(i_parent, 0, 710);
        process_collect(i_parent, NULL);
    }

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
        size_t n = process_group_pids(a, ids, 8);
        CHECK(n == 3, "all three are in the group");

        process_signal_group(a, SIGINT);
        CHECK(process_interrupt_pending(a) && process_interrupt_pending(b)
              && process_interrupt_pending(c),
              "and one SIGINT to the group reaches every one of them");

        /*
         * a member that has ended is not one to stop, continue or
         * interrupt, and asking the scheduler about its thread after
         * the reaper has been through would be asking about nothing
         */
        process_exited(b, 0, 0);
        n = process_group_pids(a, ids, 8);
        CHECK(n == 2, "a process that ended is not in the group any more");

        process_exited(a, 0, 0);
        process_exited(c, 0, 0);
        n = process_group_pids(a, ids, 8);
        CHECK(n == 0, "and the group is over once the last of it has gone");

        /* the array is filled to what it can hold and no further */
        CHECK(process_group_pids(a, ids, 0) == 0,
              "and asking for none gets none rather than a scribble");

        process_collect(a, NULL);
        process_collect(b, NULL);
        process_collect(c, NULL);
    }


    /* the descriptor table: numbers for files, shared rather than copied */
    {
        struct file files[MAX_FDS + 1];
        struct file *p_old = NULL;
        int fp = process_create("reader", 0, 0, false, 0);

        memset(files, 0, sizeof files);
        for (size_t i = 0; i <= MAX_FDS; i++) {
            files[i].u_refs = 1;
        }
        CHECK(process_fd_count(fp) == 0,
              "a new process holds nothing until it is given something");

        CHECK(process_fd_add(fp, &files[0], 0, false) == 0
              && process_fd_add(fp, &files[1], 0, false) == 1
              && process_fd_add(fp, &files[2], 5, false) == 5,
              "a file goes at the lowest free descriptor from where asked");
        CHECK(process_fd_count(fp) == 3, "and each is counted");

        struct file *p_got = process_fd_file(fp, 1);
        CHECK(p_got == &files[1] && files[1].u_refs == 2,
              "looking one up hands back its file, with a reference");
        file_put(p_got);
        CHECK(process_fd_file(fp, 2) == NULL && process_fd_file(fp, 99) == NULL
              && process_fd_file(fp, -1) == NULL,
              "and nothing for one not open, out of range, or negative");

        file_get(&files[0]);
        CHECK(process_fd_replace(fp, 5, &files[0], false, &p_old)
              && p_old == &files[2] && files[0].u_refs == 2,
              "one file can be at two descriptors, what was there handed "
              "back to be put");
        file_put(p_old);
        CHECK(files[2].u_refs == 0, "and put, it is let go of");
        CHECK(!process_fd_replace(fp, MAX_FDS, &files[3], false, &p_old)
              && p_old == NULL,
              "a descriptor out of range is refused, nothing changed");

        CHECK(process_fd_take(fp, 1) == &files[1]
              && process_fd_take(fp, 1) == NULL,
              "closing takes the file out once, and only once");
        file_put(&files[1]);

        /* what a child is given: the same files, not copies of them */
        int child = process_create("child", fp, 0, false, 0);
        CHECK(process_fds_inherit(child, fp) && files[0].u_refs == 4
              && process_fd_file(child, 5) == &files[0],
              "a child holds its parent's files, each with one more holder");
        file_put(&files[0]);
        process_fds_release(child);
        CHECK(files[0].u_refs == 2, "and lets go of them when it ends");
        process_exited(child, 0, 0);
        process_collect(child, NULL);

        /* the ones marked close-on-exec go when it runs another program */
        CHECK(process_fd_add(fp, &files[4], 0, true) == 1,
              "a descriptor can be marked to close on exec");
        process_fds_exec(fp);
        CHECK(process_fd_file(fp, 1) == NULL && files[4].u_refs == 0,
              "and closes when the process runs another program");
        p_got = process_fd_file(fp, 0);
        CHECK(p_got == &files[0], "while the rest stay open");
        file_put(p_got);

        static struct file s_arr_spare[MAX_FDS];
        int i_opened = 0;
        for (int i = 0; i < MAX_FDS; i++) {
            s_arr_spare[i].u_refs = 1;
            if (process_fd_add(fp, &s_arr_spare[i], 0, false) >= 0) {
                i_opened++;
            }
        }
        CHECK(i_opened == MAX_FDS - 2 && process_fd_count(fp) == MAX_FDS,
              "it fills to exactly the number of descriptors there are");
        CHECK(process_fd_add(fp, &files[3], 0, false) == -EMFILE
              && files[3].u_refs == 1,
              "and then says so, leaving the file the caller's");

        process_exited(fp, 0, 0);
        process_fds_release(fp);
        CHECK(process_fd_count(fp) == 0 && files[0].u_refs == 0,
              "a process that ends lets go of everything it held");
        process_collect(fp, NULL);

        CHECK(process_fd_add(9999, &files[3], 0, false) < 0,
              "no process, no descriptor");
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
