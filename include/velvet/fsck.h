// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/fsck.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * checking a filesystem, and mending it.
 */

/* the design notes for fsck.h are in docs/subsystems/mm.rst */

#ifndef VELVET_FSCK_H
#define VELVET_FSCK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define FSCK_SECTOR     512
#define FSCK_MAX_BLOCK  4096

typedef bool (*fsck_io)(void *ctx, uint64_t lba, uint32_t count, void *buf);
typedef bool (*fsck_out)(void *ctx, uint64_t lba, uint32_t count,
                         const void *buf);

/* the kinds of problem are a program's to name, so they are its header's */
#include <uapi/velvet/fsck.h>

struct fsck_report {
    uint32_t found[FSCK_PROBLEMS];      /* how many of each kind */
    uint32_t mended[FSCK_PROBLEMS];     /* and how many were put right */

    uint32_t total_found;
    uint32_t total_mended;

    uint32_t blocks, inodes, groups, block_size;
    uint32_t free_blocks, free_inodes;      /* the *true* counts */

    const char *stopped;
};

/*
 * how much scratch the check needs for a filesystem this size: a bit
 * per block, a bit per inode, and a link count per inode. handed in
 * rather than allocated, because nothing else in fs/ allocates either
 *, the kernel has kmalloc and the host test has malloc and this file
 * should not have to know which one it is talking to
 */
size_t fsck_workspace_bytes(uint32_t blocks_count, uint32_t inodes_count);

bool fsck_measure(fsck_io read, void *ctx, uint32_t *blocks_out,
                  uint32_t *inodes_out);

/*
 * check it. `write` may be NULL, and then nothing is mended however
 * much is found, which is what `fsck -n` means everywhere else.
 *
 * returns false only when the check could not be *made*: a superblock
 * that does not parse, a workspace too small, a disk that will not
 * read. a filesystem full of problems is a true answer and returns
 * true, with the problems in the report
 */
bool fsck_run(fsck_io read, fsck_out write, void *ctx,
              void *workspace, size_t workspace_size,
              struct fsck_report *out);

const char *fsck_problem_name(enum fsck_problem which);

#endif
