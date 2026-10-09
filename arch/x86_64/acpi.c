// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/acpi.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * acpi: walking the tables from the rsdp to the interrupt controllers.
 */

#include <asm/acpi.h>
#include <velvet/boot.h>
#include <velvet/string.h>

/*
 * the root pointer. revision 0 means acpi 1.0 and a 32-bit rsdt;
 * anything higher means there is also an xsdt with 64-bit entries,
 * which is the one to prefer when present
 */
struct rsdp {
    char     signature[8];      /* "RSD PTR " */
    uint8_t  checksum;          /* over the first 20 bytes only */
    char     oem[6];
    uint8_t  revision;
    uint32_t rsdt_address;
    /* revision 2 and later */
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum; /* over the whole thing */
    uint8_t  reserved[3];
} __attribute__((packed));

/* every acpi table starts with this */
struct sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem[6];
    char     oem_table[8];
    uint32_t oem_revision;
    uint32_t creator;
    uint32_t creator_revision;
} __attribute__((packed));

struct madt {
    struct sdt_header header;
    uint32_t lapic_address;
    uint32_t flags;             /* bit 0: an 8259 is present and must be masked */
    /* then a run of variable-length entries */
} __attribute__((packed));

struct madt_entry {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

#define MADT_LAPIC     0
#define MADT_IOAPIC    1
#define MADT_OVERRIDE  2
#define MADT_LAPIC_ADDR_OVERRIDE 5

bool acpi_checksum_ok(const void *table, size_t len)
{
    const uint8_t *p = table;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + p[i]);
    }
    return sum == 0;
}

static bool sig_is(const char *have, const char *want, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (have[i] != want[i]) {
            return false;
        }
    }
    return true;
}

static void parse_madt(const struct madt *m, struct acpi_info *out)
{
    out->lapic_address = m->lapic_address;

    const uint8_t *p   = (const uint8_t *)m + sizeof(struct madt);
    const uint8_t *end = (const uint8_t *)m + m->header.length;

    while (p + sizeof(struct madt_entry) <= end) {
        const struct madt_entry *e = (const struct madt_entry *)p;

        /*
         * a zero length would leave the kernel here forever, and a firmware bug
         * is not a reason to hang before the console even exists
         */
        if (e->length < sizeof(struct madt_entry) || p + e->length > end) {
            break;
        }

        switch (e->type) {
        case MADT_LAPIC:
            /*
             * byte 4 is the flags; bit 0 says the cpu is usable. byte 3
             * is its apic id, which is the only way to address a core
             * that is not running yet, there is no other name for it
             */
            /*
             * FIXME: the write is capped at ACPI_MAX_CPUS and the count
             * is not, so on a machine with more usable processors than
             * that, cpu_count runs past the end of the list it counts.
             * nothing reads off the end today only because the one
             * consumer clamps with a constant of the same size, and the
             * two are independent: raise the smp limit alone and
             * smp_init() reads lapic_ids[] past its end and sends init
             * to whatever memory followed it. the same disagreement is
             * why the kernel tries to start cores past GDT_MAX_TSS,
             * which have no task segment to load and halt with a '!'
             * after two slow timeouts each. one limit, or cap the count
             * where the array is capped.
             */
            if (e->length >= 8 && (p[4] & 1)) {
                if (out->cpu_count < ACPI_MAX_CPUS) {
                    out->lapic_ids[out->cpu_count] = p[3];
                }
                out->cpu_count++;
            }
            break;

        case MADT_IOAPIC:
            if (e->length >= 12 && out->ioapic_count < ACPI_MAX_IOAPICS) {
                struct acpi_ioapic *io = &out->ioapics[out->ioapic_count++];
                io->id       = p[2];
                memcpy(&io->address,  p + 4, 4);
                memcpy(&io->gsi_base, p + 8, 4);
            }
            break;

        case MADT_OVERRIDE:
            /* the entry that says "irq 0 does not arrive on line 0" */
            if (e->length >= 10 && out->override_count < ACPI_MAX_OVERRIDES) {
                struct acpi_override *ov = &out->overrides[out->override_count++];
                ov->isa_irq = p[3];
                memcpy(&ov->gsi,   p + 4, 4);
                memcpy(&ov->flags, p + 8, 2);
            }
            break;

        case MADT_LAPIC_ADDR_OVERRIDE:
            /* a 64-bit address, which supersedes the 32-bit one above */
            if (e->length >= 12) {
                memcpy(&out->lapic_address, p + 4, 8);
            }
            break;

        default:
            break;      /* nmi sources and the rest are not its business */
        }

        p += e->length;
    }
}

struct acpi_info acpi_parse(uint64_t rsdp_phys, void *(*read)(uint64_t phys))
{
    struct acpi_info info;
    memset(&info, 0, sizeof info);

    if (rsdp_phys == 0 || read == NULL) {
        return info;
    }

    const struct rsdp *r = read(rsdp_phys);
    if (r == NULL || !sig_is(r->signature, "RSD PTR ", 8)) {
        return info;
    }
    /*
     * the first twenty bytes are the 1.0 structure, and its checksum
     * covers exactly those regardless of revision
     */
    if (!acpi_checksum_ok(r, 20)) {
        return info;
    }

    /* prefer the 64-bit table when the firmware offers one */
    bool use_xsdt = (r->revision >= 2 && r->xsdt_address != 0);
    uint64_t root_phys = use_xsdt ? r->xsdt_address : r->rsdt_address;
    if (root_phys == 0) {
        return info;
    }

    const struct sdt_header *root = read(root_phys);
    if (root == NULL || !acpi_checksum_ok(root, root->length)) {
        return info;
    }

    size_t entry_size = use_xsdt ? 8 : 4;
    size_t count = (root->length - sizeof(struct sdt_header)) / entry_size;
    const uint8_t *entries = (const uint8_t *)root + sizeof(struct sdt_header);

    for (size_t i = 0; i < count; i++) {
        uint64_t phys = 0;
        memcpy(&phys, entries + i * entry_size, entry_size);
        if (phys == 0) {
            continue;
        }

        const struct sdt_header *t = read(phys);
        if (t == NULL || !sig_is(t->signature, "APIC", 4)) {
            continue;
        }
        if (t->length < sizeof(struct madt) || !acpi_checksum_ok(t, t->length)) {
            continue;   /* it says it is the madt but does not add up */
        }

        parse_madt((const struct madt *)t, &info);
        info.found = true;
        break;
    }

    return info;
}

uint32_t acpi_gsi_for_irq(const struct acpi_info *info, uint8_t isa_irq)
{
    for (size_t i = 0; i < info->override_count; i++) {
        if (info->overrides[i].isa_irq == isa_irq) {
            return info->overrides[i].gsi;
        }
    }
    /* no override means the identity mapping everyone assumes */
    return isa_irq;
}

const struct acpi_ioapic *acpi_ioapic_for_gsi(const struct acpi_info *info,
                                              uint32_t gsi)
{
    const struct acpi_ioapic *best = NULL;
    for (size_t i = 0; i < info->ioapic_count; i++) {
        const struct acpi_ioapic *io = &info->ioapics[i];
        if (gsi >= io->gsi_base && (best == NULL || io->gsi_base > best->gsi_base)) {
            best = io;
        }
    }
    return best;
}

#ifndef VELVETOS_HOSTED

#include <velvet/pmm.h>
#include <velvet/kprintf.h>



/*
 * acpi tables live in memory the memmap calls "acpi reclaimable", which
 * the direct map covers, so reading one is just an offset away
 */
static void *phys_read(uint64_t phys)
{
    return pmm_phys_to_virt(phys);
}

struct acpi_info acpi_init(void)
{
    struct acpi_info info;
    memset(&info, 0, sizeof info);

    if (boot_handoff()->rsdp == 0) {
        kprintf("acpi       : the firmware has no acpi tables\n");
        return info;
    }

    uint64_t rsdp = (uint64_t)boot_handoff()->rsdp;
    /* it may arrive already in the direct map */
    if (rsdp >= pmm_hhdm_offset()) {
        rsdp -= pmm_hhdm_offset();
    }

    info = acpi_parse(rsdp, phys_read);
    if (!info.found) {
        kprintf("acpi       : no madt, staying on the 8259\n");
        return info;
    }

    kprintf("acpi       : lapic at %p, %zu io apic(s), %zu cpu(s)\n",
            (void *)info.lapic_address, info.ioapic_count, info.cpu_count);
    for (size_t i = 0; i < info.override_count; i++) {
        kprintf("             irq %u actually arrives on line %u\n",
                info.overrides[i].isa_irq, info.overrides[i].gsi);
    }
    return info;
}

#endif
