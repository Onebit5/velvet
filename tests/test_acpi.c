// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_acpi.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * walking from the rsdp to the madt.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "arch/x86_64/acpi.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

/* a pretend physical memory, so tables can have addresses */
static uint8_t mem[16384];
static void *read_phys(uint64_t p)
{
    return (p < sizeof mem) ? mem + p : NULL;
}

/* addresses inside it that tests build tables at */
#define RSDP_AT  0x100
#define RSDT_AT  0x200
#define MADT_AT  0x400

static void checksum(void *p, size_t len)
{
    uint8_t *b = p, sum = 0;
    b[9] = 0;                       /* sdt checksum byte */
    for (size_t i = 0; i < len; i++) sum = (uint8_t)(sum + b[i]);
    b[9] = (uint8_t)(0 - sum);
}

static void build_rsdp(uint8_t revision, uint32_t rsdt, uint64_t xsdt)
{
    uint8_t *r = mem + RSDP_AT;
    memset(r, 0, 36);
    memcpy(r, "RSD PTR ", 8);
    r[15] = revision;
    memcpy(r + 16, &rsdt, 4);
    if (revision >= 2) {
        uint32_t len = 36;
        memcpy(r + 20, &len, 4);
        memcpy(r + 24, &xsdt, 8);
    }
    uint8_t sum = 0;                /* the 1.0 checksum covers 20 bytes */
    r[8] = 0;
    for (int i = 0; i < 20; i++) sum = (uint8_t)(sum + r[i]);
    r[8] = (uint8_t)(0 - sum);
}

/* an madt with one lapic, one io apic, and one override */
static size_t build_madt(uint32_t lapic_addr)
{
    uint8_t *m = mem + MADT_AT;
    memset(m, 0, 256);
    memcpy(m, "APIC", 4);
    m[8] = 1;                                       /* revision */
    memcpy(m + 36, &lapic_addr, 4);

    uint8_t *e = m + 44;
    /* a usable cpu, apic id 0 */
    e[0] = 0; e[1] = 8; e[2] = 0; e[3] = 0; e[4] = 1;   e += 8;
    /* an io apic at 0xfec00000, gsi base 0 */
    { uint32_t a = 0xfec00000, g = 0;
      e[0] = 1; e[1] = 12; e[2] = 2;
      memcpy(e + 4, &a, 4); memcpy(e + 8, &g, 4);   e += 12; }
    /* irq 0 actually arrives on line 2, which is the usual lie */
    { uint32_t g = 2; uint16_t fl = 0;
      e[0] = 2; e[1] = 10; e[3] = 0;
      memcpy(e + 4, &g, 4); memcpy(e + 8, &fl, 2);  e += 10; }

    uint32_t len = (uint32_t)(e - m);
    memcpy(m + 4, &len, 4);
    checksum(m, len);
    return len;
}

static void build_rsdt(uint64_t madt_at, bool xsdt)
{
    uint8_t *r = mem + RSDT_AT;
    memset(r, 0, 128);
    memcpy(r, xsdt ? "XSDT" : "RSDT", 4);
    r[8] = 1;
    size_t esz = xsdt ? 8 : 4;
    memcpy(r + 36, &madt_at, esz);
    uint32_t len = (uint32_t)(36 + esz);
    memcpy(r + 4, &len, 4);
    checksum(r, len);
}

int main(void)
{
    struct acpi_info info;


    memset(mem, 0, sizeof mem);
    build_madt(0xfee00000);
    build_rsdt(MADT_AT, false);
    build_rsdp(0, RSDT_AT, 0);

    info = acpi_parse(RSDP_AT, read_phys);
    CHECK(info.found, "an acpi 1.0 chain is walked to the madt");
    CHECK(info.lapic_address == 0xfee00000, "and the lapic address read out");
    CHECK(info.ioapic_count == 1, "one io apic found");
    CHECK(info.ioapics[0].address == 0xfec00000, "at the right address");
    CHECK(info.cpu_count == 1, "one usable cpu counted");
    CHECK(info.lapic_ids[0] == 0, "and its apic id noted");

    /*
     * the override is the part that matters most: irq 0 is wired to
     * line 2 on nearly every real machine, and a kernel that assumes
     * otherwise gets no timer at all
     */
    CHECK(info.override_count == 1, "the override was noticed");
    CHECK(acpi_gsi_for_irq(&info, 0) == 2,
          "irq 0 resolves to the line it actually arrives on");
    CHECK(acpi_gsi_for_irq(&info, 1) == 1,
          "and an irq with no override keeps its number");
    CHECK(acpi_ioapic_for_gsi(&info, 2) == &info.ioapics[0],
          "and the io apic handling it is found");
    CHECK(acpi_ioapic_for_gsi(&info, 99) == &info.ioapics[0],
          "a gsi above the last base still belongs to the last chip");

    memset(mem, 0, sizeof mem);
    build_madt(0xfee00000);
    build_rsdt(MADT_AT, true);
    build_rsdp(2, 0, RSDT_AT);
    info = acpi_parse(RSDP_AT, read_phys);
    CHECK(info.found, "an acpi 2.0 chain works through the xsdt");

    /*
     * the apic id is the only way to address a core that is not running
     * yet, so counting them is not enough, they have to be named. and
     * the ids are not consecutive on real hardware, which is exactly the
     * sort of thing a kernel that assumes gets wrong on somebody else's
     * machine and never on its own
     */
    {
        uint8_t *m = mem + MADT_AT;
        memset(m, 0, 256);
        memcpy(m, "APIC", 4);
        m[8] = 1;
        uint32_t addr = 0xfee00000;
        memcpy(m + 36, &addr, 4);

        uint8_t *e = m + 44;
        /*
         * four processors with awkward ids, and a fifth the firmware
         * says is not usable, which must not be woken and must not be
         * counted, or every core after it is off by one
         */
        const uint8_t ids[4] = { 0, 2, 4, 6 };
        for (int i = 0; i < 4; i++) {
            e[0] = 0; e[1] = 8; e[2] = (uint8_t)i; e[3] = ids[i]; e[4] = 1;
            e += 8;
        }
        e[0] = 0; e[1] = 8; e[2] = 9; e[3] = 9; e[4] = 0;   /* disabled */
        e += 8;

        uint32_t len = (uint32_t)(e - m);
        memcpy(m + 4, &len, 4);
        checksum(m, len);
        build_rsdt(MADT_AT, false);
        build_rsdp(0, RSDT_AT, 0);

        info = acpi_parse(RSDP_AT, read_phys);
        CHECK(info.found, "an madt with several processors parses");
        CHECK(info.cpu_count == 4, "the usable ones are counted");
        CHECK(info.lapic_ids[0] == 0 && info.lapic_ids[1] == 2
              && info.lapic_ids[2] == 4 && info.lapic_ids[3] == 6,
              "and named, in order, with the ids the firmware gave");
    }

    memset(mem, 0, sizeof mem);
    {
        size_t len = build_madt(0xfee00000);
        uint8_t *e = mem + MADT_AT + len;
        uint64_t addr = 0x9fee00000ull;
        e[0] = 5; e[1] = 12;
        memcpy(e + 4, &addr, 8);
        uint32_t nlen = (uint32_t)(len + 12);
        memcpy(mem + MADT_AT + 4, &nlen, 4);
        checksum(mem + MADT_AT, nlen);
    }
    build_rsdt(MADT_AT, false);
    build_rsdp(0, RSDT_AT, 0);
    info = acpi_parse(RSDP_AT, read_phys);
    CHECK(info.lapic_address == 0x9fee00000ull,
          "an address override replaces the 32-bit field");


    memset(mem, 0, sizeof mem);
    build_madt(0xfee00000); build_rsdt(MADT_AT, false); build_rsdp(0, RSDT_AT, 0);
    mem[RSDP_AT + 8] ^= 0xff;                   /* break the rsdp checksum */
    CHECK(!acpi_parse(RSDP_AT, read_phys).found,
          "an rsdp that does not add up is refused");

    build_rsdp(0, RSDT_AT, 0);
    mem[RSDT_AT + 9] ^= 0xff;                   /* break the rsdt checksum */
    CHECK(!acpi_parse(RSDP_AT, read_phys).found, "and so is a bad rsdt");

    memset(mem, 0, sizeof mem);
    build_madt(0xfee00000); build_rsdt(MADT_AT, false); build_rsdp(0, RSDT_AT, 0);
    mem[MADT_AT + 9] ^= 0xff;                   /* break the madt checksum */
    CHECK(!acpi_parse(RSDP_AT, read_phys).found,
          "a madt that does not add up is not acted on, routing "
          "interrupts at an address nobody chose is worse than not routing");

    memset(mem, 0, sizeof mem);
    build_madt(0xfee00000); build_rsdt(MADT_AT, false); build_rsdp(0, RSDT_AT, 0);
    memcpy(mem + RSDP_AT, "NOT PTR ", 8);
    CHECK(!acpi_parse(RSDP_AT, read_phys).found, "wrong magic is refused");

    /*
     * an entry claiming zero length would spin here forever, and a
     * firmware bug is not a reason to hang before the console exists
     */
    memset(mem, 0, sizeof mem);
    {
        size_t len = build_madt(0xfee00000);
        mem[MADT_AT + 45] = 0;                  /* first entry's length */
        checksum(mem + MADT_AT, len);
    }
    build_rsdt(MADT_AT, false); build_rsdp(0, RSDT_AT, 0);
    info = acpi_parse(RSDP_AT, read_phys);
    CHECK(true, "a zero-length entry stops the walk instead of looping");

    /* nothing at all */
    CHECK(!acpi_parse(0, read_phys).found, "a null rsdp finds nothing");
    CHECK(!acpi_parse(RSDP_AT, NULL).found, "and so does no way to read");


    {
        uint8_t ok[4] = { 1, 2, 3, 250 };       /* sums to 256, i.e. 0 */
        uint8_t bad[4] = { 1, 2, 3, 4 };
        CHECK(acpi_checksum_ok(ok, 4), "bytes summing to zero pass");
        CHECK(!acpi_checksum_ok(bad, 4), "and anything else does not");
    }

    if (!failures) printf("all good\n");
    return failures;
}
