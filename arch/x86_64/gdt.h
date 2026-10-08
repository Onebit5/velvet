// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/gdt.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the gdt, and one tss descriptor per core.
 */

#ifndef ARCH_X86_64_GDT_H
#define ARCH_X86_64_GDT_H

#include <stdint.h>

/* selectors, offsets into the gdt */
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x18
#define GDT_USER_CODE   0x20
/* what ring 3 actually loads: the same selectors with RPL 3 */
#define GDT_USER_DATA3  (GDT_USER_DATA | 3)
#define GDT_USER_CODE3  (GDT_USER_CODE | 3)
/*
 * one tss per core, each sixteen bytes wide, starting here. every core
 * loads a different one, which is what lets a core work out which
 * core it is, by asking the cpu which task register it loaded
 */
#define GDT_TSS         0x28
#define GDT_TSS_STRIDE  16
#define GDT_MAX_TSS     8

#define GDT_TSS_FOR(cpu) (GDT_TSS + (cpu) * GDT_TSS_STRIDE)

void gdt_init(void);

/* load the shared table on whichever core is asking */
void gdt_load_here(void);

/*
 * fill in one core's tss descriptor. called by tss_init_cpu once it
 * knows where that core's tss actually lives
 */
void gdt_set_tss(unsigned cpu, uint64_t base, uint32_t limit);

#endif
