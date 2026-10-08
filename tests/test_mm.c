// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_mm.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for pmm + kmalloc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

/* kernel stubs */
void kprintf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}
void kvprintf(const char *fmt, va_list ap)
{
    vprintf(fmt, ap);
}
void panic(const char *fmt, ...)
{
    printf("PANIC: ");
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    exit(1);
}

#include "mm/pmm.h"
#include "mm/kmalloc.h"

#define ARENA (8ull * 1024 * 1024)
#define MiB   (1024ull * 1024)

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

int main(void)
{
    void *arena = aligned_alloc(4096, ARENA);
    uint64_t hhdm = (uint64_t)arena;

    /* usable 4k..1MiB, a reserved hole, usable 2MiB..8MiB */
    struct ph_memmap_entry e0 = { .base = 0x1000, .length = MiB - 0x1000,
                                      .type = PH_MEM_USABLE };
    struct ph_memmap_entry e1 = { .base = MiB, .length = MiB,
                                      .type = PH_MEM_RESERVED };
    struct ph_memmap_entry e2 = { .base = 2 * MiB, .length = 6 * MiB,
                                      .type = PH_MEM_USABLE };
    struct ph_memmap_entry map[3] = { e0, e1, e2 };

    pmm_init_from_map(map, 3, hhdm);

    uint64_t usable = (MiB - 0x1000) + 6 * MiB;
    CHECK(pmm_total_bytes() == usable, "total == sum of usable regions");

    /*
     * two things are spent before anybody asks for anything: the
     * buddy's own books, which for 8MiB of frames fit in one page and
     * are parked in e0 rather than allocated, and the table saying who
     * shares which frame, which *is* allocated. both are real memory
     * gone before the first program runs and both should be visible
     */
    CHECK(pmm_share_bytes() > 0, "the share table was built at init");
    uint64_t overhead = PAGE_SIZE + pmm_share_bytes();
    CHECK(pmm_free_bytes() == usable - overhead,
          "free == total, less the books and the share table");

    /* single frame round trip */
    uint64_t f = pmm_alloc();
    CHECK(f != 0 && f % PAGE_SIZE == 0, "alloc gives an aligned frame");
    CHECK((f >= 0x1000 && f < MiB) || (f >= 2 * MiB && f < 8 * MiB),
          "frame is inside a usable region");
    memset(pmm_phys_to_virt(f), 0xab, PAGE_SIZE);
    pmm_free(f);
    CHECK(pmm_free_bytes() == usable - overhead, "books balance after 1 frame");

    /* contiguous run: 4 frames, writable end to end */
    uint64_t run = pmm_alloc_pages(4);
    CHECK(run != 0, "got a 4-frame run");
    memset(pmm_phys_to_virt(run), 0xcd, 4 * PAGE_SIZE);
    pmm_free_pages(run, 4);

    /*
     * the buddy rounds up to a power of two, which is the one piece of
     * it that leaks through this interface. three pages costs four
     */
    uint64_t before3 = pmm_free_bytes();
    uint64_t three = pmm_alloc_pages(3);
    CHECK(three != 0, "got a 3-frame run");
    CHECK(pmm_free_bytes() == before3 - 4 * PAGE_SIZE,
          "three pages costs four, and the books say so");
    memset(pmm_phys_to_virt(three), 0xef, 3 * PAGE_SIZE);
    pmm_free_pages(three, 3);
    CHECK(pmm_free_bytes() == before3, "freeing with the same count balances");

    /* nothing may be bigger than the largest block */
    CHECK(pmm_alloc_pages(1u << 20) == 0,
          "an absurd run is refused, not quietly served something small");

    /* drain it dry: every frame handed out exactly once, no overlaps */
    uint64_t expect_frames = pmm_free_bytes() / PAGE_SIZE;
    static uint8_t seen[ARENA / PAGE_SIZE];
    memset(seen, 0, sizeof(seen));
    uint64_t got = 0;
    uint64_t addr;
    while ((addr = pmm_alloc()) != 0) {
        uint64_t idx = addr / PAGE_SIZE;
        CHECK(idx < ARENA / PAGE_SIZE, "frame within arena");
        CHECK(!seen[idx], "frame never handed out twice");
        seen[idx] = 1;
        got++;
    }
    CHECK(got == expect_frames, "drained exactly the advertised number");
    CHECK(pmm_free_bytes() == 0, "free == 0 when dry");
    CHECK(pmm_alloc_pages(1) == 0, "alloc when dry says 0");
    /* give everything back */
    for (uint64_t i = 0; i < ARENA / PAGE_SIZE; i++) {
        if (seen[i]) pmm_free(i * PAGE_SIZE);
    }
    CHECK(pmm_free_bytes() == expect_frames * PAGE_SIZE, "all returned");

    /*
     * this runs against the *real* pmm and the real lock, which is the
     * point of it being here rather than in the address space suite.
     * the first version of this built its table the first time anybody
     * asked to share a frame, inside the pmm's own lock, asking the
     * pmm for memory, and the machine died the first time anything
     * forked. a stub cannot find that; only the real lock can
     */
    {
        uint64_t frame = pmm_alloc();
        CHECK(frame != 0, "a frame to share");
        uint64_t free_before = pmm_free_bytes();

        CHECK(pmm_shares(frame) == 0,
              "a frame nobody shared has one owner, which is what zero means");

        pmm_ref(frame);
        CHECK(pmm_shares(frame) == 1, "sharing it gives it a second holder");

        CHECK(!pmm_unref(frame), "the first to let go does not free it");
        CHECK(pmm_free_bytes() == free_before,
              "and the frame really is still out, freeing it here is a "
              "page handed out again while somebody is reading it");
        CHECK(pmm_shares(frame) == 0, "with one holder left");

        CHECK(pmm_unref(frame), "and the last one frees it");
        CHECK(pmm_free_bytes() == free_before + PAGE_SIZE, "for real");

        /*
         * several at once, which is what a program forked twice looks
         * like from down here
         */
        uint64_t f2 = pmm_alloc();
        free_before = pmm_free_bytes();
        pmm_ref(f2);
        pmm_ref(f2);
        pmm_ref(f2);
        CHECK(pmm_shares(f2) == 3, "a frame can have several extra holders");
        CHECK(!pmm_unref(f2) && !pmm_unref(f2) && !pmm_unref(f2),
              "and none of them frees it");
        CHECK(pmm_free_bytes() == free_before, "it is still out");
        CHECK(pmm_unref(f2), "until the last");
        CHECK(pmm_free_bytes() == free_before + PAGE_SIZE, "and then it goes");

        /*
         * a frame nobody ever shared is freed by one unref, which is
         * what makes unref safe to use everywhere instead of free
         */
        uint64_t f3 = pmm_alloc();
        free_before = pmm_free_bytes();
        CHECK(pmm_unref(f3), "an unshared frame goes back on the first unref");
        CHECK(pmm_free_bytes() == free_before + PAGE_SIZE, "properly");

        /* nonsense addresses must not scribble outside the table */
        pmm_ref(0xffffffffull * PAGE_SIZE);
        CHECK(pmm_shares(0xffffffffull * PAGE_SIZE) == 0,
              "a frame beyond the table is not counted, and not written to");

        CHECK(pmm_can_share(), "and this machine can share at all");
    }


    uint64_t used0 = kheap_used_bytes();
    size_t sizes[6] = { 1, 24, 1000, 16384, 512, 4096 };
    uint8_t *p[6];
    for (int i = 0; i < 6; i++) {
        p[i] = kmalloc(sizes[i]);
        CHECK(p[i] != NULL, "kmalloc says yes");
        CHECK(((uint64_t)p[i] & 15) == 0, "payload 16-byte aligned");
        memset(p[i], 0x40 + i, sizes[i]);
    }
    for (int i = 0; i < 6; i++) {
        int ok = 1;
        for (size_t j = 0; j < sizes[i]; j++) {
            if (p[i][j] != (uint8_t)(0x40 + i)) ok = 0;
        }
        CHECK(ok, "heap block kept its pattern (no overlap)");
    }
    int order[6] = { 3, 0, 5, 1, 4, 2 };
    for (int i = 0; i < 6; i++) kfree(p[order[i]]);
    CHECK(kheap_used_bytes() == used0, "heap books balance");

    /*
     * size classes: a hundred small blocks of the same size share a
     * handful of pages rather than taking one each, and the heap gives
     * every one of those pages back when they are returned
     */
    uint64_t total0 = kheap_total_bytes();
    uint8_t *many[100];
    for (int i = 0; i < 100; i++) {
        many[i] = kmalloc(100);
        CHECK(many[i] != NULL, "a run of same-size allocations all succeed");
    }
    CHECK(kheap_total_bytes() - total0 <= 8 * PAGE_SIZE,
          "a hundred small blocks share a few pages");
    for (int i = 0; i < 100; i++) kfree(many[i]);
    CHECK(kheap_total_bytes() == total0, "and the pages went back");

    /* zero and absurd */
    CHECK(kmalloc(0) == NULL, "kmalloc(0) is NULL");
    CHECK(kmalloc(64 * MiB) == NULL, "kmalloc beyond ram is NULL, not a crash");
    uint8_t *after = kmalloc(64);
    CHECK(after != NULL, "heap still works after an oom");
    kfree(after);

    /*
     * a fresh map with a bootloader-reclaimable region ABOVE the last
     * usable one, which is where it really sits on a pc, if the
     * allocator is only sized to cover usable ram, those frames fall
     * off the end and reclaiming them silently does nothing
     */
    struct ph_memmap_entry r0 = { .base = 0x1000, .length = MiB - 0x1000,
                                      .type = PH_MEM_USABLE };
    struct ph_memmap_entry r1 = { .base = 2 * MiB, .length = 4 * MiB,
                                      .type = PH_MEM_USABLE };
    struct ph_memmap_entry r2 = { .base = 6 * MiB, .length = MiB,
                                      .type = PH_MEM_LOADER };
    struct ph_memmap_entry rmap[3] = { r0, r1, r2 };

    pmm_init_from_map(rmap, 3, hhdm);

    /*
     * a second init builds a second share table, the first one came
     * out of an allocator that no longer exists
     */
    CHECK(pmm_share_bytes() > 0, "and again after the allocator is rebuilt");

    uint64_t total_before = pmm_total_bytes();
    uint64_t free_before  = pmm_free_bytes();
    CHECK(total_before == (MiB - 0x1000) + 4 * MiB,
          "reclaimable memory is not counted as ram until the test takes it");
    /* it must be beyond the usable regions but still covered */
    CHECK(pmm_translate_is_tracked(6 * MiB),
          "the allocator reaches the loader's memory");

    uint64_t gained = pmm_reclaim_bootloader();
    CHECK(gained == MiB, "the whole reclaimable region came back");
    CHECK(pmm_total_bytes() == total_before + MiB, "and now counts as ram");
    CHECK(pmm_free_bytes()  == free_before + MiB,  "and is free to hand out");

    /* the recovered frames have to actually be usable */
    uint64_t rf = pmm_alloc_pages(256);      /* 1 MiB worth */
    CHECK(rf != 0, "the test can allocate out of the reclaimed region");
    memset(pmm_phys_to_virt(rf), 0x5a, 256 * PAGE_SIZE);
    pmm_free_pages(rf, 256);

    /* calling twice must not double-count */
    CHECK(pmm_reclaim_bootloader() == 0, "a second reclaim finds nothing");
    CHECK(pmm_total_bytes() == total_before + MiB, "and changes no numbers");

    /* the firmware's own tables are not in memory it calls usable. */
    struct ph_memmap_entry t0 = { .base = 0x1000, .length = MiB - 0x1000,
                                  .type = PH_MEM_USABLE };
    struct ph_memmap_entry t1 = { .base = 2 * MiB, .length = 4 * MiB,
                                  .type = PH_MEM_USABLE };
    struct ph_memmap_entry t2 = { .base = 6 * MiB, .length = 128 * 1024,
                                  .type = PH_MEM_RESERVED };
    struct ph_memmap_entry t3 = { .base = 0xfd00000000ull, .length = 3ull * MiB,
                                  .type = PH_MEM_RESERVED };
    struct ph_memmap_entry tmap[4] = { t0, t1, t2, t3 };

    pmm_init_from_map(tmap, 4, hhdm);

    CHECK(pmm_highest_address() == 6 * MiB + 128 * 1024,
          "the map reaches over reserved memory sitting on top of ram");
    CHECK(pmm_highest_address() < 0x1000000ull,
          "and stops well short of the reserved holes near the terabyte mark");
    CHECK(pmm_total_bytes() == (MiB - 0x1000) + 4 * MiB,
          "reaching over it does not make it usable");

    /*
     * a gap of more than a megabyte is a different part of the address
     * space, not padding
     */
    struct ph_memmap_entry g2 = { .base = 6 * MiB + 8 * MiB,
                                  .length = 128 * 1024,
                                  .type = PH_MEM_RESERVED };
    struct ph_memmap_entry gmap[3] = { t0, t1, g2 };
    pmm_init_from_map(gmap, 3, hhdm);
    CHECK(pmm_highest_address() == 6 * MiB,
          "a reserved region well clear of ram is left out of the map");

    if (failures == 0) printf("all good\n");
    return failures;
}
