// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/jbd2.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the journal, which is ext4's answer to the question ordered writes
 * answer the other way.
 */

/* the design notes for jbd2.h are in docs/subsystems/mm.rst */

#ifndef FS_JBD2_H
#define FS_JBD2_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define JBD_MAGIC        0xc03b3998u

#define JBD_DESCRIPTOR   1
#define JBD_COMMIT       2
#define JBD_SUPERBLOCK_V1 3
#define JBD_SUPERBLOCK_V2 4
#define JBD_REVOKE       5

#define JBD_FEATURE_INCOMPAT_REVOKE  0x00000001u
#define JBD_FEATURE_INCOMPAT_64BIT   0x00000002u
#define JBD_FEATURE_INCOMPAT_ASYNC   0x00000004u
#define JBD_FEATURE_INCOMPAT_CSUM_V2 0x00000008u
#define JBD_FEATURE_INCOMPAT_CSUM_V3 0x00000010u

#define JBD_FLAG_ESCAPE    0x0001   /* the block began with the magic */
#define JBD_FLAG_SAME_UUID 0x0002   /* no uuid follows this tag */
#define JBD_FLAG_DELETED   0x0004
#define JBD_FLAG_LAST_TAG  0x0008

#define JBD_MAX_BLOCKS   16

#define JBD_MAP_MAX      1024

struct ext4;

struct jbd {
    struct ext4 *fs;

    bool     ready;             /* there is a journal and the kernel understands it */
    bool     recovered;         /* and it has been replayed */

    uint32_t block_size;
    uint32_t maxlen;            /* blocks in the journal */
    uint32_t first;             /* the first that may hold a transaction */
    uint32_t sequence;          /* the id the next commit will carry */
    uint32_t start;             /* where the next transaction goes */

    uint8_t  uuid[16];

    uint32_t map[JBD_MAP_MAX];

    uint32_t depth;
    uint32_t staged;
    uint32_t home[JBD_MAX_BLOCKS];
    bool     escaped[JBD_MAX_BLOCKS];   /* it began with the magic */
};

/*
 * find the journal, understand it, and replay anything committed that
 * has not reached its home yet.
 *
 * false means there is no usable journal, which is not an error by
 * itself: a filesystem without one is the ordered arrangement and works
 * exactly as it did
 */
bool jbd_mount(struct jbd *j, struct ext4 *fs);

void jbd_begin(struct jbd *j);
bool jbd_commit(struct jbd *j);

/*
 * and the end of the log's working life: say it holds nothing, so that
 * the next mount replays nothing. the one thing that makes a start of
 * zero true, and the reason it means what it means
 */
bool jbd_close(struct jbd *j);

bool jbd_open(const struct jbd *j);

bool jbd_stage(struct jbd *j, uint32_t block, const void *data);

bool jbd_peek(struct jbd *j, uint32_t block, void *into);

#endif
