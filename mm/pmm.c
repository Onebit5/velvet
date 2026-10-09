// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/pmm.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the physical memory manager.
 */

#include <velvet/pmm.h>
#include <velvet/boot.h>
#include "buddy.h"
#include <velvet/kprintf.h>
#include <asm/irq.h>
#include <velvet/spinlock.h>
#include <stdbool.h>
#include <velvet/panic.h>
#include <velvet/string.h>

/* the buddy free lists, which every allocation walks and rewrites */
static struct spinlock pmm_lock = SPINLOCK("pmm", LOCK_RANK_PMM);

static uint64_t hhdm_offset;
static uint64_t managed_frames;  /* how many frames the allocator covers */
static uint64_t total_frames;    /* usable frames overall */
static uint64_t highest_addr;    /* top of the direct map, see pmm.h */
static uint64_t peak_used;       /* the high-water mark, in frames */
static uint64_t meta_bytes;      /* what the buddy's bookkeeping costs */

/* who shares what: one byte per frame, holding how many *extra* holders it has. */
static uint8_t *shares;
static uint64_t shares_frames;
static uint64_t share_bytes;

/* the frames the bookkeeping itself lives in, so the kernel knows not to give them away. */
static uint64_t meta_first, meta_last;

/*
 * the loader's own memory, noted down at init because the memmap the kernel would otherwise read it from is itself sitting in that memory
 */
#define MAX_RECLAIM 16
static struct { uint64_t base, length; } reclaim[MAX_RECLAIM];
static size_t reclaim_count;

static const char *memmap_type_name(uint64_t type)
{
    switch (type) {
    case PH_MEM_USABLE:            return "usable";
    case PH_MEM_RESERVED:          return "reserved";
    case PH_MEM_ACPI_RECLAIMABLE:  return "acpi reclaimable";
    case PH_MEM_ACPI_NVS:          return "acpi nvs";
    case PH_MEM_BAD:               return "bad memory (yikes)";
    case PH_MEM_LOADER:            return "the loader's (reclaimable)";
    case PH_MEM_KERNEL:            return "kernel + ramdisk";
    default:                       return "???";
    }
}

/*
 * the buddy threads its free lists through the free pages themselves,
 * so it needs a way to reach one. through the hhdm, same as everything
 */
static void *frame_to_virt(uint64_t frame)
{
    return (void *)(frame * PAGE_SIZE + hhdm_offset);
}

/*
 * hand [first, last) to the allocator, minus the two things that are
 * never anyone's to allocate: frame 0, so that a physical address of 0
 * can safely mean "no", and whatever the bookkeeping sits in. returns
 * how many frames actually went in
 */
static uint64_t give_away(uint64_t first, uint64_t last)
{
    if (first == 0) {
        first = 1;
    }
    if (last > managed_frames) {
        last = managed_frames;
    }
    if (first >= last) {
        return 0;
    }

    /*
     * if the metadata is somewhere in the middle, give away the two
     * halves on either side of it. neither of those overlaps it, so
     * this recurses exactly one level deep
     */
    if (meta_first < last && meta_last > first) {
        uint64_t added = 0;
        if (meta_first > first) {
            added += give_away(first, meta_first);
        }
        if (meta_last < last) {
            added += give_away(meta_last, last);
        }
        return added;
    }

    buddy_add_range(first, last - first);
    return last - first;
}

void pmm_init_from_map(const struct ph_memmap_entry *entries, size_t count,
                       uint64_t hhdm)
{
    hhdm_offset = hhdm;

    /* pass 1: how far up does usable ram go, and how much is there */
    uint64_t highest = 0;
    total_frames = 0;
    highest_addr = 0;
    reclaim_count = 0;
    peak_used = 0;

    /*
     * the old table, if there was one, came out of an allocator that is
     * about to be thrown away entirely. dropping the pointer is the
     * whole of freeing it
     */
    shares = NULL;
    shares_frames = 0;
    share_bytes = 0;
    for (size_t i = 0; i < count; i++) {
        const struct ph_memmap_entry *e = &entries[i];
        kprintf("  %016lx - %016lx  %s\n", e->base, e->base + e->length,
                memmap_type_name(e->type));
        if (e->type != PH_MEM_RESERVED
            && e->type != PH_MEM_BAD
            && e->base + e->length > highest_addr) {
            highest_addr = e->base + e->length;
        }
        if (e->type == PH_MEM_USABLE) {
            total_frames += e->length / PAGE_SIZE;
        }
        /*
         * the allocator has to cover the loader's memory too, or the kernel would
         * have nowhere to record those frames when the kernel reclaims them
         * later, they sit above the last usable region on most
         * machines
         */
        if (e->type == PH_MEM_USABLE
            || e->type == PH_MEM_LOADER) {
            if (e->base + e->length > highest) {
                highest = e->base + e->length;
            }
        }
        if (e->type == PH_MEM_LOADER
            && reclaim_count < MAX_RECLAIM) {
            reclaim[reclaim_count].base = e->base;
            reclaim[reclaim_count].length = e->length;
            reclaim_count++;
        }
    }

    /*
     * the direct map has to reach anything the kernel will ever read,
     * and the firmware's own tables are not in memory it calls usable.
     * on this machine they sit immediately above the last usable region
     * in something the bios types "reserved", so grow the top to
     * swallow reserved regions that butt up against what is already
     * there, and stop at the first real gap.
     *
     * that gap is the whole point. the enormous reserved holes up near
     * the terabyte mark are on the far side of it and stay out; mapping
     * as far as those would cost more page tables than this machine has
     * memory. this went unnoticed until the kernel stopped using a bootloader
     * that quietly retyped that region for the kernel
     */
    for (bool grew = true; grew; ) {
        grew = false;
        for (size_t i = 0; i < count; i++) {
            const struct ph_memmap_entry *e = &entries[i];
            uint64_t end = e->base + e->length;
            if (e->type == PH_MEM_BAD || end <= highest_addr) {
                continue;
            }
            /*
             * adjacent, or near enough that the gap is padding rather
             * than a different part of the address space entirely
             */
            if (e->base <= highest_addr + 1024 * 1024) {
                highest_addr = end;
                grew = true;
            }
        }
    }

    managed_frames = highest / PAGE_SIZE;
    meta_bytes = buddy_metadata_bytes(managed_frames);

    /* pass 2: find a usable region big enough to park the bookkeeping */
    void *meta = NULL;
    for (size_t i = 0; i < count; i++) {
        const struct ph_memmap_entry *e = &entries[i];
        if (e->type == PH_MEM_USABLE && e->length >= meta_bytes) {
            meta = (void *)(e->base + hhdm_offset);
            meta_first = e->base / PAGE_SIZE;
            meta_last  = (e->base + meta_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
            break;
        }
    }
    if (meta == NULL) {
        panic("nowhere to put the page allocator's books (%lu bytes). "
              "how little ram is this?", meta_bytes);
    }

    buddy_init(managed_frames, meta, frame_to_virt);

    /*
     * pass 3: everything starts taken, and the usable regions are given
     * away one at a time
     */
    for (size_t i = 0; i < count; i++) {
        const struct ph_memmap_entry *e = &entries[i];
        if (e->type != PH_MEM_USABLE) {
            continue;
        }
        give_away(e->base / PAGE_SIZE, (e->base + e->length) / PAGE_SIZE);
    }

    /* one byte per frame, so that a frame can have more than one owner. */
    pmm_shares_init();
}

/* the plumbing, which needs a real boot to have happened. */
#ifndef VELVETOS_HOSTED

void pmm_init(void)
{
    const struct ph_handoff *h = boot_handoff();
    kprintf("memory map, as philemon found it:\n");
    pmm_init_from_map((const struct ph_memmap_entry *)h->memmap,
                      h->memmap_count, h->hhdm);
    kprintf("  -> %lu MiB usable, %lu KiB on the buddy's books, "
            "%lu KiB on who shares what\n",
            pmm_total_bytes() / (1024 * 1024), meta_bytes / 1024,
            pmm_share_bytes() / 1024);
}

#endif

uint64_t pmm_alloc_pages(size_t count)
{
    if (count == 0) {
        return 0;
    }

    /* the buddy rounds up to a power of two, so five pages costs eight. */
    unsigned order = buddy_order_for(count);

    /*
     * threads can be preempted mid-list and the allocator would hand
     * the same block to two of them
     */
    uint64_t flags = spin_lock_irq(&pmm_lock);

    uint64_t frame = buddy_alloc(order);
    if (frame == BUDDY_NO_BLOCK) {
        spin_unlock_irq(&pmm_lock, flags);
        return 0;       /* the well is dry */
    }

    uint64_t used = total_frames - buddy_free_frames();
    if (used > peak_used) {
        peak_used = used;
    }

    spin_unlock_irq(&pmm_lock, flags);
    return frame * PAGE_SIZE;
}

void pmm_free_pages(uint64_t phys, size_t count)
{
    if (phys % PAGE_SIZE != 0) {
        panic("pmm_free_pages: %016lx is not page aligned, what is this", phys);
    }
    if (count == 0) {
        return;
    }

    uint64_t frame = phys / PAGE_SIZE;
    unsigned order = buddy_order_for(count);

    uint64_t flags = spin_lock_irq(&pmm_lock);
    /*
     * FIXME: this check cannot see a double free whose first free
     * coalesced. buddy_is_free_block() asks whether a free block of
     * this order sits at this frame, and buddy_free() sets the bit at
     * the order the block merged to instead, so a second free of the
     * same frames passes, is linked onto a list it is already part of,
     * and leaves two overlapping free blocks that two callers are then
     * handed. keep an allocated bitmap that coalescing does not clear,
     * and panic unless the frames are marked allocated.
     */
    if (frame >= managed_frames || buddy_is_free_block(frame, order)) {
        panic("pmm: freeing frame %016lx which was never thine to free", phys);
    }
    buddy_free(frame, order);
    spin_unlock_irq(&pmm_lock, flags);
}

uint64_t pmm_alloc(void)
{
    return pmm_alloc_pages(1);
}
void     pmm_free(uint64_t phys)
{
    pmm_free_pages(phys, 1);
}

/* one byte per frame, holding how many *extra* holders it has. */
static uint8_t *share_slot(uint64_t phys)
{
    uint64_t frame = phys / PAGE_SIZE;
    if (shares == NULL || frame >= shares_frames) {
        return NULL;
    }
    return &shares[frame];
}

/* built once, at the end of pmm_init_from_map, and never again. */
void pmm_shares_init(void)
{
    if (shares != NULL || highest_addr == 0) {
        return;
    }
    uint64_t frames = highest_addr / PAGE_SIZE;
    uint64_t bytes = (frames + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t pages = bytes / PAGE_SIZE;

    /*
     * the buddy rounds to a power of two, so ask for exactly that and
     * free with the same number, which is the one rule its interface
     * has
     */
    size_t order_pages = 1;
    while (order_pages < pages) {
        order_pages *= 2;
    }

    uint64_t phys = pmm_alloc_pages(order_pages);
    if (phys == 0) {
        return;     /* no memory for it, so nothing may be shared. honest */
    }
    shares = pmm_phys_to_virt(phys);
    memset(shares, 0, order_pages * PAGE_SIZE);
    shares_frames = frames;
    share_bytes = order_pages * PAGE_SIZE;
}

/* may anything be shared at all? */
bool pmm_can_share(void)
{
    return shares != NULL;
}

uint64_t pmm_share_bytes(void)
{
    return share_bytes;
}

void pmm_ref(uint64_t phys)
{
    uint64_t flags = spin_lock_irq(&pmm_lock);
    uint8_t *slot = share_slot(phys);
    /*
     * FIXME: the count saturates. a page referenced more than 255 times
     * loses the extra holders silently, and unref decides to free by
     * asking whether the slot reached zero, so the free lands early: with
     * 257 holders the slot stops at 255, the 256th unref sees zero and
     * returns the frame to the buddy while the last holder still has it
     * mapped. a program can get there with a loop of forks, since every
     * shared leaf takes one reference. widen the slot, or refuse the ref
     * the way free_pages above refuses a free it does not believe.
     */
    if (slot != NULL && *slot < 255) {
        (*slot)++;
    }
    spin_unlock_irq(&pmm_lock, flags);
}

bool pmm_unref(uint64_t phys)
{
    uint64_t flags = spin_lock_irq(&pmm_lock);
    uint8_t *slot = share_slot(phys);
    bool last = true;
    if (slot != NULL && *slot > 0) {
        (*slot)--;
        last = false;
    }
    spin_unlock_irq(&pmm_lock, flags);

    if (last) {
        pmm_free_pages(phys, 1);
    }
    return last;
}

unsigned pmm_shares(uint64_t phys)
{
    uint64_t flags = spin_lock_irq(&pmm_lock);
    const uint8_t *slot = share_slot(phys);
    unsigned n = (slot != NULL) ? *slot : 0;
    spin_unlock_irq(&pmm_lock, flags);
    return n;
}

void *pmm_phys_to_virt(uint64_t phys)
{
    return (void *)(phys + hhdm_offset);
}

uint64_t pmm_reclaim_bootloader(void)
{
    uint64_t flags = spin_lock_irq(&pmm_lock);
    uint64_t gained = 0;

    for (size_t i = 0; i < reclaim_count; i++) {
        uint64_t first = (reclaim[i].base + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t last  = (reclaim[i].base + reclaim[i].length) / PAGE_SIZE;

        uint64_t added = give_away(first, last);
        total_frames += added;      /* it counts as ram from now on */
        gained += added * PAGE_SIZE;
    }

    /* forget the ranges, so a second call cant double free them */
    reclaim_count = 0;

    spin_unlock_irq(&pmm_lock, flags);
    return gained;
}

bool pmm_translate_is_tracked(uint64_t phys)
{
    return phys / PAGE_SIZE < managed_frames;
}

uint64_t pmm_hhdm_offset(void)
{
    return hhdm_offset;
}
uint64_t pmm_highest_address(void)
{
    return highest_addr;
}

uint64_t pmm_metadata_bytes(void)
{
    return meta_bytes;
}
uint64_t pmm_blocks_at(unsigned order)
{
    return buddy_blocks_at(order);
}

uint64_t pmm_peak_bytes(void)
{
    return peak_used * PAGE_SIZE;
}
uint64_t pmm_total_bytes(void)
{
    return total_frames * PAGE_SIZE;
}
uint64_t pmm_free_bytes(void)
{
    return buddy_free_frames() * PAGE_SIZE;
}
uint64_t pmm_used_bytes(void)
{
    return (total_frames - buddy_free_frames()) * PAGE_SIZE;
}
