// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/elf.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * just enough elf64 to load a static executable.
 */

/* the design notes for elf.h are in docs/subsystems/mm.rst */

#ifndef VELVET_ELF_H
#define VELVET_ELF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct elf_load_result {
    uint64_t entry;         /* where to start executing */
    uint64_t brk;           /* first address past everything the kernel mapped */
    bool     ok;
    const char *error;      /* why not, when ok is false */
};

bool elf_is_loadable(const void *image, uint64_t size, const char **why);

/*
 * map every PT_LOAD segment into the given address space with user
 * permissions, copying from the image. frames come from the pmm.
 *
 * the space does not have to be the live one: the copy goes through the
 * direct map, which is mapped identically everywhere
 */
struct elf_load_result elf_load(const void *image, uint64_t size,
                                uint64_t pml4);

/*
 * for an image that will still be in memory when the program runs, the
 * loader has nothing to do at load time: the segments can be *recorded*
 * and each page fetched from the image the first time it is touched. a
 * program starts without a single byte of it having been read.
 *
 * "will still be in memory" is the whole condition, and it is why this
 * is a separate call rather than the only one. the ramdisk is a tar
 * the loader handed over and never moves; a program read off the disk is
 * a copy on the heap that somebody has to free.
 */

#define ELF_SEGMENTS_MAX 8

struct elf_segment {
    uint64_t vaddr;         /* page aligned down */
    uint64_t end;           /* past the last byte, page aligned up */
    uint64_t file_end;      /* past the last byte with a *file* byte behind
                             * it. between here and `end` is bss */
    uint64_t offset;        /* offset in the image of `vaddr` */
    uint64_t flags;         /* the PTE flags a page here should have */
};

bool elf_describe(const void *image, uint64_t size,
                  struct elf_segment *out, size_t max, size_t *count,
                  uint64_t *entry, uint64_t *brk, const char **why);

#endif
