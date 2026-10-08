// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_pipe.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for pipes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

void panic(const char *fmt, ...)
{
    printf("PANIC: ");
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    exit(1);
}
void kprintf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}
void *kmalloc(size_t n)
{
    return malloc(n);
}
void kfree(void *p)
{
    free(p);
}

#include "sched/process.h"
#include "fs/pipe.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)



static struct pipe *sleeping_on;     /* set by the test, drained on sleep */
static int sleeps, wakes;

void waitq_enqueue(struct waitq *q)
{
    (void)q;
}
void waitq_wake_all(struct waitq *q)
{
    (void)q; wakes++;
}

/* what a reader on another core would have done while the test was asleep. */
void waitq_sleep(void)
{
    sleeps++;
    if (sleeping_on != NULL) {
        static char bin[PIPE_BUF];
        pipe_get(sleeping_on, bin, PIPE_BUF);
    }
    if (sleeps > 1000) {
        printf("FAIL: the blocking loop never made progress\n");
        exit(1);
    }
}

static bool interrupt_pending;
bool process_interrupt_pending(int pid)
{
    (void)pid; return interrupt_pending;
}

/* what a process was holding, so pipe_release_for has something to take away. */
static struct pipe *held_in, *held_out;
size_t process_take_pipes(int pid, struct pipe_end *out, size_t max)
{
    (void)pid;
    size_t n = 0;
    if (held_in != NULL && n < max) {
        out[n].p = held_in;
        out[n].writing = false;
        n++;
        held_in = NULL;
    }
    if (held_out != NULL && n < max) {
        out[n].p = held_out;
        out[n].writing = true;
        n++;
        held_out = NULL;
    }
    return n;
}

int main(void)
{
    static struct pipe p;



    pipe_reset(&p);
    CHECK(pipe_pending(&p) == 0, "a new pipe holds nothing");
    CHECK(pipe_room(&p) == PIPE_BUF, "and has room for a whole buffer");

    CHECK(pipe_put(&p, "hee-ho", 6) == 6, "six bytes go in");
    CHECK(pipe_pending(&p) == 6, "and are waiting");
    CHECK(pipe_room(&p) == PIPE_BUF - 6, "with the room down by six");

    char out[64];
    CHECK(pipe_get(&p, out, 6) == 6, "and come back out");
    CHECK(memcmp(out, "hee-ho", 6) == 0, "in one piece");
    CHECK(pipe_pending(&p) == 0, "leaving it empty again");



    static char big[PIPE_BUF * 2];
    memset(big, 'x', sizeof big);

    pipe_reset(&p);
    CHECK(pipe_put(&p, big, PIPE_BUF) == PIPE_BUF, "a whole buffer fits");
    CHECK(pipe_room(&p) == 0, "exactly, with nothing left over");
    CHECK(pipe_put(&p, "x", 1) == 0, "and one more byte does not");

    /* full and empty must not look alike. */
    CHECK(pipe_pending(&p) == PIPE_BUF, "a full pipe is full, not empty");
    CHECK(pipe_get(&p, big, PIPE_BUF) == PIPE_BUF, "it all comes back");
    CHECK(pipe_pending(&p) == 0, "and now it really is empty");
    CHECK(pipe_get(&p, big, 1) == 0, "which reads as nothing");

    /* a put larger than the buffer takes what fits and says how much */
    pipe_reset(&p);
    CHECK(pipe_put(&p, big, PIPE_BUF * 2) == PIPE_BUF,
          "an oversized write takes what fits and no more");

    /*
     * push the head most of the way round, then move a block that has
     * to be split across the end of the buffer
     */
    pipe_reset(&p);
    pipe_put(&p, big, PIPE_BUF - 10);
    pipe_get(&p, big, PIPE_BUF - 10);        /* head is now near the end */
    CHECK(pipe_pending(&p) == 0, "the pipe is empty with the head near the end");

    const char *straddle = "abcdefghijklmnopqrst";
    CHECK(pipe_put(&p, straddle, 20) == 20, "twenty bytes go in across the wrap");
    memset(out, 0, sizeof out);
    CHECK(pipe_get(&p, out, 20) == 20, "and twenty come back");
    CHECK(memcmp(out, straddle, 20) == 0,
          "in the right order, which is the whole point of the wrap");

    /* random sizes in and out, with the bytes checked against a counter that never repeats. */
    {
        pipe_reset(&p);
        unsigned seed = 12345;
        #define NEXT() (seed = seed * 1103515245u + 12345u, (seed >> 16) & 0x7fff)

        static uint8_t src[PIPE_BUF], dst[PIPE_BUF];
        uint32_t written = 0, read = 0;
        bool ok = true;

        for (int round = 0; round < 100000 && ok; round++) {
            uint32_t want = NEXT() % 300 + 1;
            for (uint32_t i = 0; i < want; i++) {
                src[i] = (uint8_t)((written + i) & 0xff);
            }
            written += pipe_put(&p, src, want);

            uint32_t take = NEXT() % 300 + 1;
            uint32_t got = pipe_get(&p, dst, take);
            for (uint32_t i = 0; i < got; i++) {
                if (dst[i] != (uint8_t)((read + i) & 0xff)) {
                    ok = false;
                    break;
                }
            }
            read += got;

            if (pipe_pending(&p) != written - read) {
                ok = false;
            }
        }
        CHECK(ok, "a hundred thousand random moves keep the stream in order");
        CHECK(written >= read, "and nothing came out that never went in");
        #undef NEXT
    }



    pipe_reset(&p);
    pipe_put(&p, "left over\n", 10);
    p.writers = 0;              /* the writer has gone */

    CHECK(pipe_read(&p, 1, out, sizeof out) == 10,
          "what was already written is still readable after the writer goes");
    CHECK(pipe_read(&p, 1, out, sizeof out) == 0,
          "and then it reads zero, which is end of file");
    CHECK(pipe_read(&p, 1, out, sizeof out) == 0, "every time after, too");

    /* that ordering is the whole thing. */



    pipe_reset(&p);
    p.readers = 0;
    CHECK(pipe_write(&p, 1, "nobody is listening", 19) == -1,
          "writing with no reader left fails rather than filling a buffer");

    /* a write that got partway before the reader left reports what it managed. */
    pipe_reset(&p);
    memset(big, 'y', sizeof big);
    sleeping_on = NULL;
    sleeps = 0;
    pipe_put(&p, big, PIPE_BUF - 4);        /* nearly full already */
    {
        /* fill the last four, then the reader vanishes */
        int64_t n = pipe_write(&p, 1, big, 4);
        CHECK(n == 4, "a write that exactly fills the pipe returns");
        p.readers = 0;
        CHECK(pipe_write(&p, 1, big, 4) == -1,
              "and the next one has nowhere to go");
    }

    /* fill the pipe, then write more. */
    pipe_reset(&p);
    sleeping_on = &p;
    sleeps = 0;
    wakes = 0;
    pipe_put(&p, big, PIPE_BUF);
    CHECK(pipe_write(&p, 1, big, 100) == 100,
          "a write into a full pipe waits for room and then completes");
    CHECK(sleeps > 0, "having really slept rather than spun");
    CHECK(wakes > 0, "and woken the reader when it put something in");
    sleeping_on = NULL;

    /* an interrupt gets a blocked reader out. */
    pipe_reset(&p);
    interrupt_pending = true;
    CHECK(pipe_read(&p, 1, out, sizeof out) == -1,
          "an interrupted read gives up rather than waiting for ever");
    interrupt_pending = false;



    size_t before = pipe_count();
    struct pipe *dyn = pipe_create();
    CHECK(dyn != NULL, "a pipe can be made");
    CHECK(pipe_count() == before + 1, "and is counted");
    CHECK(dyn->readers == 1 && dyn->writers == 1,
          "held open at both ends, which are exactly the two about to be "
          "handed out");

    pipe_close_write(dyn);
    CHECK(pipe_count() == before + 1, "one end closing does not free it");
    pipe_close_read(dyn);
    CHECK(pipe_count() == before,
          "the second one does, which is why nothing above has to think "
          "about lifetime");

    pipe_close_read(NULL);      /* must not fall over */
    pipe_close_write(NULL);
    CHECK(pipe_count() == before, "and closing nothing does nothing");

    /*
     * the thing that has to be true is that calling it twice is
     * harmless, because there are three places that call it and no way
     * to know which will get there first: a program that leaves runs
     * one, a program that is killed never does and gets another
     */
    {
        struct pipe *a = pipe_create();
        struct pipe *b = pipe_create();
        CHECK(pipe_count() == before + 2, "two pipes for a process to hold");

        held_in = a;
        held_out = b;
        pipe_release_for(7);
        CHECK(a->readers == 0, "releasing lets go of the reading end");
        CHECK(b->writers == 0, "and the writing one");
        CHECK(pipe_count() == before + 2,
              "though neither is freed while the other end is still held");

        pipe_release_for(7);
        CHECK(a->readers == 0 && b->writers == 0,
              "and a second release takes nothing further, which is what "
              "lets three different paths all call it safely");

        pipe_close_write(a);
        pipe_close_read(b);
        CHECK(pipe_count() == before, "and then both go");
    }

    CHECK(pipe_count() == before, "nothing leaked along the way");

    if (failures == 0) printf("all good\n");
    return failures;
}
