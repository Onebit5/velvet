// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/include/asm/mmu.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * telling an x86_64 processor about page tables. included as
 * <asm/mmu.h>.
 */

#ifndef ARCH_X86_64_ASM_MMU_H
#define ARCH_X86_64_ASM_MMU_H

#include <stdint.h>

#include <velvet/compiler.h>
#include <asm/smp.h>

/*
 * telling an x86_64 processor about page tables. included as
 * <asm/mmu.h>.
 */

#ifndef VELVETOS_HOSTED

/*
 * cr3. every instruction after this one is fetched through the tables at
 * `phys_root`, and the whole translation cache is thrown away with it,
 * which is why callers check whether anything actually changed first
 */
ARCH_INLINE void mmu_load_table(uint64_t phys_root)
{
    asm volatile ("mov %0, %%cr3" : : "r"(phys_root) : "memory");
}

/*
 * one page's translation is stale. nothing in the hardware notices that
 * the table underneath it moved
 */
ARCH_INLINE void mmu_flush_page(uint64_t virt)
{
    asm volatile ("invlpg (%0)" : : "r"(virt) : "memory");
}

/*
 * CR0.WP. without it ring 0 may scribble on read-only pages regardless
 * of what the tables say, which would make the whole W^X exercise
 * decorative, the tables would be right and nothing would be checking
 * them
 */
/*
 * the other cores still have the mappings the kernel just changed, and nothing
 * in the hardware tells them. on x86 that means an inter-processor
 * interrupt and waiting for an answer; the caller only needs to know
 * that changing a mapping is not finished until this returns
 */
ARCH_INLINE void mmu_shootdown(void)
{
    smp_tlb_shootdown();
}

ARCH_INLINE void mmu_enforce_write_protect(void)
{
    uint64_t cr0;
    asm volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1ull << 16);
    asm volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

#else

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

#endif
