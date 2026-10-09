// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/bcache.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the block cache.
 */

#include <velvet/bcache.h>
#include <velvet/string.h>

/* no lock in here, and that is deliberate rather than forgotten. */

struct block {
    uint64_t index;         /* which block of the disk, not which sector */
    bool     valid;
    bool     dirty;
    uint64_t used_at;       /* the clock reading when it was last touched */
    uint8_t  data[BCACHE_BLOCK_BYTES];
};

static struct block blocks[BCACHE_BLOCKS];
static struct bcache_stats stats;

static bcache_in  disk_read;
static bcache_out disk_write;
static void      *disk_ctx;

/* a counter rather than a real clock. */
static uint64_t tick;

void bcache_init(bcache_in read, bcache_out write, void *ctx)
{
    disk_read = read;
    disk_write = write;
    disk_ctx = ctx;

    memset(blocks, 0, sizeof blocks);
    memset(&stats, 0, sizeof stats);
    tick = 0;
}

static struct block *find(uint64_t index)
{
    for (size_t i = 0; i < BCACHE_BLOCKS; i++) {
        if (blocks[i].valid && blocks[i].index == index) {
            return &blocks[i];
        }
    }
    return NULL;
}

static bool flush(struct block *b)
{
    if (!b->valid || !b->dirty) {
        return true;
    }
    if (disk_write == NULL) {
        return false;
    }
    if (!disk_write(disk_ctx, b->index * BCACHE_BLOCK_SECTORS,
                    BCACHE_BLOCK_SECTORS, b->data)) {
        return false;
    }
    b->dirty = false;
    stats.writebacks++;
    return true;
}

/* the block to throw out. */
static struct block *victim(void)
{
    struct block *oldest = &blocks[0];

    for (size_t i = 0; i < BCACHE_BLOCKS; i++) {
        if (!blocks[i].valid) {
            return &blocks[i];
        }
        if (blocks[i].used_at < oldest->used_at) {
            oldest = &blocks[i];
        }
    }

    if (!flush(oldest)) {
        return NULL;    /* it cannot be written, so it cannot be replaced */
    }
    stats.evictions++;
    return oldest;
}

/* the block holding `index`, read in if it is not here yet */
static struct block *fetch(uint64_t index)
{
    struct block *b = find(index);
    if (b != NULL) {
        stats.hits++;
        b->used_at = ++tick;
        return b;
    }

    stats.misses++;
    b = victim();
    if (b == NULL || disk_read == NULL) {
        return NULL;
    }

    if (!disk_read(disk_ctx, index * BCACHE_BLOCK_SECTORS,
                   BCACHE_BLOCK_SECTORS, b->data)) {
        /*
         * it may have scribbled over half of what was there, so what is
         * in this slot is now nobody's data
         */
        b->valid = false;
        b->dirty = false;
        return NULL;
    }

    b->index = index;
    b->valid = true;
    b->dirty = false;
    b->used_at = ++tick;
    return b;
}

bool bcache_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    (void)ctx;
    uint8_t *out = buf;

    while (count > 0) {
        struct block *b = fetch(bcache_block_of(lba));
        if (b == NULL) {
            return false;
        }

        uint32_t at = bcache_offset_of(lba);
        uint32_t take = BCACHE_BLOCK_SECTORS - at;
        if (take > count) {
            take = count;
        }

        memcpy(out, &b->data[at * BCACHE_SECTOR], take * BCACHE_SECTOR);

        out += take * BCACHE_SECTOR;
        lba += take;
        count -= take;
    }
    return true;
}

bool bcache_write(void *ctx, uint64_t lba, uint32_t count, const void *buf)
{
    (void)ctx;
    const uint8_t *in = buf;

    while (count > 0) {
        uint64_t index = bcache_block_of(lba);
        uint32_t at = bcache_offset_of(lba);
        uint32_t take = BCACHE_BLOCK_SECTORS - at;
        if (take > count) {
            take = count;
        }

        struct block *b;
        if (take == BCACHE_BLOCK_SECTORS) {
            /*
             * the whole block is being replaced, so reading it first
             * would be fetching bytes about to be thrown away. take a
             * slot and overwrite it.
             *
             * this is not a micro-optimisation, it is what makes
             * writing a large file cost one trip to the drive per block
             * rather than two
             */
            b = find(index);
            if (b == NULL) {
                b = victim();
                if (b == NULL) {
                    return false;
                }
                b->index = index;
                b->valid = true;
                b->dirty = false;
            }
            b->used_at = ++tick;
        } else {
            /*
             * part of a block. the rest of it has to be right, so it
             * has to be read
             */
            b = fetch(index);
            if (b == NULL) {
                return false;
            }
        }

        memcpy(&b->data[at * BCACHE_SECTOR], in, take * BCACHE_SECTOR);
        b->dirty = true;
        stats.writes++;

        in += take * BCACHE_SECTOR;
        lba += take;
        count -= take;
    }
    return true;
}

bool bcache_sync(void)
{
    bool ok = true;
    for (size_t i = 0; i < BCACHE_BLOCKS; i++) {
        if (!flush(&blocks[i])) {
            ok = false;
        }
    }
    return ok;
}

bool bcache_dirty(void)
{
    for (size_t i = 0; i < BCACHE_BLOCKS; i++) {
        if (blocks[i].valid && blocks[i].dirty) {
            return true;
        }
    }
    return false;
}

void bcache_get_stats(struct bcache_stats *out)
{
    *out = stats;
    out->held = 0;
    out->dirty = 0;
    for (size_t i = 0; i < BCACHE_BLOCKS; i++) {
        if (!blocks[i].valid) {
            continue;
        }
        out->held++;
        if (blocks[i].dirty) {
            out->dirty++;
        }
    }
}
