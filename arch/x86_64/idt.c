// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/idt.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the idt: 256 gates, every single one pointing at its stub from isr.asm.
 */

#include "arch/x86_64/idt.h"
#include "arch/x86_64/gdt.h"
#include <stdint.h>

/*
 * the idt: 256 gates, every single one pointing at its stub from isr.asm.
 * unhandled vectors still go through the common path and get logged
 * instead of triple faulting, which is the whole point
 */

struct __attribute__((packed)) idt_entry {
    uint16_t off_low;
    uint16_t selector;
    uint8_t  ist;       /* interrupt stack table index, 0 = dont switch */
    uint8_t  flags;
    uint16_t off_mid;
    uint32_t off_high;
    uint32_t reserved;
};

struct __attribute__((packed)) idtr {
    uint16_t limit;
    uint64_t base;
};

static struct idt_entry idt[256];

/* built by isr.asm */
extern void *isr_stub_table[256];

/* the idtr, and loading it on whichever core is asking */
void idt_load_here(void)
{
    struct __attribute__((packed)) { uint16_t limit; uint64_t base; } idtr = {
        .limit = sizeof(idt) - 1,
        .base  = (uint64_t)idt,
    };
    asm volatile ("lidt %0" :: "m"(idtr));
}

void idt_init(void)
{
    for (int i = 0; i < 256; i++) {
        uint64_t off = (uint64_t)isr_stub_table[i];
        idt[i] = (struct idt_entry)
{
            .off_low  = off & 0xffff,
            .selector = GDT_KERNEL_CODE,
            .ist      = 0,
            .flags    = 0x8e,   /* present, dpl 0, interrupt gate (ints off on entry) */
            .off_mid  = (off >> 16) & 0xffff,
            .off_high = (uint32_t)(off >> 32),
            .reserved = 0,
        };
    }

    struct idtr idtr = {
        .limit = sizeof(idt) - 1,
        .base  = (uint64_t)idt,
    };
    asm volatile ("lidt %0" : : "m"(idtr));
}

void idt_set_ist(uint8_t vector, uint8_t ist)
{
    idt[vector].ist = ist & 0x7;
}
