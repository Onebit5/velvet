// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/elf.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * reading what an elf64 executable says about itself.
 */

#include <velvet/elf.h>
#include <velvet/string.h>

/* what a segment turns into, in page table terms. */
#include <velvet/vmm.h>

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define ET_EXEC 2
#define ET_DYN  3
#define PT_LOAD 1
#define PT_INTERP 3
#define PT_PHDR 6

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

_Static_assert(sizeof(struct elf64_header) == ELF_HEADER_SIZE,
               "the header is 64 bytes, at the start of every file");
_Static_assert(sizeof(struct elf64_phdr) == ELF_PHDR_SIZE,
               "a program header is 56 bytes, as AT_PHENT says");

bool elf_check(const void *p_header, uint64_t u_size, uint64_t *p_phoff,
               size_t *p_phlen, const char **pp_why)
{
    const char *str_ignored;
    if (pp_why == NULL) {
        pp_why = &str_ignored;
    }

    if (p_header == NULL || u_size < sizeof(struct elf64_header)) {
        *pp_why = "too small to be an elf at all";
        return false;
    }

    const struct elf64_header *h = p_header;
    if (h->ident[0] != 0x7f || h->ident[1] != 'E'
        || h->ident[2] != 'L' || h->ident[3] != 'F') {
        *pp_why = "no elf magic";
        return false;
    }
    if (h->ident[4] != 2) {
        *pp_why = "not 64-bit";
        return false;
    }
    if (h->ident[5] != 1) {
        *pp_why = "not little endian";
        return false;
    }
    if (h->machine != 0x3e) {
        *pp_why = "not x86-64";
        return false;
    }
    /*
     * a program built for one place, or one built to be loaded anywhere,
     * which a shared library is too, a libc's loader among them
     */
    if (h->type != ET_EXEC && h->type != ET_DYN) {
        *pp_why = "not a program: an object file, or a core";
        return false;
    }
    if (h->phnum == 0) {
        *pp_why = "no program headers, so nothing to load";
        return false;
    }
    if (h->phentsize != sizeof(struct elf64_phdr)) {
        *pp_why = "program headers are the wrong size";
        return false;
    }
    if (h->phnum > ELF_PHDRS_MAX) {
        *pp_why = "more program headers than the kernel reads";
        return false;
    }
    if (h->phoff > u_size
        || (uint64_t)h->phnum * h->phentsize > u_size - h->phoff) {
        *pp_why = "program headers run past the end of the file";
        return false;
    }
    if (h->entry == 0) {
        *pp_why = "no entry point";
        return false;
    }
    *p_phoff = h->phoff;
    *p_phlen = (size_t)h->phnum * sizeof(struct elf64_phdr);
    return true;
}

/*
 * where the program headers are once the program is in memory, which a
 * libc reads its thread-local template out of: where PT_PHDR says, or
 * wherever the segment holding them in the file puts them, or nowhere
 */
static uint64_t headers_at(const struct elf64_header *h,
                           const struct elf64_phdr *ph)
{
    uint64_t u_bytes = (uint64_t)h->phnum * sizeof *ph;
    for (uint16_t i = 0; i < h->phnum; i++) {
        if (ph[i].type == PT_PHDR) {
            return ph[i].vaddr;
        }
    }
    for (uint16_t i = 0; i < h->phnum; i++) {
        if (ph[i].type == PT_LOAD && ph[i].offset <= h->phoff
            && h->phoff + u_bytes <= ph[i].offset + ph[i].filesz) {
            return ph[i].vaddr + (h->phoff - ph[i].offset);
        }
    }
    return 0;
}

bool elf_describe(const void *p_header, const void *p_phdrs, uint64_t u_size,
                  struct elf_image *p_out, const char **pp_why)
{
    const char *str_ignored;
    if (pp_why == NULL) {
        pp_why = &str_ignored;
    }
    uint64_t u_phoff;
    size_t sz_phlen;
    if (!elf_check(p_header, u_size, &u_phoff, &sz_phlen, pp_why)) {
        return false;
    }

    const struct elf64_header *h = p_header;
    const struct elf64_phdr *ph = p_phdrs;
    memset(p_out, 0, sizeof *p_out);
    p_out->u_entry = h->entry;
    p_out->u_phnum = h->phnum;
    p_out->u_phdr = headers_at(h, ph);
    p_out->b_anywhere = h->type == ET_DYN;
    p_out->u_low = ~0ull;

    for (uint16_t i = 0; i < h->phnum; i++) {
        /*
         * a program that names a loader is the loader's to start, and
         * started at its own entry it runs without the libc it was
         * linked against having been loaded at all
         */
        if (ph[i].type == PT_INTERP) {
            *pp_why = "it names a loader, which this kernel does not start";
            return false;
        }
        if (ph[i].type != PT_LOAD || ph[i].memsz == 0) {
            continue;
        }
        if (ph[i].offset > u_size || ph[i].filesz > u_size - ph[i].offset) {
            *pp_why = "a segment reaches past the end of the file";
            return false;
        }
        if (ph[i].filesz > ph[i].memsz) {
            *pp_why = "a segment holds more of the file than it has room for";
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
        if (ph[i].vaddr >= 0xffff800000000000ull
            || ph[i].memsz > 0xffff800000000000ull - ph[i].vaddr) {
            *pp_why = "a segment wants to live in kernel space";
            return false;
        }
        if (p_out->sz_segs == ELF_SEGMENTS_MAX) {
            *pp_why = "more segments than the kernel can describe";
            return false;
        }

        uint64_t start = ph[i].vaddr & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t end = (ph[i].vaddr + ph[i].memsz + PAGE_SIZE - 1)
                     & ~(uint64_t)(PAGE_SIZE - 1);

        /*
         * the page a segment starts on is read from the file from that
         * page's start, so the segment cannot begin further into its
         * page than into the file
         */
        if (ph[i].offset < ph[i].vaddr - start) {
            *pp_why = "a segment starts before the file does";
            return false;
        }

        /*
         * two segments sharing a page cannot both be described: one
         * record would have to fetch the other's bytes, and they may
         * not even agree about whether the page is writable. the
         * linker puts each on its own page, so this is a check rather
         * than a case
         */
        for (size_t j = 0; j < p_out->sz_segs; j++) {
            if (start < p_out->arr_segs[j].end
                && end > p_out->arr_segs[j].vaddr) {
                *pp_why = "two segments share a page";
                return false;
            }
        }

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

        struct elf_segment *p_seg = &p_out->arr_segs[p_out->sz_segs++];
        p_seg->vaddr = start;
        p_seg->end = end;
        p_seg->file_end = ph[i].vaddr + ph[i].filesz;
        p_seg->offset = ph[i].offset - (ph[i].vaddr - start);
        p_seg->flags = flags;

        if (end > p_out->u_high) {
            p_out->u_high = end;
        }
        if (start < p_out->u_low) {
            p_out->u_low = start;
        }
    }

    if (p_out->sz_segs == 0) {
        *pp_why = "nothing to load";
        return false;
    }
    return true;
}

void elf_move(struct elf_image *p_image, uint64_t u_by)
{
    p_image->u_entry += u_by;
    if (p_image->u_phdr != 0) {
        p_image->u_phdr += u_by;
    }
    p_image->u_low += u_by;
    p_image->u_high += u_by;
    for (size_t sz_at = 0; sz_at < p_image->sz_segs; sz_at++) {
        p_image->arr_segs[sz_at].vaddr += u_by;
        p_image->arr_segs[sz_at].end += u_by;
        p_image->arr_segs[sz_at].file_end += u_by;
    }
}
