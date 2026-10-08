// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_buddy.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for the buddy allocator.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

void panic(const char *fmt, ...)
{
    printf("PANIC: ");
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    exit(1);
}

#include "mm/buddy.h"

#define FRAMES 4096                     /* four order-10 blocks' worth */
#define FRAME_SIZE 4096

static uint8_t *arena;

static void *to_virt(uint64_t frame)
{
    return arena + frame * FRAME_SIZE;
}

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

/* how many frames are sitting on the lists, counted the slow way. */
static uint64_t count_by_hand(void)
{
    uint64_t frames = 0;
    for (unsigned o = 0; o <= BUDDY_MAX_ORDER; o++) {
        frames += buddy_blocks_at(o) * (1ull << o);
    }
    return frames;
}

static void reset(void *meta)
{
    buddy_init(FRAMES, meta, to_virt);
}

int main(void)
{
    arena = malloc((size_t)FRAMES * FRAME_SIZE);
    void *meta = malloc(buddy_metadata_bytes(FRAMES));



    CHECK(buddy_order_for(1) == 0, "one frame is order 0");
    CHECK(buddy_order_for(2) == 1, "two frames is order 1");
    CHECK(buddy_order_for(3) == 2, "three frames rounds up to order 2");
    CHECK(buddy_order_for(5) == 3, "five frames rounds up to eight");
    CHECK(buddy_order_for(1024) == 10, "1024 frames is the largest block");
    CHECK(buddy_order_for(1025) > BUDDY_MAX_ORDER,
          "too large gets an order that will be refused, not a small block");



    reset(meta);
    CHECK(buddy_free_frames() == 0, "starts with everything taken");
    CHECK(buddy_alloc(0) == BUDDY_NO_BLOCK, "and hands out nothing");



    reset(meta);
    buddy_add_range(0, 1024);
    CHECK(buddy_free_frames() == 1024, "a whole order-10 block went in");
    CHECK(buddy_blocks_at(10) == 1, "as exactly one block");

    uint64_t one = buddy_alloc(0);
    CHECK(one == 0, "the single frame comes off the front");
    CHECK(buddy_free_frames() == 1023, "and the count went down by one");
    /* splitting 1024 down to 1 leaves one block at every order below */
    for (unsigned o = 0; o < 10; o++) {
        CHECK(buddy_blocks_at(o) == 1, "one leftover block at each order");
    }
    CHECK(buddy_blocks_at(10) == 0, "and the big one is gone");

    buddy_free(one, 0);
    CHECK(buddy_free_frames() == 1024, "the frame came back");
    CHECK(buddy_blocks_at(10) == 1,
          "and the whole 4 MiB block reassembled itself");
    for (unsigned o = 0; o < 10; o++) {
        CHECK(buddy_blocks_at(o) == 0, "with nothing left in pieces");
    }



    reset(meta);
    buddy_add_range(0, 1024);
    uint64_t a = buddy_alloc(0);
    uint64_t b = buddy_alloc(0);
    CHECK(a != b, "two allocations are two different frames");
    CHECK((a ^ b) == 1, "and they are each other's buddies");

    buddy_free(a, 0);
    CHECK(buddy_blocks_at(10) == 0,
          "one half back is not enough to rebuild anything");
    buddy_free(b, 0);
    CHECK(buddy_blocks_at(10) == 1, "both halves back and it all merges");



    reset(meta);
    buddy_add_range(1, 100);            /* neither aligned nor a power of two */
    CHECK(buddy_free_frames() == 100, "an awkward range goes in whole");
    CHECK(count_by_hand() == 100, "and the lists agree with the count");

    reset(meta);
    buddy_add_range(3, 4093);
    CHECK(buddy_free_frames() == 4093, "so does one that runs to the end");
    CHECK(count_by_hand() == 4093, "lists still agree");

    /*
     * nothing may merge past the end of what the test manages: the frame just
     * off the top does not exist, and treating it as free would hand
     * out memory that is not there
     */
    reset(meta);
    buddy_add_range(2048, 2048);
    CHECK(buddy_free_frames() == 2048, "the top half went in");
    uint64_t top = buddy_alloc(10);
    CHECK(top != BUDDY_NO_BLOCK, "and a full block comes out of it");
    buddy_free(top, 10);
    CHECK(buddy_free_frames() == 2048, "and goes back");



    reset(meta);
    buddy_add_range(0, FRAMES);
    uint64_t expect = buddy_free_frames();
    CHECK(expect == FRAMES, "all four thousand frames are in");

    static uint8_t seen[FRAMES];
    memset(seen, 0, sizeof(seen));
    uint64_t got = 0, f;
    while ((f = buddy_alloc(0)) != BUDDY_NO_BLOCK) {
        CHECK(f < FRAMES, "frame is inside the arena");
        CHECK(!seen[f], "frame never handed out twice");
        seen[f] = 1;
        got++;
    }
    CHECK(got == expect, "drained exactly what was advertised");
    CHECK(buddy_free_frames() == 0, "and the count says so");

    /*
     * give them back in a deliberately awful order, every odd frame
     * first, then every even one, so almost every free has to check a
     * partner that is not ready yet
     */
    for (uint64_t i = 1; i < FRAMES; i += 2) buddy_free(i, 0);
    for (uint64_t i = 0; i < FRAMES; i += 2) buddy_free(i, 0);

    CHECK(buddy_free_frames() == FRAMES, "everything came home");
    CHECK(buddy_blocks_at(10) == FRAMES / 1024,
          "and merged all the way back into whole blocks");
    CHECK(count_by_hand() == FRAMES, "lists and count agree at the end");



    reset(meta);
    buddy_add_range(0, FRAMES);
    uint64_t before = buddy_free_frames();

    uint64_t blocks[32];
    unsigned orders[32];
    for (int i = 0; i < 32; i++) {
        orders[i] = (unsigned)(i % 7);          /* 1 to 64 frames */
        blocks[i] = buddy_alloc(orders[i]);
        CHECK(blocks[i] != BUDDY_NO_BLOCK, "mixed-size allocation succeeded");
        CHECK((blocks[i] & ((1ull << orders[i]) - 1)) == 0,
              "a block is aligned to its own size");
        /* write the whole thing, so an overlap would corrupt a neighbour */
        memset(to_virt(blocks[i]), i + 1,
               (size_t)(1ull << orders[i]) * FRAME_SIZE);
    }
    for (int i = 0; i < 32; i++) {
        uint8_t *p = to_virt(blocks[i]);
        int ok = 1;
        for (size_t j = 0; j < (size_t)(1ull << orders[i]) * FRAME_SIZE; j++) {
            if (p[j] != (uint8_t)(i + 1)) ok = 0;
        }
        CHECK(ok, "each block kept its pattern, so none overlapped");
    }
    for (int i = 31; i >= 0; i--) {
        buddy_free(blocks[i], orders[i]);
    }
    CHECK(buddy_free_frames() == before, "mixed sizes balance out");
    CHECK(buddy_blocks_at(10) == FRAMES / 1024, "and it is whole again");

    if (failures == 0) printf("all good\n");
    return failures;
}
