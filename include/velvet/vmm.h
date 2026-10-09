// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/vmm.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * virtual memory. four levels of page tables, which the manuals call pml4
 * -> pdpt -> pd -> pt, and which everyone else calls "the reason its kernel
 * triple faults".
 */

/* the design notes for vmm.h are in docs/subsystems/mm.rst */

#ifndef VELVET_VMM_H
#define VELVET_VMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * virtual memory. four levels of page tables, which the manuals call
 * pml4 -> pdpt -> pd -> pt, and which everyone else calls "the reason
 * its kernel triple faults".
 *
 * up to now the kernel was riding on the page tables philemon built for the kernel.
 * these are the kernel's
 */

#define PTE_PRESENT (1ull << 0)
#define PTE_WRITE   (1ull << 1)
#define PTE_USER    (1ull << 2)
#define PTE_WRITETHROUGH (1ull << 3)
#define PTE_NO_CACHE (1ull << 4)    /* mmio: the cpu must not remember
                                     * what a register said last time */
#define PTE_HUGE    (1ull << 7)     /* at pd level: a 2MiB page */
#define PTE_GLOBAL  (1ull << 8)
#define PTE_NX      (1ull << 63)    /* needs EFER.NXE, or its a fault */

#define PTE_COW     (1ull << 9)

#define PTE_ADDR_MASK 0x000ffffffffff000ull

#define PAGE_SIZE_2M (2ull * 1024 * 1024)

#define VMM_NO_MAPPING UINT64_MAX

void vmm_init(void);

/*
 * these take the pml4 by physical address rather than assuming the
 * current one, which is what lets the host tests build a whole address
 * space over a malloc'd arena and inspect it without a cpu involved
 */

uint64_t vmm_new_address_space(void);

bool vmm_map_range(uint64_t pml4, uint64_t virt, uint64_t phys,
                   uint64_t size, uint64_t flags);

uint64_t vmm_translate(uint64_t pml4, uint64_t virt);

uint64_t vmm_flags(uint64_t pml4, uint64_t virt);

bool vmm_unmap_page(uint64_t pml4, uint64_t virt);

uint64_t vmm_kernel_pml4(void);

uint64_t vmm_nx(void);

void vmm_flush_page(uint64_t virt);

void vmm_dump(uint64_t virt);

#endif
