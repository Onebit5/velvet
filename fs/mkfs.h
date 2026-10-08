// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/mkfs.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * making a filesystem, rather than reading one.
 */

/* the design notes for mkfs.h are in docs/subsystems/mm.rst */

#ifndef FS_MKFS_H
#define FS_MKFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* making a filesystem rather than reading one. */

typedef bool (*mkfs_io)(void *ctx, uint64_t lba, uint32_t count,
                        const void *buf);

struct mkfs_result {
    uint64_t blocks;            /* 1 KiB each */
    uint64_t inodes;
    uint64_t free_blocks;
    uint32_t groups;
};

bool mkfs_ext4(mkfs_io write, void *ctx, uint64_t sectors,
               const char *label, uint32_t now,
               struct mkfs_result *out, const char **error);

#define MKFS_MIN_SECTORS 512

#endif
