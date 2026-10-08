// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/buddy.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a buddy allocator, in place of a linear scan.
 */

/* the design notes for buddy.h are in docs/subsystems/mm.rst */

#ifndef MM_BUDDY_H
#define MM_BUDDY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define BUDDY_MAX_ORDER 10          /* 2^10 frames = 4 MiB, the largest block */
#define BUDDY_NO_BLOCK  UINT64_MAX

uint64_t buddy_metadata_bytes(uint64_t frames);

void buddy_init(uint64_t frames, void *metadata,
                void *(*to_virt)(uint64_t frame));

void buddy_add_range(uint64_t first_frame, uint64_t count);

unsigned buddy_order_for(uint64_t frames);

bool buddy_is_free_block(uint64_t frame, unsigned order);

uint64_t buddy_alloc(unsigned order);

/*
 * give one back. the order must be the one it was taken with, which is
 * why callers free with the same count they allocated
 */
void buddy_free(uint64_t frame, unsigned order);

uint64_t buddy_free_frames(void);

uint64_t buddy_blocks_at(unsigned order);

#endif
