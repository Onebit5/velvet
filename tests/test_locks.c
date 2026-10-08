// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_locks.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the locks, hammered.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <pthread.h>

void kprintf(const char *fmt, ...)
{
    (void)fmt;
}
void kvprintf(const char *fmt, va_list ap)
{
    (void)fmt; (void)ap;
}
void panic(const char *fmt, ...)
{
    printf("PANIC: ");
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    exit(1);
}

#include "sched/spinlock.h"
#include "mm/pmm.h"
#include "mm/slab.h"
#include "mm/kmalloc.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

#define THREADS 8
#define ARENA (32ull * 1024 * 1024)



static struct spinlock counter_lock = SPINLOCK("counter", LOCK_RANK_DEVICE);
static volatile uint64_t guarded;
static volatile uint64_t unguarded;

/* deliberately not atomic. */
static void bump(volatile uint64_t *v)
{
    uint64_t was = *v;
    for (volatile int i = 0; i < 40; i++) { }
    *v = was + 1;
}

#define BUMPS 20000

static void *hammer_counter(void *arg)
{
    (void)arg;
    for (int i = 0; i < BUMPS; i++) {
        uint64_t flags = spin_lock_irq(&counter_lock);
        bump(&guarded);
        spin_unlock_irq(&counter_lock, flags);
        bump(&unguarded);
    }
    return NULL;
}



/*
 * every frame any thread is holding, so two threads being given the
 * same one is caught rather than merely being possible
 */
static volatile uint8_t *owned;
static uint64_t arena_frames;
static struct spinlock owned_lock = SPINLOCK("owned", LOCK_RANK_DEVICE);
static volatile int double_handouts;

static void *hammer_pmm(void *arg)
{
    (void)arg;
    uint64_t mine[16];

    for (int round = 0; round < 300; round++) {
        int held = 0;
        for (int i = 0; i < 16; i++) {
            uint64_t p = pmm_alloc();
            if (p == 0) {
                break;
            }
            mine[held++] = p;

            uint64_t f = p / PAGE_SIZE;
            uint64_t fl = spin_lock_irq(&owned_lock);
            if (f >= arena_frames || owned[f]) {
                double_handouts++;      /* the same frame, twice, at once */
            }
            owned[f] = 1;
            spin_unlock_irq(&owned_lock, fl);

            /*
             * scribble, so an overlap corrupts something rather than
             * merely being undetected
             */
            memset(pmm_phys_to_virt(p), (int)(f & 0xff), PAGE_SIZE);
        }
        for (int i = 0; i < held; i++) {
            uint64_t f = mine[i] / PAGE_SIZE;
            uint64_t fl = spin_lock_irq(&owned_lock);
            owned[f] = 0;
            spin_unlock_irq(&owned_lock, fl);
            pmm_free(mine[i]);
        }
    }
    return NULL;
}



static void *hammer_heap(void *arg)
{
    (void)arg;
    static const size_t sizes[] = { 8, 40, 100, 300, 900, 2000, 5000 };
    void *mine[24];

    for (int round = 0; round < 200; round++) {
        int held = 0;
        for (int i = 0; i < 24; i++) {
            size_t want = sizes[(round + i) % 7];
            void *p = kmalloc(want);
            if (p == NULL) {
                break;
            }
            /*
             * fill it, then check it: an allocator that hands the same
             * bytes to two callers loses this pattern
             */
            memset(p, (int)(want & 0xff), want);
            mine[held++] = p;

            int intact = 1;
            for (size_t j = 0; j < want; j++) {
                if (((uint8_t *)p)[j] != (uint8_t)(want & 0xff)) intact = 0;
            }
            if (!intact) {
                __atomic_fetch_add(&double_handouts, 1, __ATOMIC_SEQ_CST);
            }
        }
        for (int i = 0; i < held; i++) {
            kfree(mine[i]);
        }
    }
    return NULL;
}

static void run(void *(*fn)(void *))
{
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++) {
        pthread_create(&t[i], NULL, fn, NULL);
    }
    for (int i = 0; i < THREADS; i++) {
        pthread_join(t[i], NULL);
    }
}

int main(void)
{
    void *arena = aligned_alloc(4096, ARENA);
    struct ph_memmap_entry map[1] = {
        { .base = 0x1000, .length = ARENA - 0x1000, .type = PH_MEM_USABLE },
    };
    pmm_init_from_map(map, 1, (uint64_t)arena);

    arena_frames = ARENA / PAGE_SIZE;
    owned = calloc(arena_frames, 1);
    uint64_t at_rest = pmm_free_bytes();

    /* the guarded count is exact every single time, and that is the assertion. */
    bool saw_a_lost_update = false;
    for (int attempt = 0; attempt < 8; attempt++) {
        guarded = 0;
        unguarded = 0;
        run(hammer_counter);

        CHECK(guarded == (uint64_t)THREADS * BUMPS,
              "a counter under the lock is exact after eight threads");
        if (unguarded < (uint64_t)THREADS * BUMPS) {
            saw_a_lost_update = true;
            break;
        }
    }
    CHECK(saw_a_lost_update,
          "the same counter without the lock loses updates, which is "
          "what makes the line above mean anything");



    double_handouts = 0;
    run(hammer_pmm);

    CHECK(double_handouts == 0,
          "no frame was handed to two threads at once");
    CHECK(pmm_free_bytes() == at_rest,
          "and the pmm's books balance after all of it");



    double_handouts = 0;
    uint64_t heap_at_rest = pmm_free_bytes();
    run(hammer_heap);

    CHECK(double_handouts == 0,
          "no heap block was handed to two threads at once");
    CHECK(kheap_used_bytes() == 0, "the heap gave everything back");
    CHECK(pmm_free_bytes() == heap_at_rest,
          "and every page under it went back to the pmm");



    double_handouts = 0;
    uint64_t both_at_rest = pmm_free_bytes();
    {
        pthread_t t[THREADS];
        for (int i = 0; i < THREADS; i++) {
            pthread_create(&t[i], NULL,
                           (i % 2) ? hammer_heap : hammer_pmm, NULL);
        }
        for (int i = 0; i < THREADS; i++) {
            pthread_join(t[i], NULL);
        }
    }
    CHECK(double_handouts == 0,
          "the heap and the pmm hammered together hand nothing out twice");
    CHECK(pmm_free_bytes() == both_at_rest,
          "and still balance. the heap takes the pmm's lock while holding "
          "its own, which is why the order is declared and not assumed");



    uint64_t total_contention = 0;
    for (size_t i = 0; i < spin_count(); i++) {
        total_contention += spin_at(i)->contended;
    }
    CHECK(total_contention > 0,
          "somebody actually had to wait, so this test was really parallel");

    if (failures == 0) printf("all good\n");
    return failures;
}
