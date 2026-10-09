// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/include/asm/smp.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * waking the other cores.
 */

#ifndef ARCH_X86_64_ASM_SMP_H
#define ARCH_X86_64_ASM_SMP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <asm/acpi.h>

/*
 * waking the other cores. the firmware reports how many processors the
 * machine has and the kernel has been using one of them; the rest sit in
 * reset, and the only thing that can bring one out is an interrupt from
 * another core's local apic.
 *
 * a core that wakes does not resume anything, it starts in real mode,
 * sixteen bits wide, below a megabyte, so each one makes the same climb
 * into long mode the bootloader made, on a page of code left at a fixed
 * address for that purpose. on arrival they report their apic id, which
 * is proof they ran there, and halt: giving them work cannot come first,
 * because every place in this kernel that turns interrupts off and calls
 * that mutual exclusion is a lie the moment a second core runs kernel
 * code
 */

#define SMP_MAX_CPUS 32

/*
 * how often each core's own timer fires. the same rate the first core
 * has always used, so a thread's slice means the same thing wherever it
 * runs
 */
#define SMP_TICK_HZ 100

/*
 * the page a woken core starts executing at, in real mode. the startup
 * message carries a vector, and the vector is a page number: 8 means
 * 0x8000. it has to be below a megabyte because that is all real mode
 * can see
 */
#define SMP_TRAMPOLINE_VECTOR 0x08
#define SMP_TRAMPOLINE_ADDR   ((uint64_t)SMP_TRAMPOLINE_VECTOR * 0x1000)

/*
 * the arguments left for it, past the code, at offsets the assembly
 * agrees with. keep these next to the ones in trampoline.asm
 */
#define SMP_ARG_CR3   0x0f00
#define SMP_ARG_STACK 0x0f08
#define SMP_ARG_ENTRY 0x0f10
#define SMP_ARG_CPU   0x0f18
#define SMP_ARG_EFER  0x0f20
#define SMP_ARG_LOUD  0x0f28

struct cpu {
    uint32_t index;             /* 0 is the one that booted the machine */
    uint32_t apic_id;           /* what the firmware said */

    /*
     * what the core itself said once it was awake. it reads this out of
     * its own local apic, so a match is proof the code ran where it was
     * meant to and not twice on the same processor
     */
    volatile uint32_t reported_id;
    volatile bool     online;

    uint64_t stack_top;
    bool     bootstrap;

    /* has this core entered the scheduler and started taking work */
    volatile bool scheduling;

    /*
     * what the first core measured the local apic timer at. the clock is
     * the same piece of silicon on every core, so measuring once and
     * telling the others is honest, and measuring again on each would
     * need the pit, which only one core can be using at a time
     */
    uint64_t timer_ticks_per_second;
};

/*
 * wake everything the firmware listed. safe to call on a machine with
 * one core, or no usable local apic, or firmware that will not say,
 * it does nothing and reports one cpu, which is what was true before
 */
bool smp_init(const struct acpi_info *info, uint64_t timer_ticks_per_second);

size_t smp_cpu_count(void);      /* how many the firmware described */
size_t smp_online_count(void);   /* how many actually answered */
const struct cpu *smp_cpu_at(size_t index);

/* which core is asking */
uint32_t smp_this_cpu(void);

/*
 * the vector one core interrupts another on, to make it throw away what
 * it remembers about the page tables
 */
#define SMP_IPI_TLB 0x41

/*
 * every other core has its own cached translations, and nothing in the
 * hardware tells it when this one changes a page table. so it has to be
 * told: an interrupt to each, and a wait until every one has answered.
 *
 * the wait is the part that matters. returning before they have all
 * flushed would mean carrying on while another core is still using a
 * mapping the kernel has already taken away, which is a use-after-free with the
 * page table as the thing freed
 */
void smp_tlb_shootdown(void);

/* the handler, called from the interrupt dispatcher */
void smp_tlb_ipi(void);

#endif
