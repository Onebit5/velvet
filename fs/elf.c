// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/elf.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * loading an elf64 executable.
 */

#include "fs/elf.h"
#include "lib/string.h"

/* what a segment turns into, in page table terms. */
#include "mm/vmm.h"

/* elf_load gets this from pmm.h, which needs a real machine. */
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define ET_EXEC 2
#define PT_LOAD 1

#define PF_X 1
#define PF_W 2
#define PF_R 4

struct elf64_header {
    uint8_t  ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} __attribute__((packed));

bool elf_is_loadable(const void *image, uint64_t size, const char **why)
{
    const char *ignored;
    if (why == NULL) {
        why = &ignored;
    }

    if (image == NULL || size < sizeof(struct elf64_header)) {
        *why = "too small to be an elf at all";
        return false;
    }

    const struct elf64_header *h = image;
    if (h->ident[0] != 0x7f || h->ident[1] != 'E'
        || h->ident[2] != 'L' || h->ident[3] != 'F') {
        *why = "no elf magic";
        return false;
    }
    if (h->ident[4] != 2) {
        *why = "not 64-bit";
        return false;
    }
    if (h->ident[5] != 1) {
        *why = "not little endian";
        return false;
    }
    if (h->machine != 0x3e) {
        *why = "not x86-64";
        return false;
    }
    if (h->type != ET_EXEC) {
        *why = "not a static executable (the kernel cannot relocate)";
        return false;
    }
    if (h->phnum == 0) {
        *why = "no program headers, so nothing to load";
        return false;
    }
    if (h->phentsize != sizeof(struct elf64_phdr)) {
        *why = "program headers are the wrong size";
        return false;
    }
    if (h->phoff + (uint64_t)h->phnum * h->phentsize > size) {
        *why = "program headers run past the end of the file";
        return false;
    }
    if (h->entry == 0) {
        *why = "no entry point";
        return false;
    }
    return true;
}

bool elf_describe(const void *image, uint64_t size,
                  struct elf_segment *out, size_t max, size_t *count,
                  uint64_t *entry, uint64_t *brk, const char **why)
{
    if (!elf_is_loadable(image, size, why)) {
        return false;
    }

    const struct elf64_header *h = image;
    const struct elf64_phdr *ph =
        (const struct elf64_phdr *)((const uint8_t *)image + h->phoff);

    *count = 0;
    *entry = h->entry;
    *brk = 0;

    for (uint16_t i = 0; i < h->phnum; i++) {
        if (ph[i].type != PT_LOAD || ph[i].memsz == 0) {
            continue;
        }
        if (ph[i].offset + ph[i].filesz > size) {
            *why = "a segment reaches past the end of the file";
            return false;
        }
        /*
         * FIXME: this refuses a segment in the higher half and accepts
         * one at zero. a program with a writable page at address 0 can
         * clear the gs base with a plain `mov gs, ax` from ring 3, and
         * then the syscall entry reads its kernel stack pointer out of
         * that page, in ring 0. the FIXME in arch/x86_64/syscall.asm is
         * the other half of this. refuse anything below the point where
         * the kernel stops trusting an address, and pick that point on
         * purpose rather than leaving it at zero.
         */
        /* the higher half is the kernel's. */
        if (ph[i].vaddr >= 0xffff800000000000ull) {
            *why = "a segment wants to live in kernel space";
            return false;
        }
        if (*count == max) {
            *why = "more segments than the kernel can describe";
            return false;
        }

        uint64_t start = ph[i].vaddr & ~(PAGE_SIZE - 1);
        uint64_t end = (ph[i].vaddr + ph[i].memsz + PAGE_SIZE - 1)
                     & ~(PAGE_SIZE - 1);

        /*
         * two segments sharing a page cannot both be described: one
         * record would have to fetch the other's bytes, and they may
         * not even agree about whether the page is writable. the
         * linker puts each on its own page, so this is a check rather
         * than a case
         */
        for (size_t j = 0; j < *count; j++) {
            if (start < out[j].end && end > out[j].vaddr) {
                *why = "two segments share a page";
                return false;
            }
        }

        uint64_t flags = PTE_USER;
        if (ph[i].flags & PF_W) {
            flags |= PTE_WRITE | vmm_nx();
        } else if (!(ph[i].flags & PF_X)) {
            flags |= vmm_nx();
        }

        out[*count].vaddr = start;
        out[*count].end = end;
        out[*count].file_end = ph[i].vaddr + ph[i].filesz;
        /*
         * the offset of `start` rather than of vaddr, since the record
         * describes whole pages and the segment may begin partway into
         * one, though with a page-aligned linker script it never
         * does, and the arithmetic is the same either way
         */
        out[*count].offset = ph[i].offset - (ph[i].vaddr - start);
        out[*count].flags = flags;
        (*count)++;

        if (end > *brk) {
            *brk = end;
        }
    }

    if (*count == 0) {
        *why = "nothing to load";
        return false;
    }
    return true;
}

#ifndef VELVETOS_HOSTED

#include "mm/pmm.h"

struct elf_load_result elf_load(const void *image, uint64_t size,
                                uint64_t pml4)
{
    struct elf_load_result r = { 0, 0, false, NULL };

    if (!elf_is_loadable(image, size, &r.error)) {
        return r;
    }

    const struct elf64_header *h = image;
    const struct elf64_phdr *ph =
        (const struct elf64_phdr *)((const uint8_t *)image + h->phoff);

    for (uint16_t i = 0; i < h->phnum; i++) {
        if (ph[i].type != PT_LOAD || ph[i].memsz == 0) {
            continue;
        }
        if (ph[i].offset + ph[i].filesz > size) {
            r.error = "a segment reaches past the end of the file";
            return r;
        }
        /* the higher half is the kernel's. */
        if (ph[i].vaddr >= 0xffff800000000000ull) {
            r.error = "a segment wants to live in kernel space";
            return r;
        }

        uint64_t start = ph[i].vaddr & ~(PAGE_SIZE - 1);
        uint64_t end   = (ph[i].vaddr + ph[i].memsz + PAGE_SIZE - 1)
                       & ~(PAGE_SIZE - 1);

        for (uint64_t v = start; v < end; v += PAGE_SIZE) {
            uint64_t phys = pmm_alloc_pages(1);
            if (phys == 0) {
                r.error = "out of memory";
                return r;
            }
            memset(pmm_phys_to_virt(phys), 0, PAGE_SIZE);

            /*
             * W^X holds in ring 3 too: writable segments are never
             * executable, and the rest are read-only
             */
            uint64_t flags = PTE_USER;
            if (ph[i].flags & PF_W) {
                flags |= PTE_WRITE | vmm_nx();
            } else if (!(ph[i].flags & PF_X)) {
                flags |= vmm_nx();
            }

            if (!vmm_map_range(pml4, v, phys, PAGE_SIZE, flags)) {
                r.error = "could not map a segment";
                return r;
            }
        }

        /*
         * copy the file's bytes in through the direct map rather than
         * the new mapping, which may well be read-only by now
         */
        const uint8_t *src = (const uint8_t *)image + ph[i].offset;
        for (uint64_t b = 0; b < ph[i].filesz; b++) {
            uint64_t va = ph[i].vaddr + b;
            uint64_t pa = vmm_translate(pml4, va);
            if (pa == VMM_NO_MAPPING) {
                r.error = "a segment byte landed nowhere";
                return r;
            }
            *(uint8_t *)pmm_phys_to_virt(pa) = src[b];
        }

        if (end > r.brk) {
            r.brk = end;
        }
    }

    r.entry = h->entry;
    r.ok = true;
    return r;
}

#endif /* VELVETOS_HOSTED */
