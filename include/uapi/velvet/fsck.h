// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/fsck.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what a check of a filesystem found, as the program that asked sees it.
 */

#ifndef UAPI_VELVET_FSCK_H
#define UAPI_VELVET_FSCK_H

#include <stdint.h>

enum fsck_problem {
    FSCK_SUPERBLOCK,        /* it does not describe itself consistently */
    FSCK_BLOCK_UNMARKED,    /* a file claims a block the bitmap calls free */
    FSCK_BLOCK_CROSSED,     /* two files claim the same block */
    FSCK_BLOCK_LEAKED,      /* the bitmap says used and nobody claims it */
    FSCK_INODE_UNMARKED,    /* a name points at an inode marked free */
    FSCK_INODE_ORPHANED,    /* in use, and no name points at it */
    FSCK_LINKS_WRONG,       /* the count is not the number of names */
    FSCK_DIR_NO_DOTS,       /* a directory without . or .. */
    FSCK_DIR_BAD_PARENT,    /* .. that is not the parent */
    FSCK_DIR_BAD_RECORD,    /* a record whose length walks off the block */
    FSCK_EXTENT_BAD,        /* a tree that does not parse */
    FSCK_COUNTS_WRONG,      /* a free count that is not the true one */
    FSCK_PROBLEMS
};

/* SYS_FSCK's flag. without it the check looks and changes nothing */
#define FSCK_MEND 1

#define FSCK_WHY_MAX 64

/*
 * what SYS_FSCK found: how many of each kind of problem and how many of
 * those were put right, then the filesystem as the check counted it.
 * when the call returns -1, `arr_why` says what stopped the check being
 * made, and nothing else in here means anything
 */
struct user_fsck {
    uint32_t arr_found[FSCK_PROBLEMS];
    uint32_t arr_mended[FSCK_PROBLEMS];
    uint32_t u_total_found, u_total_mended;
    uint32_t u_blocks, u_inodes, u_groups, u_block_size;
    uint32_t u_free_blocks, u_free_inodes;
    char     arr_why[FSCK_WHY_MAX];
};

#endif
