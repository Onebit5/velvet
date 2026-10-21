// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_tty.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * who the keyboard belongs to.
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>

/* what the lock complains through */
void panic(const char *fmt, ...)
{
    printf("PANIC: ");
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    exit(1);
}

#include <stdbool.h>

static char out[2048];
static size_t out_len;
static void out_reset(void)
{
    out[0] = 0; out_len = 0;
}
void kprintf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    out_len += vsnprintf(out + out_len, sizeof out - out_len, fmt, ap);
    va_end(ap);
}

/* which console the calling thread is on, and which is being looked at. */
#include <velvet/thread.h>
static struct thread this_thread;
struct thread *sched_current(void)
{
    return &this_thread;
}

static unsigned shown;
static int switches;
unsigned console_active(void)
{
    return shown;
}
void console_switch(unsigned n)
{
    shown = n; switches++;
}
static int scrolled;
void console_scroll_back(int lines)
{
    scrolled += lines;
}

/* what the tty does to threads, recorded rather than done */
static int woken_thread = -1;
static int killed_thread = -1;
void sched_wake_thread(int id)
{
    woken_thread = id;
}
#include <velvet/sched.h>
enum sched_kill_result sched_kill(int id)
{
    killed_thread = id; return SCHED_KILL_OK;
}

/* which threads ctrl+z suspended. */
static int stopped[8];
static int stopped_count;
void sched_set_stopped(int id, bool stop)
{
    if (stop && stopped_count < 8) {
        stopped[stopped_count++] = id;
    }
}
bool sched_thread_stopped(int id)
{
    for (int i = 0; i < stopped_count; i++) {
        if (stopped[i] == id) return true;
    }
    return false;
}

/*
 * a scripted keyboard: the test says what gets typed, and the line
 * discipline reads it exactly as it would read a person
 */
static const int *script;
static int script_len, script_at;
int input_getchar_blocking(void)
{
    return (script_at < script_len) ? script[script_at++] : '\n';
}

#include <velvet/process.h>
#include <velvet/tty.h>
#include <velvet/console.h>

/* a pipe a forked child inherits gains a holder rather than being copied. */
struct pipe;
void pipe_share(struct pipe *p, bool writing)
{
    (void)p; (void)writing;
}

#include <velvet/input.h>

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

int main(void)
{

    CHECK(tty_foreground() == TTY_NOBODY,
          "the shell holds the terminal when nothing is running");

    /*
     * the shell wants ctrl+c as an ordinary key: its line editor uses it
     * to abandon a line, so the tty must not swallow it
     */
    CHECK(!tty_intercept(KEY_CTRL_C),
          "ctrl+c reaches the shell as a key, not as an interrupt");
    CHECK(!tty_intercept('a'), "and so does everything else");
    CHECK(!tty_intercept(KEY_UP), "including keys that are not characters");


    int pid = process_create("bin/ask", 0, 0, false, 0);
    process_set_thread(pid, 9);
    tty_set_foreground(pid);
    CHECK(tty_foreground() == pid, "a program can hold the terminal");

    CHECK(!tty_intercept('x'), "ordinary keys still go to the buffer");
    CHECK(!process_interrupt_pending(pid), "and are not interrupts");


    /*
     * a program in front: ctrl+c is SIGINT to its group, whether or not
     * a program has taken the console as its terminal
     */
    woken_thread = -1;
    killed_thread = -1;
    CHECK(tty_intercept(KEY_CTRL_C),
          "ctrl+c aimed at a program is taken");
    CHECK(process_signal_state(pid)->pending & (1u << SIGINT),
          "as a SIGINT to it");
    CHECK(process_interrupt_pending(pid),
          "which is what a call it is blocked in sees");
    CHECK(woken_thread == 9,
          "and it is woken, so a sleeping program finds out now rather "
          "than whenever it next happens to ask for something");
    CHECK(killed_thread == -1, "but it is not killed, it gets to decide");

    /* asking does not consume it; taking does */
    CHECK(process_interrupt_pending(pid), "asking again still finds it");
    CHECK(process_take_interrupt(pid), "taking it works");
    CHECK(!process_interrupt_pending(pid), "and it is gone afterwards");
    CHECK(!process_take_interrupt(pid), "and cannot be taken twice");

    tty_intercept(KEY_CTRL_C);
    killed_thread = -1;
    CHECK(tty_intercept(KEY_CTRL_C) && killed_thread == -1,
          "a second ctrl+c is another SIGINT, not a kill: what becomes "
          "of a program is the business of whoever started it");
    process_take_interrupt(pid);

    /*
     * `cat x | grep y | wc -l` is three processes and one thing the
     * person typing it is thinking about. interrupting only the last of
     * three would leave the other two writing into a pipe nobody reads
     */
    {
        int a = process_create("cat", 0, 0, false, 0);
        int b = process_create("grep", 0, 0, false, 0);
        int c = process_create("wc", 0, 0, false, 0);
        process_set_thread(a, 21);
        process_set_thread(b, 22);
        process_set_thread(c, 23);
        process_set_pgid(b, a);
        process_set_pgid(c, a);
        tty_set_foreground(a);

        CHECK(process_pgid(a) == a && process_pgid(b) == a,
              "a pipeline is one group, named after the first of them");

        CHECK(tty_intercept(KEY_CTRL_C), "ctrl+c is taken");
        CHECK(process_interrupt_pending(a), "and reaches the first");
        CHECK(process_interrupt_pending(b), "and the middle");
        CHECK(process_interrupt_pending(c),
              "and the last, all of it, or the survivors write into a "
              "pipe nobody is reading");

        process_take_interrupt(a);
        process_take_interrupt(b);
        process_take_interrupt(c);

        stopped_count = 0;
        CHECK(tty_intercept(KEY_CTRL_Z), "ctrl+z is taken as well");
        CHECK(stopped_count == 3, "and stops every member of the job");
        CHECK(sched_thread_stopped(21) && sched_thread_stopped(22)
              && sched_thread_stopped(23), "all three of them");
        CHECK(tty_foreground() == a,
              "and the terminal stays where it is, for whoever started "
              "the job to take back");

        process_exited(a, 0, 0); process_collect(a, NULL);
        process_exited(b, 0, 0); process_collect(b, NULL);
        process_exited(c, 0, 0); process_collect(c, NULL);
    }

    /*
     * ctrl+z with no program in front means nothing, but it must not
     * reach the line editor as a stray character either
     */
    tty_set_foreground(TTY_NOBODY);
    stopped_count = 0;
    CHECK(tty_intercept(KEY_CTRL_Z),
          "ctrl+z at a prompt is swallowed rather than typed");
    CHECK(stopped_count == 0, "and stops nobody");

    /* the terminal layer was one machine pretending to be one seat. */
    {
        this_thread.console = 0;
        shown = 0;
        switches = 0;

        /*
         * switching is answered by the terminal itself rather than
         * passed on, which screen is shown is the machine's business,
         * not the business of whatever happens to be running
         */
        CHECK(tty_intercept(KEY_CONSOLE_1 + 2), "alt+f3 is taken");
        CHECK(shown == 2 && switches == 1, "and shows console 3");
        CHECK(tty_intercept(KEY_CONSOLE_1), "alt+f1 too");
        CHECK(shown == 0, "and comes back");

        scrolled = 0;
        CHECK(tty_intercept(KEY_SCROLL_UP), "shift+pageup is taken");
        CHECK(scrolled > 0, "and looks back up the console");
        CHECK(tty_intercept(KEY_SCROLL_DOWN), "shift+pagedown too");
        CHECK(scrolled == 0, "and comes back down");

        /* the foreground is per console */
        int a = process_create("one", 0, 0, false, 0);
        int b = process_create("two", 0, 0, false, 0);
        process_set_thread(a, 41);
        process_set_thread(b, 42);

        this_thread.console = 0;
        tty_set_foreground(a);
        this_thread.console = 1;
        tty_set_foreground(b);

        this_thread.console = 0;
        CHECK(tty_foreground() == a, "console 1 has its own foreground");
        this_thread.console = 1;
        CHECK(tty_foreground() == b, "and console 2 has another");

        /*
         * the front of each console may read, shown or not. keys only
         * ever land in the ring of the console being looked at, so one
         * nobody is looking at waits for its own rather than taking
         * somebody else's
         */
        shown = 0;
        this_thread.console = 0;
        CHECK(tty_is_current(a), "the front of the shown console may read");
        this_thread.console = 1;
        CHECK(tty_is_current(b),
              "and so may the front of a console nobody is looking at");

        static const int s_arr_typed[] = { 'h', 'i', '\n' };
        char buf[16];
        script = s_arr_typed; script_len = 3; script_at = 0;
        CHECK(tty_read_line(b, buf, sizeof buf) == 3
              && memcmp(buf, "hi\n", 3) == 0,
              "its read waits for what is typed at its own console");
        CHECK(!tty_is_current(a),
              "while a process at the front of another console is not "
              "at the front of this one");

        shown = 1;

        /*
         * ctrl+c goes to the console being looked at, not to whichever
         * console the interrupted thread happened to be on
         */
        shown = 0;
        this_thread.console = 3;        /* an interrupt is on nobody's */
        CHECK(tty_intercept(KEY_CTRL_C), "ctrl+c is taken");
        CHECK(process_interrupt_pending(a),
              "and reaches the front of the console on the screen");
        CHECK(!process_interrupt_pending(b),
              "and not the front of one that is not");
        process_take_interrupt(a);

        this_thread.console = 0;
        tty_set_foreground(TTY_NOBODY);
        this_thread.console = 1;
        tty_set_foreground(TTY_NOBODY);
        this_thread.console = 0;
        shown = 0;

        process_exited(a, 0, 0); process_collect(a, NULL);
        process_exited(b, 0, 0); process_collect(b, NULL);
    }


    tty_set_foreground(TTY_NOBODY);
    killed_thread = -1;
    CHECK(!tty_intercept(KEY_CTRL_C),
          "with the shell back at the front, ctrl+c is a key again");
    CHECK(killed_thread == -1, "and kills nobody");

    /* a foreground pid that no longer exists must not crash the tty */
    tty_set_foreground(4242);
    (void)tty_intercept(KEY_CTRL_C);
    (void)tty_intercept(KEY_CTRL_C);
    CHECK(true, "a foreground pid that has gone does not take the test with it");
    tty_set_foreground(TTY_NOBODY);

    /* a program in ring 3 never sees the keys go past, so it cannot echo them itself. */
    {
        int pid2 = process_create("bin/ask", 0, 0, false, 0);
        process_set_thread(pid2, 11);
        tty_set_foreground(pid2);

        char buf[64];

        /* typing a word and pressing enter */
        static const int typed[] = { 'I', 'g', 'o', 'r', '\n' };
        script = typed; script_len = 5; script_at = 0;
        out_reset();
        int64_t n = tty_read_line(pid2, buf, sizeof buf);
        CHECK(n == 5, "a line comes back with its newline");
        CHECK(memcmp(buf, "Igor\n", 5) == 0, "and holds what was typed");
        CHECK(strcmp(out, "Igor\n") == 0,
              "and every character was echoed as it was typed, without "
              "this you type into a void");

        /*
         * backspace takes a character off the screen as well as the
         * buffer, or the display and the line stop agreeing
         */
        static const int fixed[] = { 'I', 'g', 'p', '\b', 'o', 'r', '\n' };
        script = fixed; script_len = 7; script_at = 0;
        out_reset();
        n = tty_read_line(pid2, buf, sizeof buf);
        CHECK(n == 5 && memcmp(buf, "Igor\n", 5) == 0,
              "backspace removes the character from the line");
        CHECK(strstr(out, "\b \b") != NULL,
              "and erases it from the screen too");

        /*
         * backspace on an empty line must not run backwards past the
         * start, over the prompt somebody else printed
         */
        static const int over[] = { '\b', '\b', 'a', '\n' };
        script = over; script_len = 4; script_at = 0;
        out_reset();
        n = tty_read_line(pid2, buf, sizeof buf);
        CHECK(n == 2 && buf[0] == 'a', "backspace on an empty line does nothing");
        CHECK(strstr(out, "\b \b") == NULL, "and erases nothing");

        /*
         * arrows have no meaning in a line this simple, and echoing
         * them would draw nonsense
         */
        static const int arrows[] = { 'h', KEY_UP, KEY_LEFT, 'i', '\n' };
        script = arrows; script_len = 5; script_at = 0;
        out_reset();
        n = tty_read_line(pid2, buf, sizeof buf);
        CHECK(n == 3 && memcmp(buf, "hi\n", 3) == 0, "arrows are ignored");
        CHECK(strcmp(out, "hi\n") == 0, "and never echoed");

        /*
         * a full buffer hands over what it has rather than writing past
         * the end or dropping keys in silence
         */
        static const int lots[] = { 'a','b','c','d','e','f','\n' };
        script = lots; script_len = 7; script_at = 0;
        n = tty_read_line(pid2, buf, 4);
        CHECK(n == 3, "a full buffer returns early");
        CHECK(memcmp(buf, "abc", 3) == 0, "with exactly what fitted");

        /* an interrupt part way through abandons the line */
        static const int cut[] = { 'x', 'y' };
        script = cut; script_len = 2; script_at = 0;
        process_interrupt(pid2);
        out_reset();
        n = tty_read_line(pid2, buf, sizeof buf);
        CHECK(n == -1, "an interrupt before the first key abandons the read");

        /* and a process that is not at the front may not read at all */
        tty_set_foreground(TTY_NOBODY);
        CHECK(tty_read_line(pid2, buf, sizeof buf) == -1,
              "a background process is refused the keyboard");
        CHECK(tty_read_line(pid2, buf, 0) == -1, "and a zero-length read too");

        process_exited(pid2, 0, 0);
        process_collect(pid2, NULL);
    }

    /*
     * a console a program owns: it and the group in front may hand the
     * terminal on, nobody else may, and ctrl+c and ctrl+z are signals
     * to the group in front and nothing more
     */
    {
        int i_sh = process_create("sh", 0, 1000, false, 0);
        int i_job = process_create("job", i_sh, 1000, false, 0);
        int i_stranger = process_create("stranger", 0, 1000, false, 0);
        int i_rootly = process_create("rootly", 0, 0, false, 0);
        process_set_thread(i_job, 41);

        CHECK(!tty_attach(i_sh, VCONSOLE_COUNT)
              && this_thread.console == 0,
              "a console that is not there is nobody's to take");
        CHECK(tty_attach(i_sh, 1) && this_thread.console == 1
              && tty_foreground() == i_sh,
              "attaching puts the program's group in front of its console");
        CHECK(tty_hand_to(i_sh, i_job) && tty_foreground() == i_job,
              "the console's program hands it to a job");
        CHECK(!tty_hand_to(i_stranger, i_stranger),
              "a stranger may not take it");
        CHECK(!tty_hand_to(i_sh, 30000),
              "nor may it go to a group with nobody in it");
        CHECK(!tty_hand_to(i_sh, i_rootly), "nor to another user's");

        shown = 1;
        stopped_count = 0;
        killed_thread = -1;
        CHECK(tty_intercept(KEY_CTRL_C)
              && (process_signal_state(i_job)->pending & (1u << SIGINT)),
              "ctrl+c is SIGINT to the group in front");
        CHECK(tty_intercept(KEY_CTRL_C) && killed_thread == -1,
              "and a second is another SIGINT, not a kill");
        CHECK(tty_intercept(KEY_CTRL_Z) && stopped_count == 1
              && tty_foreground() == i_job,
              "ctrl+z stops the job and leaves the terminal where it is");
        CHECK(tty_hand_to(i_sh, i_sh) && tty_foreground() == i_sh,
              "for the program to take back itself");

        int arr_pids[] = { i_sh, i_job, i_stranger, i_rootly };
        for (int i_at = 0; i_at < 4; i_at++) {
            process_exited(arr_pids[i_at], 0, 0);
            process_collect(arr_pids[i_at], NULL);
        }
        shown = 0;
        this_thread.console = 0;
    }

    if (!failures) printf("all good\n");
    return failures;
}
