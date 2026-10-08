// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/lapic.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the local apic, and the registers it answers to.
 */

#include "arch/x86_64/lapic.h"
#include "arch/x86_64/msr.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "lib/kprintf.h"

#define MSR_APIC_BASE       0x1b
#define APIC_BASE_ENABLE    (1ull << 11)

/*
 * registers, as byte offsets. every one is 32 bits wide and must be
 * accessed as exactly 32 bits, a byte write to an apic register does
 * not do a quarter of the job, it does nothing anybody wants
 */
#define LAPIC_ID            0x020
#define LAPIC_VERSION       0x030
#define LAPIC_TPR           0x080
#define LAPIC_EOI           0x0b0
#define LAPIC_SPURIOUS      0x0f0
#define LAPIC_LVT_TIMER     0x320
#define LAPIC_TIMER_INIT    0x380
#define LAPIC_TIMER_CURRENT 0x390
#define LAPIC_TIMER_DIVIDE  0x3e0
#define LAPIC_ICR_LOW       0x300
#define LAPIC_ICR_HIGH      0x310

/*
 * the interrupt command register, which is how one core says anything
 * at all to another
 */
#define ICR_DELIVERY_INIT   (5u << 8)
#define ICR_DELIVERY_STARTUP (6u << 8)
#define ICR_LEVEL_ASSERT    (1u << 14)
#define ICR_PENDING         (1u << 12)

#define SPURIOUS_ENABLE     (1u << 8)
#define LVT_MASKED          (1u << 16)
#define LVT_PERIODIC        (1u << 17)

/*
 * where the kernel mapped it. the apic lives above every scrap of ram, so the
 * direct map does not reach it and it needs a mapping of its own
 */
static volatile uint8_t *lapic;

static void write32(uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)(lapic + reg) = value;
}

static uint32_t read32(uint32_t reg)
{
    return *(volatile uint32_t *)(lapic + reg);
}

bool lapic_available(void)
{
    return lapic != NULL;
}

bool lapic_init(uint64_t phys_address)
{
    if (phys_address == 0) {
        return false;
    }

    /*
     * the direct map stops at the top of usable memory and this is
     * nowhere near it, so map the page itself. write-through would
     * be wrong here and so would caching: these are registers, and the
     * cpu must not remember what one of them said
     */
    uint64_t virt = pmm_hhdm_offset() + phys_address;
    if (!vmm_map_range(vmm_kernel_pml4(), virt, phys_address, PAGE_SIZE,
                       PTE_WRITE | PTE_NO_CACHE | vmm_nx())) {
        return false;
    }
    vmm_flush_page(virt);
    lapic = (volatile uint8_t *)virt;

    /*
     * the enable bit in the msr is a separate thing from the software
     * enable below, and both are needed
     */
    wrmsr(MSR_APIC_BASE, rdmsr(MSR_APIC_BASE) | APIC_BASE_ENABLE);

    /*
     * accept every priority. the tpr is a filter and it starts closed
     * on some firmware
     */
    write32(LAPIC_TPR, 0);

    /*
     * software enable, and a vector for the interrupts that arrive
     * without anybody having raised one. a spurious interrupt gets no
     * eoi, which is why it needs a vector of its own to be recognised
     */
    write32(LAPIC_SPURIOUS, SPURIOUS_ENABLE | LAPIC_SPURIOUS_VECTOR);

    write32(LAPIC_LVT_TIMER, LVT_MASKED);    /* until it is calibrated */
    return true;
}

void lapic_eoi(void)
{
    if (lapic != NULL) {
        write32(LAPIC_EOI, 0);
    }
}

uint32_t lapic_id(void)
{
    return lapic != NULL ? (read32(LAPIC_ID) >> 24) : 0;
}

uint64_t lapic_calibrate(void (*wait_ms)(uint64_t), uint64_t ms)
{
    if (lapic == NULL || ms == 0) {
        return 0;
    }

    write32(LAPIC_TIMER_DIVIDE, 0x3);        /* divide by 16 */
    write32(LAPIC_LVT_TIMER, LVT_MASKED);    /* count, but tell nobody */
    write32(LAPIC_TIMER_INIT, 0xffffffff);   /* and count down from the top */

    wait_ms(ms);

    uint32_t left = read32(LAPIC_TIMER_CURRENT);
    write32(LAPIC_TIMER_INIT, 0);            /* stop */

    uint64_t elapsed = 0xffffffffull - left;
    return (elapsed * 1000) / ms;            /* apic ticks per second */
}

void lapic_timer_start(uint32_t hz, uint64_t ticks_per_second)
{
    if (lapic == NULL || hz == 0 || ticks_per_second == 0) {
        return;
    }

    uint64_t count = ticks_per_second / hz;
    if (count == 0) {
        count = 1;
    }

    write32(LAPIC_TIMER_DIVIDE, 0x3);
    write32(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | LVT_PERIODIC);
    write32(LAPIC_TIMER_INIT, (uint32_t)count);
}


void lapic_enable_here(void)
{
    if (!lapic_available()) {
        return;
    }
    write32(LAPIC_TPR, 0);      /* accept every priority */
    write32(LAPIC_SPURIOUS, SPURIOUS_ENABLE | LAPIC_SPURIOUS_VECTOR);
}


/*
 * the command is only accepted when the last one has been delivered, and
 * "delivered" is a bit the hardware clears in its own time
 */
static bool icr_idle(void)
{
    for (int spin = 0; spin < 1000000; spin++) {
        if (!(read32(LAPIC_ICR_LOW) & ICR_PENDING)) {
            return true;
        }
    }
    return false;
}

/*
 * the target goes in the high half and the command in the low half, and
 * writing the low half is what sends it, so the order matters and is
 * the wrong way round from how it reads
 */
static bool send(uint32_t apic_id, uint32_t command)
{
    if (!lapic_available() || !icr_idle()) {
        return false;
    }
    write32(LAPIC_ICR_HIGH, apic_id << 24);
    write32(LAPIC_ICR_LOW, command);
    return icr_idle();
}

bool lapic_send_init(uint32_t apic_id)
{
    return send(apic_id, ICR_DELIVERY_INIT | ICR_LEVEL_ASSERT);
}

bool lapic_send_ipi(uint32_t apic_id, uint8_t vector)
{
    /* delivery mode 0, a plain interrupt at the given vector */
    return send(apic_id, ICR_LEVEL_ASSERT | vector);
}

bool lapic_send_startup(uint32_t apic_id, uint8_t vector)
{
    /*
     * the vector is a page number, not an address: the core begins at
     * vector * 0x1000, in real mode, knowing nothing
     */
    return send(apic_id, ICR_DELIVERY_STARTUP | ICR_LEVEL_ASSERT | vector);
}
