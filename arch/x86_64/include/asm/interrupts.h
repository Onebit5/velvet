// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/include/asm/interrupts.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what isr_common in isr.asm leaves on the stack, low address first. if
 * you touch the push order over there, touch this too.
 */

#ifndef ARCH_X86_64_ASM_INTERRUPTS_H
#define ARCH_X86_64_ASM_INTERRUPTS_H

#include <stdint.h>
#include <stdbool.h>
#include <asm/acpi.h>

/*
 * what isr_common in isr.asm leaves on the stack, low address first.
 * if you touch the push order over there, touch this too. the static
 * asserts in interrupts.c will yell at you if you forget
 */

struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    /* pushed by the cpu itself */
    uint64_t rip, cs, rflags, rsp, ss;
};

void interrupt_dispatch(struct interrupt_frame *frame);

/*
 * bring up the apics if the firmware describes any, and move every
 * interrupt over to them. returns false if the kernel is staying on the 8259,
 * which is not a failure, it is the same behaviour by an older road
 */
bool interrupts_use_apic(void);

/*
 * what acpi said about this machine, read once at boot. smp needs the
 * list of processors out of it, and there is no reason to walk the
 * tables twice
 */
const struct acpi_info *interrupts_acpi(void);

/*
 * what the first core measured the local apic timer at, in its own ticks
 * per second. every core has one of these timers and they all run off
 * the same clock, so the other cores are told rather than each taking
 * the pit in turn to find out
 */
uint64_t interrupts_timer_rate(void);

/*
 * move the external interrupts to the io apic as well. deliberately not
 * done at boot, see the note in interrupts.c. returns false without
 * changing anything if the routing cannot be verified
 */
bool interrupts_use_ioapic(void);

/*
 * true once the move has happened. mostly so `dmesg` and the shell can
 * say which hardware is actually carrying the interrupts
 */
bool interrupts_on_apic(void);

/*
 * hooking a handler onto a line is `irq_install`, in <asm/irq.h>, it is
 * the one part of this header a driver still wanted, and drivers should
 * not be reading a file that spells out rax through r15 to get it
 */

/*
 * interrupt masking used to live here, and nineteen files across mm,
 * sched, fs, drivers and lib included this header to get it, four
 * lines of masking, and a struct listing rax through r15 and the whole
 * 8259 along with them.
 *
 * it is <asm/irq.h> now. included here so that a file which genuinely
 * wants both, a driver registering a handler and taking a lock, is
 * not made to say so twice
 */
#include <asm/irq.h>


#endif
