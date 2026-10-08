// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_philemon.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for the 64-bit half of philemon.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../boot/philemon.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)


struct eh {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct ph {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

#define KERNEL_VIRT 0xffffffff80000000ull
#define KERNEL_LINK_PHYS 0x100000ull

/*
 * two segments, the way a real kernel has them: text with contents, and
 * bss which exists only as a promise about its size
 */
static uint8_t *make_kernel(uint64_t *out_size, uint64_t text_bytes,
                            uint64_t bss_bytes, const void *extra,
                            uint64_t extra_size)
{
    uint64_t phoff = sizeof(struct eh);
    uint64_t text_off = 0x1000;
    uint64_t total = text_off + text_bytes + extra_size;

    uint8_t *img = calloc(1, total);
    struct eh *e = (struct eh *)img;

    e->ident[0] = 0x7f; e->ident[1] = 'E'; e->ident[2] = 'L'; e->ident[3] = 'F';
    e->ident[4] = 2;            /* 64-bit */
    e->ident[5] = 1;            /* little endian */
    e->type = 2;                /* executable */
    e->machine = 0x3e;          /* x86-64 */
    e->version = 1;
    e->entry = KERNEL_VIRT + 0x40;
    e->phoff = phoff;
    e->ehsize = sizeof *e;
    e->phentsize = sizeof(struct ph);
    e->phnum = 2;

    struct ph *p = (struct ph *)(img + phoff);
    p[0].type = 1;              /* PT_LOAD */
    p[0].flags = 5;             /* r-x */
    p[0].offset = text_off;
    p[0].vaddr = KERNEL_VIRT;
    p[0].paddr = KERNEL_LINK_PHYS;
    p[0].filesz = text_bytes + extra_size;
    p[0].memsz = text_bytes + extra_size;
    p[0].align = 0x1000;

    p[1].type = 1;
    p[1].flags = 6;             /* rw- */
    p[1].offset = total;        /* nothing in the file at all */
    p[1].vaddr = KERNEL_VIRT + 0x200000;
    p[1].paddr = KERNEL_LINK_PHYS + 0x200000;
    p[1].filesz = 0;
    p[1].memsz = bss_bytes;
    p[1].align = 0x1000;

    /*
     * something recognisable in the text, so a segment landing in the
     * wrong place is visible rather than merely wrong
     */
    for (uint64_t i = 0; i < text_bytes; i++) {
        img[text_off + i] = (uint8_t)(i * 7 + 1);
    }
    if (extra != NULL) {
        memcpy(img + text_off + text_bytes, extra, extra_size);
    }

    *out_size = total;
    return img;
}



#define M0 0xc7b1dd30df4c8b88ull
#define M1 0x0a82e883a194f07bull

struct request {
    uint64_t id[4];
    uint64_t revision;
    uint64_t response;
};

int main(void)
{


    uint64_t size;
    uint8_t *img = make_kernel(&size, 0x800, 0x4000, NULL, 0);

    /* somewhere for it to land, standing in for physical memory */
    uint64_t arena_bytes = 0x400000;
    uint8_t *arena = aligned_alloc(0x1000, arena_bytes);
    memset(arena, 0xcc, arena_bytes);

    uint64_t phys_base = 0, virt_base = 0;
    const char *error = NULL;
    uint64_t entry = ph_load_elf(img, size, (uint64_t)arena,
                                   &phys_base, &virt_base, &error);

    CHECK(entry == KERNEL_VIRT + 0x40, "the entry point comes back unchanged");
    CHECK(phys_base == (uint64_t)arena, "the kernel lands where it was put");
    CHECK(virt_base == KERNEL_VIRT, "and reports the address it was linked for");

    int intact = 1;
    for (uint64_t i = 0; i < 0x800; i++) {
        if (arena[i] != (uint8_t)(i * 7 + 1)) intact = 0;
    }
    CHECK(intact, "the text segment arrived byte for byte");

    /*
     * the second segment is 2 MiB further on, and that distance is the
     * linker's decision, a loader that packed them would break every
     * absolute address in the kernel
     */
    intact = 1;
    for (uint64_t i = 0; i < 0x4000; i++) {
        if (arena[0x200000 + i] != 0) intact = 0;
    }
    CHECK(intact, "bss is zeroed, and at the distance the linker chose");
    CHECK(arena[0x200000 + 0x4000] == 0xcc,
          "and not one byte beyond what it asked for");



    uint64_t junk_size;
    uint8_t *junk = make_kernel(&junk_size, 0x100, 0, NULL, 0);

    junk[0] = 'X';
    CHECK(ph_load_elf(junk, junk_size, (uint64_t)arena, &phys_base,
                        &virt_base, &error) == 0, "a non-elf is refused");
    junk[0] = 0x7f;

    ((struct eh *)junk)->ident[4] = 1;
    CHECK(ph_load_elf(junk, junk_size, (uint64_t)arena, &phys_base,
                        &virt_base, &error) == 0, "a 32-bit elf is refused");
    ((struct eh *)junk)->ident[4] = 2;

    ((struct eh *)junk)->machine = 0x28;    /* arm */
    CHECK(ph_load_elf(junk, junk_size, (uint64_t)arena, &phys_base,
                        &virt_base, &error) == 0,
          "an elf for another machine is refused");
    ((struct eh *)junk)->machine = 0x3e;

    ((struct eh *)junk)->phnum = 0;
    CHECK(ph_load_elf(junk, junk_size, (uint64_t)arena, &phys_base,
                        &virt_base, &error) == 0,
          "an elf with nothing to load is refused");
    ((struct eh *)junk)->phnum = 2;

    /*
     * a segment reaching past the end of the file must not be believed:
     * that is a read off the end of whatever the test loaded it into
     */
    struct ph *jp = (struct ph *)(junk + sizeof(struct eh));
    jp[0].filesz = junk_size * 4;
    jp[0].memsz = junk_size * 4;
    CHECK(ph_load_elf(junk, junk_size, (uint64_t)arena, &phys_base,
                        &virt_base, &error) == 0,
          "a segment running off the end of the file is refused");

    CHECK(ph_load_elf(junk, 8, (uint64_t)arena, &phys_base, &virt_base,
                        &error) == 0, "and so is a file too small to be one");
    free(junk);
    free(img);

    /*
     * a machine describes itself, and philemon has to hand that on with
     * everything it has already spent carved out of it. the regions do
     * not overlap each other, because a bios reporting overlapping ones
     * would be lying about its own memory and nothing here could tell
     */

    static struct e820_entry firmware_map[] = {
        { 0x00000000,   0x0009fc00, E820_USABLE,       0 },
        { 0x0009fc00,   0x00000400, E820_RESERVED,     0 },
        { 0x000f0000,   0x00010000, E820_RESERVED,     0 },
        { 0x00100000,   0x7fef0000, E820_USABLE,       0 },
        { 0x7fff0000,   0x00010000, E820_ACPI_RECLAIM, 0 },
    };

    static struct ph_memmap_entry map[256];
    uint64_t count = ph_build_memmap(firmware_map, 5, PH_RAMDISK_ADDR, 4096,
                                     map, 256);
    CHECK(count > 5, "the map comes out longer than it went in");

    uint64_t usable = 0;
    int saw_kernel = 0, saw_ramdisk = 0, saw_loader = 0;
    int overlaps = 0, unaligned = 0;

    for (uint64_t i = 0; i < count; i++) {
        uint64_t base = map[i].base, len = map[i].length, type = map[i].type;

        if (type == PH_MEM_USABLE) {
            usable += len;
            if ((base & 0xfff) || (len & 0xfff)) unaligned++;
        }
        if (type == PH_MEM_KERNEL && base == PH_KERNEL_PHYS) saw_kernel = 1;
        if (type == PH_MEM_KERNEL && base == PH_RAMDISK_ADDR) saw_ramdisk = 1;
        if (type == PH_MEM_LOADER) saw_loader = 1;

        /*
         * nothing may be described twice, or the kernel would hand the
         * same page to two different things
         */
        for (uint64_t j = i + 1; j < count; j++) {
            uint64_t b2 = map[j].base, l2 = map[j].length;
            if (len && l2 && base < b2 + l2 && b2 < base + len) overlaps++;
        }
    }

    CHECK(saw_kernel, "the memory the kernel was loaded into is marked as its");
    CHECK(saw_ramdisk, "and so is the ramdisk, which the kernel still needs");
    CHECK(saw_loader, "the loader's own memory is offered back");
    CHECK(overlaps == 0, "no region is described twice");
    CHECK(unaligned == 0, "every usable region is whole pages");
    CHECK(usable < 0x7fef0000ull + 0x9fc00,
          "usable memory is less than the machine has, since some was spent");
    CHECK(usable > 0x70000000ull, "but not very much less");

    /* the ramdisk is behind the kernel in memory but ahead of it in the list of things already spent. */
    int ramdisk_is_free = 0;
    for (uint64_t i = 0; i < count; i++) {
        if (map[i].type != PH_MEM_USABLE) continue;
        if (PH_RAMDISK_ADDR >= map[i].base
            && PH_RAMDISK_ADDR < map[i].base + map[i].length) {
            ramdisk_is_free = 1;
        }
    }
    CHECK(!ramdisk_is_free, "and the ramdisk is not inside a usable region");

    /* a machine with no ramdisk at all still gets a sensible map */
    count = ph_build_memmap(firmware_map, 5, 0, 0, map, 256);
    CHECK(count > 5, "a machine with no ramdisk still gets a map");



    static uint8_t bios[0x20000];
    memset(bios, 0, sizeof bios);
    CHECK(ph_find_rsdp(NULL, bios) == 0, "no rsdp where there is none");

    uint8_t *rsdp = bios + 0x5320;
    memcpy(rsdp, "RSD PTR ", 8);
    for (int i = 8; i < 20; i++) rsdp[i] = (uint8_t)i;
    uint8_t sum = 0;
    for (int i = 0; i < 20; i++) sum = (uint8_t)(sum + rsdp[i]);
    /*
     * byte 19, not byte 7: seven is the trailing space of the signature,
     * and correcting the checksum there would break the very thing that
     * makes it findable
     */
    rsdp[19] = (uint8_t)(rsdp[19] - sum);

    CHECK(ph_find_rsdp(NULL, bios) == (uint64_t)rsdp,
          "an rsdp on a sixteen-byte boundary is found");

    rsdp[9]++;      /* break the checksum without touching the signature */
    CHECK(ph_find_rsdp(NULL, bios) == 0,
          "one that does not add up is not believed");

    if (failures == 0) printf("all good\n");
    return failures;
}
