// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/tss.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the task state segment, and the per-core stacks it points at.
 */

#include "arch/x86_64/tss.h"
#include "arch/x86_64/gdt.h"
#include "mm/pmm.h"
#include "lib/panic.h"
#include "lib/string.h"

#define IST_STACK_PAGES 4       /* 16k, plenty for a handler that only reports */

/*
 * the layout the cpu expects, and it is a strange one, the reserved
 * holes are where the 32-bit fields used to live back when task
 * switching was a hardware feature
 */
struct __attribute__((packed)) tss {
    uint32_t reserved0;
    uint64_t rsp[3];            /* rsp0..rsp2, one per privilege level */
    uint64_t reserved1;
    uint64_t ist[7];            /* ist1..ist7, indexed from 0 here */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
};

_Static_assert(sizeof(struct tss) == 104, "the tss is a fixed shape, check the packing");

/*
 * one per core, and every field in them is that core's alone. two cores
 * trapping in from ring 3 at the same instant have to land on different
 * stacks, and this is where the cpu reads which
 */
static struct tss tables[GDT_MAX_TSS];

static uint64_t alloc_stack(const char *what)
{
    uint64_t phys = pmm_alloc_pages(IST_STACK_PAGES);
    if (phys == 0) {
        panic("tss: no memory for the %s stack", what);
    }
    /* stacks grow down, so hand back the top */
    return (uint64_t)pmm_phys_to_virt(phys) + IST_STACK_PAGES * PAGE_SIZE;
}

/*
 * which core is asking, read from the cpu itself.
 *
 * every core loads a different tss selector, so the task register *is* a
 * core's name, and asking the processor which one it loaded costs a
 * couple of cycles and needs no per-core memory to have been reachable
 * first. that last part is why it is done this way: everything else that
 * could answer this question has to be found before it can be asked, and
 * this does not.
 */
unsigned tss_current_cpu(void)
{
    uint16_t tr;
    asm volatile ("str %0" : "=r"(tr));

    /*
     * XXX: both fallbacks below answer "the boot core", which is a lie
     * for a core that merely has no task segment of its own. every
     * per-core structure in the kernel is indexed by this, and
     * smp_this_cpu() is this, so a core that guessed 0 would use the
     * boot core's run queue, syscall area and tss. nothing reaches the
     * wrong answer today only because a core whose tss_init_cpu() failed
     * halts in smp_ap_entry before it can run a line. answer with
     * something a caller cannot mistake for a real core.
     */
    if (tr < GDT_TSS) {
        return 0;       /* nothing loaded yet: only the boot core exists */
    }
    unsigned cpu = (unsigned)((tr - GDT_TSS) / GDT_TSS_STRIDE);
    return cpu < GDT_MAX_TSS ? cpu : 0;
}

void tss_set_rsp0(uint64_t rsp0)
{
    tables[tss_current_cpu()].rsp[0] = rsp0;
}

static bool setup(unsigned cpu)
{
    if (cpu >= GDT_MAX_TSS) {
        return false;
    }
    struct tss *t = &tables[cpu];
    memset(t, 0, sizeof *t);

    t->ist[IST_DOUBLE_FAULT - 1] = alloc_stack("double fault");

    /*
     * where the cpu lands on a trap from ring 3. it follows whichever
     * thread is on this core and is rewritten on every switch, or two
     * user threads trapping at once would land on the same stack
     */
    t->rsp[0] = alloc_stack("ring 0 entry");

    /*
     * an iomap base past the end of the segment means "no io bitmap",
     * which is how you say "ring 3 may not touch ports"
     */
    t->iomap_base = sizeof *t;

    gdt_set_tss(cpu, (uint64_t)t, sizeof *t - 1);

    /* loading it is also what tells this core its own name */
    uint16_t selector = (uint16_t)GDT_TSS_FOR(cpu);
    asm volatile ("ltr %w0" :: "r"(selector) : "memory");
    return true;
}

void tss_init(void)
{
    if (!setup(0)) {
        panic("tss: the boot core could not be given a task segment");
    }
}

bool tss_init_cpu(unsigned cpu)
{
    return setup(cpu);
}
