// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/bcache.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a cache of disk blocks, between the filesystem and the drive.
 */

/* the design notes for bcache.h are in docs/subsystems/mm.rst */

#ifndef FS_BCACHE_H
#define FS_BCACHE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define BCACHE_SECTOR         512
#define BCACHE_BLOCK_SECTORS  8                     /* 4KiB, one page */
#define BCACHE_BLOCK_BYTES    (BCACHE_SECTOR * BCACHE_BLOCK_SECTORS)
#define BCACHE_BLOCKS         64                    /* 256KiB of cache */

typedef bool (*bcache_in)(void *ctx, uint64_t lba, uint32_t count, void *buf);
typedef bool (*bcache_out)(void *ctx, uint64_t lba, uint32_t count,
                           const void *buf);

void bcache_init(bcache_in read, bcache_out write, void *ctx);

bool bcache_read(void *ctx, uint64_t lba, uint32_t count, void *buf);
bool bcache_write(void *ctx, uint64_t lba, uint32_t count, const void *buf);

bool bcache_sync(void);

bool bcache_dirty(void);

struct bcache_stats {
    uint64_t hits;          /* answered without touching the drive */
    uint64_t misses;        /* had to go and get it */
    uint64_t writes;        /* blocks written by somebody above */
    uint64_t writebacks;    /* blocks actually handed to the drive */
    uint64_t evictions;
    size_t   held;          /* blocks with something in them */
    size_t   dirty;
};

void bcache_get_stats(struct bcache_stats *out);

static inline uint64_t bcache_block_of(uint64_t lba)
{
    return lba / BCACHE_BLOCK_SECTORS;
}
static inline uint32_t bcache_offset_of(uint64_t lba)
{
    return (uint32_t)(lba % BCACHE_BLOCK_SECTORS);
}

#endif
