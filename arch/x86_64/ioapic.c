// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/ioapic.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the io apic: where external interrupts arrive.
 */

#include <asm/ioapic.h>
#include <velvet/vmm.h>
#include <velvet/pmm.h>
#include <velvet/kprintf.h>

/*
 * the registers are behind a window rather than mapped directly: write
 * an index to one address, then read or write the data at another. two
 * accesses per register, and they must not be interleaved with anybody
 * else's, which on one core means interrupts off
 */
#define IOAPIC_INDEX  0x00
#define IOAPIC_DATA   0x10

#define REG_ID        0x00
#define REG_VERSION   0x01
#define REG_REDIRECT  0x10      /* two 32-bit words per line, from here */

#define REDIR_MASKED       (1u << 16)
#define REDIR_LEVEL        (1u << 15)
#define REDIR_ACTIVE_LOW   (1u << 13)

static volatile uint8_t *base;
static uint32_t first_gsi;
static uint32_t line_count;

static uint32_t reg_read(uint32_t reg)
{
    *(volatile uint32_t *)(base + IOAPIC_INDEX) = reg;
    return *(volatile uint32_t *)(base + IOAPIC_DATA);
}

static void reg_write(uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)(base + IOAPIC_INDEX) = reg;
    *(volatile uint32_t *)(base + IOAPIC_DATA) = value;
}

bool ioapic_available(void)
{
    return base != NULL;
}

uint32_t ioapic_lines(void)
{
    return line_count;
}

bool ioapic_init(uint32_t phys_address, uint32_t gsi_base)
{
    if (phys_address == 0) {
        return false;
    }

    /*
     * like the lapic, this sits above every scrap of ram, so the direct
     * map does not reach it and caching would be actively wrong
     */
    uint64_t virt = pmm_hhdm_offset() + phys_address;
    if (!vmm_map_range(vmm_kernel_pml4(), virt, phys_address, PAGE_SIZE,
                       PTE_WRITE | PTE_NO_CACHE | vmm_nx())) {
        return false;
    }
    vmm_flush_page(virt);

    base = (volatile uint8_t *)virt;
    first_gsi = gsi_base;

    /*
     * the chip says how many lines it has: bits 16..23 of the version
     * register, and the field is one less than the count.
     *
     * if that read gives something implausible then the mapping is not
     * working and every routing call below would be silently skipped by
     * its own range check, which looks exactly like a keyboard that
     * stopped existing. say so instead
     */
    uint32_t version = reg_read(REG_VERSION);
    line_count = ((version >> 16) & 0xff) + 1;
    if (line_count < 16 || line_count > 240) {
        kprintf("ioapic     : version register read %08x, i.e. %u lines, "
                "which cannot be right\n", version, line_count);
        base = NULL;
        return false;
    }

    /*
     * everything starts masked. a line nobody has claimed firing into a
     * vector nobody handles is a bad way to begin
     */
    for (uint32_t i = 0; i < line_count; i++) {
        ioapic_mask(first_gsi + i);
    }
    return true;
}

bool ioapic_route(uint32_t gsi, uint8_t vector, uint32_t lapic_id,
                  uint16_t flags)
{
    if (base == NULL || gsi < first_gsi || gsi >= first_gsi + line_count) {
        kprintf("ioapic     : line %u is not one of the kernel's (%u..%u)\n",
                gsi, first_gsi, first_gsi + line_count - 1);
        return false;
    }
    uint32_t reg = REG_REDIRECT + (gsi - first_gsi) * 2;

    uint32_t low = vector;      /* fixed delivery, physical mode, unmasked */

    /*
     * the madt's flags, honoured rather than assumed. a level-triggered
     * line left as edge-triggered fires once and stops; an edge one
     * treated as level fires forever
     */
    if ((flags & 0x3) == 0x3) {
        low |= REDIR_ACTIVE_LOW;
    }
    if (((flags >> 2) & 0x3) == 0x3) {
        low |= REDIR_LEVEL;
    }

    reg_write(reg + 1, lapic_id << 24);
    reg_write(reg, low);

    /*
     * read it back. an mmio write that went nowhere is invisible from
     * here otherwise, and the consequence, an interrupt that never
     * arrives, is indistinguishable from hardware that is simply
     * quiet until somebody presses a key and nothing happens
     */
    /*
     * bits 12 and 14 are set by the chip, not by the kernel, so compare only
     * what the kernel actually wrote: the vector and the mask bit
     */
    uint32_t back = reg_read(reg);
    if ((back & 0xff) != (low & 0xff) || (back & REDIR_MASKED) != 0) {
        kprintf("ioapic     : line %u wrote %08x and read back %08x\n",
                gsi, low, back);
        return false;
    }
    return true;
}

void ioapic_mask(uint32_t gsi)
{
    if (base == NULL || gsi < first_gsi || gsi >= first_gsi + line_count) {
        return;
    }
    uint32_t reg = REG_REDIRECT + (gsi - first_gsi) * 2;
    reg_write(reg, REDIR_MASKED);
}
