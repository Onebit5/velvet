// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/buddy.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the buddy allocator.
 */

#include "mm/buddy.h"
#include "lib/string.h"

/*
 * one bit per block per order: set means "this block is free and on the
 * list for its order". that bit is what makes coalescing possible,
 * without it, asking whether a buddy is free would mean checking every
 * frame inside it
 */
static uint64_t *free_map[BUDDY_MAX_ORDER + 1];
static uint64_t  head[BUDDY_MAX_ORDER + 1];

static uint64_t total_frames;
static uint64_t free_count;
static void *(*frame_to_virt)(uint64_t frame);

/* the free lists are threaded through the free pages themselves. */
struct free_block {
    uint64_t next;      /* frame number, or BUDDY_NO_BLOCK */
};

static uint64_t blocks_at_order(uint64_t frames, unsigned order)
{
    return (frames >> order) + 1;   /* +1 so a partial block still has a bit */
}

uint64_t buddy_metadata_bytes(uint64_t frames)
{
    uint64_t bytes = 0;
    for (unsigned o = 0; o <= BUDDY_MAX_ORDER; o++) {
        bytes += (blocks_at_order(frames, o) + 7) / 8;
    }
    /* round up so each order's bitmap can start on its own word */
    return bytes + 8 * (BUDDY_MAX_ORDER + 1);
}

unsigned buddy_order_for(uint64_t frames)
{
    unsigned order = 0;
    while (order <= BUDDY_MAX_ORDER && (1ull << order) < frames) {
        order++;
    }
    return order;
}

static uint64_t block_index(uint64_t frame, unsigned order)
{
    return frame >> order;
}

static bool is_free(uint64_t frame, unsigned order)
{
    uint64_t i = block_index(frame, order);
    return (free_map[order][i / 64] >> (i % 64)) & 1;
}

static void mark_free(uint64_t frame, unsigned order, bool value)
{
    uint64_t i = block_index(frame, order);
    if (value) {
        free_map[order][i / 64] |= 1ull << (i % 64);
    } else {
        free_map[order][i / 64] &= ~(1ull << (i % 64));
    }
}

static void list_push(uint64_t frame, unsigned order)
{
    struct free_block *b = frame_to_virt(frame);
    b->next = head[order];
    head[order] = frame;
    mark_free(frame, order, true);
}

static void list_remove(uint64_t frame, unsigned order)
{
    uint64_t *link = &head[order];
    while (*link != BUDDY_NO_BLOCK) {
        if (*link == frame) {
            struct free_block *b = frame_to_virt(frame);
            *link = b->next;
            mark_free(frame, order, false);
            return;
        }
        struct free_block *b = frame_to_virt(*link);
        link = &b->next;
    }
}

void buddy_init(uint64_t frames, void *metadata,
                void *(*to_virt)(uint64_t frame))
{
    total_frames = frames;
    free_count = 0;
    frame_to_virt = to_virt;

    uint8_t *p = metadata;
    memset(metadata, 0, buddy_metadata_bytes(frames));

    for (unsigned o = 0; o <= BUDDY_MAX_ORDER; o++) {
        free_map[o] = (uint64_t *)p;
        uint64_t bytes = (blocks_at_order(frames, o) + 7) / 8;
        p += (bytes + 7) & ~7ull;       /* keep each one word-aligned */
        head[o] = BUDDY_NO_BLOCK;
    }
}

void buddy_add_range(uint64_t first, uint64_t count)
{
    /*
     * a usable region is neither aligned nor a power of two, so give it
     * away as the largest aligned blocks that will fit, biggest first,
     * which naturally leaves the ragged ends as small ones
     */
    while (count > 0) {
        unsigned order = BUDDY_MAX_ORDER;

        for (;;) {
            uint64_t size = 1ull << order;
            /* a block has to be aligned to its own size, and fit */
            if (size <= count && (first & (size - 1)) == 0) {
                break;
            }
            if (order == 0) {
                break;
            }
            order--;
        }

        buddy_free(first, order);
        uint64_t size = 1ull << order;
        first += size;
        count -= size;
    }
}

uint64_t buddy_alloc(unsigned order)
{
    if (order > BUDDY_MAX_ORDER) {
        return BUDDY_NO_BLOCK;
    }

    /* the smallest block big enough. */
    unsigned from = order;
    while (from <= BUDDY_MAX_ORDER && head[from] == BUDDY_NO_BLOCK) {
        from++;
    }
    if (from > BUDDY_MAX_ORDER) {
        return BUDDY_NO_BLOCK;      /* nothing large enough anywhere */
    }

    uint64_t frame = head[from];
    struct free_block *b = frame_to_virt(frame);
    head[from] = b->next;
    mark_free(frame, from, false);

    /* split down, putting each unused half back on its own list */
    while (from > order) {
        from--;
        uint64_t half = frame + (1ull << from);
        list_push(half, from);
    }

    free_count -= 1ull << order;
    return frame;
}

void buddy_free(uint64_t frame, unsigned order)
{
    if (order > BUDDY_MAX_ORDER) {
        return;
    }
    free_count += 1ull << order;

    /* merge upwards for as long as its partner is also free. */
    while (order < BUDDY_MAX_ORDER) {
        uint64_t buddy = frame ^ (1ull << order);

        if (buddy + (1ull << order) > total_frames) {
            break;      /* its partner is off the end of memory */
        }
        if (!is_free(buddy, order)) {
            break;      /* it is in use, so there is nothing to merge with */
        }

        list_remove(buddy, order);

        /* the merged block starts at whichever half is lower */
        if (buddy < frame) {
            frame = buddy;
        }
        order++;
    }

    list_push(frame, order);
}

bool buddy_is_free_block(uint64_t frame, unsigned order)
{
    if (order > BUDDY_MAX_ORDER || frame >= total_frames) {
        return false;
    }
    return is_free(frame, order);
}

uint64_t buddy_free_frames(void)
{
    return free_count;
}

uint64_t buddy_blocks_at(unsigned order)
{
    if (order > BUDDY_MAX_ORDER) {
        return 0;
    }
    uint64_t n = 0;
    for (uint64_t f = head[order]; f != BUDDY_NO_BLOCK; ) {
        n++;
        struct free_block *b = frame_to_virt(f);
        f = b->next;
    }
    return n;
}
