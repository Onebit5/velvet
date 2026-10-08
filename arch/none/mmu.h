// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/none/mmu.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the portable arch's memory hooks, which have nothing to do.
 */

#ifndef ARCH_NONE_MMU_H
#define ARCH_NONE_MMU_H

#include <stdint.h>
#include "arch/inline.h"

ARCH_INLINE void mmu_load_table(uint64_t phys_root)
{
    (void)phys_root;
}
ARCH_INLINE void mmu_flush_page(uint64_t virt)
{
    (void)virt;
}
ARCH_INLINE void mmu_shootdown(void)
{
}
ARCH_INLINE void mmu_enforce_write_protect(void)
{
}

#endif
