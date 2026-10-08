// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/addrspace.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a private view of memory.
 */

/* the design notes for addrspace.h are in docs/subsystems/mm.rst */

#ifndef MM_ADDRSPACE_H
#define MM_ADDRSPACE_H

#include <stdint.h>
#include <stdbool.h>

/*
 * a private view of memory: the lower half belongs to the program, the
 * upper half, where the kernel and the direct map live, is shared by
 * reference so the kernel is reachable no matter whose tables are
 * loaded. it has to be, since the stack the kernel is standing on when
 * it switches is up there
 */

#define VMA_MAX 16

enum vma_kind {
    VMA_NONE = 0,
    VMA_ANON,       /* zeroes. a stack, or something mmap handed out */
    VMA_FILE,       /* bytes out of an image that is already in memory */
};

struct vma {
    enum vma_kind kind;
    uint64_t start, end;        /* [start, end), page aligned */
    uint64_t flags;             /* what a page here is allowed to be */

    const uint8_t *image;
    uint64_t image_offset;      /* offset in the image of `start` */
    uint64_t file_end;          /* last address with a byte behind it */
};

struct addrspace {
    uint64_t pages_in_use;

    uint64_t pml4;      /* physical address of the top level table */

    struct vma vmas[VMA_MAX];
};

bool addrspace_add_region(struct addrspace *as, uint64_t start, uint64_t end,
                          uint64_t flags, enum vma_kind kind,
                          const uint8_t *image, uint64_t image_offset,
                          uint64_t file_end);

bool addrspace_drop_region(struct addrspace *as, uint64_t start);

uint64_t addrspace_reserve(struct addrspace *as, uint64_t len, uint64_t flags);

struct addrspace *addrspace_create(uint64_t kernel_pml4);

struct addrspace *addrspace_fork(const struct addrspace *from,
                                 uint64_t kernel_pml4);

/*
 * a fault. one of two things, and the difference is `present`:
 *
 *   a write to a page that is there but marked copy-on-write. give this
 *   space a private copy and let the write through.
 *
 *   a touch of a page that is not there at all. if the address is
 *   inside a region this space agreed to, make one; otherwise it is a
 *   wild pointer and always was.
 *
 * returns false if it was neither, which means the fault was a real one
 * and the caller should treat it as such
 */
bool addrspace_fault(struct addrspace *as, uint64_t virt, bool write,
                     bool present);

void addrspace_destroy(struct addrspace *as);

void addrspace_switch(struct addrspace *as);

uint64_t addrspace_frames(struct addrspace *as);

#endif
