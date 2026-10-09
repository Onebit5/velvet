// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/gdt.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the gdt, and one tss descriptor per core.
 */

#include <asm/gdt.h>
#include <stdint.h>
#include <stddef.h>

/*
 * segmentation is basically dead in long mode but the cpu still demands
 * a gdt, so here is the flattest one possible. the bootloader gave the kernel a
 * perfectly fine one, but it is living in memory the kernel will reclaim eventually,
 * plus the kernel needs its own once the tss shows up.
 *
 * not const: loading a tss later flips the busy bit in its descriptor,
 * so the table has to be writable
 */

static uint64_t gdt[] = {
    0,                      /* null descriptor, mandatory tribute */
    0x00af9a000000ffff,     /* 0x08 kernel code: present, exec, long mode */
    0x00af92000000ffff,     /* 0x10 kernel data: present, rw */
    /*
     * the order of these two is not the kernel's to choose. sysret computes
     * CS = STAR[63:48] + 16 and SS = STAR[63:48] + 8, so data must sit
     * eight bytes below code or returning to ring 3 lands nowhere
     */
    0x00aff2000000ffff,     /* 0x18 user data: present, rw, dpl 3 */
    0x00affa000000ffff,     /* 0x20 user code: present, exec, long mode, dpl 3 */
    /*
     * 0x28 onwards: one sixteen-byte tss descriptor per core, filled in
     * by tss_init_cpu as each one comes up. a core with no tss loaded
     * has none of these, which is why the boot core must take slot 0
     * before anybody asks which core they are
     */
    0, 0,  0, 0,  0, 0,  0, 0,
    0, 0,  0, 0,  0, 0,  0, 0,
};

/*
 * a 64-bit tss descriptor is twice the width of a normal one and its
 * fields are scattered across it in the least convenient order the
 * 1980s could devise. type 9 is "available 64-bit tss"
 */
void gdt_set_tss(unsigned cpu, uint64_t base, uint32_t limit)
{
    if (cpu >= GDT_MAX_TSS) {
        return;
    }
    size_t at = GDT_TSS_FOR(cpu) / 8;
    gdt[at] = (uint64_t)(limit & 0xffff)
            | ((base & 0xffffff) << 16)
            | (0x89ull << 40)                      /* present, type 9 */
            | ((uint64_t)((limit >> 16) & 0xf) << 48)
            | (((base >> 24) & 0xff) << 56);
    gdt[at + 1] = base >> 32;
}

/*
 * the gdt itself is shared, it is the same table for every core, and
 * only the tss entries differ. so a core coming up just loads it
 */
void gdt_load_here(void)
{
    struct __attribute__((packed)) { uint16_t limit; uint64_t base; } gdtr = {
        .limit = sizeof(gdt) - 1,
        .base  = (uint64_t)gdt,
    };
    asm volatile ("lgdt %0" :: "m"(gdtr));
}

struct __attribute__((packed)) gdtr {
    uint16_t limit;
    uint64_t base;
};

void gdt_init(void)
{
    struct gdtr gdtr = {
        .limit = sizeof(gdt) - 1,
        .base  = (uint64_t)gdt,
    };

    /*
     * lgdt, then reload every segment register. cs cant be mov'd into,
     * you have to far-return your way into it like its 1985
     */
    asm volatile (
        "lgdt %0\n"
        "pushq %[cs]\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw %w[ds], %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%ss\n"
        "xorl %%eax, %%eax\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        :
        : "m"(gdtr), [cs] "i"(GDT_KERNEL_CODE), [ds] "i"(GDT_KERNEL_DATA)
        : "rax", "memory");
}
