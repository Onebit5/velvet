// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/include/asm/machine.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the pc, rather than the processor. included as <asm/machine.h>.
 */

#ifndef ARCH_X86_64_ASM_MACHINE_H
#define ARCH_X86_64_ASM_MACHINE_H

#include <stdbool.h>

/*
 * the pc, rather than the processor. included as <asm/machine.h>.
 *
 * every one of these is a *pc* thing rather than an x86_64 thing, the
 * 8042's reset line, the acpi ports three emulators happen to watch,
 * and a keyboard controller that has been at port 0x60 since 1981. an
 * x86_64 machine that was not a pc would need all three replaced and
 * not one instruction changed, which is the reason this header is not
 * <asm/cpu.h>.
 */

/* the three halves of a boot that are not portable. */
void machine_bring_up_early(void);
void machine_bring_up_late(void);
void machine_start_clock(void);

void machine_reset(void) __attribute__((noreturn));
bool machine_poweroff(void);
bool machine_key_pressed(void);

#endif
