// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_gdt.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the tss descriptor, decoded back apart.
 */

#include <stdio.h>
#include <stdint.h>

#include "arch/x86_64/gdt.c"

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

static void check_roundtrip(uint64_t base, uint32_t limit, const char *what)
{
    gdt_set_tss(0, base, limit);

    uint64_t lo = gdt[GDT_TSS / 8];
    uint64_t hi = gdt[GDT_TSS / 8 + 1];

    uint64_t got_base = ((lo >> 16) & 0xffffff)
                      | (((lo >> 56) & 0xff) << 24)
                      | ((hi & 0xffffffffull) << 32);
    uint32_t got_limit = (lo & 0xffff) | (((lo >> 48) & 0xf) << 16);

    if (got_base != base) {
        printf("FAIL %s: base came back %#lx, put in %#lx\n", what, got_base, base);
        failures++;
    }
    if (got_limit != limit) {
        printf("FAIL %s: limit came back %#x, put in %#x\n", what, got_limit, limit);
        failures++;
    }
}

int main(void)
{
    /*
     * the descriptor the kernel actually builds: a tss up in the higher
     * half, 103 bytes long
     */
    check_roundtrip(0xffffffff8000c160ull, 103, "a realistic kernel tss");

    /* and the awkward cases, all of which straddle a field boundary */
    check_roundtrip(0x0000000000000000ull, 0,        "all zeroes");
    check_roundtrip(0x0000000000ffffffull, 0xffff,   "base right below the 24-bit split");
    check_roundtrip(0x0000000001000000ull, 0x10000,  "base right above it, limit above 16 bits");
    check_roundtrip(0x00000000ffffffffull, 0xfffff,  "everything at 32 bits");
    check_roundtrip(0x0000000100000000ull, 0,        "base needing the high qword");
    check_roundtrip(0xffffffffffffffffull, 0xfffff,  "every bit set");

    /* now the type bits, which decide whether ltr works at all */
    gdt_set_tss(0, 0xffffffff8000c160ull, 103);
    uint64_t lo = gdt[GDT_TSS / 8];
    CHECK(((lo >> 40) & 0xf) == 9, "type is 9 (available 64-bit tss)");
    CHECK(((lo >> 44) & 1) == 0,   "S is 0, marking it a system segment");
    CHECK(((lo >> 45) & 3) == 0,   "DPL is 0, ring 0 only");
    CHECK(((lo >> 47) & 1) == 1,   "present");

    /* the user descriptors, whose layout sysret dictates rather than the test */
    CHECK(((gdt[GDT_USER_CODE / 8] >> 45) & 3) == 3, "user code is dpl 3");
    CHECK(((gdt[GDT_USER_DATA / 8] >> 45) & 3) == 3, "user data is dpl 3");
    CHECK((gdt[GDT_USER_CODE / 8] >> 53) & 1, "user code is a long mode segment");
    CHECK(GDT_USER_CODE == GDT_USER_DATA + 8,
          "sysret computes CS = base+16 and SS = base+8, so code must sit "
          "eight bytes above data");
    CHECK(GDT_USER_DATA == GDT_KERNEL_DATA + 8,
          "and STAR[63:48] = kernel data makes those two land right");

    /* the descriptor must not have trampled its neighbours */
    CHECK(gdt[0] == 0, "null descriptor still null");
    CHECK(gdt[GDT_KERNEL_CODE / 8] == 0x00af9a000000ffff, "kernel code intact");
    CHECK(gdt[GDT_KERNEL_DATA / 8] == 0x00af92000000ffff, "kernel data intact");

    /* the tss takes two slots and the table has to be big enough */
    CHECK(sizeof(gdt) / 8 >= GDT_TSS / 8 + 2, "the gdt has room for both halves");

    /*
     * one tss per core, and each core loads a different selector, which
     * is what lets a core name itself by asking the cpu which one it
     * loaded. so the table has to have room for all of them, and the
     * arithmetic in both directions has to agree
     */
    CHECK(sizeof(gdt) / 8 >= (GDT_TSS_FOR(GDT_MAX_TSS - 1) / 8) + 2,
          "and room for every core's, not just the first");
    for (unsigned cpu = 0; cpu < GDT_MAX_TSS; cpu++) {
        uint16_t sel = (uint16_t)GDT_TSS_FOR(cpu);
        CHECK((unsigned)((sel - GDT_TSS) / GDT_TSS_STRIDE) == cpu,
              "a selector maps back to the core that loaded it");
    }

    /* and each one lands in its own slot rather than on its neighbour */
    for (unsigned cpu = 0; cpu < GDT_MAX_TSS; cpu++) {
        gdt_set_tss(cpu, 0x1000 + cpu * 0x100, 103);
    }
    int distinct = 1;
    for (unsigned cpu = 0; cpu < GDT_MAX_TSS; cpu++) {
        uint64_t e = gdt[GDT_TSS_FOR(cpu) / 8];
        uint64_t got = (e >> 16) & 0xffffff;
        if (got != 0x1000 + cpu * 0x100) distinct = 0;
    }
    CHECK(distinct, "every core's descriptor holds its own base");

    if (!failures) printf("all good\n");
    return failures;
}
