// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/addrspace.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a private view of memory.
 */

/* the design notes for addrspace.h are in docs/subsystems/mm.rst */

#ifndef VELVET_ADDRSPACE_H
#define VELVET_ADDRSPACE_H

#include <stdint.h>
#include <stdbool.h>

/*
 * a private view of memory: the lower half belongs to the program, the
 * upper half, where the kernel and the direct map live, is shared by
 * reference so the kernel is reachable no matter whose tables are
 * loaded. it has to be, since the stack the kernel is standing on when
 * it switches is up there
 */

/*
 * regions one space may hold. neighbours with the same flags merge, so
 * this counts the distinct stretches a program has, not how often it
 * asked: a heap grown a page at a time is still one
 */
#define VMA_MAX 64

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

    uint64_t u_brk_start;       /* where the program break began, at exec */
    uint64_t u_brk;             /* and where it is now */

    struct vma vmas[VMA_MAX];
};

bool addrspace_add_region(struct addrspace *as, uint64_t start, uint64_t end,
                          uint64_t flags, enum vma_kind kind,
                          const uint8_t *image, uint64_t image_offset,
                          uint64_t file_end);

bool addrspace_drop_region(struct addrspace *as, uint64_t start);

uint64_t addrspace_reserve(struct addrspace *as, uint64_t len, uint64_t flags);

/*
 * anonymous memory, u_len rounded up to pages, made as it is touched: at
 * u_hint when that range is free, anywhere free otherwise, or there and
 * nowhere else, over whatever was there, when b_fixed. where it went, or
 * 0
 */
uint64_t addrspace_map_anon(struct addrspace *p_as, uint64_t u_hint,
                            uint64_t u_len, uint64_t u_flags, bool b_fixed);

/*
 * nothing at [u_start, u_start + u_len) any more, whatever regions it cut
 * through, which keep what is either side of it. false only when one
 * region would have to become two and there is no room for the second,
 * with nothing changed
 */
bool addrspace_unmap(struct addrspace *p_as, uint64_t u_start, uint64_t u_len);

/*
 * [u_start, u_start + u_len) allowed to be u_flags from now on, pages already
 * there included. false when part of the range is in no region, or a
 * region would have to be split with no room, with nothing changed
 */
bool addrspace_protect(struct addrspace *p_as, uint64_t u_start,
                       uint64_t u_len, uint64_t u_flags);

/*
 * the program break moved to u_want, the heap growing or shrinking with
 * it, u_flags being what a page of it is: where the break is afterwards,
 * which is where it was when it could not move, and is all a u_want below
 * where it began asks for
 */
uint64_t addrspace_brk(struct addrspace *p_as, uint64_t u_want,
                       uint64_t u_flags);

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
