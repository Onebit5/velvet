// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/pmm.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * physical memory manager. a buddy allocator underneath (see buddy.h):
 * free lists per block size, and blocks that put themselves back together
 * when both halves come home.
 */

/* the design notes for pmm.h are in docs/subsystems/mm.rst */

#ifndef MM_PMM_H
#define MM_PMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "philemon.h"

/*
 * physical memory manager over the buddy allocator in buddy.h: free
 * lists per block size, and blocks that put themselves back together
 * when both halves come home. sizes round up to a power of two, so
 * asking for five pages costs eight, and the count freed has to be the
 * count allocated, since that is what says which list the block is on
 */

#define PAGE_SIZE 4096

void pmm_init(void);

void pmm_init_from_map(const struct ph_memmap_entry *entries, size_t count,
                       uint64_t hhdm);

uint64_t pmm_alloc_pages(size_t count);
void     pmm_free_pages(uint64_t phys, size_t count);

uint64_t pmm_alloc(void);
void     pmm_free(uint64_t phys);

/*
 * copy on write means two address spaces pointing at one frame, and
 * whichever of them ends first must not hand it back while the other is
 * still reading it. so a frame can be shared, and freeing it only
 * really frees it when the last holder lets go.
 *
 * the count kept is of *extra* holders, so zero means one owner and the
 * ordinary path costs nothing but a byte's worth of lookup. a frame
 * nobody has shared behaves exactly as it always did.
 */

void pmm_ref(uint64_t phys);

bool pmm_unref(uint64_t phys);

unsigned pmm_shares(uint64_t phys);

/*
 * build the table. called once from pmm_init_from_map, where one core
 * is running and nothing is held, it allocates, so it can never be
 * called from anywhere that already has the pmm's lock
 */
void pmm_shares_init(void);

bool pmm_can_share(void);

uint64_t pmm_share_bytes(void);

void *pmm_phys_to_virt(uint64_t phys);

uint64_t pmm_hhdm_offset(void);

uint64_t pmm_highest_address(void);

uint64_t pmm_reclaim_bootloader(void);

uint64_t pmm_metadata_bytes(void);
uint64_t pmm_blocks_at(unsigned order);

bool pmm_translate_is_tracked(uint64_t phys);

uint64_t pmm_peak_bytes(void);

uint64_t pmm_total_bytes(void);
uint64_t pmm_free_bytes(void);
uint64_t pmm_used_bytes(void);

#endif
