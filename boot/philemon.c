// SPDX-License-Identifier: GPL-2.0-only
/*
 * boot/philemon.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the 64-bit half of philemon.
 */

#include "philemon.h"



static void copy(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    for (uint64_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

static void fill(void *dst, uint8_t v, uint64_t n)
{
    uint8_t *d = dst;
    for (uint64_t i = 0; i < n; i++) {
        d[i] = v;
    }
}



struct elf64_header {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf64_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

#define PT_LOAD   1
#define ET_EXEC   2
#define EM_X86_64 0x3e

uint64_t ph_load_elf(const void *image, uint64_t size, uint64_t phys_at,
                     uint64_t *phys_base, uint64_t *virt_base,
                     const char **error)
{
    if (size < sizeof(struct elf64_header)) {
        *error = "the kernel is too small to be an elf at all";
        return 0;
    }

    const struct elf64_header *eh = image;
    if (eh->ident[0] != 0x7f || eh->ident[1] != 'E'
        || eh->ident[2] != 'L' || eh->ident[3] != 'F') {
        *error = "that is not an elf";
        return 0;
    }
    if (eh->ident[4] != 2) {
        *error = "a 32-bit elf, which this machine has outgrown";
        return 0;
    }
    if (eh->machine != EM_X86_64 || eh->type != ET_EXEC) {
        *error = "an elf, but not an x86-64 executable";
        return 0;
    }
    if (eh->phoff == 0 || eh->phnum == 0
        || eh->phentsize < sizeof(struct elf64_phdr)) {
        *error = "an elf with nothing to load";
        return 0;
    }
    if (eh->phoff + (uint64_t)eh->phnum * eh->phentsize > size) {
        *error = "the program headers run off the end of the file";
        return 0;
    }

    /* where does the kernel want to be? */
    uint64_t lowest_virt = ~0ull;
    uint64_t lowest_phys = ~0ull;
    bool any = false;

    for (uint16_t i = 0; i < eh->phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)((const uint8_t *)image + eh->phoff
                                        + (uint64_t)i * eh->phentsize);
        if (ph->type != PT_LOAD || ph->memsz == 0) {
            continue;
        }
        if (ph->vaddr < lowest_virt) {
            lowest_virt = ph->vaddr;
        }
        if (ph->paddr < lowest_phys) {
            lowest_phys = ph->paddr;
        }
        any = true;
    }
    if (!any) {
        *error = "no loadable segments";
        return 0;
    }

    /*
     * every segment moves by the same amount, so the gaps between them
     *, which the linker chose, and which the code depends on, come
     * through intact
     */
    uint64_t slide = phys_at - lowest_phys;

    for (uint16_t i = 0; i < eh->phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)((const uint8_t *)image + eh->phoff
                                        + (uint64_t)i * eh->phentsize);
        if (ph->type != PT_LOAD || ph->memsz == 0) {
            continue;
        }
        if (ph->filesz > ph->memsz) {
            *error = "a segment claiming more in the file than in memory";
            return 0;
        }
        if (ph->offset + ph->filesz > size) {
            *error = "a segment running off the end of the file";
            return 0;
        }

        uint8_t *dst = (uint8_t *)(ph->paddr + slide);
        copy(dst, (const uint8_t *)image + ph->offset, ph->filesz);

        /*
         * bss is in the file only as a promise about its size, and a
         * kernel that found rubbish there would go wrong slowly
         */
        fill(dst + ph->filesz, 0, ph->memsz - ph->filesz);
    }

    /*
     * FIXME: nothing here bounds the segments against the room the
     * memory map is about to declare as the kernel's. PH_KERNEL_ROOM is
     * sixteen megabytes and the kernel is about one, so this does not
     * bite on the machine today, but a kernel that grows past the room
     * is copied over memory ph_build_memmap has just offered as free,
     * and the page allocator will hand it out from under the kernel.
     * track the highest paddr + memsz and refuse an image that does not
     * fit between PH_KERNEL_PHYS and PH_KERNEL_PHYS + PH_KERNEL_ROOM.
     */
    *phys_base = lowest_phys + slide;
    *virt_base = lowest_virt;
    return eh->entry;
}



static uint64_t translate(uint32_t bios_type)
{
    switch (bios_type) {
    case E820_USABLE:       return PH_MEM_USABLE;
    case E820_ACPI_RECLAIM: return PH_MEM_ACPI_RECLAIMABLE;
    case E820_ACPI_NVS:     return PH_MEM_ACPI_NVS;
    case E820_BAD:          return PH_MEM_BAD;
    default:                return PH_MEM_RESERVED;
    }
}

uint64_t ph_build_memmap(const struct e820_entry *bios, uint64_t bios_count,
                         uint64_t ramdisk, uint64_t ramdisk_size,
                         struct ph_memmap_entry *out, uint64_t out_max)
{
    /*
     * what philemon has already spent, and must not offer to the kernel as
     * though it were free
     */
    struct { uint64_t base, length, type; } mine[3];
    int taken = 0;

    mine[taken].base = PH_KERNEL_PHYS;
    mine[taken].length = PH_KERNEL_ROOM;
    mine[taken].type = PH_MEM_KERNEL;
    taken++;

    if (ramdisk_size > 0) {
        mine[taken].base = ramdisk;
        mine[taken].length = (ramdisk_size + 0xfff) & ~0xfffull;
        mine[taken].type = PH_MEM_KERNEL;
        taken++;
    }

    /* everything philemon is standing in. */
    mine[taken].base = 0;
    mine[taken].length = PH_LOADER_ROOM;
    mine[taken].type = PH_MEM_LOADER;
    taken++;

    /*
     * in address order, because the carving below walks each region once
     * and only ever moves forward. out of order, a piece lying behind
     * where philemon had got to would be stepped over, and the memory it sits
     * in handed to the kernel as free
     */
    for (int i = 1; i < taken; i++) {
        for (int j = i; j > 0 && mine[j].base < mine[j - 1].base; j--) {
            uint64_t b = mine[j].base, l = mine[j].length, t = mine[j].type;
            mine[j].base = mine[j - 1].base;
            mine[j].length = mine[j - 1].length;
            mine[j].type = mine[j - 1].type;
            mine[j - 1].base = b;
            mine[j - 1].length = l;
            mine[j - 1].type = t;
        }
    }

    uint64_t n = 0;
    for (uint64_t i = 0; i < bios_count && n + 4 < out_max; i++) {
        if (bios[i].length == 0) {
            continue;
        }

        uint64_t base = bios[i].base;
        uint64_t end = base + bios[i].length;
        uint64_t type = translate(bios[i].type);

        if (type != PH_MEM_USABLE) {
            out[n].base = base;
            out[n].length = end - base;
            out[n].type = type;
            n++;
            continue;
        }

        /*
         * a usable region with something of philemon's in it comes apart into
         * the pieces either side, and the piece itself
         */
        for (int k = 0; k < taken && base < end; k++) {
            uint64_t mb = mine[k].base;
            uint64_t me = mb + mine[k].length;
            if (me <= base || mb >= end) {
                continue;
            }
            if (mb > base) {
                out[n].base = base;
                out[n].length = mb - base;
                out[n].type = PH_MEM_USABLE;
                n++;
            }
            uint64_t cut = (me < end) ? me : end;
            out[n].base = (mb > base) ? mb : base;
            out[n].length = cut - out[n].base;
            out[n].type = mine[k].type;
            n++;
            base = cut;
        }

        if (base < end) {
            out[n].base = base;
            out[n].length = end - base;
            out[n].type = PH_MEM_USABLE;
            n++;
        }
    }

    /*
     * the kernel allocates a page at a time, so a usable region that
     * does not begin and end on one is worth less than it looks
     */
    for (uint64_t i = 0; i < n; i++) {
        if (out[i].type != PH_MEM_USABLE) {
            continue;
        }
        uint64_t base = (out[i].base + 0xfff) & ~0xfffull;
        uint64_t end = (out[i].base + out[i].length) & ~0xfffull;
        out[i].base = base;
        out[i].length = (end > base) ? end - base : 0;
    }

    return n;
}



uint64_t ph_find_rsdp(const void *ebda, const void *bios_area)
{
    static const char want[8] = { 'R', 'S', 'D', ' ', 'P', 'T', 'R', ' ' };

    /*
     * on a sixteen-byte boundary in one of two places, and the start of
     * a twenty-byte structure that has to add up to zero
     */
    const uint8_t *places[2] = { ebda, bios_area };
    uint64_t sizes[2] = { 1024, 0x20000 };

    for (int p = 0; p < 2; p++) {
        const uint8_t *at = places[p];
        if (at == 0) {
            continue;
        }
        for (uint64_t off = 0; off + 20 <= sizes[p]; off += 16) {
            const uint8_t *candidate = at + off;
            bool match = true;
            for (int i = 0; i < 8; i++) {
                if (candidate[i] != (uint8_t)want[i]) {
                    match = false;
                    break;
                }
            }
            if (!match) {
                continue;
            }
            uint8_t sum = 0;
            for (int i = 0; i < 20; i++) {
                sum = (uint8_t)(sum + candidate[i]);
            }
            if (sum == 0) {
                return (uint64_t)candidate;
            }
        }
    }
    return 0;
}



#ifndef VELVETOS_HOSTED

static void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" :: "a"(value), "Nd"(port));
}

static uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

#define COM1 0x3f8

static void say_char(char c)
{
    for (int spin = 0; spin < 100000; spin++) {
        if (inb(COM1 + 5) & 0x20) {
            break;
        }
    }
    outb(COM1, (uint8_t)c);
}

static void say(const char *s)
{
    while (*s != '\0') {
        if (*s == '\n') {
            say_char('\r');
        }
        say_char(*s++);
    }
}

static void say_hex(uint64_t v)
{
    static const char digits[] = "0123456789abcdef";
    say("0x");
    bool started = false;
    for (int shift = 60; shift >= 0; shift -= 4) {
        char c = digits[(v >> shift) & 0xf];
        if (c != '0' || started || shift == 0) {
            say_char(c);
            started = true;
        }
    }
}

static void die(const char *why)
{
    say("\n[philemon] cannot go on: ");
    say(why);
    say("\n[philemon] halting. the machine is fine; philemon is not.\n");
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}



/*
 * gcc is right that reading and writing fixed numeric addresses is
 * usually a bug: it cannot see an object there, so it cannot know the
 * access is in bounds. down here there are no objects, there is a
 * machine, and agreed addresses in it, which is the whole of how the
 * two halves of this loader speak to each other
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"

__attribute__((section(".text.entry"), used, noreturn))
void philemon_main(void)
{
    say("[philemon] long mode. the 64-bit half is running.\n");

    const struct ph_early *early = (const struct ph_early *)PH_HANDOFF_ADDR;
    if (early->magic != PHILEMON_MAGIC) {
        die("the 16-bit half left nothing behind it");
    }

    uint64_t phys_base = 0, virt_base = 0;
    const char *error = "unknown";
    uint64_t entry = ph_load_elf((const void *)early->kernel_image,
                                 early->kernel_size, PH_KERNEL_PHYS,
                                 &phys_base, &virt_base, &error);
    if (entry == 0) {
        die(error);
    }

    say("[philemon] kernel at ");
    say_hex(phys_base);
    say(" for ");
    say_hex(virt_base);
    say(", entry ");
    say_hex(entry);
    say("\n");

    /*
     * the acpi tables, where the firmware leaves them: behind a pointer
     * at 0x40e, and failing that in the read-only region at the top of
     * the first megabyte
     */
    uint64_t ebda = (uint64_t)(*(volatile uint16_t *)0x40e) << 4;

    struct ph_handoff *out = (struct ph_handoff *)PH_HANDOFF_ADDR;
    struct ph_memmap_entry *map = (struct ph_memmap_entry *)PH_MEMMAP_ADDR;

    /*
     * read everything out of the early struct before writing over it,
     * they share an address, because low memory is scarce and the two
     * are never both needed
     */
    struct ph_early saved = *early;

    uint64_t count = ph_build_memmap((const struct e820_entry *)saved.e820,
                                     saved.e820_count, saved.ramdisk,
                                     saved.ramdisk_size, map,
                                     (PH_BOUNCE_ADDR - PH_MEMMAP_ADDR)
                                         / sizeof *map);

    for (uint64_t i = 0; i < sizeof *out; i++) {
        ((uint8_t *)out)[i] = 0;
    }

    out->magic = PHILEMON_MAGIC;
    out->revision = PHILEMON_REVISION;
    out->hhdm = PHILEMON_HHDM;
    out->kernel_phys = phys_base;
    out->kernel_virt = virt_base;
    out->memmap = PH_MEMMAP_ADDR + PHILEMON_HHDM;
    out->memmap_count = count;
    out->ramdisk = saved.ramdisk_size ? saved.ramdisk + PHILEMON_HHDM : 0;
    out->ramdisk_size = saved.ramdisk_size;
    /*
     * XXX: every other pointer in this struct is written already offset
     * by the direct map, and this one is not. ph_find_rsdp returns an
     * address in the loader's identity map, which is to say a physical
     * one, and the kernel reads that ambiguity back off the wire and
     * guesses at it, subtracting the offset only when the value is above
     * it. that works because the direct map is far above any address a
     * firmware uses, and nothing else. add PHILEMON_HHDM here the way
     * memmap, ramdisk and fb.address above are written, and delete the
     * guess.
     */
    out->rsdp = ph_find_rsdp(ebda ? (const void *)ebda : 0,
                             (const void *)0xe0000);

    if (saved.fb_width != 0 && saved.fb_address != 0) {
        out->fb.address = saved.fb_address + PHILEMON_HHDM;
        out->fb.pitch = saved.fb_pitch;
        out->fb.width = saved.fb_width;
        out->fb.height = saved.fb_height;
        out->fb.bpp = saved.fb_bpp;
        out->fb.red_shift = saved.fb_red_shift;
        out->fb.red_size = saved.fb_red_size;
        out->fb.green_shift = saved.fb_green_shift;
        out->fb.green_size = saved.fb_green_size;
        out->fb.blue_shift = saved.fb_blue_shift;
        out->fb.blue_size = saved.fb_blue_size;
    }

    say("[philemon] ");
    say_hex(count);
    say(" regions of memory");
    if (out->rsdp == 0) {
        say(", no acpi tables");
    }
    say("\n[philemon] thou art I... and I am thou.\n\n");

    /* the kernel is entered the way any function is called: one pointer, in rdi. */
    __asm__ volatile (
        "movq %0, %%rsp\n"
        "xorq %%rbp, %%rbp\n"
        "pushq $0\n"
        "movq %1, %%rdi\n"
        "jmp *%2\n"
        :: "r"(PHILEMON_HHDM + 0x00700000ull),
           "r"((uint64_t)out + PHILEMON_HHDM),
           "r"(entry)
        : "memory");

    __builtin_unreachable();
}

#pragma GCC diagnostic pop

#endif  /* VELVETOS_HOSTED */
