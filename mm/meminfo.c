// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/meminfo.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what memory says about itself in /proc, laid out the way the programs
 * that show it read it.
 */

#include <velvet/meminfo.h>
#include <velvet/kprintf.h>
#include <velvet/pmm.h>
#include <velvet/pressure.h>
#include <velvet/proc.h>
#include <velvet/ramfs.h>
#include <velvet/slab.h>
#include <velvet/thread.h>
#include <velvet/vmm.h>

#include "buddy.h"

/* one line of meminfo: the name and its colon in sixteen, then kilobytes */
static void show_kb(const char *str_name, uint64_t u_bytes)
{
    kprintf("%-16s%8lu kB\n", str_name, u_bytes / 1024);
}

/*
 * meminfo: the frames there are, the ones free and what a program may
 * still have of them, the files kept in memory, which is what a program
 * calls cached, the object caches and the kernel's stacks, and no swap.
 * a thing the kernel does not count, such as which frames two programs
 * share, has no line rather than a line saying none
 */
void mm_show_meminfo(void)
{
    uint64_t u_free = pmm_free_bytes();
    uint64_t u_slab = 0;
    for (struct slab_cache *c = slab_first_cache(); c != NULL; c = c->next) {
        u_slab += c->pages * PAGE_SIZE;
    }

    show_kb("MemTotal:", pmm_total_bytes());
    show_kb("MemFree:", u_free);
    show_kb("MemAvailable:",
            pressure_available(u_free / PAGE_SIZE) * PAGE_SIZE);
    show_kb("Buffers:", 0);
    show_kb("Cached:", ramfs_bytes());
    show_kb("SwapCached:", 0);
    show_kb("SwapTotal:", 0);
    show_kb("SwapFree:", 0);
    show_kb("Slab:", u_slab);
    show_kb("SReclaimable:", 0);
    show_kb("SUnreclaim:", u_slab);
    show_kb("KernelStack:", thread_stack_bytes());
}

/*
 * slabinfo, version 2.1: each object cache, the objects out and the room
 * there is for them, their size, and its slabs, which are a page each
 * and every one in use. there is nothing to tune
 */
void mm_show_slabinfo(void)
{
    kprintf("slabinfo - version: 2.1\n");
    kprintf("# name            <active_objs> <num_objs> <objsize>"
            " <objperslab> <pagesperslab>"
            " : tunables <limit> <batchcount> <sharedfactor>"
            " : slabdata <active_slabs> <num_slabs> <sharedavail>\n");
    for (struct slab_cache *c = slab_first_cache(); c != NULL; c = c->next) {
        kprintf("%-17s %6lu %6lu %6lu %4lu %4d", c->name,
                (uint64_t)c->in_use, (uint64_t)(c->pages * c->per_slab),
                (uint64_t)c->obj_size, (uint64_t)c->per_slab, 1);
        kprintf(" : tunables %4d %4d %4d : slabdata %6lu %6lu %6d\n", 0, 0, 0,
                (uint64_t)c->pages, (uint64_t)c->pages, 0);
    }
}

/* buddyinfo: how many free blocks of each size, one frame and up */
static void show_buddyinfo(void)
{
    kprintf("Node 0, zone   Normal ");
    for (unsigned u_order = 0; u_order <= BUDDY_MAX_ORDER; u_order++) {
        kprintf("%6lu ", pmm_blocks_at(u_order));
    }
    kprintf("\n");
}

void mm_proc_init(void)
{
    proc_register("meminfo", PROC_PUBLIC, mm_show_meminfo);
    proc_register("slabinfo", PROC_PRIVATE, mm_show_slabinfo);
    proc_register("buddyinfo", PROC_PUBLIC, show_buddyinfo);
    proc_register("vmm", PROC_PRIVATE, vmm_show);
}
