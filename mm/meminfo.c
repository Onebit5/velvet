// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/meminfo.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what memory says about itself, to a person or in /proc.
 */

#include <velvet/meminfo.h>
#include <velvet/kmalloc.h>
#include <velvet/kprintf.h>
#include <velvet/pmm.h>
#include <velvet/pressure.h>
#include <velvet/proc.h>
#include <velvet/slab.h>

/* the frames, what a program may still have, and the heap on top of them. */
void mm_show_meminfo(void)
{
    uint64_t total = pmm_total_bytes();
    uint64_t used  = pmm_used_bytes();
    uint64_t freeb = pmm_free_bytes();

    /* what a *program* may still ask for, which is not the free count. */
    uint64_t avail = pressure_available(freeb / 4096) * 4096;
    kprintf("a program may still ask for %lu KiB; %lu KiB beyond that is "
            "held back so that running out costs one program rather than "
            "the machine\n", avail / 1024, (freeb - avail) / 1024);

    kprintf("physical frames\n");
    kprintf("  total  %lu MiB (%lu frames)\n", total / (1024 * 1024),
            total / PAGE_SIZE);
    kprintf("  used   %lu KiB (%lu frames)\n", used / 1024, used / PAGE_SIZE);
    kprintf("  free   %lu MiB (%lu frames)\n", freeb / (1024 * 1024),
            freeb / PAGE_SIZE);
    kprintf("  peak   %lu KiB ever in use at once\n",
            pmm_peak_bytes() / 1024);
    kprintf("  books  %lu KiB, what the allocator spends on itself\n",
            pmm_metadata_bytes() / 1024);

    /* how much is free matters less than what shape it is in. */
    kprintf("free blocks, by size\n ");
    for (unsigned order = 0; order <= 10; order++) {
        uint64_t blocks = pmm_blocks_at(order);
        if (blocks == 0) {
            continue;
        }
        uint64_t kib = (PAGE_SIZE << order) / 1024;
        if (kib < 1024) {
            kprintf(" %lux%luK", blocks, kib);
        } else {
            kprintf(" %lux%luM", blocks, kib / 1024);
        }
    }
    kprintf("\n");

    kprintf("kernel heap\n");
    kprintf("  total  %lu KiB claimed from the pmm\n", kheap_total_bytes() / 1024);
    kprintf("  used   %lu bytes handed out\n", kheap_used_bytes());
}

/* every object cache, and what it holds against what it is using. */
void mm_show_slabinfo(void)
{

    kprintf("%-14s %6s %6s %6s %6s %6s\n",
            "cache", "size", "/page", "live", "peak", "pages");

    uint64_t held = 0, wanted = 0;
    for (struct slab_cache *c = slab_first_cache(); c != NULL; c = c->next) {
        kprintf("%-14s %6zu %6zu %6zu %6zu %6zu\n",
                c->name, c->obj_size, c->per_slab,
                c->in_use, c->high_water, c->pages);
        held += c->pages * PAGE_SIZE;
        wanted += c->in_use * c->obj_size;
    }

    kprintf("holding %lu KiB for %lu KiB of objects\n",
            held / 1024, wanted / 1024);
}

void mm_proc_init(void)
{
    proc_register("meminfo", mm_show_meminfo);
    proc_register("slabinfo", mm_show_slabinfo);
}
