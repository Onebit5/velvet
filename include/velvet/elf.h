// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/elf.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * just enough elf64 to load a static executable.
 */

/* the design notes for elf.h are in docs/subsystems/fs.rst */

#ifndef VELVET_ELF_H
#define VELVET_ELF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* how big the header at the start of every elf file is */
#define ELF_HEADER_SIZE 64

/* how big one program header is */
#define ELF_PHDR_SIZE 56

/*
 * the most program headers the kernel reads. a program has a dozen, and
 * a file claiming thousands is asking for that much of the kernel's heap
 */
#define ELF_PHDRS_MAX 64

#define ELF_SEGMENTS_MAX 8

/*
 * a segment as the pages it covers, each of which is read out of the
 * file the first time it is touched
 */
struct elf_segment {
    uint64_t vaddr;         /* page aligned down */
    uint64_t end;           /* past the last byte, page aligned up */
    uint64_t file_end;      /* past the last byte with a *file* byte behind
                             * it. between here and `end` is bss */
    uint64_t offset;        /* offset in the file of `vaddr` */
    uint64_t flags;         /* the PTE flags a page here should have */
};

/* a program as its headers describe it, at the addresses it was linked at */
struct elf_image {
    uint64_t u_entry;           /* where it starts */
    uint64_t u_phdr;            /* where its headers are in memory, or 0 */
    uint64_t u_phnum;
    uint64_t u_high;            /* past the last page of any segment */
    size_t sz_segs;
    struct elf_segment arr_segs[ELF_SEGMENTS_MAX];
};

/*
 * the header, the first ELF_HEADER_SIZE bytes of a file u_size bytes
 * long: whether it is a program this kernel can load, and if it is,
 * where in the file its program headers are and how many bytes they take
 */
bool elf_check(const void *p_header, uint64_t u_size, uint64_t *p_phoff,
               size_t *p_phlen, const char **pp_why);

/*
 * the program the header and the program headers elf_check pointed at
 * describe, without anything mapped. false, with why, for one that
 * cannot be described
 */
bool elf_describe(const void *p_header, const void *p_phdrs, uint64_t u_size,
                  struct elf_image *p_out, const char **pp_why);

#endif
