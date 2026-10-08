// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_slab.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for the slab caches and the heap built on them.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

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

#include "mm/pmm.h"
#include "mm/slab.h"
#include "mm/kmalloc.h"

#define ARENA (8ull * 1024 * 1024)
#define MiB   (1024ull * 1024)

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

struct thing { uint64_t a, b, c; char name[24]; };   /* 48 bytes */

int main(void)
{
    void *arena = aligned_alloc(4096, ARENA);

    struct ph_memmap_entry e0 = { .base = 0x1000, .length = ARENA - 0x1000,
                                      .type = PH_MEM_USABLE };
    struct ph_memmap_entry map[1] = { e0 };
    pmm_init_from_map(map, 1, (uint64_t)arena);

    uint64_t ram_at_rest = pmm_free_bytes();


    static struct slab_cache things;
    slab_cache_init(&things, "thing", sizeof(struct thing));

    CHECK(things.obj_size == 48, "48 bytes needs no rounding");
    CHECK(things.per_slab > 1, "more than one fits in a page");
    CHECK(things.pages == 0, "a fresh cache holds no memory at all");

    struct thing *t1 = slab_alloc(&things);
    CHECK(t1 != NULL, "the first object came out");
    CHECK(things.pages == 1, "which cost exactly one page");
    CHECK(pmm_free_bytes() == ram_at_rest - PAGE_SIZE, "and the pmm agrees");

    struct thing *t2 = slab_alloc(&things);
    CHECK(t2 != t1, "the second is a different object");
    CHECK(things.pages == 1, "and came out of the same page");
    CHECK(((uintptr_t)t1 & 15) == 0 && ((uintptr_t)t2 & 15) == 0,
          "objects are 16-byte aligned");



    size_t n = things.per_slab;
    struct thing **all = malloc(n * sizeof(*all));
    all[0] = t1; all[1] = t2;
    for (size_t i = 2; i < n; i++) {
        all[i] = slab_alloc(&things);
        CHECK(all[i] != NULL, "a page's worth of objects all came out");
    }
    CHECK(things.pages == 1, "and they all fit in the one page");
    CHECK(things.in_use == n, "the cache counted every one");

    for (size_t i = 0; i < n; i++) {
        all[i]->a = i; all[i]->b = ~i; all[i]->c = i * 7;
        memset(all[i]->name, (int)(i & 0x7f), sizeof(all[i]->name));
    }
    int intact = 1;
    for (size_t i = 0; i < n; i++) {
        if (all[i]->a != i || all[i]->b != ~i || all[i]->c != i * 7) intact = 0;
        for (size_t j = 0; j < sizeof(all[i]->name); j++) {
            if (all[i]->name[j] != (char)(i & 0x7f)) intact = 0;
        }
    }
    CHECK(intact, "every object kept its own contents");

    /* one more than fits has to come from a second page */
    struct thing *overflow = slab_alloc(&things);
    CHECK(overflow != NULL, "the cache grows when a page fills");
    CHECK(things.pages == 2, "by exactly one page");



    slab_free(overflow);
    CHECK(things.pages == 1, "an emptied page is returned to the pmm");
    CHECK(pmm_free_bytes() == ram_at_rest - PAGE_SIZE, "which the pmm sees");

    /* freeing one and taking one must reuse the same slot */
    struct thing *hole = all[n / 2];
    slab_free(hole);
    struct thing *refill = slab_alloc(&things);
    CHECK(refill == hole, "a freed object is the next one handed out");
    all[n / 2] = refill;

    for (size_t i = 0; i < n; i++) {
        slab_free(all[i]);
    }
    CHECK(things.in_use == 0, "everything came home");
    CHECK(things.pages == 0, "and the cache is holding no memory");
    CHECK(pmm_free_bytes() == ram_at_rest, "the pmm has all its pages back");
    CHECK(things.high_water == n + 1, "the high-water mark remembers the peak");
    free(all);



    struct thing *live = slab_alloc(&things);
    CHECK(slab_owns(live), "a slab object is recognised");
    uint64_t loose = pmm_alloc();
    CHECK(!slab_owns(pmm_phys_to_virt(loose)),
          "a plain page from the pmm is not");
    CHECK(!slab_owns(NULL), "and neither is nothing");
    pmm_free(loose);
    slab_free(live);



    uint64_t at_rest = pmm_free_bytes();

    void *small = kmalloc(1);
    CHECK(small != NULL, "kmalloc(1) works");
    CHECK(slab_owns(small), "and comes from a size class, not a page");
    kfree(small);

    void *big = kmalloc(9000);
    CHECK(big != NULL, "kmalloc(9000) works");
    CHECK(!slab_owns(big), "and comes from whole pages instead");
    memset(big, 0x5a, 9000);
    kfree(big);
    CHECK(pmm_free_bytes() == at_rest, "both kinds gave their memory back");

    /*
     * a size class is picked by size, and the same size twice reuses a
     * page rather than taking a new one each time
     */
    void *p[64];
    uint64_t before_pages = pmm_free_bytes();
    for (int i = 0; i < 64; i++) {
        p[i] = kmalloc(100);
        CHECK(p[i] != NULL, "a run of same-size allocations all succeed");
    }
    uint64_t spent = (before_pages - pmm_free_bytes()) / PAGE_SIZE;
    CHECK(spent <= 4, "64 objects of 100 bytes cost only a few pages");
    for (int i = 0; i < 64; i++) kfree(p[i]);
    CHECK(pmm_free_bytes() == before_pages, "and all of it came back");

    /*
     * a request too large for any block is refused rather than served
     * something too small, the buddy caps out at 4 MiB
     */
    CHECK(kmalloc(64 * MiB) == NULL, "an absurd request is NULL, not a crash");
    void *after = kmalloc(64);
    CHECK(after != NULL, "and the heap still works afterwards");
    kfree(after);

    CHECK(pmm_free_bytes() == at_rest, "the pmm ends where it started");

    if (failures == 0) printf("all good\n");
    return failures;
}
