// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/include/asm/pic.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the 8259 pic pair. ancient, cranky, still the simplest way to get
 * hardware interrupts flowing.
 */

#ifndef ARCH_X86_64_ASM_PIC_H
#define ARCH_X86_64_ASM_PIC_H

#include <stdint.h>
#include <stdbool.h>

/*
 * the 8259 pic pair. ancient, cranky, still the simplest way to get
 * hardware interrupts flowing. apic can wait
 */

/* irqs land on vectors 32..47, right after the cpu exceptions */
#define PIC_IRQ_BASE 32

void pic_init(void);
void pic_mask(uint8_t irq);
void pic_unmask(uint8_t irq);
void pic_send_eoi(uint8_t irq);
/* irq 7/15 ghosts. returns true if this one never really happened */
bool pic_is_spurious(uint8_t irq);

#endif
