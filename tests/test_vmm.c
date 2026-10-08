// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_vmm.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * page table construction, exercised without a cpu.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#define ARENA_PAGES 4096            /* 16 MiB of "physical memory" */
static uint8_t *arena;
static uint64_t next_free = 1;      /* page 0 stays reserved, like the real pmm */
static int alloc_failures_wanted = -1;   /* -1 = never fail */


uint64_t pmm_alloc_pages(size_t count)
{
    if (alloc_failures_wanted == 0) return 0;
    if (alloc_failures_wanted > 0) alloc_failures_wanted--;
    if (next_free + count > ARENA_PAGES) return 0;
    uint64_t phys = next_free * 4096;
    next_free += count;
    return phys;
}
void *pmm_phys_to_virt(uint64_t phys)
{
    return arena + phys;
}

void kprintf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}
void panic(const char *fmt, ...)
{
    printf("PANIC: ");
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    exit(1);
}

#include "mm/vmm.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

static void check_maps(uint64_t pml4, uint64_t virt, uint64_t want,
                       const char *what)
{
    uint64_t got = vmm_translate(pml4, virt);
    if (got != want) {
        printf("FAIL %s: %#lx -> %#lx, want %#lx\n", what, virt, got, want);
        failures++;
    }
}

int main(void)
{
    arena = aligned_alloc(4096, (size_t)ARENA_PAGES * 4096);
    memset(arena, 0, (size_t)ARENA_PAGES * 4096);

    uint64_t pml4 = vmm_new_address_space();
    CHECK(pml4 != 0, "a fresh address space is born");

    /* nothing is mapped in a new space */
    CHECK(vmm_translate(pml4, 0x1000) == VMM_NO_MAPPING,
          "an empty space maps nothing");
    CHECK(vmm_translate(pml4, 0xffffffff80000000ull) == VMM_NO_MAPPING,
          "not even up in the higher half");


    CHECK(vmm_map_range(pml4, 0x400000, 0x800000, 4096, PTE_WRITE),
          "map one small page");
    check_maps(pml4, 0x400000, 0x800000, "the page itself");
    check_maps(pml4, 0x400abc, 0x800abc, "an offset inside the page");
    CHECK(vmm_translate(pml4, 0x401000) == VMM_NO_MAPPING,
          "the next page along is still empty");
    CHECK(vmm_flags(pml4, 0x400000) & PTE_WRITE, "flags survive the round trip");
    CHECK(vmm_flags(pml4, 0x400000) & PTE_PRESENT, "present is set for the test");
    CHECK(!(vmm_flags(pml4, 0x400000) & PTE_HUGE), "and its not a huge page");


    uint64_t kbase = 0xffffffff80000000ull;
    CHECK(vmm_map_range(pml4, kbase, 0x100000, 0x8000, 0),
          "map something at the top of the address space");
    check_maps(pml4, kbase, 0x100000, "kernel base");
    check_maps(pml4, kbase + 0x7fff, 0x107fff, "the last byte of it");
    CHECK(!(vmm_flags(pml4, kbase) & PTE_WRITE),
          "read-only stays read-only (this is what W^X rests on)");


    uint64_t hh = 0xffff800000000000ull;
    CHECK(vmm_map_range(pml4, hh, 0, 8 * 1024 * 1024, PTE_WRITE | PTE_NX),
          "map 8 MiB of direct map");
    CHECK(vmm_flags(pml4, hh) & PTE_HUGE,
          "an aligned range gets 2MiB pages, not 512 small ones");
    check_maps(pml4, hh, 0, "direct map base");
    check_maps(pml4, hh + 0x1234, 0x1234, "an offset inside a huge page");
    check_maps(pml4, hh + 0x200000, 0x200000, "the second huge page");
    check_maps(pml4, hh + 0x7fffff, 0x7fffff, "the very last byte of the range");
    CHECK(vmm_translate(pml4, hh + 8 * 1024 * 1024) == VMM_NO_MAPPING,
          "and nothing beyond it");
    CHECK(vmm_flags(pml4, hh) & PTE_NX, "NX survives on a huge page");

    /* a range that starts aligned but is too short falls back to 4KiB */
    uint64_t small = 0xffff900000000000ull;
    CHECK(vmm_map_range(pml4, small, 0x40000000, 4096 * 3, PTE_WRITE),
          "map three pages");
    CHECK(!(vmm_flags(pml4, small) & PTE_HUGE),
          "too short for a huge page, so small ones");
    check_maps(pml4, small + 8192, 0x40002000, "third small page");

    /* misaligned physical address also forces 4KiB */
    uint64_t mis = 0xffffa00000000000ull;
    CHECK(vmm_map_range(pml4, mis, 0x1000, 4 * 1024 * 1024, PTE_WRITE),
          "map 4 MiB from a misaligned physical base");
    CHECK(!(vmm_flags(pml4, mis) & PTE_HUGE),
          "phys not 2MiB aligned, so no huge pages");
    check_maps(pml4, mis, 0x1000, "misaligned base still translates");
    check_maps(pml4, mis + 0x300000, 0x301000, "and so does the far end");

    /*
     * the cpu ANDs U/S across pml4, pdpt, pd and pt, so a leaf marked
     * user under intermediates that are not is unreachable from ring 3
     *, and it looks exactly like a correct mapping in a dump
     */
    {
        uint64_t upml4 = vmm_new_address_space();
        uint64_t uva = 0x400000;
        CHECK(vmm_map_range(upml4, uva, 0x900000, 4096, PTE_USER | PTE_WRITE),
              "map a page for ring 3");
        CHECK(vmm_flags(upml4, uva) & PTE_USER, "the leaf says user");

        uint64_t *t = (uint64_t *)(arena + upml4);
        uint64_t pml4e = t[(uva >> 39) & 0x1ff];
        CHECK(pml4e & PTE_USER, "and so does the pml4 entry");
        t = (uint64_t *)(arena + (pml4e & 0x000ffffffffff000ull));
        uint64_t pdpte = t[(uva >> 30) & 0x1ff];
        CHECK(pdpte & PTE_USER, "and the pdpt entry");
        t = (uint64_t *)(arena + (pdpte & 0x000ffffffffff000ull));
        CHECK(t[(uva >> 21) & 0x1ff] & PTE_USER, "and the pd entry");

        /* a kernel mapping sharing the path must not become reachable */
        CHECK(vmm_map_range(upml4, uva + 0x1000, 0x901000, 4096, PTE_WRITE),
              "map a kernel-only page beside it");
        CHECK(!(vmm_flags(upml4, uva + 0x1000) & PTE_USER),
              "its leaf stays supervisor-only even though the tables above "
              "it are now user-accessible");
    }


    CHECK(vmm_unmap_page(pml4, 0x400000), "unmap the small page");
    CHECK(vmm_translate(pml4, 0x400000) == VMM_NO_MAPPING, "and its gone");

    /*
     * punch a single 4KiB hole in the middle of a 2MiB page: the page
     * has to be split into 512 small ones first, and everything either
     * side of the hole must survive untouched. this is exactly what a
     * thread stack guard page needs
     */
    uint64_t hole = hh + 0x10000;
    CHECK(vmm_unmap_page(pml4, hole), "punch a hole in a huge page");
    CHECK(vmm_translate(pml4, hole) == VMM_NO_MAPPING, "the hole is a hole");
    CHECK(!(vmm_flags(pml4, hh) & PTE_HUGE), "the huge page got split");
    check_maps(pml4, hh, 0, "the page before the hole survived");
    check_maps(pml4, hole - 4096, 0xf000, "right up to the hole");
    check_maps(pml4, hole + 4096, 0x11000, "and right after it");
    check_maps(pml4, hh + 0x1ff000, 0x1ff000, "the far end of the split page");
    CHECK(vmm_flags(pml4, hh) & PTE_WRITE, "split preserved the write bit");
    CHECK(vmm_flags(pml4, hh) & PTE_NX, "split preserved NX");
    /* the neighbouring huge page must not have been disturbed */
    CHECK(vmm_flags(pml4, hh + 0x200000) & PTE_HUGE,
          "the next huge page is still huge");
    check_maps(pml4, hh + 0x200000, 0x200000, "and still correct");

    /* unmapping something that was never mapped shouldnt claim success */
    CHECK(!vmm_unmap_page(pml4, 0xffffb00000000000ull),
          "unmapping nothing fails honestly");

    uint64_t small_arena_pml4 = vmm_new_address_space();
    alloc_failures_wanted = 1;   /* one more alloc, then the well runs dry */
    CHECK(!vmm_map_range(small_arena_pml4, 0xffffc00000000000ull, 0,
                         64 * 1024 * 1024, PTE_WRITE),
          "a map that runs out of frames reports failure, not success");
    alloc_failures_wanted = -1;

    if (!failures) printf("all good\n");
    return failures;
}
